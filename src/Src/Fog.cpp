#include "pch.h"
#include "Fog.h"
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <mutex>
#include <vector>
#include <cmath>
#include <algorithm>

namespace
{
    // ---- the shaders ----
    // P: the view's projection (view+0x7390) as stored, rows P[0..3]. Either matrix convention is handled: the row or
    // the column holding (0, 0, +-1, 0) says which. V: its view matrix (view+0x7450, rows -left / up / -forward: ENGINE.md),
    // for each pixel's ray in the world. The fog has ONE colour per map: the average of the sky near the horizon all the
    // way round (64 world directions, each learned as the player looks that way), settled once every direction has been
    // seen (or 30 s into the map with 8 of them) and kept for the map; the fog fades in then. Per-direction colours
    // (tried 2026-10-31) differed between the eyes at a sky's transitions and could not show on the sky itself.
    const char* kHlsl = R"(
cbuffer FogCB : register(b0)
{
    float4 P[4];
    float4 V[3];   // the view matrix's rows (view+0x7450: -left, up, -forward in world, z up), xyz
    float4 g0;   // start (m), 1 / (end - start), opacity, use the fixed colour (0 / 1)
    float4 g1;   // the fixed colour (linear, times CheapFogBrightness)
    float4 g2;   // width, height, sample step of the sky's survey, 1 = learning the colours now
    float4 g3;   // 1 = the map's time to settle on a colour is up, the fog's fade-in (0..1), frames each direction is learned over,
                 // sin(CheapFogSkyHeight) (0: sky not fogged)
};
Texture2D<float4> Color : register(t0);
Texture2D<float> Depth : register(t1);
StructuredBuffer<float4> SummaryIn : register(t2);
RWStructuredBuffer<uint> Acc : register(u0);        // this frame's horizon sky, per direction: r, g, b, weight (x256)
RWStructuredBuffer<float4> Bins : register(u1);     // learned per direction: rgb, frames averaged (kept once full)
RWStructuredBuffer<float4> Summary : register(u2);  // [0] the average of the learned directions, how many; [1] darkest /
                                                    // brightest; [2] THE fog colour for the map, w 1 = settled (then kept)
static const uint kBins = 64;

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

// The sky near the horizon (-10 to +30 deg, weighted towards it), by the world direction it lies in: 64 directions
// round the horizon. Summed per group, then once per group into Acc.
groupshared uint ga[kBins * 4];
[numthreads(16, 16, 1)]
void sky(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    ga[gi] = 0;
    GroupMemoryBarrierWithGroupSync();
    uint2 p = (gid.xy * 16 + tid.xy) * (uint)g2.z;
    if (p.x < (uint)g2.x && p.y < (uint)g2.y && Distance(p + 0.5, Depth[p]) == 0)
    {
        float3 wd = WorldDir(p + 0.5);
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
}

// Each direction: this frame's sky added to what it has learned (until it has enough: then kept as it is), the
// frame's sums cleared; then the average round the horizon. It becomes the map's one fog colour, kept from then on,
// once every direction has been seen -- or, after the map's first 30 s, once at least 8 of them have.
groupshared float4 gb[kBins];
[numthreads(64, 1, 1)]
void skyfinal(uint gi : SV_GroupIndex)
{
    uint4 a = uint4(Acc[gi * 4 + 0], Acc[gi * 4 + 1], Acc[gi * 4 + 2], Acc[gi * 4 + 3]);
    Acc[gi * 4 + 0] = 0; Acc[gi * 4 + 1] = 0; Acc[gi * 4 + 2] = 0; Acc[gi * 4 + 3] = 0;
    float4 bin = Bins[gi];
    if (g2.w > 0 && a.w > 20 * 256 && bin.w < g3.z)   // enough of this direction's sky seen this frame
    {
        float3 c = float3(a.xyz) / (float)a.w;
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
        Summary[1] = float4(n > 0 ? lo : 0, hi, 0, 0);
        if (Summary[2].w <= 0 && (n >= kBins || (g3.x > 0 && n >= 8))) Summary[2] = float4(avg, 1);
    }
}

struct VO { float4 pos : SV_Position; };
VO vs(uint id : SV_VertexID)
{
    VO o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 ps(VO i) : SV_Target
{
    uint2 p = (uint2)i.pos.xy;
    float d = Distance(i.pos.xy, Depth[p]);
    float3 wd = WorldDir(i.pos.xy);
    float3 col = g1.rgb;
    if (g0.w < 0.5)
    {   // the map's one colour, once settled
        float4 c = SummaryIn[2];
        if (c.w <= 0) return float4(0, 0, 0, 0);
        col = c.rgb;
    }
    float f = 0;
    if (d > 0)
    {
        float t = saturate((d - g0.x) * g0.y);
        f = g0.z * t * t * (3 - 2 * t);
    }
    else if (g3.w > 0)
    {   // the sky: as thick as the far distance at the horizon (and below it), thinning out to none at CheapFogSkyHeight
        float t = 1 - saturate(wd.z / g3.w);
        f = g0.z * t * t * (3 - 2 * t);
    }
    return float4(col, f * g3.y);   // blended: scene * (1 - f) + fog colour * f
}
)";

    std::atomic<bool> g_Want{ true }, g_GameFogOff{ false };
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
    // the horizon's colours (see the shaders): the frame's sums, the learned directions, the summary; a copy read back
    constexpr UINT kBins = 64, kFilled = 3;
    ID3D11Buffer* g_Acc = nullptr; ID3D11UnorderedAccessView* g_AccUav = nullptr;
    ID3D11Buffer* g_Bins = nullptr; ID3D11UnorderedAccessView* g_BinsUav = nullptr;
    ID3D11Buffer* g_Summary = nullptr; ID3D11UnorderedAccessView* g_SummaryUav = nullptr; ID3D11ShaderResourceView* g_SummarySrv = nullptr;
    ID3D11Buffer* g_Stage = nullptr; bool g_StagePending = false; double g_StageAt = 0, g_LogAt = 0, g_LogAt2 = 0;
    constexpr UINT kSkyStep = 8;
    // the scene's colour / depth targets, with the mod's views of them
    struct Target
    {
        ID3D11Texture2D* color = nullptr; ID3D11Texture2D* depth = nullptr;
        ID3D11RenderTargetView* rtv = nullptr; ID3D11ShaderResourceView* colorSrv = nullptr; ID3D11ShaderResourceView* depthSrv = nullptr;
        UINT w = 0, h = 0, gx = 0, gy = 0; bool bad = false;
    };
    Target g_T[4];
    // what the end of FORWARD_DYNAMIC left, per frame (the view's frame number), for that frame's ALPHA_LIT
    struct Note { unsigned frame = 0; int target = -1; float proj[16] = {}, view[16] = {}; bool valid = false; };
    Note g_Notes[8]; Note g_Last;
    std::atomic<unsigned long long> g_Drawn{ 0 }, g_Matched{ 0 }, g_Fallback{ 0 }, g_NoNote{ 0 }, g_Immediate{ 0 };
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
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 176; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
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
        if (!sbuf(kBins * 4, 4, false, &g_Acc, &g_AccUav, nullptr) || !sbuf(kBins, 16, false, &g_Bins, &g_BinsUav, nullptr)
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
        g_Dev = dev; g_Dev->AddRef();
        g_Failed = false; g_Ready = true;
        LOGI("Fog: shaders ready (sky colour, fog)");
        return true;
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
            for (Target& t : g_T) { Rel(t.depthSrv); Rel(t.colorSrv); Rel(t.rtv); Rel(t.depth); Rel(t.color); t = Target{}; }
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
        LOGI("Fog: the scene's targets: colour {}x{} format {} and depth format {} (read as {}): fog drawn into them", t.w, t.h, (int)cd.Format, (int)dd.Format, (int)sf);
        return slot;
    }

    //! Every piece of the context's state the fog touches, as it was, put back exactly afterwards: whatever the game had
    //! bound there (and what its own state cache believes is bound) is what its ALPHA_LIT work then finds. (Putting back
    //! "a fresh context's defaults" instead was reported to break the transparent surfaces: 2026-10-31.)
    struct Saved
    {
        ID3D11ComputeShader* cs = nullptr; ID3D11Buffer* csCb = nullptr; ID3D11ShaderResourceView* csSrv[2] = {}; ID3D11UnorderedAccessView* csUav[2] = {};
        D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED; ID3D11InputLayout* il = nullptr;
        ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11Buffer* psCb = nullptr; ID3D11ShaderResourceView* psSrv[3] = {};
        ID3D11RasterizerState* rs = nullptr; D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {}; UINT nvp = 0;
        ID3D11BlendState* bs = nullptr; float bf[4] = {}; UINT mask = 0xFFFFFFFF;
        ID3D11DepthStencilState* dss = nullptr; UINT ref = 0;
        ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {}; ID3D11DepthStencilView* dsv = nullptr;
        void Save(ID3D11DeviceContext* c)
        {
            c->CSGetShader(&cs, nullptr, nullptr); c->CSGetConstantBuffers(0, 1, &csCb); c->CSGetShaderResources(0, 2, csSrv); c->CSGetUnorderedAccessViews(0, 2, csUav);
            c->IAGetPrimitiveTopology(&topo); c->IAGetInputLayout(&il);
            c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr); c->PSGetConstantBuffers(0, 1, &psCb); c->PSGetShaderResources(0, 3, psSrv);
            c->RSGetState(&rs); nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetViewports(&nvp, vp);
            c->OMGetBlendState(&bs, bf, &mask); c->OMGetDepthStencilState(&dss, &ref);
            c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        }
        void Restore(ID3D11DeviceContext* c)
        {
            const UINT keep[2] = { (UINT)-1, (UINT)-1 };
            c->CSSetShader(cs, nullptr, 0); c->CSSetConstantBuffers(0, 1, &csCb); c->CSSetUnorderedAccessViews(0, 2, csUav, keep); c->CSSetShaderResources(0, 2, csSrv);
            c->IASetPrimitiveTopology(topo); c->IASetInputLayout(il);
            c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0); c->PSSetConstantBuffers(0, 1, &psCb); c->PSSetShaderResources(0, 3, psSrv);
            c->RSSetState(rs); c->RSSetViewports(nvp, nvp ? vp : nullptr);
            c->OMSetBlendState(bs, bf, mask); c->OMSetDepthStencilState(dss, ref);
            c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
            Rel(cs); Rel(csCb); for (auto*& p : csSrv) Rel(p); for (auto*& p : csUav) Rel(p); Rel(il); Rel(vs); Rel(ps); Rel(psCb);
            for (auto*& p : psSrv) Rel(p); Rel(rs); Rel(bs); Rel(dss); for (auto*& p : rtv) Rel(p); Rel(dsv);
        }
    };
    double g_SettledAt = 0;                     // when the map's colour was seen settled (render thread; 0 = not yet): the fade-in
    double Now() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart / (double)f.QuadPart; }
}

