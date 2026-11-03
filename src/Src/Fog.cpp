#include "pch.h"
#include "Fog.h"
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <mutex>
#include <vector>
#include <cmath>
#include <algorithm>
#include <unordered_map>
#include <d3d11shader.h>
#include "GpuTrace.h"
#include "GameInfo.h"
#include "Settings.h"

namespace
{
    // ---- the shaders ----
    // P: the view's projection (view+0x7390) as stored, rows P[0..3]. Either matrix convention is handled: the row or
    // the column holding (0, 0, +-1, 0) says which. V: its view matrix (view+0x7450, rows -left / up / -forward: ENGINE.md),
    // for each pixel's ray in the world. The fog has ONE colour per map: the average of the sky near the horizon all the
    // way round (64 world directions, each learned as the player looks that way), settled once every direction has been
    // seen (or 20 s into the map with 8 of them) and kept for the map. The fog fades in as soon as 4 directions are seen,
    // with their average, refined as more are seen until it settles (2026-11-01: it waited for the settled colour, up to
    // 40 s into a map). Per-direction colours
    // (tried 2026-10-31) differed between the eyes at a sky's transitions and could not show on the sky itself.
    // The colour follows the game's EXPOSURE (2026-10-31, reported: the fog changed with a hand over the head or under
    // a covering -- "depending on the player, not the environment" -- and was too dark in a second map). The game draws
    // the scene into "compressed" HDR buffers: divided by the auto-exposure's EngineLuminanceFactor (its shaders: the
    // sun sprite's colour / (EngineLuminanceFactor x white level) when cb_usecompressedhdrbuffers; a debug view
    // multiplies it back). Measured: the sky's drawn brightness moved as 1 / EngineLuminanceFactor (0.5x at 2x the
    // factor). So the fog's colours are kept as drawn x EngineLuminanceFactor (the game's own buffer
    // ro_postfx_luminance_buffautoexposure, bound here as Expo) and divided by the factor of the frame being drawn: the
    // fog then changes with the exposure exactly as the scene does. 512 sky cells (64 directions x 8 heights) learned
    // the same way check it: their ratio now (Summary[4].x) stays ~1 when the model holds (logged). And the fog never
    // goes darker than CheapFogMinBrightness (real fog scatters light: never black or dark grey).
    // (in pieces: MSVC takes at most ~16 KB in one string literal, C2026; adjacent literals are joined)
    const char* kHlsl = R"(
cbuffer FogCB : register(b0)
{
    float4 P[4];
    float4 V[3];   // the view matrix's rows (view+0x7450: -left, up, -forward in world, z up), xyz
    float4 g0;   // start (m), 1 / (end - start), opacity, use the fixed colour (0 / 1)
    float4 g1;   // the fixed colour (linear, times CheapFogBrightness); w 1 = the sky's fog reaches up to the far background
    float4 g2;   // width, height, sample step of the sky's survey, 1 = learning the colours now
    float4 g3;   // 1 = the map's time to settle on a colour is up, the fog's fade-in (0..1), frames each direction is learned over,
                 // sin(CheapFogSkyHeight) (0: sky not fogged)
    float4 g4;   // the fog's least luminance (x EngineLuminanceFactor), 1 = follow the game's exposure, 1 = the water's
                 // depth is bound as Water, elements of the game's exposure buffer bound as Expo (0: not found, the factor taken as 1)
};
Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
StructuredBuffer<float4> SummaryIn : register(t2);
Texture2D<float> Water : register(t4);              // RENDER_WATER's depth buffer (the scene's depth copied, the water's
                                                    // surface drawn into it), when it has one: g4.z = 1
RWStructuredBuffer<uint> Acc : register(u0);        // this frame's horizon sky, per direction: r, g, b, weight (x256);
                                                    // [256] the highest far-background pixel's sin(elevation) + 2, as uint;
                                                    // [257], [258] the upper sky's (25-75 deg) luminance (x256) and pixels;
                                                    // from kCell0: per sky cell luminance (x4096) and pixels;
                                                    // [kWater0], [+1]: how far below the eye (m x16) the ground is where
                                                    // nothing lies beyond it below the horizon, and how many such samples
RWStructuredBuffer<float4> Bins : register(u1);     // learned per direction: rgb, frames averaged (kept once full)
RWStructuredBuffer<float4> Summary : register(u2);  // [0] the average of the learned directions, how many; [1] darkest /
                                                    // brightest, the far background's top (sin elevation, the map's
                                                    // highest); [2] THE fog colour for the map, w 1 = settled (then kept);
                                                    // [3] the upper sky's mean luminance, frames averaged; [4] the exposure
                                                    // now relative to the reference, and when the colour was set; [5]
                                                    // the plane below the horizon's empty pixels (m below the eye), frames
RWStructuredBuffer<float4> Ref : register(u3);      // per sky cell: its luminance x EngineLuminanceFactor, frames learned
struct AutoExposure { float4 a; float4 b; };        // postfx_luminance_autoexposure_t: a.x = EngineLuminanceFactor
StructuredBuffer<AutoExposure> Expo : register(t3); // the game's own (ro_postfx_luminance_buffautoexposure)
float Elf()
{   // the game's EngineLuminanceFactor now (the two elements are the last two frames': their mean); 1 = not known
    if (g4.w < 0.5) return 1;
    float e = g4.w > 1.5 ? 0.5 * (Expo[0].a.x + Expo[1].a.x) : Expo[0].a.x;
    return (e > 1e-4 && e < 1e4) ? e : 1;
}
static const uint kBins = 64;
static const uint kCells = 512, kCell0 = kBins * 4 + 4, kWater0 = kCell0 + kCells * 2;
static const float3 kLum = float3(0.2126, 0.7152, 0.0722);

// the distance (m) along the eye's ray to what pixel `pix` (its centre, in pixels) shows; 0 = the sky: the depth
// buffer's clear value (no geometry), or nothing nearer than 20 km
float Distance(float2 pix, float depth)
{
    float4 R0 = P[0], R1 = P[1], R2 = P[2], R3 = P[3];
    bool colVec = abs(abs(R3.z) - 1) < 1e-3;
    float s = colVec ? R3.z : R2.w;
    float a = R2.z;
    float b = colVec ? R2.w : R3.z;
    float offx = colVec ? R0.z : R2.x, offy = colVec ? R1.z : R2.y;
    float cleared = saturate(a * s) > 0.5 ? 1.0 : 0.0;   // depth at infinity: 1 (standard) or 0 (reversed)
    if (depth == cleared) return 0;
    float den = depth - a * s;
    if (abs(den) < 1e-12) return 0;
    float d = b / den;
    if (!(d > 0) || d > 20000) return 0;
    float2 ndc = float2(pix.x / g2.x * 2 - 1, 1 - pix.y / g2.y * 2);
    float rx = (ndc.x * s - offx) / R0.x, ry = (ndc.y * s - offy) / R1.y;
    return d * sqrt(1 + rx * rx + ry * ry);
}
// the distance to what the pixel shows, the water's surface included (the nearer of the scene's depth and the water's)
float SceneDistance(float2 pix, uint2 p)
{
    float d = Distance(pix, Depth[p]);
    if (g4.z > 0.5)
    {
        float w = Distance(pix, Water[p]);
        d = d > 0 && w > 0 ? min(d, w) : max(d, w);
    }
    return d;
}
// the pixel's ray in the world (unit; z up)
float3 WorldDir(float2 pix)
{
    float4 R0 = P[0], R1 = P[1], R2 = P[2], R3 = P[3];
    bool colVec = abs(abs(R3.z) - 1) < 1e-3;
    float s = colVec ? R3.z : R2.w;
    float offx = colVec ? R0.z : R2.x, offy = colVec ? R1.z : R2.y;
    float2 ndc = float2(pix.x / g2.x * 2 - 1, 1 - pix.y / g2.y * 2);
    float3 dv = float3(s * (ndc.x * s - offx) / R0.x, s * (ndc.y * s - offy) / R1.y, -1);   // view space: x right, y up, -z forward
    return normalize(dv.x * V[0].xyz + dv.y * V[1].xyz + dv.z * V[2].xyz);
}
float BinOf(float3 wd) { return (atan2(wd.y, wd.x) / 6.2831853 + 0.5) * kBins; }
// the sky cell of a direction (-10 to +76 deg): 64 directions x 8 heights
uint CellOf(float3 wd) { return min(7u, (uint)((wd.z + 0.17) / 1.14 * 8)) * kBins + min(kBins - 1, (uint)BinOf(wd)); }

)"
R"(// The sky near the horizon (-10 to +30 deg, weighted towards it), by the world direction it lies in: 64 directions
// round the horizon. Summed per group, then once per group into Acc.
groupshared uint ga[kBins * 4];
groupshared uint gtop;
groupshared uint gup[2];
groupshared uint gc[kCells * 2];
groupshared uint gh[2];
[numthreads(16, 16, 1)]
void sky(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    ga[gi] = 0;
    for (uint z = gi; z < kCells * 2; z += 256) gc[z] = 0;
    if (gi == 0) { gtop = 0; gup[0] = 0; gup[1] = 0; gh[0] = 0; gh[1] = 0; }
    GroupMemoryBarrierWithGroupSync();
    uint2 p = (gid.xy * 16 + tid.xy) * (uint)g2.z;
    float dist = (p.x < (uint)g2.x && p.y < (uint)g2.y) ? SceneDistance(p + 0.5, p) : -1;
    if (dist >= g0.x + 1 / g0.y)
    {   // the far background (beyond the fog's end): how high above the horizon it reaches
        float3 wd = WorldDir(p + 0.5);
        InterlockedMax(gtop, asuint(clamp(wd.z, -1, 1) + 2));
    }
    if (dist == 0)
    {
        float3 wd = WorldDir(p + 0.5);
        if (wd.z < -0.002)
        {   // nothing below the horizon (the sea beyond its floor, with no water depth): how far below the eye the
            // ground just under it on the screen is -- the plane those pixels are fogged at, so the two meet
            uint2 q = p + uint2(0, (uint)g2.z);
            float dq = q.y < (uint)g2.y ? SceneDistance(q + 0.5, q) : 0;
            if (dq > 0)
            {
                float h = dq * -WorldDir(q + 0.5).z;
                if (h > 0.3 && h < 250) { InterlockedAdd(gh[0], (uint)(h * 16)); InterlockedAdd(gh[1], 1); }
            }
        }
        if (wd.z > 0.42 && wd.z < 0.966)
        {   // the upper sky (for CheapFogColor = auto: night, dusk or day)
            float y = min(dot(Color[p].rgb, float3(0.2126, 0.7152, 0.0722)), 16.0);
            if (y == y) { InterlockedAdd(gup[0], (uint)(y * 256)); InterlockedAdd(gup[1], 1); }
        }
        if (wd.z > -0.17 && wd.z < 0.97)
        {   // the exposure's reference: the sky's luminance by cell
            float y = min(dot(Color[p].rgb, kLum), 16.0);
            if (y == y) { uint cell = CellOf(wd); InterlockedAdd(gc[cell * 2], (uint)(y * 4096)); InterlockedAdd(gc[cell * 2 + 1], 1); }
        }
        float w = saturate(1 - wd.z / 0.5);
        float3 c = min(Color[p].rgb, 16.0);
        if (wd.z > -0.17 && w > 0 && all(c == c))
        {
            uint b = min(kBins - 1, (uint)BinOf(wd));
            InterlockedAdd(ga[b * 4 + 0], (uint)(c.r * w * 256));
            InterlockedAdd(ga[b * 4 + 1], (uint)(c.g * w * 256));
            InterlockedAdd(ga[b * 4 + 2], (uint)(c.b * w * 256));
            InterlockedAdd(ga[b * 4 + 3], (uint)(w * 256));
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (ga[gi] != 0) InterlockedAdd(Acc[gi], ga[gi]);
    if (gi == 0 && gtop != 0) InterlockedMax(Acc[kBins * 4], gtop);
    if (gi == 0 && gup[1] != 0) { InterlockedAdd(Acc[kBins * 4 + 1], gup[0]); InterlockedAdd(Acc[kBins * 4 + 2], gup[1]); }
    if (gi == 0 && gh[1] != 0) { InterlockedAdd(Acc[kWater0], gh[0]); InterlockedAdd(Acc[kWater0 + 1], gh[1]); }
    for (uint k = gi; k < kCells * 2; k += 256) if (gc[k] != 0) InterlockedAdd(Acc[kCell0 + k], gc[k]);
}

)"
R"(// Each direction: this frame's sky added to what it has learned (until it has enough: then kept as it is), the
// frame's sums cleared; then the average round the horizon. It becomes the map's one fog colour, kept from then on,
// once every direction has been seen -- or, after the map's first 30 s, once at least 8 of them have.
groupshared float4 gb[kBins];
groupshared float gw[kBins], gl[kBins];
groupshared float gr, gm;
[numthreads(64, 1, 1)]
void skyfinal(uint gi : SV_GroupIndex)
{
    // the check of the exposure model: the sky seen this frame x EngineLuminanceFactor against its cells' reference
    // (a weighted mean of the log ratios; ~1 when the model holds); no cell to compare with: as it was
    const float e = Elf();
    const float rPrev = Summary[4].x > 0 ? Summary[4].x : 1;
    float sw = 0, sl = 0;
    float cur[8]; uint cn[8];
    [unroll] for (uint k = 0; k < 8; k++)
    {
        uint cell = gi * 8 + k;
        uint s = Acc[kCell0 + cell * 2], n = Acc[kCell0 + cell * 2 + 1];
        Acc[kCell0 + cell * 2] = 0; Acc[kCell0 + cell * 2 + 1] = 0;
        cn[k] = n; cur[k] = n > 0 ? (float)s / 4096.0 / (float)n * e : 0;
        float4 ref = Ref[cell];
        if (n >= 4 && ref.y >= 4 && ref.x > 1e-3 && cur[k] > 1e-3) { float w = min((float)n, 64.0); sw += w; sl += w * log(cur[k] / ref.x); }
    }
    gw[gi] = sw; gl[gi] = sl;
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
    {
        float W = 0, L = 0;
        for (uint i = 0; i < kBins; i++) { W += gw[i]; L += gl[i]; }
        gr = W >= 64 ? clamp(exp(L / W), 1.0 / 64, 64) : rPrev;
        gm = W >= 64 ? 1 : 0;
    }
    GroupMemoryBarrierWithGroupSync();
    const float r = gr;
    [unroll] for (uint k2 = 0; k2 < 8; k2++)
    {   // cells not learned yet
        uint cell = gi * 8 + k2;
        float4 ref = Ref[cell];
        if (g2.w > 0 && cn[k2] >= 4 && cur[k2] > 1e-3 && ref.y < 60) Ref[cell] = float4((ref.x * ref.y + cur[k2]) / (ref.y + 1), ref.y + 1, 0, 0);
    }

    uint4 a = uint4(Acc[gi * 4 + 0], Acc[gi * 4 + 1], Acc[gi * 4 + 2], Acc[gi * 4 + 3]);
    Acc[gi * 4 + 0] = 0; Acc[gi * 4 + 1] = 0; Acc[gi * 4 + 2] = 0; Acc[gi * 4 + 3] = 0;
    float4 bin = Bins[gi];
    if (g2.w > 0 && a.w > 20 * 256 && bin.w < g3.z)   // enough of this direction's sky seen this frame
    {
        float3 c = float3(a.xyz) / (float)a.w * e;   // x EngineLuminanceFactor: what the exposure does not change
        bin = float4((bin.rgb * bin.w + c) / (bin.w + 1), bin.w + 1);
        Bins[gi] = bin;
    }
    gb[gi] = bin;
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
    {
        float3 sum = 0; float n = 0, lo = 1e9, hi = 0;
        for (uint i = 0; i < kBins; i++)
            if (gb[i].w > 0)
            {
                sum += gb[i].rgb; n += 1;
                float y = dot(gb[i].rgb, float3(0.2126, 0.7152, 0.0722));
                lo = min(lo, y); hi = max(hi, y);
            }
        float3 avg = n > 0 ? sum / n : 0;
        Summary[0] = float4(avg, n);
        float top = Summary[1].z;
        uint ft = Acc[kBins * 4];
        Acc[kBins * 4] = 0;
        if (g2.w > 0 && ft != 0) top = max(top, min(asfloat(ft) - 2, 0.7071));   // at most 45 deg
        Summary[1] = float4(n > 0 ? lo : 0, hi, top, 0);
        uint us = Acc[kBins * 4 + 1], un = Acc[kBins * 4 + 2];
        Acc[kBins * 4 + 1] = 0; Acc[kBins * 4 + 2] = 0;
        float4 up = Summary[3];
        if (g2.w > 0 && un > 50)
        {
            float y = (float)us / 256.0 / (float)un;
            up = float4((up.x * up.y + y) / (up.y + 1), up.y + 1, 0, 0);
            Summary[3] = up;
        }
        float4 ex = Summary[4];
        if (Summary[2].w <= 0 && (n >= kBins || (g3.x > 0 && n >= 8))) { Summary[2] = float4(avg, 1); ex.y = e; }
        // what the factor leaves (measured 2026-10-31: the check still moved 0.5x - 1.8x; the white level the buffers are
        // also divided by changes by area): the check smoothed over ~1 s, the fog scaled by it too -- it follows the
        // sky as drawn. No sky seen: as it was
        float rs = ex.w > 0 ? ex.w : 1;
        if (gm > 0.5 && g2.w > 0) rs = lerp(rs, clamp(r, 0.4, 2.5), 0.02);
        Summary[4] = float4(r, ex.y, e, rs);   // the check, the factor when the colour was set, the factor now, the residual
        // the plane below the horizon's empty pixels: this frame's estimate, smoothed (~0.3 s); none this frame: as it was
        uint hs = Acc[kWater0], hn = Acc[kWater0 + 1];
        Acc[kWater0] = 0; Acc[kWater0 + 1] = 0;
        float4 wp = Summary[5];
        if (hn >= 2)
        {
            float h = (float)hs / 16.0 / (float)hn;
            wp = float4(wp.y > 0 ? lerp(wp.x, h, 0.05) : h, min(wp.y + 1, 1e6), 0, 0);
            Summary[5] = wp;
        }
    }
}

)"
R"(// The screen as a grid of kGrid x kGrid cells, each vertex at the depth of the sphere of radius CheapFogStart round the
// eye in its direction. The scene's depth buffer is bound (read only) with a depth test: what is nearer than the grid
// is never shaded (early depth rejection) -- most of the screen, most of the time. Conservative: 1 / view depth is
// linear across the screen, and the sphere's (sqrt(1 + rx^2 + ry^2) / start) is convex, so between vertices the grid
// lies a little nearer than the sphere, never beyond it.
static const uint kGrid = 8;
struct VO { float4 pos : SV_Position; };
VO vs(uint id : SV_VertexID)
{
    VO o;
    uint cell = id / 6, k = id % 6;
    uint2 c = uint2(cell % kGrid, cell / kGrid) + uint2(k == 1 || k == 4 || k == 5 ? 1 : 0, k == 2 || k == 3 || k == 5 ? 1 : 0);
    float2 ndc = float2(c) / kGrid * 2 - 1;
    float4 R0 = P[0], R1 = P[1], R2 = P[2], R3 = P[3];
    bool colVec = abs(abs(R3.z) - 1) < 1e-3;
    float s = colVec ? R3.z : R2.w;
    float a = R2.z, b = colVec ? R2.w : R3.z;
    float offx = colVec ? R0.z : R2.x, offy = colVec ? R1.z : R2.y;
    float rx = (ndc.x * s - offx) / R0.x, ry = (ndc.y * s - offy) / R1.y;
    float z = g0.x > 0.5 ? saturate(a * s + b * sqrt(1 + rx * rx + ry * ry) / g0.x) : 0;
    o.pos = float4(ndc, z, 1);
    return o;
}

