#include "pch.h"
#include "FovMask.h"
#include "CameraHook.h"
#include "VRRuntime.h"
#include "VRMath.h"
#include "XRSession.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mutex>
#include <vector>
#include <cmath>
#include <algorithm>

namespace
{
    const char* kHlsl = R"(
StructuredBuffer<float4> Verts : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position { return Verts[id]; }
)";
    constexpr uint32_t kViewProj = 0x7390;   // the view's projection (row-major, as stored)
    constexpr int kMaxVerts = 3 * 4096;

    std::atomic<bool> g_On{ true };
    // 1 = everything outside the native fov; 2 = only the OUTER side of each eye (left of the left eye, right of the
    // right eye: "like a scope"). Reported 2026-10-31 with 1: vertical slices missing from the screen-space
    // reflections, hair and glass broken; thought to come from the strips on the nose side, between the eyes' views
    std::atomic<int> g_Mode{ 1 };
    std::atomic<float> g_Margin{ 2.f };
    std::atomic<bool> g_Lens{ true };       // OutOfViewMaskLens: the lens's hidden area too (XR_KHR_visibility_mask)
    // OutOfViewMaskCorners: the two outer corners of each eye's rendered picture outside a superellipse |x|^n + |y|^n = 1
    // (x, y the picture's own -1..1 coordinates: through the middles of its edges); 0 = none. For a headset whose
    // runtime gives no lens mask (Pimax via SteamVR: none): the lens shows a rounded picture, its corners are never seen.
    // n = 4 masks ~3.5 % of the fov (two corners of four), 3 ~6 %, 2 (an ellipse) ~11 %
    std::atomic<float> g_Corners{ 0.f };
    float g_CornerShare[2] = {};

    std::mutex g_M;   // everything below
    bool g_Ready = false, g_Failed = false;
    ID3D11Device* g_Dev = nullptr;
    ID3D11VertexShader* g_Vs = nullptr;
    ID3D11BlendState* g_NoColor = nullptr; ID3D11DepthStencilState* g_Write = nullptr; ID3D11RasterizerState* g_Rs = nullptr;
    struct Eye
    {
        float key[16 + 4 + 4 + 1 + 1] = {};  // what the geometry was built from: projection, native fov, turn, margin, lens mesh
        bool built = false, empty = true;
        ID3D11Buffer* buf = nullptr; ID3D11ShaderResourceView* srv = nullptr; UINT n = 0;
        float masked = 0;                   // the share of the rectangle masked (outside the fov)
        float lensShare = 0; int lensTris = 0;   // the lens's hidden triangles, their share of the rectangle
        unsigned long long drawn = 0;
        bool logged = false;
    } g_Eye[2];
    std::atomic<unsigned long long> g_Drawn{ 0 }, g_NotScene{ 0 }, g_NoFov{ 0 };
    std::string g_Why = "not run yet";

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
        if (!compile) { g_Why = "d3dcompiler_47 not available"; LOGI("FovMask: {}", g_Why); return false; }
        ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
        if (FAILED(compile(kHlsl, strlen(kHlsl), "fovmask", nullptr, nullptr, "vs", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err)))
        {
            g_Why = "shader compile failed";
            LOGI("FovMask: the shader failed to compile: {}", err ? (const char*)err->GetBufferPointer() : "?");
            if (err) err->Release(); return false;
        }
        if (err) err->Release();
        const bool vsOk = SUCCEEDED(dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_Vs));
        b->Release();
        if (!vsOk) { g_Why = "vertex shader"; return false; }
        D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].RenderTargetWriteMask = 0;   // (applies to every target: no colour written)
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_ALWAYS; ds.StencilEnable = FALSE;
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = FALSE; rd.ScissorEnable = FALSE;
        if (FAILED(dev->CreateBlendState(&bl, &g_NoColor)) || FAILED(dev->CreateDepthStencilState(&ds, &g_Write)) || FAILED(dev->CreateRasterizerState(&rd, &g_Rs)))
        { g_Why = "pipeline states"; return false; }
        g_Dev = dev; g_Dev->AddRef();
        g_Failed = false; g_Ready = true;
        LOGI("FovMask: ready (the part of each eye's image outside its native fov + {:.1f} deg is masked in the depth buffer before the depth pass)", g_Margin.load());
        return true;
    }

    // ---- the geometry, in the render's NDC ----
    struct V2 { float x, y; };
    //! The projection as stored: column vectors (clip = P v) when its row 3 holds (0, 0, +-1, 0), else row vectors.
    bool Project(const float* P, const float v[3], V2& out)
    {
        const bool colVec = fabsf(fabsf(P[14]) - 1.f) < 1e-3f;
        float c[4];
        for (int i = 0; i < 4; i++)
            c[i] = colVec ? P[i * 4 + 0] * v[0] + P[i * 4 + 1] * v[1] + P[i * 4 + 2] * v[2] + P[i * 4 + 3]
                          : v[0] * P[0 * 4 + i] + v[1] * P[1 * 4 + i] + v[2] * P[2 * 4 + i] + P[3 * 4 + i];
        if (!(c[3] > 1e-4f)) return false;
        out = { c[0] / c[3], c[1] / c[3] };
        return std::isfinite(out.x) && std::isfinite(out.y);
    }
    //! The depth at the near plane: 0 (standard), 1 (reversed: the far plane at 0)
    float NearDepth(const float* P)
    {
        const bool colVec = fabsf(fabsf(P[14]) - 1.f) < 1e-3f;
        const float s = colVec ? P[14] : P[11], a = P[10];
        return std::clamp(a * s, 0.f, 1.f) > 0.5f ? 0.f : 1.f;
    }
    float Side(V2 a, V2 b, V2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); }
    //! `poly` cut to the half-plane where Side(a, b, p) * sign >= 0
    std::vector<V2> Clip(const std::vector<V2>& poly, V2 a, V2 b, float sign)
    {
        std::vector<V2> out;
        const size_t n = poly.size();
        for (size_t i = 0; i < n; i++)
        {
            const V2 p = poly[i], q = poly[(i + 1) % n];
            const float sp = Side(a, b, p) * sign, sq = Side(a, b, q) * sign;
            if (sp >= 0) out.push_back(p);
            if ((sp >= 0) != (sq >= 0))
            {
                const float t = sp / (sp - sq);
                out.push_back({ p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t });
            }
        }
        return out;
    }
    float Area(const std::vector<V2>& p)
    {
        float a = 0; for (size_t i = 0; i < p.size(); i++) { const V2 u = p[i], w = p[(i + 1) % p.size()]; a += u.x * w.y - w.x * u.y; }
        return fabsf(a) * 0.5f;
    }
    //! The triangles of the rectangle [-1, 1]^2 outside the native fov's quad, as float4 (x, y, depth, 1); false = no
    //! geometry (the fov not known, or the quad not in front of the view)
    bool Build(int eye, const float* P, std::vector<float>& verts, float& masked, float& lensShare, int& lensTris, std::string& why)
    {
        CameraHook::Tans t; float rot[4] = { 0, 0, 0, 1 };
        if (!CameraHook::EyeTans(eye, t, rot) || !t.valid) { why = "the eye's render frustum is not known"; return false; }
        const VRRuntime::Placement pl = VRRuntime::GetPlacement(eye);
        if (!pl.valid || !(pl.fov[1] > pl.fov[0]) || !(pl.fov[2] > pl.fov[3])) { why = "the eye's native fov is not known"; return false; }
        const float m = std::max(g_Margin.load(), CameraHook::EyeProjMargin()) / 57.29578f;
        const float lim = 85.f / 57.29578f;
        const float L = tanf(std::max(-lim, pl.fov[0] - m)), R = tanf(std::min(lim, pl.fov[1] + m));
        const float U = tanf(std::min(lim, pl.fov[2] + m)), D = tanf(std::max(-lim, pl.fov[3] - m));
        // the native fov's corners (eye frame, XR: x right, y up, -z forward) into the render frame: v_render = conj(rot) v_eye
        const float conj[4] = { -rot[0], -rot[1], -rot[2], rot[3] };
        const float cx[4] = { L, R, R, L }, cy[4] = { U, U, D, D };
        std::vector<V2> quad(4);
        for (int i = 0; i < 4; i++)
        {
            const float ve[3] = { cx[i], cy[i], -1.f }; float vr[3];
            VRMath::QRotate(conj, ve, vr);
            if (!(vr[2] < -1e-3f) || !Project(P, vr, quad[i])) { why = "a corner of the native fov is not in front of the view"; return false; }
        }
        V2 cen{ 0, 0 }; for (const V2& q : quad) { cen.x += q.x * 0.25f; cen.y += q.y * 0.25f; }
        const std::vector<V2> rect = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
        // the visible part: the rectangle inside all four edges; the masked share is the rest
        std::vector<V2> in = rect;
        for (int i = 0; i < 4 && !in.empty(); i++) { const V2 a = quad[i], b = quad[(i + 1) % 4]; in = Clip(in, a, b, Side(a, b, cen) >= 0 ? 1.f : -1.f); }
        masked = 1.f - Area(in) / 4.f;
        const float z = NearDepth(P);
        verts.clear();
        // the quad's edges: 0 top, 1 right, 2 bottom, 3 left. Mode 2: only the outer side's (the left eye's left edge,
        // the right eye's right edge)
        const int sides = g_Mode.load();
        const int outer = eye == 0 ? 3 : 1;
        if (sides == 2) masked = 0;
        for (int i = 0; i < 4; i++)
        {   // outside edge i (overlaps with the next edge's region are drawn twice: depth writes, harmless)
            if (sides == 2 && i != outer) continue;
            const V2 a = quad[i], b = quad[(i + 1) % 4];
            const std::vector<V2> out = Clip(rect, a, b, Side(a, b, cen) >= 0 ? -1.f : 1.f);
            if (out.size() < 3 || Area(out) < 1e-6f) continue;
            if (sides == 2) masked += Area(out) / 4.f;
            for (size_t k = 1; k + 1 < out.size(); k++)
                for (const V2& p : { out[0], out[k], out[k + 1] }) { verts.push_back(p.x); verts.push_back(p.y); verts.push_back(z); verts.push_back(1.f); }
        }
        // the lens's own hidden area (XR_KHR_visibility_mask): its triangles, from the eye's z = -1 plane into the render
        // frame and projected the same way
        lensShare = 0; lensTris = 0;
        std::vector<float> lens;
        if (g_Lens.load() && sides != 2 && XRVisMask::Get(eye, lens, nullptr))
        {   // its points must lie within the eye's fov (tangents on z = -1, as the extension defines them): else not used
            const float tl = tanf(pl.fov[0]), tr = tanf(pl.fov[1]), tu = tanf(pl.fov[2]), td = tanf(pl.fov[3]);
            const float sx = 0.05f * (tr - tl), sy = 0.05f * (tu - td);
            bool inside = true;
            for (size_t k = 0; k + 1 < lens.size() && inside; k += 2)
                inside = lens[k] >= tl - sx && lens[k] <= tr + sx && lens[k + 1] >= td - sy && lens[k + 1] <= tu + sy;
            if (!inside)
            {
                static bool told[2] = {};
                if (!told[eye & 1]) LOGI("FovMask: eye {}'s lens mask from the runtime does not lie within its fov's tangents: not used", eye);
                told[eye & 1] = true; lens.clear();
            }
        }
        if (!lens.empty())
            for (size_t k = 0; k + 5 < lens.size(); k += 6)
            {
                V2 q[3]; bool ok = true;
                for (int c = 0; c < 3 && ok; c++)
                {
                    const float ve[3] = { lens[k + 2 * c], lens[k + 2 * c + 1], -1.f }; float vr[3];
                    VRMath::QRotate(conj, ve, vr);
                    ok = vr[2] < -1e-3f && Project(P, vr, q[c]);
                }
                if (!ok) continue;
                std::vector<V2> tri = { q[0], q[1], q[2] };
                for (int i = 0; i < 4 && !tri.empty(); i++) { const V2 a = rect[i], b = rect[(i + 1) % 4]; tri = Clip(tri, a, b, 1.f); }   // (the rectangle is counter-clockwise)
                if (tri.size() < 3) continue;
                lensShare += Area(tri) / 4.f; lensTris++;
                for (const V2& p : q) { verts.push_back(p.x); verts.push_back(p.y); verts.push_back(z); verts.push_back(1.f); }
            }
        // the picture's two outer corners outside a superellipse (OutOfViewMaskCorners), in the render rectangle itself
        // (NDC, linear in the view's tangents): the first version fitted it to the native fov, whose corners lie outside
        // the rendered rectangle on a Pimax (the rectangle sits inside the fov there) -- it masked nothing
        g_CornerShare[eye & 1] = 0;
        if (const float n = g_Corners.load(); n >= 1.f)
        {
            constexpr int kSeg = 16;
            const float e = 2.f / n;
            const float sx = eye == 0 ? -1.f : 1.f;              // the outer side: the left eye's left, the right eye's right
            for (int q = 0; q < 2; q++)
            {
                const float sy = q == 0 ? 1.f : -1.f;
                const V2 corner{ sx, sy };
                V2 prev{ sx, 0.f };
                for (int k = 1; k <= kSeg; k++)
                {
                    const float th = 1.5707963f * (float)k / (float)kSeg;
                    const V2 cur{ sx * powf(std::max(0.f, cosf(th)), e), sy * powf(std::max(0.f, sinf(th)), e) };
                    g_CornerShare[eye & 1] += Area({ corner, prev, cur }) / 4.f;
                    for (const V2& p : { corner, prev, cur }) { verts.push_back(p.x); verts.push_back(p.y); verts.push_back(z); verts.push_back(1.f); }
                    prev = cur;
                }
            }
        }
        if (verts.size() / 4 > (size_t)kMaxVerts) { why = "too many triangles"; return false; }
        return true;
    }
    //! The eye's geometry, rebuilt when what it depends on changed
    Eye* EyeFor(int eye, const float* P)
    {
        Eye& E = g_Eye[eye];
        CameraHook::Tans t; float rot[4] = { 0, 0, 0, 1 };
        CameraHook::EyeTans(eye, t, rot);
        const VRRuntime::Placement pl = VRRuntime::GetPlacement(eye);
        float key[26];
        unsigned gen = 0; { std::vector<float> tmp; XRVisMask::Get(eye, tmp, &gen); }
        memcpy(key, P, 64); memcpy(key + 16, pl.fov, 16); memcpy(key + 20, rot, 16); key[24] = g_Margin.load() + 100.f * (float)g_Mode.load() + 1000.f * g_Corners.load(); key[25] = g_Lens.load() ? (float)gen + 1.f : 0.f;
        bool same = E.built;
        for (int i = 0; same && i < 26; i++) same = fabsf(key[i] - E.key[i]) < (i < 16 ? 2e-3f : 1e-5f);   // (a TAA jitter is not a change)
        if (same) return &E;
        std::vector<float> v; float masked = 0, lensShare = 0; int lensTris = 0; std::string why;
        Rel(E.srv); Rel(E.buf); E.n = 0; E.empty = true; E.built = true; memcpy(E.key, key, sizeof key);
        if (!Build(eye, P, v, masked, lensShare, lensTris, why)) { g_Why = why; return &E; }
        E.masked = masked; E.lensShare = lensShare; E.lensTris = lensTris;
        if (v.empty()) { if (!E.logged) LOGI("FovMask: eye {}: its whole image is inside its native fov: nothing to mask", eye); E.logged = true; return &E; }
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = (UINT)(v.size() * 4); bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 16;
        D3D11_SUBRESOURCE_DATA sd{ v.data(), 0, 0 };
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = DXGI_FORMAT_UNKNOWN; vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; vd.Buffer.NumElements = (UINT)(v.size() / 4);
        if (FAILED(g_Dev->CreateBuffer(&bd, &sd, &E.buf)) || FAILED(g_Dev->CreateShaderResourceView(E.buf, &vd, &E.srv))) { Rel(E.buf); g_Why = "vertex buffer"; return &E; }
        E.n = (UINT)(v.size() / 4); E.empty = false;
        LOGI("FovMask: eye {}: {:.0f} % of its image lies outside its native fov (+ {:.1f} deg){}, {}: masked with {} triangles at depth {:.0f}",
             eye, masked * 100.f, std::max(g_Margin.load(), CameraHook::EyeProjMargin()),
             g_Corners.load() >= 1.f ? fmt::format(", the fov's corners {:.0f} % more (OutOfViewMaskCorners {:g})", g_CornerShare[eye & 1] * 100.f, g_Corners.load()) : std::string(),
             !g_Lens.load() ? std::string("the lens's hidden area not used (OutOfViewMaskLens = 0)")
                            : lensTris ? fmt::format("the lens's hidden area {} triangles, {:.0f} % more (some of it overlapping)", lensTris, lensShare * 100.f)
                                       : std::string("no lens mask from the runtime (yet)"),
             E.n / 3, v[2]);
        return &E;
    }

    //! What the mask's draw changes, as it was, put back exactly after it (the game's next draw is its own first)
    struct Saved
    {
        ID3D11VertexShader* vs = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11GeometryShader* gs = nullptr;
        ID3D11HullShader* hs = nullptr; ID3D11DomainShader* ds = nullptr; ID3D11ShaderResourceView* vsSrv = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED; ID3D11InputLayout* il = nullptr;
        ID3D11RasterizerState* rs = nullptr; ID3D11BlendState* bs = nullptr; float bf[4] = {}; UINT mask = 0xFFFFFFFF;
        ID3D11DepthStencilState* dss = nullptr; UINT ref = 0;
        void Save(ID3D11DeviceContext* c)
        {
            c->VSGetShader(&vs, nullptr, nullptr); c->PSGetShader(&ps, nullptr, nullptr); c->GSGetShader(&gs, nullptr, nullptr);
            c->HSGetShader(&hs, nullptr, nullptr); c->DSGetShader(&ds, nullptr, nullptr); c->VSGetShaderResources(0, 1, &vsSrv);
            c->IAGetPrimitiveTopology(&topo); c->IAGetInputLayout(&il); c->RSGetState(&rs);
            c->OMGetBlendState(&bs, bf, &mask); c->OMGetDepthStencilState(&dss, &ref);
        }
        void Restore(ID3D11DeviceContext* c)
        {
            c->VSSetShader(vs, nullptr, 0); c->PSSetShader(ps, nullptr, 0); c->GSSetShader(gs, nullptr, 0);
            c->HSSetShader(hs, nullptr, 0); c->DSSetShader(ds, nullptr, 0); c->VSSetShaderResources(0, 1, &vsSrv);
            c->IASetPrimitiveTopology(topo); c->IASetInputLayout(il); c->RSSetState(rs);
            c->OMSetBlendState(bs, bf, mask); c->OMSetDepthStencilState(dss, ref);
            Rel(vs); Rel(ps); Rel(gs); Rel(hs); Rel(ds); Rel(vsSrv); Rel(il); Rel(rs); Rel(bs); Rel(dss);
        }
    };
}