void Fog::SetParams(const Params& p)
{
    { std::lock_guard lk(g_PM); g_P = p; }
    g_Want = p.on;
    if (p.on) LOGI("Fog: the mod's own fog when the game's is off (CheapFog): from {} m, full at {} m, opacity {:.2f}, colour {}; the sky {}",
                   p.start, p.end, p.opacity,
                   p.sky ? std::string("the average of the sky round the horizon (one per map, settled as one looks round, then kept)")
                         : fmt::format("fixed: {:.3f},{:.3f},{:.3f} (linear, x{:.2f})", p.rgb[0], p.rgb[1], p.rgb[2], p.brightness),
                   p.skyHeight > 0 ? fmt::format("fogged up to {:.0f} deg above the horizon", p.skyHeight) : std::string("not fogged"));
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

    struct { float P[16]; float V[12]; float g0[4], g1[4], g2[4], g3[4]; } k{};
    memcpy(k.P, n.proj, 64);
    for (int r = 0; r < 3; r++) for (int col = 0; col < 3; col++) k.V[r * 4 + col] = n.view[r * 4 + col];
    const float range = std::max(1.f, P.end - P.start);
    k.g0[0] = P.start; k.g0[1] = 1.f / range; k.g0[2] = std::min(1.f, std::max(0.f, P.opacity)); k.g0[3] = P.sky ? 0.f : 1.f;
    for (int i = 0; i < 3; i++) k.g1[i] = P.rgb[i] * P.brightness;
    k.g2[0] = (float)t.w; k.g2[1] = (float)t.h; k.g2[2] = (float)kSkyStep; k.g2[3] = Now() - g_MapAt.load() > 5.0 ? 1.f : 0.f;
    k.g3[0] = Now() - g_MapAt.load() > 35.0 ? 1.f : 0.f;
    k.g3[1] = P.sky ? (g_SettledAt > 0 ? (float)std::min(1.0, (Now() - g_SettledAt) / 2.0) : 0.f) : 1.f;
    k.g3[2] = kLearnFrames;
    k.g3[3] = P.skyHeight > 0.f ? sinf(std::min(90.f, P.skyHeight) * 0.0174533f) : 0.f;
    Saved saved; saved.Save(c);
    c->UpdateSubresource(g_Cb, 0, nullptr, &k, 0, 0);

    ID3D11ShaderResourceView* nullSrv[3] = {}; ID3D11UnorderedAccessView* nullUav[3] = {};
    // nothing bound for output while the scene's targets are read (a resource bound both ways is read as nothing)
    c->OMSetRenderTargets(0, nullptr, nullptr);
    if (g_ResetColour.exchange(false))
    {   // a new map: its horizon is learned anew (and its colour settled anew)
        const float z[4] = {}; const UINT zu[4] = {};
        c->ClearUnorderedAccessViewFloat(g_BinsUav, z); c->ClearUnorderedAccessViewFloat(g_SummaryUav, z); c->ClearUnorderedAccessViewUint(g_AccUav, zu);
        g_SettledAt = 0;
    }
    {   // 1) the horizon's colours, round it (only what this eye sees of the sky adds to them; with a fixed colour, for the log)
        c->CSSetConstantBuffers(0, 1, &g_Cb);
        { ID3D11ShaderResourceView* srv[2] = { t.colorSrv, t.depthSrv }; c->CSSetShaderResources(0, 2, srv); }
        { ID3D11UnorderedAccessView* uav[3] = { g_AccUav, g_BinsUav, g_SummaryUav }; c->CSSetUnorderedAccessViews(0, 3, uav, nullptr); }
        c->CSSetShader(g_CsSky, nullptr, 0);
        c->Dispatch(t.gx, t.gy, 1);
        c->CSSetShader(g_CsFinal, nullptr, 0);
        c->Dispatch(1, 1, 1);
        c->CSSetUnorderedAccessViews(0, 3, nullUav, nullptr);
        c->CSSetShaderResources(0, 2, nullSrv);
    }
    // 2) the fog, blended over the scene
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->IASetInputLayout(nullptr);
    c->VSSetShader(g_Vs, nullptr, 0);
    c->PSSetShader(g_Ps, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &g_Cb);
    { ID3D11ShaderResourceView* srv[3] = { nullptr, t.depthSrv, g_SummarySrv }; c->PSSetShaderResources(0, 3, srv); }
    c->RSSetState(g_Rs);
    D3D11_VIEWPORT vp{ 0.f, 0.f, (float)t.w, (float)t.h, 0.f, 1.f };
    c->RSSetViewports(1, &vp);
    c->OMSetBlendState(g_Blend, nullptr, 0xFFFFFFFF);
    c->OMSetDepthStencilState(g_NoDepth, 0);
    c->OMSetRenderTargets(1, &t.rtv, nullptr);
    c->Draw(3, 0);
    // the game's state as it was (the PS's inputs first: the colour target goes back to being an output)
    c->PSSetShaderResources(0, 3, nullSrv);
    c->OMSetRenderTargets(0, nullptr, nullptr);
    saved.Restore(c);
    g_Drawn++;
    if (g_Drawn.load() == 1) LOGI("Fog: first fog drawn (frame {}, {}x{})", frame, t.w, t.h);
    g_Why = "running";
    return true;
}

