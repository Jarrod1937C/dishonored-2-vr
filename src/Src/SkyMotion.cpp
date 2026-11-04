#include "pch.h"
#include "SkyMotion.h"
#include "CameraHook.h"
#include "StereoTaa.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mutex>
#include <cmath>
#include <algorithm>

namespace
{
    // T: the pixel's NDC (x, y, 1) -> this eye's current position without the jitter (Tc) and its previous one (Tp),
    // both homogeneous (x, y, w). Vp: the viewport's x, y, 2 / width, 2 / height. Z.x: the depth of the cleared far value.
    const char* kHlsl = R"(
cbuffer SkyCB : register(b0) { float4 Tc[3]; float4 Tp[3]; float4 Vp; float4 Z; };
float4 vs(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), Z.x, 1);
}
float2 ps(float4 pos : SV_Position) : SV_Target
{
    float3 h = float3((pos.x - Vp.x) * Vp.z - 1, 1 - (pos.y - Vp.y) * Vp.w, 1);
    float3 c = float3(dot(Tc[0].xyz, h), dot(Tc[1].xyz, h), dot(Tc[2].xyz, h));
    float3 p = float3(dot(Tp[0].xyz, h), dot(Tp[1].xyz, h), dot(Tp[2].xyz, h));
    if (!(c.z > 1e-6) || !(p.z > 1e-6)) return float2(0, 0);   // behind the previous view: no history to follow
    float2 mv = (c.xy / c.z - p.xy / p.z) * float2(0.5, -0.5);  // as the engine's: (current - previous) x (0.5, -0.5), UV units
    return all(abs(mv) < 1) ? mv : float2(0, 0);
}
)";
    // the view object (ENGINE.md; StereoTaa writes this eye's own previous matrices into it when the view is built)
    constexpr uint32_t kProj = 0x7390, kView = 0x7450, kPrevProj = 0x7410, kPrevView = 0x74D0, kCurJitter = 0x7850, kPrevJitter = 0x7858;

    std::atomic<bool> g_On{ true };
    std::mutex g_M;   // everything below but the counters
    bool g_Ready = false, g_Failed = false;
    ID3D11VertexShader* g_Vs = nullptr; ID3D11PixelShader* g_Ps = nullptr; ID3D11Buffer* g_Cb = nullptr;
    ID3D11BlendState* g_Blend = nullptr; ID3D11RasterizerState* g_Rs = nullptr;
    ID3D11DepthStencilState* g_AtFar[2] = {};   // [0] standard depth (far = 1: passes where depth >= 1), [1] reversed (far = 0)
    struct Ro { ID3D11Resource* tex = nullptr; ID3D11DepthStencilView* dsv = nullptr; unsigned long long used = 0; };
    Ro g_Ro[4];                                 // read-only views of the scenes' depth buffers (none of their SRVs is unbound)
    unsigned long long g_Use = 0;
    std::atomic<unsigned long long> g_Drawn{ 0 }, g_NoTargets{ 0 }, g_NoView{ 0 }, g_NoRo{ 0 }, g_Immediate{ 0 };
    bool g_LoggedFirst = false;
    double g_LastLog = 0; unsigned long long g_LastDrawn = 0;
    float g_MaxPx = 0;                          // the largest motion at the screen's centre since the last log line (pixels)

    struct Cap { ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr; D3D11_VIEWPORT vp{}; int tries = 0; bool on = false; };
    thread_local Cap t_Cap;

    template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    bool ReadBytes(void* dst, const void* src, size_t n)
    {
        __try { memcpy(dst, src, n); return true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }
    double Now() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return (double)c.QuadPart / (double)f.QuadPart; }
    void Drop(Cap& k) { Rel(k.rtv); Rel(k.dsv); k = Cap{}; }

    bool Init(ID3D11Device* dev)
    {
        if (g_Ready || g_Failed) return g_Ready;
        g_Failed = true;
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile) { LOGI("SkyMotion: d3dcompiler_47 not available: the sky keeps no motion vectors"); return false; }
        ID3DBlob* vb = nullptr; ID3DBlob* pb = nullptr; ID3DBlob* err = nullptr;
        auto one = [&](const char* entry, const char* target, ID3DBlob** b) {
            if (SUCCEEDED(compile(kHlsl, strlen(kHlsl), "skymotion", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, b, &err))) { Rel(err); return true; }
            LOGI("SkyMotion: the shader '{}' failed to compile: {}", entry, err ? (const char*)err->GetBufferPointer() : "?");
            Rel(err); return false;
        };
        const bool compiled = one("vs", "vs_5_0", &vb) && one("ps", "ps_5_0", &pb);
        bool ok = compiled && SUCCEEDED(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &g_Vs))
                           && SUCCEEDED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &g_Ps));
        Rel(vb); Rel(pb);
        if (!ok) return false;
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 128; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_BLEND_DESC bl{}; bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = FALSE; rd.ScissorEnable = FALSE;
        D3D11_DEPTH_STENCIL_DESC ds{}; ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; ds.StencilEnable = FALSE;
        ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;   // 1 <= depth: the far value
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_Cb)) && SUCCEEDED(dev->CreateBlendState(&bl, &g_Blend)) && SUCCEEDED(dev->CreateRasterizerState(&rd, &g_Rs))
          && SUCCEEDED(dev->CreateDepthStencilState(&ds, &g_AtFar[0]));
        ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;   // 0 >= depth (reversed depth)
        ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&ds, &g_AtFar[1]));
        if (!ok) { LOGI("SkyMotion: the pipeline states could not be made: the sky keeps no motion vectors"); return false; }
        g_Failed = false; g_Ready = true;
        return true;
    }
    //! a read-only view of the depth buffer `dsv` views, made once per buffer
    ID3D11DepthStencilView* RoOf(ID3D11DepthStencilView* dsv)
    {
        ID3D11Resource* tex = nullptr; dsv->GetResource(&tex);
        if (!tex) return nullptr;
        Ro* slot = nullptr;
        for (Ro& r : g_Ro) if (r.tex == tex) { slot = &r; break; }
        if (!slot)
        {
            slot = &g_Ro[0];
            for (Ro& r : g_Ro) if (r.used < slot->used) slot = &r;
            Rel(slot->dsv); Rel(slot->tex);
            D3D11_DEPTH_STENCIL_VIEW_DESC d{}; dsv->GetDesc(&d);
            const bool stencil = d.Format == DXGI_FORMAT_D24_UNORM_S8_UINT || d.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            d.Flags = D3D11_DSV_READ_ONLY_DEPTH | (stencil ? D3D11_DSV_READ_ONLY_STENCIL : 0);
            ID3D11Device* dev = nullptr; dsv->GetDevice(&dev);
            ID3D11DepthStencilView* ro = nullptr;
            if (!dev || FAILED(dev->CreateDepthStencilView(tex, &d, &ro))) ro = nullptr;
            Rel(dev);
            slot->tex = tex; tex->AddRef(); slot->dsv = ro;
            if (!ro) LOGI("SkyMotion: no read-only view of the scene's depth (format {}): the sky keeps no motion vectors", (int)d.Format);
        }
        slot->used = ++g_Use;
        tex->Release();
        return slot->dsv;
    }

    //! the projection as a column-vector matrix's rows 0, 1, 3 x columns 0, 1, 2 (a point at infinity: direction ->
    //! clip x, y, w), with the TAA jitter (twice the jitter field, in p02 / p12: StereoTaa) taken out
    void Dir3(const float* P, const float* jit, double A[3][3])
    {
        const bool colVec = fabsf(fabsf(P[14]) - 1.f) < 1e-3f;
        static const int rows[3] = { 0, 1, 3 };
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
            A[i][j] = colVec ? P[rows[i] * 4 + j] : P[j * 4 + rows[i]];
        if (jit) { A[0][2] -= 2.0 * jit[0]; A[1][2] -= 2.0 * jit[1]; }
    }
    void Mul3(const double a[3][3], const double b[3][3], double o[3][3])
    {
        double t[3][3];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { t[i][j] = 0; for (int k = 0; k < 3; k++) t[i][j] += a[i][k] * b[k][j]; }
        memcpy(o, t, sizeof(t));
    }
    bool Inv3(const double m[3][3], double o[3][3])
    {
        const double d = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if (!(fabs(d) > 1e-12)) return false;
        o[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) / d; o[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / d; o[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / d;
        o[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) / d; o[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / d; o[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / d;
        o[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) / d; o[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / d; o[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / d;
        return true;
    }

    //! every piece of the context's state the pass touches, put back exactly afterwards
    struct Saved
    {
        ID3D11VertexShader* vs = nullptr; ID3D11Buffer* vsCb = nullptr; ID3D11PixelShader* ps = nullptr; ID3D11Buffer* psCb = nullptr;
        ID3D11GeometryShader* gs = nullptr; ID3D11HullShader* hs = nullptr; ID3D11DomainShader* ds = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED; ID3D11InputLayout* il = nullptr;
        ID3D11RasterizerState* rs = nullptr; D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {}; UINT nvp = 0;
        ID3D11BlendState* bs = nullptr; float bf[4] = {}; UINT mask = 0xFFFFFFFF;
        ID3D11DepthStencilState* dss = nullptr; UINT ref = 0;
        ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {}; ID3D11DepthStencilView* dsv = nullptr;
        void Save(ID3D11DeviceContext* c)
        {
            c->VSGetShader(&vs, nullptr, nullptr); c->VSGetConstantBuffers(0, 1, &vsCb);
            c->PSGetShader(&ps, nullptr, nullptr); c->PSGetConstantBuffers(0, 1, &psCb);
            c->GSGetShader(&gs, nullptr, nullptr); c->HSGetShader(&hs, nullptr, nullptr); c->DSGetShader(&ds, nullptr, nullptr);
            c->IAGetPrimitiveTopology(&topo); c->IAGetInputLayout(&il);
            c->RSGetState(&rs); nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetViewports(&nvp, vp);
            c->OMGetBlendState(&bs, bf, &mask); c->OMGetDepthStencilState(&dss, &ref);
            c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        }
        void Restore(ID3D11DeviceContext* c)
        {
            c->VSSetShader(vs, nullptr, 0); c->VSSetConstantBuffers(0, 1, &vsCb);
            c->PSSetShader(ps, nullptr, 0); c->PSSetConstantBuffers(0, 1, &psCb);
            c->GSSetShader(gs, nullptr, 0); c->HSSetShader(hs, nullptr, 0); c->DSSetShader(ds, nullptr, 0);
            c->IASetPrimitiveTopology(topo); c->IASetInputLayout(il);
            c->RSSetState(rs); c->RSSetViewports(nvp, nvp ? vp : nullptr);
            c->OMSetBlendState(bs, bf, mask); c->OMSetDepthStencilState(dss, ref);
            c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
            Rel(vs); Rel(vsCb); Rel(ps); Rel(psCb); Rel(gs); Rel(hs); Rel(ds); Rel(il); Rel(rs); Rel(bs); Rel(dss);
            for (auto*& p : rtv) Rel(p); Rel(dsv);
        }
    };
}

void SkyMotion::SetEnabled(bool on)
{
    if (g_On.exchange(on) != on || on)
        LOGI("SkyMotion: TaaSkyMotion {}", on ? "on: with the game's TAA, the sky (no geometry in the depth pass) is given the motion of a point at infinity as its motion vectors, so the TAA keeps it in place as the head turns"
                                             : "off: the sky keeps no motion vectors (the TAA drags it a little with the head)");
}
bool SkyMotion::Wanted() { return g_On.load(std::memory_order_relaxed); }

bool SkyMotion::Begin(ID3D11DeviceContext* c)
{
    if (t_Cap.rtv || t_Cap.dsv) Drop(t_Cap);
    t_Cap = Cap{};
    if (!c || !g_On.load(std::memory_order_relaxed) || !StereoTaa::TaaInUse()) return false;
    if (c->GetType() != D3D11_DEVICE_CONTEXT_DEFERRED) { g_Immediate++; return false; }   // only on a task's own context
    t_Cap.on = true;
    return true;
}

bool SkyMotion::AtDraw(ID3D11DeviceContext* c)
{
    if (!t_Cap.on || !c) return true;
    ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(1, &rtv, &dsv);
    bool ok = false;
    if (rtv && dsv)
    {
        D3D11_RENDER_TARGET_VIEW_DESC rd{}; rtv->GetDesc(&rd);
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dsv->GetDesc(&dd);
        if (rd.Format == DXGI_FORMAT_R16G16_FLOAT && rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D && dd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D)
        {
            ID3D11Resource* a = nullptr; ID3D11Resource* b = nullptr; rtv->GetResource(&a); dsv->GetResource(&b);
            D3D11_TEXTURE2D_DESC ta{}, tb{};
            if (a) static_cast<ID3D11Texture2D*>(a)->GetDesc(&ta);
            if (b) static_cast<ID3D11Texture2D*>(b)->GetDesc(&tb);
            ok = ta.Width && ta.Width == tb.Width && ta.Height == tb.Height && ta.SampleDesc.Count == 1 && tb.SampleDesc.Count == 1;
            Rel(a); Rel(b);
        }
    }
    if (ok)
    {
        UINT n = 1; D3D11_VIEWPORT vp{}; c->RSGetViewports(&n, &vp);
        t_Cap.rtv = rtv; t_Cap.dsv = dsv; t_Cap.vp = vp; t_Cap.on = false;
        return true;
    }
    Rel(rtv); Rel(dsv);
    if (++t_Cap.tries >= 256) { t_Cap.on = false; return true; }
    return false;
}

void SkyMotion::End(ID3D11DeviceContext* c, const void* view)
{
    Cap k = t_Cap; t_Cap = Cap{};
    if (!k.rtv || !k.dsv) { Drop(k); g_NoTargets++; return; }
    if (!c || !view) { Drop(k); g_NoView++; return; }
    float P[16], V[16], Pp[16], Vp[16], cj[2], pj[2];
    const uint8_t* v = (const uint8_t*)view;
    if (!ReadBytes(P, v + kProj, 64) || !ReadBytes(V, v + kView, 64) || !ReadBytes(Pp, v + kPrevProj, 64) || !ReadBytes(Vp, v + kPrevView, 64)
        || !ReadBytes(cj, v + kCurJitter, 8) || !ReadBytes(pj, v + kPrevJitter, 8))
    { Drop(k); g_NoView++; return; }
    // the pixel's NDC -> view direction (this frame's projection, with its jitter: the picture is rendered jittered) ->
    // world -> the previous view -> each projection without its jitter
    double A[3][3], Ai[3][3], Ac[3][3], Ap[3][3], Rc[3][3], RpRcT[3][3], Tc[3][3], Tp[3][3];
    Dir3(P, nullptr, A); Dir3(P, cj, Ac); Dir3(Pp, pj, Ap);
    if (!Inv3(A, Ai)) { Drop(k); g_NoView++; return; }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) Rc[i][j] = V[i * 4 + j];   // rows: the view's axes in the world
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
    { double s = 0; for (int m = 0; m < 3; m++) s += (double)Vp[i * 4 + m] * Rc[j][m]; RpRcT[i][j] = s; }
    Mul3(Ac, Ai, Tc);
    Mul3(RpRcT, Ai, Tp); Mul3(Ap, Tp, Tp);
    const bool colVec = fabsf(fabsf(P[14]) - 1.f) < 1e-3f;
    const float s = colVec ? P[14] : P[11];
    const int reversed = std::clamp(P[10] * s, 0.f, 1.f) > 0.5f ? 0 : 1;   // the depth at infinity: 1 standard, 0 reversed
    if (!(k.vp.Width > 0) || !(k.vp.Height > 0))
    {   // no viewport noted: the whole target
        ID3D11Resource* r = nullptr; k.rtv->GetResource(&r); D3D11_TEXTURE2D_DESC td{};
        if (r) static_cast<ID3D11Texture2D*>(r)->GetDesc(&td);
        Rel(r);
        k.vp = D3D11_VIEWPORT{ 0, 0, (float)td.Width, (float)td.Height, 0, 1 };
    }
    struct { float Tc[12]; float Tp[12]; float vp[4]; float z[4]; } cb{};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { cb.Tc[i * 4 + j] = (float)Tc[i][j]; cb.Tp[i * 4 + j] = (float)Tp[i][j]; }
    cb.vp[0] = k.vp.TopLeftX; cb.vp[1] = k.vp.TopLeftY; cb.vp[2] = 2.f / k.vp.Width; cb.vp[3] = 2.f / k.vp.Height;
    cb.z[0] = reversed ? 0.f : 1.f;

    std::lock_guard lk(g_M);
    if (!g_Ready) { ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (dev) { Init(dev); dev->Release(); } }
    ID3D11DepthStencilView* ro = g_Ready ? RoOf(k.dsv) : nullptr;
    if (!ro) { Drop(k); g_NoRo++; return; }
    Saved sv; sv.Save(c);
    c->UpdateSubresource(g_Cb, 0, nullptr, &cb, 0, 0);
    D3D11_VIEWPORT vp = k.vp; vp.MinDepth = 0; vp.MaxDepth = 1;
    c->VSSetShader(g_Vs, nullptr, 0); c->VSSetConstantBuffers(0, 1, &g_Cb);
    c->PSSetShader(g_Ps, nullptr, 0); c->PSSetConstantBuffers(0, 1, &g_Cb);
    c->GSSetShader(nullptr, nullptr, 0); c->HSSetShader(nullptr, nullptr, 0); c->DSSetShader(nullptr, nullptr, 0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); c->IASetInputLayout(nullptr);
    c->RSSetState(g_Rs); c->RSSetViewports(1, &vp);
    c->OMSetBlendState(g_Blend, nullptr, 0xFFFFFFFF); c->OMSetDepthStencilState(g_AtFar[reversed], 0);
    c->OMSetRenderTargets(1, &k.rtv, ro);
    c->Draw(3, 0);
    sv.Restore(c);
    const unsigned long long drawn = ++g_Drawn;
    if (::Log::Enabled() || !g_LoggedFirst)
    {   // the motion at the screen's centre, in pixels (the log only)
        const double h[3] = { 0, 0, 1 };
        double cc[3] = {}, pp[3] = {};
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { cc[i] += Tc[i][j] * h[j]; pp[i] += Tp[i][j] * h[j]; }
        float px = 0;
        if (cc[2] > 1e-6 && pp[2] > 1e-6)
            px = (float)std::hypot((cc[0] / cc[2] - pp[0] / pp[2]) * 0.5 * k.vp.Width, (cc[1] / cc[2] - pp[1] / pp[2]) * 0.5 * k.vp.Height);
        g_MaxPx = std::max(g_MaxPx, px);
        const double now = Now();
        if (!g_LoggedFirst)
        {
            g_LoggedFirst = true; g_LastLog = now; g_LastDrawn = drawn;
            LOGI("SkyMotion: the sky's motion vectors drawn for the first time ({}x{} viewport, {} depth; the screen centre moved {:.2f} px since this eye's last frame)",
                 (int)k.vp.Width, (int)k.vp.Height, reversed ? "reversed" : "standard", px);
        }
        else if (now - g_LastLog >= 30.0)
        {
            LOGI("SkyMotion: the sky's motion vectors drawn {} times in {:.0f} s (largest motion at the screen centre {:.1f} px); not drawn: targets not seen {}, view unread {}, no depth view {}, immediate context {}",
                 drawn - g_LastDrawn, now - g_LastLog, g_MaxPx, g_NoTargets.load(), g_NoView.load(), g_NoRo.load(), g_Immediate.load());
            g_LastLog = now; g_LastDrawn = drawn; g_MaxPx = 0;
        }
    }
    Drop(k);
}

std::string SkyMotion::Status()
{
    return fmt::format("skymotion on {} drawn {} notargets {} noview {} nodepthview {} immediate {}", g_On.load() ? 1 : 0, g_Drawn.load(),
                       g_NoTargets.load(), g_NoView.load(), g_NoRo.load(), g_Immediate.load());
}