float4 ps(VO i) : SV_Target
{
    uint2 p = (uint2)i.pos.xy;
    float d = SceneDistance(i.pos.xy, p);
    float3 col = g1.rgb;
    if (g0.w < 0.5)
    {   // the map's one colour, once settled; until then the average of the sky's directions seen so far, from 4 of them
        // (2026-11-01, reported: the fog took a long while to show -- 43 s in one map -- waiting for the whole horizon)
        float4 c = SummaryIn[2];
        if (c.w <= 0) { float4 a = SummaryIn[0]; if (a.w < 4) discard; c.rgb = a.rgb; }
        col = c.rgb;
    }
    {   // never dark: real fog scatters the light (no black or dark grey fog)
        float y = dot(col, kLum);
        if (y < g4.x) col = y > 1e-4 ? col * (g4.x / y) : g4.x;
    }
    // into the scene's compressed buffer: divided by the frame's EngineLuminanceFactor (it then changes with the
    // exposure as everything else does); not following it: as the scene was when the colour was set
    float4 ex = SummaryIn[4];
    col /= g4.y > 0.5 ? Elf() : (ex.y > 0 ? ex.y : 1);
    if (g4.y > 0.5 && ex.w > 0) col *= ex.w;
    float f = 0;
    float3 wd = 0;
    if (d <= 0)
    {   // nothing there: below the horizon, the plane the ground around such pixels lies on (the sea beyond its floor,
        // when the water's own depth is not known), so the fog meets the ground's there without a line
        wd = WorldDir(i.pos.xy);
        float h = SummaryIn[5].x;
        if (wd.z < -0.001 && h > 0) d = h / -wd.z;
    }
    if (d > 0)
    {
        float t = saturate((d - g0.x) * g0.y);
        f = g0.z * t * t * (3 - 2 * t);
    }
    else if (g3.w > 0)
    {   // the sky: as thick as the far distance at the horizon (and below it; with `auto`, up to the top of the far
        // background seen in the map), thinning out to none CheapFogSkyHeight above that
        float base = g1.w > 0.5 ? max(SummaryIn[1].z, 0) : 0;
        float t = 1 - saturate((wd.z - base) / g3.w);
        f = g0.z * t * t * (3 - 2 * t);
    }
    f *= g3.y;
    if (f < 1.0 / 512) discard;     // no fog here: the colour target is not read or written
    return float4(col, f);          // blended: scene * (1 - f) + fog colour * f
}
)";

    std::atomic<bool> g_Want{ true }, g_GameFogOff{ false };
    std::atomic<float> g_Expo{ 0.f };           // the game's EngineLuminanceFactor as last read back (the log), 0 = not known
    std::mutex g_EsM; ID3D11ShaderResourceView* g_ExpoSrv = nullptr; UINT g_ExpoElems = 0;   // the game's exposure buffer, for the shaders
    unsigned g_Gen = 0, g_StageGen = 0;         // the colour's generation (a new map), and the one a pending read-back was copied in
    std::atomic<bool> g_Menu{ false };          // the main menu's map (its 3D scene): no fog there
    std::atomic<bool> g_ResetColour{ false };   // a new map: the fog's colour is learned again
    std::atomic<double> g_MapAt{ 0 };           // when the map began: the colour is learned from 5 s in (after its fade-in)
    constexpr float kLearnFrames = 120;         // eye frames each direction's colour is averaged over, then kept for the map
    std::mutex g_PM; Fog::Params g_P;

    std::mutex g_M;                              // everything below
    bool g_Failed = false, g_Ready = false;
    ID3D11Device* g_Dev = nullptr;
    ID3D11ComputeShader* g_CsSky = nullptr; ID3D11ComputeShader* g_CsFinal = nullptr;
    ID3D11VertexShader* g_Vs = nullptr; ID3D11PixelShader* g_Ps = nullptr;
    ID3D11Buffer* g_Cb = nullptr;
    ID3D11BlendState* g_Blend = nullptr; ID3D11DepthStencilState* g_NoDepth = nullptr; ID3D11RasterizerState* g_Rs = nullptr;
    // the depth test against the grid at the fog's start (see the vertex shader): standard depth (1 far) / reversed (0 far)
    ID3D11DepthStencilState* g_DepthLess = nullptr; ID3D11DepthStencilState* g_DepthGreater = nullptr;
    constexpr UINT kGridVerts = 8 * 8 * 6;
    // the horizon's colours (see the shaders): the frame's sums, the learned directions, the summary; a copy read back
    constexpr UINT kBins = 64, kFilled = 6, kCells = 512, kAcc = kBins * 4 + 4 + kCells * 2 + 2;
    ID3D11Buffer* g_Acc = nullptr; ID3D11UnorderedAccessView* g_AccUav = nullptr;
    ID3D11Buffer* g_Ref = nullptr; ID3D11UnorderedAccessView* g_RefUav = nullptr;
    float g_LoggedExpo = 1.f;                   // the exposure (relative to when the colour was set) as last logged
    ID3D11Buffer* g_Bins = nullptr; ID3D11UnorderedAccessView* g_BinsUav = nullptr;
    ID3D11Buffer* g_Summary = nullptr; ID3D11UnorderedAccessView* g_SummaryUav = nullptr; ID3D11ShaderResourceView* g_SummarySrv = nullptr;
    ID3D11Buffer* g_Stage = nullptr; bool g_StagePending = false; double g_StageAt = 0, g_LogAt = 0, g_LogAt2 = 0;
    constexpr UINT kSkyStep = 8;
    // the scene's colour / depth targets, with the mod's views of them
    struct Target
    {
        ID3D11Texture2D* color = nullptr; ID3D11Texture2D* depth = nullptr;
        ID3D11RenderTargetView* rtv = nullptr; ID3D11ShaderResourceView* colorSrv = nullptr; ID3D11ShaderResourceView* depthSrv = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;   // read only: the depth test that keeps the fog off what is nearer than its start
        UINT w = 0, h = 0, gx = 0, gy = 0; bool bad = false;
    };
    Target g_T[4];
    // RENDER_WATER's depth buffer (2026-11-01: the water is drawn with a copy of the scene's depth, not the depth the
    // fog was reading -- the fog measured to the sea floor, and found nothing where the floor ends: a line there)
    struct WaterT { ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; UINT w = 0, h = 0; bool bad = false; };
    WaterT g_W[4];
    struct WNote { unsigned frame = 0; int slot = -1; };
    WNote g_WNotes[8]; WNote g_WLast;
    std::atomic<unsigned long long> g_WaterUsed{ 0 }, g_EarlyZ{ 0 };
    float g_LoggedPlane = -1.f;
    // what the end of FORWARD_DYNAMIC left, per frame (the view's frame number), for that frame's ALPHA_LIT
    struct Note { unsigned frame = 0; int target = -1; float proj[16] = {}, view[16] = {}; bool valid = false; };
    Note g_Notes[8]; Note g_Last;
    std::atomic<unsigned long long> g_Drawn{ 0 }, g_Matched{ 0 }, g_Fallback{ 0 }, g_NoNote{ 0 }, g_Immediate{ 0 }, g_Faded{ 0 };
    std::string g_Why = "not seen yet";
    constexpr uint32_t kViewProj = 0x7390, kViewFrame = 0x77B4, kViewMatrix = 0x7450;   // the view object (StereoTaa: current projection, frame number)

    template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    bool ReadBytes(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool Init(ID3D11Device* dev)
    {
        if (g_Ready || g_Failed) return g_Ready;
        g_Failed = true;
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile) { g_Why = "d3dcompiler_47 not available"; LOGI("Fog: {}", g_Why); return false; }
        auto blob = [&](const char* entry, const char* target) -> ID3DBlob*
        {
            ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
            if (FAILED(compile(kHlsl, strlen(kHlsl), "fog", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err)))
            {
                LOGI("Fog: shader '{}' failed to compile: {}", entry, err ? (const char*)err->GetBufferPointer() : "?");
                if (err) err->Release(); return nullptr;
            }
            if (err) err->Release();
            return b;
        };
        ID3DBlob* bs = blob("sky", "cs_5_0"); ID3DBlob* bf = blob("skyfinal", "cs_5_0"); ID3DBlob* bv = blob("vs", "vs_5_0"); ID3DBlob* bp = blob("ps", "ps_5_0");
        bool ok = bs && bf && bv && bp;
        if (ok)
        {
            ok = SUCCEEDED(dev->CreateComputeShader(bs->GetBufferPointer(), bs->GetBufferSize(), nullptr, &g_CsSky))
              && SUCCEEDED(dev->CreateComputeShader(bf->GetBufferPointer(), bf->GetBufferSize(), nullptr, &g_CsFinal))
              && SUCCEEDED(dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &g_Vs))
              && SUCCEEDED(dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &g_Ps));
        }
        for (ID3DBlob* b : { bs, bf, bv, bp }) if (b) b->Release();
        if (!ok) { g_Why = "shader compile failed"; return false; }
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 192; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_Cb))) { g_Why = "constant buffer"; return false; }
        auto sbuf = [&](UINT n, UINT stride, bool srv, ID3D11Buffer** b, ID3D11UnorderedAccessView** u, ID3D11ShaderResourceView** v)
        {
            D3D11_BUFFER_DESC fd{}; fd.ByteWidth = n * stride; fd.Usage = D3D11_USAGE_DEFAULT;
            fd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (srv ? D3D11_BIND_SHADER_RESOURCE : 0);
            fd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; fd.StructureByteStride = stride;
            std::vector<uint8_t> zero(n * stride); D3D11_SUBRESOURCE_DATA sd{ zero.data(), 0, 0 };
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = n;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = DXGI_FORMAT_UNKNOWN; vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; vd.Buffer.NumElements = n;
            return SUCCEEDED(dev->CreateBuffer(&fd, &sd, b)) && SUCCEEDED(dev->CreateUnorderedAccessView(*b, &ud, u))
                && (!srv || SUCCEEDED(dev->CreateShaderResourceView(*b, &vd, v)));
        };
        if (!sbuf(kAcc, 4, false, &g_Acc, &g_AccUav, nullptr) || !sbuf(kBins, 16, false, &g_Bins, &g_BinsUav, nullptr)
            || !sbuf(kCells, 16, false, &g_Ref, &g_RefUav, nullptr)
            || !sbuf(kFilled, 16, true, &g_Summary, &g_SummaryUav, &g_SummarySrv)) { g_Why = "fog colour buffers"; return false; }
        D3D11_BUFFER_DESC sb{}; sb.ByteWidth = kFilled * 16; sb.Usage = D3D11_USAGE_STAGING; sb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&sb, nullptr, &g_Stage))) g_Stage = nullptr;   // only the log's readback
        D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].BlendEnable = TRUE;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = FALSE; ds.StencilEnable = FALSE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        if (FAILED(dev->CreateBlendState(&bl, &g_Blend)) || FAILED(dev->CreateDepthStencilState(&ds, &g_NoDepth)) || FAILED(dev->CreateRasterizerState(&rd, &g_Rs)))
        { g_Why = "pipeline states"; return false; }
        D3D11_DEPTH_STENCIL_DESC dt = ds; dt.DepthEnable = TRUE; dt.DepthFunc = D3D11_COMPARISON_LESS;
        if (FAILED(dev->CreateDepthStencilState(&dt, &g_DepthLess))) g_DepthLess = nullptr;
        dt.DepthFunc = D3D11_COMPARISON_GREATER;
        if (FAILED(dev->CreateDepthStencilState(&dt, &g_DepthGreater))) g_DepthGreater = nullptr;
        g_Dev = dev; g_Dev->AddRef();
        g_Failed = false; g_Ready = true;
        LOGI("Fog: shaders ready (sky colour, fog)");
        return true;
    }
    DXGI_FORMAT DepthDsvFormat(DXGI_FORMAT f, bool* stencil)
    {
        *stencil = f == DXGI_FORMAT_R24G8_TYPELESS || f == DXGI_FORMAT_R32G8X24_TYPELESS;
        switch (f)
        {
        case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_D32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_D16_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }
    DXGI_FORMAT DepthSrvFormat(DXGI_FORMAT f)
    {
        switch (f)
        {
        case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }
    //! The mod's views of a (colour, depth) pair, made once; -1 when they cannot be read / drawn into.
    int TargetOf(ID3D11Texture2D* color, ID3D11Texture2D* depth)
    {
        for (int i = 0; i < 4; i++) if (g_T[i].color == color && g_T[i].depth == depth) return g_T[i].bad ? -1 : i;
        int slot = -1;
        for (int i = 0; i < 4 && slot < 0; i++) if (!g_T[i].color) slot = i;
        if (slot < 0)
        {   // more than four pairs: start again
            for (Target& t : g_T) { Rel(t.dsv); Rel(t.depthSrv); Rel(t.colorSrv); Rel(t.rtv); Rel(t.depth); Rel(t.color); t = Target{}; }
            slot = 0;
        }
        Target& t = g_T[slot];
        t.color = color; color->AddRef(); t.depth = depth; depth->AddRef();
        D3D11_TEXTURE2D_DESC cd{}, dd{}; color->GetDesc(&cd); depth->GetDesc(&dd);
        const DXGI_FORMAT sf = DepthSrvFormat(dd.Format);
        auto fail = [&](const char* why)
        {
            LOGI("Fog: the scene's targets (colour {}x{} format {} bind 0x{:X}, depth {}x{} format {} bind 0x{:X}): {} -- no fog on them",
                 cd.Width, cd.Height, (int)cd.Format, cd.BindFlags, dd.Width, dd.Height, (int)dd.Format, dd.BindFlags, why);
            g_Why = why; t.bad = true; return -1;
        };
        if (cd.SampleDesc.Count > 1 || dd.SampleDesc.Count > 1) return fail("multisampled");
        if (dd.Width != cd.Width || dd.Height != cd.Height) return fail("sizes differ");
        if (!(dd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || sf == DXGI_FORMAT_UNKNOWN) return fail("the depth buffer cannot be read by a shader");
        if (!(cd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || !(cd.BindFlags & D3D11_BIND_RENDER_TARGET)) return fail("the colour target cannot be read / drawn into");
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = sf; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
        D3D11_SHADER_RESOURCE_VIEW_DESC cv{}; cv.Format = cd.Format; cv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; cv.Texture2D.MipLevels = 1;
        D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = cd.Format; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_Dev->CreateShaderResourceView(depth, &vd, &t.depthSrv))) return fail("depth view");
        if (FAILED(g_Dev->CreateShaderResourceView(color, &cv, &t.colorSrv)) || FAILED(g_Dev->CreateRenderTargetView(color, &rv, &t.rtv))) return fail("colour views");
        t.w = cd.Width; t.h = cd.Height;
        t.gx = (t.w + 16 * kSkyStep - 1) / (16 * kSkyStep); t.gy = (t.h + 16 * kSkyStep - 1) / (16 * kSkyStep);
        {   // the depth test (read only: it is read by the shader at the same time)
            bool st = false; const DXGI_FORMAT df = DepthDsvFormat(dd.Format, &st);
            D3D11_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = df; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dv.Flags = D3D11_DSV_READ_ONLY_DEPTH | (st ? D3D11_DSV_READ_ONLY_STENCIL : 0);
            if (!(dd.BindFlags & D3D11_BIND_DEPTH_STENCIL) || df == DXGI_FORMAT_UNKNOWN || !g_DepthLess || !g_DepthGreater
                || FAILED(g_Dev->CreateDepthStencilView(depth, &dv, &t.dsv)))
            { t.dsv = nullptr; LOGI("Fog: no read-only depth view of the scene's depth (format {}): every pixel is shaded", (int)dd.Format); }
        }
        LOGI("Fog: the scene's targets: colour {}x{} format {} and depth format {} (read as {}): fog drawn into them{}", t.w, t.h, (int)cd.Format, (int)dd.Format, (int)sf,
             t.dsv ? ", what is nearer than its start skipped by the depth test" : "");
        return slot;
    }
    //! The mod's view of RENDER_WATER's depth buffer, made once; -1 when it cannot be read
    int WaterOf(ID3D11Texture2D* tex)
    {
        for (int i = 0; i < 4; i++) if (g_W[i].tex == tex) return g_W[i].bad ? -1 : i;
        int slot = -1;
        for (int i = 0; i < 4 && slot < 0; i++) if (!g_W[i].tex) slot = i;
        if (slot < 0) { for (WaterT& w : g_W) { Rel(w.srv); Rel(w.tex); w = WaterT{}; } slot = 0; }
        WaterT& w = g_W[slot];
        w.tex = tex; tex->AddRef();
        D3D11_TEXTURE2D_DESC dd{}; tex->GetDesc(&dd);
        const DXGI_FORMAT sf = DepthSrvFormat(dd.Format);
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = sf; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
        if (dd.SampleDesc.Count > 1 || dd.ArraySize != 1 || !(dd.BindFlags & D3D11_BIND_SHADER_RESOURCE) || sf == DXGI_FORMAT_UNKNOWN
            || FAILED(g_Dev->CreateShaderResourceView(tex, &vd, &w.srv)))
        {
            LOGI("Fog: the water's depth buffer ({}x{} format {} bind 0x{:X}) cannot be read: the fog does not see the water's surface", dd.Width, dd.Height, (int)dd.Format, dd.BindFlags);
            w.srv = nullptr; w.bad = true; return -1;
        }
        w.w = dd.Width; w.h = dd.Height;
        return slot;
    }

    //! Every piece of the context's state the fog touches, as it was, put back exactly afterwards: whatever the game had
    //! bound there (and what its own state cache believes is bound) is what its ALPHA_LIT work then finds. (Putting back
    //! "a fresh context's defaults" instead was reported to break the transparent surfaces: 2026-10-31.)
    struct Saved
    {
        ID3D11ComputeShader* cs = nullptr; ID3D11Buffer* csCb = nullptr; ID3D11ShaderResourceView* csSrv[5] = {}; ID3D11UnorderedAccessView* csUav[4] = {};
        D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED; ID3D11InputLayout* il = nullptr;
        ID3D11VertexShader* vs = nullptr; ID3D11Buffer* vsCb = nullptr;
        ID3D11PixelShader* ps = nullptr; ID3D11Buffer* psCb = nullptr; ID3D11ShaderResourceView* psSrv[5] = {};
        ID3D11RasterizerState* rs = nullptr; D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {}; UINT nvp = 0;
        ID3D11BlendState* bs = nullptr; float bf[4] = {}; UINT mask = 0xFFFFFFFF;
        ID3D11DepthStencilState* dss = nullptr; UINT ref = 0;
        ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {}; ID3D11DepthStencilView* dsv = nullptr;
        void Save(ID3D11DeviceContext* c)
        {
            c->CSGetShader(&cs, nullptr, nullptr); c->CSGetConstantBuffers(0, 1, &csCb); c->CSGetShaderResources(0, 5, csSrv); c->CSGetUnorderedAccessViews(0, 4, csUav);
            c->IAGetPrimitiveTopology(&topo); c->IAGetInputLayout(&il);
            c->VSGetShader(&vs, nullptr, nullptr); c->VSGetConstantBuffers(0, 1, &vsCb);
            c->PSGetShader(&ps, nullptr, nullptr); c->PSGetConstantBuffers(0, 1, &psCb); c->PSGetShaderResources(0, 5, psSrv);
            c->RSGetState(&rs); nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetViewports(&nvp, vp);
            c->OMGetBlendState(&bs, bf, &mask); c->OMGetDepthStencilState(&dss, &ref);
            c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        }
        void Restore(ID3D11DeviceContext* c)
        {
            const UINT keep[4] = { (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1 };
            c->CSSetShader(cs, nullptr, 0); c->CSSetConstantBuffers(0, 1, &csCb); c->CSSetUnorderedAccessViews(0, 4, csUav, keep); c->CSSetShaderResources(0, 5, csSrv);
            c->IASetPrimitiveTopology(topo); c->IASetInputLayout(il);
            c->VSSetShader(vs, nullptr, 0); c->VSSetConstantBuffers(0, 1, &vsCb);
            c->PSSetShader(ps, nullptr, 0); c->PSSetConstantBuffers(0, 1, &psCb); c->PSSetShaderResources(0, 5, psSrv);
            c->RSSetState(rs); c->RSSetViewports(nvp, nvp ? vp : nullptr);
            c->OMSetBlendState(bs, bf, mask); c->OMSetDepthStencilState(dss, ref);
            c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
            Rel(cs); Rel(csCb); for (auto*& p : csSrv) Rel(p); for (auto*& p : csUav) Rel(p); Rel(il); Rel(vs); Rel(vsCb); Rel(ps); Rel(psCb);
            for (auto*& p : psSrv) Rel(p); Rel(rs); Rel(bs); Rel(dss); for (auto*& p : rtv) Rel(p); Rel(dsv);
        }
    };
    double g_SettledAt = 0;
    double g_ProvAt = 0;                        // when a first colour (4 directions of the sky seen) was read back: the fade-in
    float g_LoggedTop = -9.f;                   // the far background's top as last logged
    // CheapFogColor = auto: the category chosen for this map (-1 = not yet) and its colour (linear, at its brightness)
    int g_Cat = -1; float g_CatRgb[3] = {}; double g_CatAt = 0;
    const char* kCatName[3] = { "NIGHT", "DUSK", "DAY" };                     // when the map's colour was seen settled (render thread; 0 = not yet): the fade-in
    double Now() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart / (double)f.QuadPart; }
}

void Fog::SetParams(const Params& p)
{
    { std::lock_guard lk(g_PM); g_P = p; }
    g_Want = p.on;
    if (p.on) LOGI("Fog: the mod's own fog when the game's is off (CheapFog): from {} m, full at {} m, opacity {:.2f}, colour {}; the sky {}",
                   p.start, p.end, p.opacity,
                   p.category ? fmt::format("by the map's sky: night (upper sky under {:.2f}), dusk, or day (from {:.2f}), one per map", p.nightBelow, p.dayAbove)
                   : p.sky ? std::string("the average of the sky round the horizon (one per map, settled as one looks round, then kept)")
                         : fmt::format("fixed: {:.3f},{:.3f},{:.3f} (linear, x{:.2f})", p.rgb[0], p.rgb[1], p.rgb[2], p.brightness),
                   p.skyHeight <= 0 ? std::string("not fogged") : p.skyAuto ? fmt::format("fogged up to the top of the far background, thinning out {:.0f} deg above it", p.skyHeight)
                                    : fmt::format("fogged up to {:.0f} deg above the horizon", p.skyHeight));
    if (p.on) LOGI("Fog: never darker than {:.2f} x EngineLuminanceFactor (CheapFogMinBrightness){}", p.minBrightness,
                   p.followExposure ? "; it follows the game's exposure and the sky's drawn brightness (CheapFogFollowExposure)" : "");
}
bool Fog::Wanted() { return g_Want.load(); }

void Fog::SetGameFogOff(bool off)
{
    if (g_GameFogOff.exchange(off) != off && g_Want.load())
        LOGI("Fog: the mod's fog {}", off ? "ON (the game's fog is off)" : "off (the game's own fog is on)");
}

void Fog::AfterOpaque(ID3D11DeviceContext* c, const void* view)
{
    if (!g_Want.load(std::memory_order_relaxed) || !g_GameFogOff.load(std::memory_order_relaxed) || !c || !view) return;
    ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11Resource* cr = nullptr; ID3D11Resource* dr = nullptr;
    if (rtv) rtv->GetResource(&cr);
    if (dsv) dsv->GetResource(&dr);
    D3D11_RESOURCE_DIMENSION a{}, b{};
    if (cr) cr->GetType(&a);
    if (dr) dr->GetType(&b);
    if (a == D3D11_RESOURCE_DIMENSION_TEXTURE2D && b == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
    {
        Note n; n.valid = ReadBytes(&n.frame, (const uint8_t*)view + kViewFrame, 4) && ReadBytes(n.proj, (const uint8_t*)view + kViewProj, 64)
                      && ReadBytes(n.view, (const uint8_t*)view + kViewMatrix, 64);
        std::lock_guard lk(g_M);
        if (!g_Ready) { ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (dev) { Init(dev); dev->Release(); } }
        if (g_Ready && n.valid)
        {
            n.target = TargetOf(static_cast<ID3D11Texture2D*>(cr), static_cast<ID3D11Texture2D*>(dr));
            if (n.target >= 0) { g_Notes[n.frame % 8] = n; g_Last = n; }
        }
    }
    if (cr) cr->Release();
    if (dr) dr->Release();
    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
}

void Fog::AfterWater(ID3D11DeviceContext* c, const void* view)
{
    if (!g_Want.load(std::memory_order_relaxed) || !g_GameFogOff.load(std::memory_order_relaxed) || !c || !view) return;
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(0, nullptr, &dsv);
    if (!dsv) return;
    ID3D11Resource* dr = nullptr; dsv->GetResource(&dr);
    D3D11_RESOURCE_DIMENSION b{};
    if (dr) dr->GetType(&b);
    unsigned frame = 0;
    if (b == D3D11_RESOURCE_DIMENSION_TEXTURE2D && ReadBytes(&frame, (const uint8_t*)view + kViewFrame, 4))
    {
        // whether the water's last draw wrote depth (for the log: the depth is the water's surface only if it does)
        ID3D11DepthStencilState* st = nullptr; UINT ref = 0; c->OMGetDepthStencilState(&st, &ref);
        D3D11_DEPTH_STENCIL_DESC sd{}; if (st) { st->GetDesc(&sd); st->Release(); }
        std::lock_guard lk(g_M);
        if (g_Ready)
        {
            const int slot = WaterOf(static_cast<ID3D11Texture2D*>(dr));
            if (slot >= 0)
            {
                static ID3D11Texture2D* told = nullptr;
                if (told != g_W[slot].tex)
                {
                    told = g_W[slot].tex;
                    LOGI("Fog: RENDER_WATER's depth buffer ({}x{}; its last draw: depth test {}, depth write {}): the fog measures to the nearer of it and the scene's depth (the water's surface)",
                         g_W[slot].w, g_W[slot].h, sd.DepthEnable ? "on" : "off", sd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL ? "on" : "off");
                }
                g_WNotes[frame % 8] = WNote{ frame, slot }; g_WLast = g_WNotes[frame % 8];
            }
        }
    }
    if (dr) dr->Release();
    dsv->Release();
}

bool Fog::BeforeTransparent(ID3D11DeviceContext* c, const void* view)
{
    if (!g_Want.load(std::memory_order_relaxed) || !g_GameFogOff.load(std::memory_order_relaxed) || g_Menu.load(std::memory_order_relaxed) || !c || !view) return false;
    if (c->GetType() != D3D11_DEVICE_CONTEXT_DEFERRED) { g_Immediate++; return false; }   // only on a task's own context
    unsigned frame = 0;
    if (!ReadBytes(&frame, (const uint8_t*)view + kViewFrame, 4)) return false;
    Params P; { std::lock_guard lk(g_PM); P = g_P; }
    std::lock_guard lk(g_M);
    if (!g_Ready) return false;
    Note n = g_Notes[frame % 8];
    if (n.valid && n.frame == frame) g_Matched++;
    else if (g_Last.valid) { n = g_Last; g_Fallback++; }   // recorded before this frame's opaque pass was: the latest targets
    else { g_NoNote++; return false; }
    if (n.target < 0) return false;
    Target& t = g_T[n.target];
    if (!t.rtv) return false;
    // the water's depth this frame (or within the last few: the same buffer, rewritten every frame), if it is not the
    // scene's own and has its size
    ID3D11ShaderResourceView* water = nullptr;
    {
        WNote w = g_WNotes[frame % 8];
        if (!(w.slot >= 0 && w.frame == frame) && g_WLast.slot >= 0 && frame - g_WLast.frame < 4) w = g_WLast;
        if (w.slot >= 0 && (w.frame == frame || frame - w.frame < 4))
        {
            const WaterT& wt = g_W[w.slot];
            if (wt.srv && wt.tex != t.depth && wt.w == t.w && wt.h == t.h) water = wt.srv;
        }
    }

    struct { float P[16]; float V[12]; float g0[4], g1[4], g2[4], g3[4], g4[4]; } k{};
    memcpy(k.P, n.proj, 64);
    for (int r = 0; r < 3; r++) for (int col = 0; col < 3; col++) k.V[r * 4 + col] = n.view[r * 4 + col];
    const float range = std::max(1.f, P.end - P.start);
    k.g0[0] = P.start; k.g0[1] = 1.f / range; k.g0[2] = std::min(1.f, std::max(0.f, P.opacity)); k.g0[3] = P.sky ? 0.f : 1.f;
    for (int i = 0; i < 3; i++) k.g1[i] = P.category ? g_CatRgb[i] : P.rgb[i] * P.brightness;
    k.g1[3] = P.skyAuto ? 1.f : 0.f;
    k.g2[0] = (float)t.w; k.g2[1] = (float)t.h; k.g2[2] = (float)kSkyStep; k.g2[3] = Now() - g_MapAt.load() > 5.0 ? 1.f : 0.f;
    k.g3[0] = Now() - g_MapAt.load() > 20.0 ? 1.f : 0.f;
    // the fade-in from the map's first colour (sky: 4 directions seen, refined until it settles; auto: the category, picked
    // by the upper sky); a fixed colour: at once
    {
        const double from = P.category ? (g_Cat >= 0 ? g_CatAt : 0) : (g_ProvAt > 0 ? g_ProvAt : g_SettledAt);
        k.g3[1] = P.sky || P.category ? (from > 0 ? (float)std::min(1.0, (Now() - from) / 2.0) : 0.f) : 1.f;
    }
    k.g3[2] = kLearnFrames;
    k.g3[3] = P.skyHeight > 0.f ? sinf(std::min(90.f, P.skyHeight) * 0.0174533f) : 0.f;
    k.g4[0] = std::max(0.f, P.minBrightness); k.g4[1] = P.followExposure ? 1.f : 0.f; k.g4[2] = water ? 1.f : 0.f;
    ID3D11ShaderResourceView* expo = nullptr;   // the game's exposure buffer (its EngineLuminanceFactor), when found
    { std::lock_guard ek(g_EsM); expo = g_ExpoSrv; if (expo) { expo->AddRef(); k.g4[3] = (float)g_ExpoElems; } }
    Saved saved; saved.Save(c);
    c->UpdateSubresource(g_Cb, 0, nullptr, &k, 0, 0);

    ID3D11ShaderResourceView* nullSrv[5] = {}; ID3D11UnorderedAccessView* nullUav[4] = {};
    // nothing bound for output while the scene's targets are read (a resource bound both ways is read as nothing)
    c->OMSetRenderTargets(0, nullptr, nullptr);
    if (g_ResetColour.exchange(false))
    {   // a new map: its horizon is learned anew (and its colour settled anew)
        const float z[4] = {}; const UINT zu[4] = {};
        c->ClearUnorderedAccessViewFloat(g_BinsUav, z); c->ClearUnorderedAccessViewFloat(g_SummaryUav, z); c->ClearUnorderedAccessViewUint(g_AccUav, zu);
        c->ClearUnorderedAccessViewFloat(g_RefUav, z);
        g_SettledAt = 0; g_ProvAt = 0; g_LoggedTop = -9.f; g_Cat = -1; g_LoggedExpo = 1.f; g_LoggedPlane = -1.f; g_Gen++;   // a read-back copied before this is stale
    }
    {   // 1) the horizon's colours, round it (only what this eye sees of the sky adds to them; with a fixed colour, for the log)
        c->CSSetConstantBuffers(0, 1, &g_Cb);
        { ID3D11ShaderResourceView* srv[5] = { t.colorSrv, t.depthSrv, nullptr, expo, water }; c->CSSetShaderResources(0, 5, srv); }
        { ID3D11UnorderedAccessView* uav[4] = { g_AccUav, g_BinsUav, g_SummaryUav, g_RefUav }; c->CSSetUnorderedAccessViews(0, 4, uav, nullptr); }
        c->CSSetShader(g_CsSky, nullptr, 0);
        c->Dispatch(t.gx, t.gy, 1);
        c->CSSetShader(g_CsFinal, nullptr, 0);
        c->Dispatch(1, 1, 1);
        c->CSSetUnorderedAccessViews(0, 4, nullUav, nullptr);
        c->CSSetShaderResources(0, 5, nullSrv);
    }
    // 2) the fog, blended over the scene: a grid at its start, depth tested against the scene (only what lies beyond it
    // is shaded); not drawn at all before it has faded in
    if (k.g3[1] > 0.f)
    {
        // which way the depth runs (as the shaders tell it: the depth at infinity is 1 or 0)
        const float* R = n.proj;
        const bool colVec = fabsf(fabsf(R[14]) - 1.f) < 1e-3f;
        const float sgn = colVec ? R[14] : R[11];
        const bool reversed = std::clamp(R[10] * sgn, 0.f, 1.f) <= 0.5f;
        ID3D11DepthStencilView* dsv = P.start >= 1.f ? t.dsv : nullptr;
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->IASetInputLayout(nullptr);
        c->VSSetShader(g_Vs, nullptr, 0);
        c->VSSetConstantBuffers(0, 1, &g_Cb);
        c->PSSetShader(g_Ps, nullptr, 0);
        c->PSSetConstantBuffers(0, 1, &g_Cb);
        { ID3D11ShaderResourceView* srv[5] = { nullptr, t.depthSrv, g_SummarySrv, expo, water }; c->PSSetShaderResources(0, 5, srv); }
        c->RSSetState(g_Rs);
        D3D11_VIEWPORT vp{ 0.f, 0.f, (float)t.w, (float)t.h, 0.f, 1.f };
        c->RSSetViewports(1, &vp);
        c->OMSetBlendState(g_Blend, nullptr, 0xFFFFFFFF);
        c->OMSetDepthStencilState(dsv ? (reversed ? g_DepthGreater : g_DepthLess) : g_NoDepth, 0);
        c->OMSetRenderTargets(1, &t.rtv, dsv);
        c->Draw(kGridVerts, 0);
        if (dsv) g_EarlyZ++;
        if (water) g_WaterUsed++;
    }
    else g_Faded++;
    // the game's state as it was (the PS's inputs first: the colour target goes back to being an output)
    c->PSSetShaderResources(0, 5, nullSrv);
    if (expo) expo->Release();
    c->OMSetRenderTargets(0, nullptr, nullptr);
    saved.Restore(c);
    g_Drawn++;
    if (g_Drawn.load() == 1) LOGI("Fog: first fog drawn (frame {}, {}x{})", frame, t.w, t.h);
    g_Why = "running";
    return true;
}


// ---- the game's auto-exposure (2026-10-31) ----
// Reported: the fog was too dark to see, except with a hand over the head -- the game's eye adaptation brightening the
// whole image then. The scene the fog is drawn into is HDR BEFORE the exposure; the tone mapping applies it later. So
// what the fog looks like depends on the exposure, and a floor on its brightness only means something after it.
// The exe names the exposure "PostFX/Luminance/buffAutoExposure" (an arkStructuredBuffer<autoExposure_t>). Every pixel
// / compute shader the game creates whose bytecode names a resource "...exposure..." is reflected: the binding slot
// and the struct's members. At the first draw / dispatch with such a shader bound, the buffer bound there is kept;
// it is read back every few frames at Present, and its "exposure" member is the multiplier the fog's floor is
// divided by (CheapFogMinBrightness is then the brightness as you see it). The members and their values go to the log.
// (The shaders' disassembly, written to DVR_exposure_shader*.txt until 2026-11-03, confirmed it: the game divides by
// EngineLuminanceFactor where it writes compressed HDR buffers.)
namespace
{
    struct ExpoMember { std::string name; UINT off = 0; UINT n = 1; bool isFloat = false; };
    struct ExpoUse { int slot = -1; bool uav = false; std::string res; UINT stride = 0; std::vector<ExpoMember> members; uint64_t hash = 0; bool cs = false; };
    std::mutex g_EM;                                       // everything below
    std::unordered_map<void*, ExpoUse> g_ExpoShaders;     // a pixel / compute shader -> where it binds the exposure
    std::atomic<bool> g_ExpoSearch{ false };              // a shader is known and no buffer yet: look at draws / dispatches
    std::atomic<unsigned long long> g_ExpoLooks{ 0 };     // draws / dispatches looked at in this search (it gives up after 20 s)
    std::atomic<double> g_ExpoSearchAt{ 0 };
    void StartExpoSearch() { g_ExpoLooks = 0; g_ExpoSearchAt = Now(); g_ExpoSearch = true; }
    ID3D11Buffer* g_ExpoBuf = nullptr; ExpoUse g_ExpoUse; int g_ExpoMember = -1; UINT g_ExpoBytes = 0;
    ID3D11Buffer* g_ExpoStage = nullptr; bool g_ExpoPending = false; unsigned g_ExpoTick = 0;
    int g_ExpoNamed = 0, g_ExpoDumpLogs = 0; float g_ExpoLogged = 0.f; double g_ExpoLogAt = 0;
    std::string Lower(std::string s) { for (char& ch : s) ch = (char)tolower((unsigned char)ch); return s; }
    bool HasExposureName(const void* code, size_t len)
    {
        static const char k[] = "xposure";
        const char* p = (const char*)code; const char* e = p + len - (sizeof k - 1);
        for (; p <= e; p++) if ((p[0] == 'x' || p[0] == 'X') && !memcmp(p + 1, k + 1, sizeof k - 2)) return true;
        return false;
    }
    //! the member that is the exposure multiplier: named "exposure", else holding it without ev / log / target...
    int PickMember(const ExpoUse& u)
    {
        for (size_t i = 0; i < u.members.size(); i++) if (u.members[i].isFloat && Lower(u.members[i].name) == "engineluminancefactor") return (int)i;
        for (size_t i = 0; i < u.members.size(); i++) if (u.members[i].isFloat && Lower(u.members[i].name) == "exposure") return (int)i;
        for (size_t i = 0; i < u.members.size(); i++)
        {
            const std::string n = Lower(u.members[i].name);
            if (!u.members[i].isFloat || n.find("exposure") == std::string::npos) continue;
            bool bad = false;
            for (const char* w : { "ev", "log", "prev", "target", "min", "max", "bias", "index", "speed", "inv" }) bad |= n.find(w) != std::string::npos;
            if (!bad) return (int)i;
        }
        if (u.members.size() == 1 && u.members[0].isFloat) return 0;
        return -1;
    }
}

void Fog::OnShader(void* sh, const void* code, size_t len, bool cs)
{
    if (!sh) return;
    {   // an address reused by another shader
        std::lock_guard lk(g_EM);
        if (!g_ExpoShaders.empty()) g_ExpoShaders.erase(sh);
    }
    if (!code || len < 32 || !HasExposureName(code, len)) return;
    using Reflect_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    static Reflect_t reflect = nullptr; static bool tried = false;
    if (!tried) { tried = true; if (HMODULE d = LoadLibraryA("d3dcompiler_47.dll")) reflect = (Reflect_t)GetProcAddress(d, "D3DReflect"); }
    ID3D11ShaderReflection* r = nullptr;
    if (!reflect || FAILED(reflect(code, len, __uuidof(ID3D11ShaderReflection), (void**)&r)) || !r) return;
    D3D11_SHADER_DESC sd{};
    ExpoUse u; bool found = false;
    if (SUCCEEDED(r->GetDesc(&sd)))
        for (UINT i = 0; i < sd.BoundResources && !found; i++)
        {
            D3D11_SHADER_INPUT_BIND_DESC b{};
            if (FAILED(r->GetResourceBindingDesc(i, &b)) || !b.Name || Lower(b.Name).find("exposure") == std::string::npos) continue;
            const bool structured = b.Type == D3D_SIT_STRUCTURED || b.Type == D3D_SIT_UAV_RWSTRUCTURED || b.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER;
            const bool srv = b.Type == D3D_SIT_STRUCTURED || b.Type == D3D_SIT_TEXTURE || b.Type == D3D_SIT_TBUFFER || b.Type == D3D_SIT_BYTEADDRESS;
            const bool uav = b.Type == D3D_SIT_UAV_RWSTRUCTURED || b.Type == D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER || b.Type == D3D_SIT_UAV_RWTYPED || b.Type == D3D_SIT_UAV_RWBYTEADDRESS;
            if (!srv && !uav) continue;
            if (uav && !cs) continue;          // a pixel shader's UAVs are bound with the targets: not looked at
            u.slot = (int)b.BindPoint; u.uav = uav; u.res = b.Name; u.cs = cs; u.hash = GpuTrace::ShaderHashOf(sh);
            if (structured)
                if (ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByName(b.Name))
                {
                    D3D11_SHADER_BUFFER_DESC bd{};
                    if (SUCCEEDED(cb->GetDesc(&bd))) u.stride = bd.Size;
                    if (ID3D11ShaderReflectionVariable* v = cb->GetVariableByIndex(0))
                        if (ID3D11ShaderReflectionType* t = v->GetType())
                        {
                            D3D11_SHADER_TYPE_DESC td{};
                            if (SUCCEEDED(t->GetDesc(&td)))
                            {
                                if (td.Members == 0) u.members.push_back({ "(element)", 0, std::max(1u, td.Columns), td.Type == D3D_SVT_FLOAT });
                                for (UINT m = 0; m < td.Members; m++)
                                {
                                    ID3D11ShaderReflectionType* mt = t->GetMemberTypeByIndex(m);
                                    D3D11_SHADER_TYPE_DESC md{};
                                    if (!mt || FAILED(mt->GetDesc(&md))) continue;
                                    const char* mn = t->GetMemberTypeName(m);
                                    u.members.push_back({ mn ? mn : "?", md.Offset, std::max(1u, md.Columns) * std::max(1u, md.Rows), md.Type == D3D_SVT_FLOAT });
                                }
                            }
                        }
                }
            found = true;
        }
    r->Release();
    if (!found) return;
    std::lock_guard lk(g_EM);
    if (g_ExpoNamed < 12)
    {
        g_ExpoNamed++;
        std::string m;
        for (const ExpoMember& e : u.members) m += fmt::format(" {}@{}{}", e.name, e.off, e.isFloat ? (e.n > 1 ? fmt::format(":float{}", e.n) : std::string(":float")) : std::string(""));
        LOGI("Fog: {} shader {:016x} {} the game's exposure '{}' ({}{}, {} bytes):{}", cs ? "compute" : "pixel", u.hash, u.uav ? "writes" : "reads",
             u.res, u.uav ? "u" : "t", u.slot, u.stride, m.empty() ? std::string(" (not a structured buffer)") : m);
        // the disassembly files are a development aid: written only while the render passes are profiled
    }
    g_ExpoShaders[sh] = u;
    if (!g_ExpoBuf && !g_ExpoSearch.load()) StartExpoSearch();
}

bool Fog::ExposureSearching() { return g_ExpoSearch.load(std::memory_order_relaxed); }

void Fog::AtWork(ID3D11DeviceContext* c, bool dispatch)
{
    if (!c) return;
    if ((g_ExpoLooks.fetch_add(1, std::memory_order_relaxed) & 4095) == 4095 && Now() - g_ExpoSearchAt.load() > 20.0)
    {   // not bound by anything drawn for 20 s: given up until the next map (the look costs every draw a lock)
        if (g_ExpoSearch.exchange(false)) LOGI("Fog: the game's exposure buffer was not seen bound in 20 s ({} draws / dispatches): not used", g_ExpoLooks.load());
        return;
    }
    void* sh = nullptr;
    if (dispatch) { ID3D11ComputeShader* s = nullptr; c->CSGetShader(&s, nullptr, nullptr); sh = s; if (s) s->Release(); }
    else { ID3D11PixelShader* s = nullptr; c->PSGetShader(&s, nullptr, nullptr); sh = s; if (s) s->Release(); }
    if (!sh) return;
    ExpoUse u;
    {
        std::lock_guard lk(g_EM);
        auto it = g_ExpoShaders.find(sh);
        if (it == g_ExpoShaders.end() || it->second.cs != dispatch) return;
        u = it->second;
    }
    ID3D11Resource* res = nullptr;
    if (u.uav) { ID3D11UnorderedAccessView* v = nullptr; c->CSGetUnorderedAccessViews(u.slot, 1, &v); if (v) { v->GetResource(&res); v->Release(); } }
    else
    {
        ID3D11ShaderResourceView* v = nullptr;
        if (dispatch) c->CSGetShaderResources(u.slot, 1, &v); else c->PSGetShaderResources(u.slot, 1, &v);
        if (v) { v->GetResource(&res); v->Release(); }
    }
    if (!res) return;
    D3D11_RESOURCE_DIMENSION d{}; res->GetType(&d);
    std::lock_guard lk(g_EM);
    if (g_ExpoBuf || d != D3D11_RESOURCE_DIMENSION_BUFFER)
    {
        static bool told = false;
        if (!g_ExpoBuf && !told) { told = true; LOGI("Fog: the game's exposure '{}' is not a buffer (dimension {}): not used", u.res, (int)d); g_ExpoSearch = false; }
        res->Release(); return;
    }
    g_ExpoBuf = static_cast<ID3D11Buffer*>(res);      // the reference GetResource took is kept
    D3D11_BUFFER_DESC bd{}; g_ExpoBuf->GetDesc(&bd);
    g_ExpoBytes = std::min<UINT>(bd.ByteWidth, 4096);
    if (!u.stride) u.stride = bd.StructureByteStride;
    g_ExpoUse = u; g_ExpoMember = PickMember(u);
    {   // a view of it for the fog's shaders (the factor at offset 0 of each 32-byte element)
        ID3D11Device* dev = nullptr; g_ExpoBuf->GetDevice(&dev);
        ID3D11ShaderResourceView* v = nullptr;
        const UINT stride = bd.StructureByteStride ? bd.StructureByteStride : u.stride;
        if (dev && (bd.BindFlags & D3D11_BIND_SHADER_RESOURCE) && (bd.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) && stride == 32)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = DXGI_FORMAT_UNKNOWN; vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            vd.Buffer.FirstElement = 0; vd.Buffer.NumElements = std::min(2u, bd.ByteWidth / 32);
            if (FAILED(dev->CreateShaderResourceView(g_ExpoBuf, &vd, &v))) v = nullptr;
        }
        if (dev) dev->Release();
        std::lock_guard ek(g_EsM);
        if (g_ExpoSrv) g_ExpoSrv->Release();
        g_ExpoSrv = v; g_ExpoElems = v ? std::min(2u, bd.ByteWidth / 32) : 0;
        if (!v) LOGI("Fog: no view of the game's exposure buffer could be made (bind 0x{:X}, misc 0x{:X}, stride {}): the fog does not follow the exposure", bd.BindFlags, bd.MiscFlags, stride);
    }
    g_ExpoSearch = false; g_ExpoDumpLogs = 0; g_ExpoLogged = 0.f;
    if (g_ExpoStage) { g_ExpoStage->Release(); g_ExpoStage = nullptr; } g_ExpoPending = false;
    LOGI("Fog: the game's exposure buffer found ({} bytes, {} per element), bound to {} shader {:016x} as '{}': {}", bd.ByteWidth, u.stride,
         dispatch ? "compute" : "pixel", u.hash, u.res,
         g_ExpoMember >= 0 ? fmt::format("its '{}' is what the scene is drawn divided by: the fog follows it", u.members[g_ExpoMember].name)
                           : std::string("no member named EngineLuminanceFactor: the values are logged"));
}

namespace
{
    //! Present: the exposure buffer read back every 4th Present (a copy, mapped when ready)
    void ReadExposure(ID3D11Device* dev, ID3D11DeviceContext* ic, double now)
    {
        std::lock_guard lk(g_EM);
        if (!g_ExpoBuf || ++g_ExpoTick % 4) return;
        if (!g_ExpoStage)
        {
            D3D11_BUFFER_DESC bd{}; g_ExpoBuf->GetDesc(&bd);
            bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_ExpoStage))) { LOGI("Fog: no staging copy of the exposure buffer: not used"); g_ExpoBuf->Release(); g_ExpoBuf = nullptr; return; }
        }
        if (!g_ExpoPending) { ic->CopyResource(g_ExpoStage, g_ExpoBuf); g_ExpoPending = true; return; }
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ic->Map(g_ExpoStage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
        g_ExpoPending = false;
        if (FAILED(hr)) return;
        std::vector<uint8_t> v(g_ExpoBytes); memcpy(v.data(), m.pData, v.size()); ic->Unmap(g_ExpoStage, 0);
        const ExpoUse& u = g_ExpoUse;
        const UINT stride = u.stride ? u.stride : (UINT)v.size();
        const UINT count = std::min<UINT>(4, (UINT)v.size() / std::max(1u, stride));
        auto f = [&](UINT el, UINT off) { float x = 0; if ((size_t)el * stride + off + 4 <= v.size()) memcpy(&x, v.data() + (size_t)el * stride + off, 4); return x; };
        float e = 0.f;
        if (g_ExpoMember >= 0) e = f(0, u.members[g_ExpoMember].off);
        const bool ok = std::isfinite(e) && e > 1e-6f && e < 1e6f;
        g_Expo = ok ? e : 0.f;
        const bool changed = ok && (g_ExpoLogged <= 0.f || fabsf(logf(e / g_ExpoLogged)) > 0.15f);
        if (g_ExpoDumpLogs < 3 || (changed && now - g_ExpoLogAt >= 30.0) || (g_ExpoMember < 0 && now - g_ExpoLogAt >= 10.0 && g_ExpoDumpLogs < 30))
        {
            g_ExpoDumpLogs++; g_ExpoLogAt = now; if (ok) g_ExpoLogged = e;
            std::string s;
            for (UINT el = 0; el < count; el++)
            {
                s += fmt::format(" [{}]", el);
                if (u.members.empty()) for (UINT o = 0; o + 4 <= std::min(stride, 32u); o += 4) s += fmt::format(" {:.4g}", f(el, o));
                for (const ExpoMember& mm : u.members)
                {
                    if (!mm.isFloat) { s += fmt::format(" {}=?", mm.name); continue; }
                    s += fmt::format(" {}=", mm.name);
                    for (UINT k = 0; k < std::min(mm.n, 4u); k++) s += fmt::format("{}{:.4g}", k ? "," : "", f(el, mm.off + 4 * k));
                }
            }
            LOGI("Fog: the game's EngineLuminanceFactor {}; the buffer:{}", ok ? fmt::format("{:.4f}", e) : std::string("(not read)"), s);
        }
    }
}

