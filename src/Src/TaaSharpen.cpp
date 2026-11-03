#include "pch.h"
#include "TaaSharpen.h"
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <mutex>
#include <atomic>
#include <algorithm>
#include "Settings.h"
#include "VRRuntime.h"
#include "ScreenMode.h"
#include <cmath>
#include <string>

namespace
{
    // ---- the filter ----
    // AMD FidelityFX CAS (MIT), the no-scaling path: a 5-tap sharpen whose negative lobe is weighted per pixel by how much
    // headroom the 3x3 neighbourhood leaves (soft min / soft max). The TAA's result is linear HDR; CAS is made for
    // gamma-encoded values, so it runs on sqrt(colour) (and squares the result): in linear light the min / max ratio of a
    // texture is far smaller and CAS all but switched itself off. The headroom is the ratio min / max only (HDR: for values
    // within 0..1 that is CAS's own min(mn, 2 - mx) / mx); no clamp to the 3x3's range (v1 had one: it took away the very
    // step past the neighbours that a sharpen is). v1 showed no difference at 0 / 0.5 / 1 (2026-11-03).
    const char* kHlsl = R"(
cbuffer C : register(b0) { float4 g; }   // x: sharpness 0..1, y: the neighbours' distance in pixels, zw: 1 / size
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState smp : register(s0);
float3 S(float2 uv) { return sqrt(max(src.SampleLevel(smp, uv, 0).rgb, 0.0)); }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint w, h; src.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const int2 p = int2(id.xy);
    const float2 uv = (float2(id.xy) + 0.5) * g.zw, k = g.y * g.zw;
    const float3 a = S(uv + float2(-k.x, -k.y)), b = S(uv + float2(0, -k.y)), c = S(uv + float2(k.x, -k.y));
    const float3 d = S(uv + float2(-k.x, 0)),    e = sqrt(max(src.Load(int3(p, 0)).rgb, 0.0)), f = S(uv + float2(k.x, 0));
    const float3 s = S(uv + float2(-k.x, k.y)),  t = S(uv + float2(0, k.y)),  u = S(uv + float2(k.x, k.y));
    const float3 mnCross = min(min(min(d, e), min(f, b)), t), mxCross = max(max(max(d, e), max(f, b)), t);
    const float3 mnAll = min(mnCross, min(min(a, c), min(s, u))), mxAll = max(mxCross, max(max(a, c), max(s, u)));
    const float3 amp = sqrt(saturate((mnCross + mnAll) / max(mxCross + mxAll, 1e-6)));
    const float3 wt = amp * (-1.0 / lerp(8.0, 5.0, saturate(g.x)));
    const float3 o = ((b + d + f + t) * wt + e) / (1.0 + 4.0 * wt);
    const float3 q = max(o, 0.0);
    dst[uint2(id.xy)] = float4(q * q, src.Load(int3(p, 0)).a);
}
)";

    template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    std::mutex g_M;                                       // everything below but the atomics
    ID3D11ComputeShader* g_Cs = nullptr; ID3D11Buffer* g_Cb = nullptr;
    // the texture the TAA writes its result into (its rw_taaresult view swapped for the mod's), read by the filter, which
    // writes the engine's own result texture: no copy of the result (before 2026-11-03 the result was copied out first)
    ID3D11Texture2D* g_Tmp = nullptr; ID3D11ShaderResourceView* g_Srv = nullptr; ID3D11UnorderedAccessView* g_Uav = nullptr;
    D3D11_TEXTURE2D_DESC g_Have{}; DXGI_FORMAT g_HaveView = DXGI_FORMAT_UNKNOWN;
    D3D11_TEXTURE2D_DESC g_Want{}; DXGI_FORMAT g_WantView = DXGI_FORMAT_UNKNOWN; bool g_WantValid = false;
    D3D11_UNORDERED_ACCESS_VIEW_DESC g_WantUav{};
    bool g_Failed = false; float g_CbValue = -1.f, g_CbRadius = -1.f; UINT g_CbW = 0, g_CbH = 0;
    ID3D11SamplerState* g_Smp = nullptr;
    std::atomic<bool> g_On{ false };                      // the setting is on and the headset's stereo view is up
    std::atomic<unsigned long long> g_Done{ 0 }, g_Waiting{ 0 };
    bool Same(const D3D11_TEXTURE2D_DESC& x, const D3D11_TEXTURE2D_DESC& y)
    { return x.Width == y.Width && x.Height == y.Height && x.Format == y.Format && x.MipLevels == y.MipLevels; }

    bool Compile(ID3D11Device* dev)
    {
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto compile = dc ? (CompileFn)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile) { LOGI("TaaSharpen: d3dcompiler_47 not available: no sharpening"); return false; }
        ID3DBlob* b = nullptr; ID3DBlob* err = nullptr;
        if (FAILED(compile(kHlsl, strlen(kHlsl), "taasharpen", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err)) || !b)
        {
            LOGI("TaaSharpen: the shader failed to compile: {}", err ? (const char*)err->GetBufferPointer() : "?");
            if (err) err->Release(); return false;
        }
        if (err) err->Release();
        const bool ok = SUCCEEDED(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_Cs)) && g_Cs;
        b->Release();
        if (!ok) { LOGI("TaaSharpen: CreateComputeShader failed: no sharpening"); return false; }
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_Cb)) || !g_Cb) { Rel(g_Cs); LOGI("TaaSharpen: no constant buffer: no sharpening"); return false; }
        D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        if (FAILED(dev->CreateSamplerState(&sd, &g_Smp)) || !g_Smp) { Rel(g_Cs); Rel(g_Cb); LOGI("TaaSharpen: no sampler: no sharpening"); return false; }
        return true;
    }
}