void Fog::SetMap(const std::string& name)
{
    const bool menu = name.rfind("campaign/menu", 0) == 0;
    g_ResetColour = true; g_MapAt = Now();
    if (g_Menu.exchange(menu) != menu && g_Want.load()) LOGI("Fog: {}", menu ? "the main menu's scene: no fog of the mod's there" : "a map to play: the mod's fog may draw again");
}

void Fog::OnPresent(IDXGISwapChain* sc)
{
    // the summary read back: every second until this map's colour has settled (for its fade-in), then every 10 s
    if (!g_Want.load(std::memory_order_relaxed) || !sc) return;
    std::lock_guard lk(g_M);
    if (!g_Ready || !g_Stage || !g_Drawn.load()) return;
    const double now = Now();
    Params P; { std::lock_guard pk(g_PM); P = g_P; }
    if (!g_StagePending && now - g_LogAt < (g_SettledAt > 0 || !P.sky ? 10.0 : 1.0)) return;   // every second until the colour is settled
    ID3D11DeviceContext* ic = nullptr; g_Dev->GetImmediateContext(&ic);
    if (!ic) return;
    if (!g_StagePending) { ic->CopyResource(g_Stage, g_Summary); g_StagePending = true; g_StageAt = now; }
    else
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT hr = ic->Map(g_Stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (SUCCEEDED(hr))
        {
            float v[kFilled * 4]; memcpy(v, m.pData, sizeof v); ic->Unmap(g_Stage, 0);
            g_StagePending = false; g_LogAt = now;
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
            if (!P.sky) LOGI("Fog: a fixed colour ({:.3f}, {:.3f}, {:.3f} linear); the sky round the horizon here, for comparison: {:.3f},{:.3f},{:.3f} over {:.0f} of 64 directions ({})",
                             P.rgb[0] * P.brightness, P.rgb[1] * P.brightness, P.rgb[2] * P.brightness, a[0], a[1], a[2], a[3], a[3] > 0 ? asIni(a) : std::string("none seen yet"));
            else if (!settled && now - g_LogAt2 >= 10.0)
            {
                g_LogAt2 = now;
                LOGI("Fog: no colour for this map yet: {:.0f} of 64 directions of the sky round the horizon seen (from 5 s in; all of them, or 8 after 30 s, settle it): no fog until then", a[3]);
            }
        }
        else if (hr != DXGI_ERROR_WAS_STILL_DRAWING || now - g_StageAt > 2.0) g_StagePending = false;
    }
    ic->Release();
}

std::string Fog::Status()
{
    std::lock_guard lk(g_M);
    return fmt::format("fog want {} gamefogoff {} ready {} drawn {} (same frame {}, latest targets {}, none {}, immediate {}) ({})", g_Want.load() ? 1 : 0,
                       g_GameFogOff.load() ? 1 : 0, g_Ready ? 1 : 0, g_Drawn.load(), g_Matched.load(), g_Fallback.load(), g_NoNote.load(), g_Immediate.load(), g_Why);
}