void Fog::SetMap(const std::string& name)
{
    const bool menu = name.rfind("campaign/menu", 0) == 0;
    g_ResetColour = true; g_MapAt = Now();
    {   // the exposure buffer looked for again (the renderer may have made a new one)
        std::lock_guard lk(g_EM);
        if (g_ExpoBuf) { g_ExpoBuf->Release(); g_ExpoBuf = nullptr; }
        if (g_ExpoStage) { g_ExpoStage->Release(); g_ExpoStage = nullptr; }
        g_ExpoPending = false; g_Expo = 0.f;
        { std::lock_guard ek(g_EsM); if (g_ExpoSrv) { g_ExpoSrv->Release(); g_ExpoSrv = nullptr; } g_ExpoElems = 0; }
        if (!g_ExpoShaders.empty()) StartExpoSearch();
    }
    if (g_Menu.exchange(menu) != menu && g_Want.load()) LOGI("Fog: {}", menu ? "the main menu's scene: no fog of the mod's there" : "a map to play: the mod's fog may draw again");
}

void Fog::OnPresent(IDXGISwapChain* sc)
{
    // the summary read back: every second until this map's colour has settled (for its fade-in), then every 10 s
    if (!g_Want.load(std::memory_order_relaxed) || !sc) return;
    std::lock_guard lk(g_M);
    if (!g_Ready || !g_Stage || !g_Drawn.load()) return;
    const double now = Now();
    { ID3D11DeviceContext* ec = nullptr; g_Dev->GetImmediateContext(&ec); if (ec) { ReadExposure(g_Dev, ec, now); ec->Release(); } }
    Params P; { std::lock_guard pk(g_PM); P = g_P; }
    if (!g_StagePending && now - g_LogAt < (g_SettledAt > 0 || !P.sky ? 10.0 : 1.0)) return;   // every second until the colour is settled
    ID3D11DeviceContext* ic = nullptr; g_Dev->GetImmediateContext(&ic);
    if (!ic) return;
    if (!g_StagePending) { ic->CopyResource(g_Stage, g_Summary); g_StagePending = true; g_StageAt = now; g_StageGen = g_Gen; }
    else
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ic->Map(g_Stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (SUCCEEDED(hr))
        {
            float v[kFilled * 4]; memcpy(v, m.pData, sizeof v); ic->Unmap(g_Stage, 0);
            g_StagePending = false; g_LogAt = now;
            // copied before a new map's reset (the immediate context's copy can precede the deferred task's clear): the
            // last map's colour. That one set this map's category once (reported: the second map's fog was the first's)
            if (g_StageGen != g_Gen) { ic->Release(); return; }
            const float* a = v; const float* r = v + 4; const float* fc = v + 8;
            // the colour as the ini's fixed one would give it: R,G,B (0-255, sRGB) x CheapFogBrightness
            auto asIni = [](const float* c)
            {
                const float b = std::max({ c[0], c[1], c[2], 1e-4f });
                int q[3];
                for (int i = 0; i < 3; i++) { const float x = c[i] / b; q[i] = (int)std::lround(255.f * (x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1 / 2.4f) - 0.055f)); }
                return fmt::format("CheapFogColor = {},{},{} with CheapFogBrightness = {:.2f}", q[0], q[1], q[2], b);
            };
            const bool settled = fc[3] > 0;
            if (!settled) g_SettledAt = 0;   // (a read-back from before a new map's reset said otherwise)
            if (settled && g_SettledAt <= 0)
            {
                g_SettledAt = now;
                if (P.sky) LOGI("Fog: this map's fog colour: {:.3f},{:.3f},{:.3f} (the sky round the horizon, {:.0f} of 64 directions seen; darkest {:.3f}, brightest {:.3f}): kept for the map, the fog fades in. As a fixed colour: {}",
                                fc[0], fc[1], fc[2], a[3], r[0], r[1], asIni(fc));
            }
            if (!settled && P.sky && g_ProvAt <= 0 && a[3] >= 4)
            {
                g_ProvAt = now;
                LOGI("Fog: a first colour for this map from {:.0f} directions of the sky round the horizon: the fog fades in now, refined as more is seen until it settles", a[3]);
            }
            if (P.category && g_Cat < 0 && (settled || (v[13] >= 30 && a[3] >= 4)))
            {   // night, dusk or day, by the upper sky (25-75 deg: the top of a level view), else the horizon's average -- as
                // soon as the upper sky has been seen for a while (not when the whole horizon has)
                const float* up = v + 12;
                const float horizonY = 0.2126f * a[0] + 0.7152f * a[1] + 0.0722f * a[2];
                const bool upSeen = up[1] >= 30;
                const float y = upSeen ? up[0] : horizonY;
                g_Cat = y < P.nightBelow ? 0 : y >= P.dayAbove ? 2 : 1;
                const float* c = P.cat[g_Cat];
                const float cy = std::max(1e-4f, 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]);
                const float rSet = 1.f;     // the horizon's average is kept x EngineLuminanceFactor already
                const float k = c[3] > 0 ? c[3] : horizonY * rSet / cy;   // no brightness given: the map's horizon brightness
                for (int i = 0; i < 3; i++) g_CatRgb[i] = c[i] * k;
                g_CatAt = now;
                LOGI("Fog: this map is {} (the {} sky's brightness {:.3f}{}; night below {:.2f}, day from {:.2f}): fog colour {:.3f},{:.3f},{:.3f}{}",
                     kCatName[g_Cat], upSeen ? "upper" : "horizon's", y, upSeen ? fmt::format(", over {:.0f} frames", up[1]) : std::string(" (no upper sky seen)"),
                     P.nightBelow, P.dayAbove, g_CatRgb[0], g_CatRgb[1], g_CatRgb[2], c[3] > 0 ? fmt::format(" (brightness {:.2f} as set)", c[3]) : fmt::format(" (at the horizon's brightness {:.3f})", horizonY * rSet));
                const float cyl = 0.2126f * g_CatRgb[0] + 0.7152f * g_CatRgb[1] + 0.0722f * g_CatRgb[2];
                LOGI("Fog: its brightness {:.3f} x EngineLuminanceFactor (the game's factor now {:.3f}){}; CheapFogMinBrightness {:.2f} keeps it from going darker",
                     cyl, v[18], cyl < P.minBrightness ? std::string(": darker than the floor, drawn brighter") : std::string(""), P.minBrightness);
            }
            if (v[21] > 0 && (g_LoggedPlane <= 0 || fabsf(logf(v[20] / g_LoggedPlane)) > 0.25f))
            {   // the plane the empty pixels below the horizon are fogged at
                g_LoggedPlane = v[20];
                LOGI("Fog: nothing drawn below the horizon here (the sea beyond its floor{}): fogged as a plane {:.1f} m below the eye, where the ground next to it lies",
                     g_WaterUsed.load() ? "" : ", the water's depth not known", v[20]);
            }
            if (P.skyAuto && fabsf(r[2] - g_LoggedTop) > 0.02f)
            {   // the sky's fog follows the far background up (CheapFogSkyHeight = auto)
                g_LoggedTop = r[2];
                LOGI("Fog: the far background reaches {:.1f} deg above the horizon: the sky is fogged up to there, thinning out {:.0f} deg above it",
                     asinf(std::clamp(r[2], -1.f, 1.f)) * 57.29578f, P.skyHeight);
            }
            if (settled)
            {   // the check of the exposure model: the sky as drawn x EngineLuminanceFactor against how it was learned (~1)
                const float e = v[16] > 0 ? v[16] : 1.f;
                if (fabsf(logf(e / g_LoggedExpo)) > 0.15f)
                {
                    LOGI("Fog: exposure check: the sky as drawn x EngineLuminanceFactor is x{:.2f} of what it was learned as (1 = the fog "
                         "follows the exposure as the scene does; the fog is scaled by it smoothed, now x{:.2f}); the factor {:.3f} now, {:.3f} when this map's colour was set{}", e, v[19], v[18], v[17],
                         P.followExposure ? "" : " (CheapFogFollowExposure = 0: the fog does not follow it)");
                    g_LoggedExpo = e;
                }
            }
            if (!P.sky && !P.category) LOGI("Fog: a fixed colour ({:.3f}, {:.3f}, {:.3f} linear); the sky round the horizon here, for comparison: {:.3f},{:.3f},{:.3f} over {:.0f} of 64 directions ({})",
                             P.rgb[0] * P.brightness, P.rgb[1] * P.brightness, P.rgb[2] * P.brightness, a[0], a[1], a[2], a[3], a[3] > 0 ? asIni(a) : std::string("none seen yet"));
            else if (!settled && now - g_LogAt2 >= 10.0)
            {
                g_LogAt2 = now;
                LOGI("Fog: no settled colour for this map yet: {:.0f} of 64 directions of the sky round the horizon seen (from 5 s in; all of them, or 8 after 20 s, settle it){}", a[3],
                     a[3] >= 4 ? ": the fog uses what has been seen so far" : ": no fog until 4 are seen");
            }
        }
        else if (hr != DXGI_ERROR_WAS_STILL_DRAWING || now - g_StageAt > 2.0) g_StagePending = false;
    }
    ic->Release();
}

std::string Fog::Status()
{
    std::lock_guard lk(g_M);
    return fmt::format("fog want {} gamefogoff {} ready {} drawn {} (same frame {}, latest targets {}, none {}, immediate {}; not faded in {}, depth-tested {}, "
                       "with the water's depth {}) ({})", g_Want.load() ? 1 : 0,
                       g_GameFogOff.load() ? 1 : 0, g_Ready ? 1 : 0, g_Drawn.load(), g_Matched.load(), g_Fallback.load(), g_NoNote.load(), g_Immediate.load(),
                       g_Faded.load(), g_EarlyZ.load(), g_WaterUsed.load(), g_Why);
}