void TaaSharpen::OnPresent(IDXGISwapChain* sc)
{
    const float s = std::clamp(Settings::TaaSharpening(), 0.f, 1.f);
    if (!(s > 0.f) || !VRRuntime::Stereo() || ScreenMode::FlatRender()) { g_On = false; return; }
    ID3D11Device* dev = nullptr;
    if (!sc || FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) { g_On = false; return; }
    std::lock_guard lk(g_M);
    if (!g_Cs && !g_Failed && !Compile(dev)) g_Failed = true;
    // the neighbours one render pixel away, as CAS has them (the taps land on pixel centres: exact)
    const float radius = 1.f;
    if (g_Cs && g_Cb && (s != g_CbValue || fabsf(radius - g_CbRadius) > 0.05f || g_Have.Width != g_CbW || g_Have.Height != g_CbH))
    {
        const float v[4] = { s, radius, g_Have.Width ? 1.f / g_Have.Width : 0.f, g_Have.Height ? 1.f / g_Have.Height : 0.f };
        ID3D11DeviceContext* imm = nullptr; dev->GetImmediateContext(&imm);
        if (imm)
        {
            imm->UpdateSubresource(g_Cb, 0, nullptr, v, 0, 0); imm->Release();
            g_CbValue = s; g_CbRadius = radius; g_CbW = g_Have.Width; g_CbH = g_Have.Height;
            LOGI("TaaSharpen: strength {:.2f} ({}x{})", s, g_Have.Width, g_Have.Height);
        }
    }
    if (g_Cs && g_WantValid && (!g_Tmp || !Same(g_Want, g_Have) || g_WantView != g_HaveView))
    {   // the TAA's result texture of the mod's: made here, never on a recording thread (its size changes with RenderScale)
        Rel(g_Uav); Rel(g_Srv); Rel(g_Tmp);
        D3D11_TEXTURE2D_DESC d = g_Want;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS; d.CPUAccessFlags = 0; d.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{}; vd.Format = g_WantView; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
        if (SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_Tmp)) && g_Tmp && SUCCEEDED(dev->CreateShaderResourceView(g_Tmp, &vd, &g_Srv)) && g_Srv
            && SUCCEEDED(dev->CreateUnorderedAccessView(g_Tmp, &g_WantUav, &g_Uav)) && g_Uav)
        {
            g_Have = g_Want; g_HaveView = g_WantView;
            LOGI("TaaSharpen: sharpening the TAA's result, {}x{} format {} (strength {:.2f})", d.Width, d.Height, (int)g_WantView, s);
        }
        else { Rel(g_Uav); Rel(g_Srv); Rel(g_Tmp); g_WantValid = false; LOGI("TaaSharpen: could not make the result texture ({}x{} format {})", d.Width, d.Height, (int)d.Format); }
    }
    g_On = g_Cs && g_Cb && g_Smp;
    dev->Release();
}