void FovMask::SetParams(int mode, float marginDeg, bool lens)
{
    g_On = mode > 0; g_Mode = mode >= 2 ? 2 : 1; g_Margin = std::clamp(marginDeg, 0.f, 15.f); g_Lens = lens;
    LOGI("FovMask: {}", mode >= 2 ? fmt::format("on, outer sides only: what each eye's image shows beyond its native fov on its OUTER side (+ {:.1f} deg) is not shaded (OutOfViewMask = 2)", g_Margin.load())
                      : mode == 1 ? fmt::format("on: what each eye's image shows outside its native fov (+ {:.1f} deg){} is not shaded (OutOfViewMask = 1)", g_Margin.load(),
                                                lens ? " and behind the lens's hidden area" : "")
                                  : std::string("off (OutOfViewMask = 0)"));
}
bool FovMask::Wanted() { return g_On.load(); }
void FovMask::SetCorners(float n)
{
    g_Corners = n < 1.f ? 0.f : std::min(n, 16.f);
    if (g_Corners.load() >= 1.f)
        LOGI("FovMask: OutOfViewMaskCorners {:g}: the two outer corners of each eye's fov outside a rounded outline are not shaded either{}", g_Corners.load(),
             g_On.load() ? "" : " (needs OutOfViewMask 1 or 2: off now)");
}