bool TaaSharpen::BeforeTaa(ID3D11DeviceContext* c, int resultSlot, int historySlot, ID3D11UnorderedAccessView** engineResult, ID3D11ShaderResourceView** taaOut)
{
    *engineResult = nullptr; *taaOut = nullptr;
    if (!g_On.load(std::memory_order_relaxed) || !c || resultSlot < 0 || resultSlot >= 8) return false;
    ID3D11UnorderedAccessView* game = nullptr; c->CSGetUnorderedAccessViews(resultSlot, 1, &game);
    if (!game) return false;
    ID3D11Resource* r = nullptr; game->GetResource(&r);
    ID3D11Resource* h = nullptr;
    if (historySlot >= 0 && historySlot < 8) { ID3D11UnorderedAccessView* hv = nullptr; c->CSGetUnorderedAccessViews(historySlot, 1, &hv); if (hv) { hv->GetResource(&h); hv->Release(); } }
    D3D11_RESOURCE_DIMENSION dim{}; if (r) r->GetType(&dim);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{}; game->GetDesc(&ud);
    D3D11_TEXTURE2D_DESC d{};
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ((ID3D11Texture2D*)r)->GetDesc(&d);
    const bool plain = r && r != h && dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && ud.ViewDimension == D3D11_UAV_DIMENSION_TEXTURE2D && d.ArraySize == 1 && d.SampleDesc.Count == 1;
    if (r) r->Release();
    if (h) h->Release();
    // the history is never sharpened (it would compound), nor anything but a plain 2D texture
    if (!plain) { game->Release(); return false; }
    ID3D11UnorderedAccessView* mine = nullptr;
    {
        std::lock_guard lk(g_M);
        if (!g_Cs || !g_Cb || !g_Smp || !g_Tmp || !g_Srv || !g_Uav || !Same(d, g_Have) || ud.Format != g_HaveView || g_CbW != d.Width || g_CbH != d.Height)
        {   // made at the next Present; this frame's result goes where the engine has it
            g_Want = d; g_WantView = ud.Format; g_WantUav = ud; g_WantValid = true; g_Waiting++;
            game->Release(); return false;
        }
        mine = g_Uav; *taaOut = g_Srv; g_Srv->AddRef();
    }
    const UINT keep = (UINT)-1;
    c->CSSetUnorderedAccessViews(resultSlot, 1, &mine, &keep);   // the context holds it while bound
    *engineResult = game;
    return true;
}

void TaaSharpen::AfterTaa(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* engineResult, ID3D11ShaderResourceView* taaOut)
{
    if (!c || !engineResult || !taaOut) return;
    ID3D11Resource* r = nullptr; engineResult->GetResource(&r);
    if (!r) return;
    D3D11_TEXTURE2D_DESC d{}; ((ID3D11Texture2D*)r)->GetDesc(&d);
    ID3D11ComputeShader* cs = nullptr; ID3D11Buffer* cb = nullptr; ID3D11SamplerState* smp = nullptr;
    {
        std::lock_guard lk(g_M);
        if (g_Cs && g_Cb && g_Smp) { cs = g_Cs; cs->AddRef(); cb = g_Cb; cb->AddRef(); smp = g_Smp; smp->AddRef(); }
    }
    if (!cs)
    {   // (cannot happen while BeforeTaa swapped: the result must still reach the engine's texture)
        ID3D11Resource* src = nullptr; taaOut->GetResource(&src);
        if (src) { c->CopyResource(r, src); src->Release(); }
        r->Release(); return;
    }
    ID3D11ComputeShader* sCs = nullptr; ID3D11Buffer* sCb = nullptr; ID3D11ShaderResourceView* sSrv = nullptr; ID3D11UnorderedAccessView* sUav[8] = {};
    ID3D11SamplerState* sSmp = nullptr;
    c->CSGetShader(&sCs, nullptr, nullptr); c->CSGetConstantBuffers(0, 1, &sCb); c->CSGetShaderResources(0, 1, &sSrv); c->CSGetUnorderedAccessViews(0, 8, sUav);
    c->CSGetSamplers(0, 1, &sSmp);
    {
        ID3D11UnorderedAccessView* u[8] = { engineResult };   // the result alone: it is also bound at the TAA's own slot
        c->CSSetUnorderedAccessViews(0, 8, u, nullptr);
        c->CSSetShaderResources(0, 1, &taaOut);
        c->CSSetConstantBuffers(0, 1, &cb);
        c->CSSetSamplers(0, 1, &smp);
        c->CSSetShader(cs, nullptr, 0);
        c->Dispatch((d.Width + 7) / 8, (d.Height + 7) / 8, 1);
    }
    const UINT keep[8] = { (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1 };
    c->CSSetShader(sCs, nullptr, 0); c->CSSetConstantBuffers(0, 1, &sCb); c->CSSetShaderResources(0, 1, &sSrv); c->CSSetUnorderedAccessViews(0, 8, sUav, keep);
    c->CSSetSamplers(0, 1, &sSmp);
    Rel(sCs); Rel(sCb); Rel(sSrv); Rel(sSmp); for (auto*& p : sUav) Rel(p);
    Rel(cs); Rel(cb); Rel(smp); r->Release();
    g_Done++;
}

std::string TaaSharpen::Status()
{
    std::lock_guard lk(g_M);
    return fmt::format("taasharpen on {} strength {:.2f} | sharpened {} waiting {}", g_On.load() ? 1 : 0, g_CbValue, g_Done.load(), g_Waiting.load());
}