bool FovMask::AtDraw(ID3D11DeviceContext* c, const void* view, int eye)
{
    if (!g_On.load(std::memory_order_relaxed) || !c || !view || eye < 0 || eye > 1) return true;
    // the scene's depth buffer bound (the eye's render size), else wait for the task's next draw
    ID3D11DepthStencilView* dsv = nullptr; c->OMGetRenderTargets(0, nullptr, &dsv);
    if (!dsv) return false;
    ID3D11Resource* res = nullptr; dsv->GetResource(&res); dsv->Release();
    D3D11_RESOURCE_DIMENSION dim{}; if (res) res->GetType(&dim);
    D3D11_TEXTURE2D_DESC td{};
    if (res && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) static_cast<ID3D11Texture2D*>(res)->GetDesc(&td);
    if (res) res->Release();
    uint32_t rw = 0, rh = 0;
    ReadBytes(&rw, (const uint8_t*)view + CameraHook::kView_RenderW, 4); ReadBytes(&rh, (const uint8_t*)view + CameraHook::kView_RenderH, 4);
    if (!td.Width || td.Width != rw || td.Height != rh || td.SampleDesc.Count != 1) { g_NotScene++; return false; }
    float P[16];
    if (!ReadBytes(P, (const uint8_t*)view + kViewProj, 64)) return true;
    std::lock_guard lk(g_M);
    if (!g_Ready) { ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (dev) { Init(dev); dev->Release(); } }
    if (!g_Ready) return true;
    Eye* E = EyeFor(eye, P);
    if (!E || E->empty || !E->srv) { g_NoFov++; return true; }
    Saved s; s.Save(c);
    c->VSSetShader(g_Vs, nullptr, 0); c->PSSetShader(nullptr, nullptr, 0); c->GSSetShader(nullptr, nullptr, 0);
    c->HSSetShader(nullptr, nullptr, 0); c->DSSetShader(nullptr, nullptr, 0);
    c->VSSetShaderResources(0, 1, &E->srv);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); c->IASetInputLayout(nullptr);
    c->RSSetState(g_Rs); c->OMSetBlendState(g_NoColor, nullptr, 0xFFFFFFFF); c->OMSetDepthStencilState(g_Write, 0);
    c->Draw(E->n, 0);
    s.Restore(c);
    E->drawn++; g_Drawn++;
    g_Why = "running";
    return true;
}

std::string FovMask::Status()
{
    std::lock_guard lk(g_M);
    return fmt::format("fovmask on {} ready {} drawn {} (eye 0 {} at {:.0f} %, eye 1 {} at {:.0f} %) not the scene's depth {} no fov {} ({})",
                       g_On.load() ? 1 : 0, g_Ready ? 1 : 0, g_Drawn.load(), g_Eye[0].drawn, g_Eye[0].masked * 100.f, g_Eye[1].drawn, g_Eye[1].masked * 100.f,
                       g_NotScene.load(), g_NoFov.load(), g_Why);
}
