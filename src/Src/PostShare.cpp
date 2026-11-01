#include "pch.h"
#include "PostShare.h"
#include "GpuTrace.h"
#include <d3d11.h>
#include <d3d11shader.h>
#include <atomic>
#include <mutex>
#include <vector>

namespace
{
    constexpr uint64_t kPsMask = 0x075df32c84f09477ull, kPsBlur = 0x2d11c3a17106ea42ull, kPsCompose = 0xd18f70002e2ca13aull;
    constexpr uint64_t kCsHist = 0xdc6d8bbce02a1266ull, kCsHistMerge = 0x9ba51c0d902365b7ull, kCsAdapt = 0x577946c9f0ee49c9ull;

    std::atomic<bool> g_Outlines{ true }, g_Exposure{ true };
    std::atomic<unsigned long long> g_OutPred{ 0 }, g_OutNoMask{ 0 }, g_ExpSkipped{ 0 }, g_ExpCopies{ 0 }, g_ExpShared{ 0 };

    struct Task
    {
        ID3D11DeviceContext* c = nullptr; int what = 0, eye = -1;
        bool begun = false, ended = false, predicated = false;
        bool skipHist = false;                 // this right eye's histogram is not built
    };
    thread_local Task t_Stack[4];
    thread_local int t_Depth = 0;
    Task* Cur() { return t_Depth > 0 && t_Depth <= 4 ? &t_Stack[t_Depth - 1] : nullptr; }

    uint64_t HashOf(const void* sh)
    {   // the last one asked for, per thread (a pass binds the same shader for several draws)
        thread_local const void* last = nullptr; thread_local uint64_t lastHash = 0;
        if (sh != last) { last = sh; lastHash = sh ? GpuTrace::ShaderHashOf(sh) : 0; }
        return sh ? lastHash : 0;
    }

    // ---- outlines: one occlusion predicate per eye ----
    std::mutex g_PredM;
    ID3D11Predicate* g_Pred[2] = {};
    ID3D11Predicate* PredFor(ID3D11DeviceContext* c, int eye)
    {
        std::lock_guard lk(g_PredM);
        if (g_Pred[eye]) return g_Pred[eye];
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        if (!dev) return nullptr;
        D3D11_QUERY_DESC qd = { D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
        if (FAILED(dev->CreatePredicate(&qd, &g_Pred[eye]))) g_Pred[eye] = nullptr;
        dev->Release();
        if (g_Pred[eye] && eye == 0) LOGI("PostShare: SkipEmptyOutlines: the outline effect's full-screen passes are predicated on its mask getting pixels");
        return g_Pred[eye];
    }

    // ---- exposure: the left eye's histogram (the resource its adaptation read) ----
    std::mutex g_ExpM;
    ID3D11Resource* g_LeftHist = nullptr;       // referenced
    ULONGLONG g_LeftAt = 0;
    ID3D11Resource* g_RightHist = nullptr;      // the right eye's own (not referenced: compared only), once seen
    bool g_RightOk = false, g_Bad = false;

    //! the shader-resource slot the adaptation shader reads the histogram from (its reflection: the one texture /
    //! buffer it reads, ro_postfx_luminance_texhistogram), -1 = unknown
    int FindHistSlot(const void* cs)
    {
        int slot = -1;
        std::vector<uint8_t> code;
        using Reflect_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
        Reflect_t reflect = nullptr;
        if (HMODULE d = LoadLibraryA("d3dcompiler_47.dll")) reflect = (Reflect_t)GetProcAddress(d, "D3DReflect");
        ID3D11ShaderReflection* r = nullptr;
        if (reflect && GpuTrace::ShaderBytecode(cs, code) && code.size() > 32 && SUCCEEDED(reflect(code.data(), code.size(), __uuidof(ID3D11ShaderReflection), (void**)&r)) && r)
        {
            D3D11_SHADER_DESC sd{};
            if (SUCCEEDED(r->GetDesc(&sd)))
                for (UINT i = 0; i < sd.BoundResources; i++)
                {
                    D3D11_SHADER_INPUT_BIND_DESC b{};
                    if (FAILED(r->GetResourceBindingDesc(i, &b)) || !b.Name) continue;
                    if (strstr(b.Name, "texhistogram") && (b.Type == D3D_SIT_TEXTURE || b.Type == D3D_SIT_STRUCTURED || b.Type == D3D_SIT_BYTEADDRESS || b.Type == D3D_SIT_TBUFFER))
                    { slot = (int)b.BindPoint; break; }
                }
            r->Release();
        }
        LOGI("PostShare: StereoExposureReuse: the exposure adaptation reads the histogram at shader resource slot {}{}", slot, slot < 0 ? " (not found: left alone)" : "");
        return slot;
    }
    int HistSlot(const void* cs)
    {
        static std::once_flag once; static int slot = -1;
        std::call_once(once, [&] { slot = FindHistSlot(cs); });
        return slot;
    }
    ID3D11Resource* AdaptInput(ID3D11DeviceContext* c, const void* cs)
    {
        const int s = HistSlot(cs);
        if (s < 0 || s >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) return nullptr;
        ID3D11ShaderResourceView* v = nullptr;
        c->CSGetShaderResources((UINT)s, 1, &v);
        ID3D11Resource* r = nullptr;
        if (v) { v->GetResource(&r); v->Release(); }
        return r;   // referenced
    }
    bool SameShape(ID3D11Resource* a, ID3D11Resource* b)
    {
        D3D11_RESOURCE_DIMENSION da, db; a->GetType(&da); b->GetType(&db);
        if (da != db) return false;
        if (da == D3D11_RESOURCE_DIMENSION_BUFFER)
        {
            D3D11_BUFFER_DESC x, y; ((ID3D11Buffer*)a)->GetDesc(&x); ((ID3D11Buffer*)b)->GetDesc(&y);
            return x.ByteWidth == y.ByteWidth;
        }
        if (da == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
        {
            D3D11_TEXTURE2D_DESC x, y; ((ID3D11Texture2D*)a)->GetDesc(&x); ((ID3D11Texture2D*)b)->GetDesc(&y);
            return x.Width == y.Width && x.Height == y.Height && x.Format == y.Format && x.MipLevels == y.MipLevels && x.ArraySize == y.ArraySize;
        }
        if (da == D3D11_RESOURCE_DIMENSION_TEXTURE1D)
        {
            D3D11_TEXTURE1D_DESC x, y; ((ID3D11Texture1D*)a)->GetDesc(&x); ((ID3D11Texture1D*)b)->GetDesc(&y);
            return x.Width == y.Width && x.Format == y.Format && x.MipLevels == y.MipLevels && x.ArraySize == y.ArraySize;
        }
        return false;
    }
}

void PostShare::SetOutlines(bool on) { g_Outlines = on; LOGI("PostShare: SkipEmptyOutlines {}", on ? "on" : "off"); }
void PostShare::SetExposure(bool on) { g_Exposure = on; LOGI("PostShare: StereoExposureReuse {}", on ? "on (the right eye adapts from the left eye's histogram)" : "off"); }
bool PostShare::OutlinesOn() { return g_Outlines.load(std::memory_order_relaxed); }
bool PostShare::ExposureOn() { return g_Exposure.load(std::memory_order_relaxed); }
bool PostShare::Active() { return t_Depth > 0; }

void PostShare::Enter(ID3D11DeviceContext* c, int what, int eye)
{
    if (t_Depth >= 4) { t_Depth++; return; }
    Task& t = t_Stack[t_Depth++];
    t = Task{}; t.c = c; t.eye = eye;
    t.what = (what == 1 && g_Outlines.load(std::memory_order_relaxed)) || (what == 2 && g_Exposure.load(std::memory_order_relaxed)) ? what : 0;
    if (t.what == 2 && eye == 1)
    {   // the right eye skips its histogram once the left's is known (recent) and the right's own was seen and fits
        std::lock_guard lk(g_ExpM);
        t.skipHist = !g_Bad && g_RightOk && g_LeftHist && GetTickCount64() - g_LeftAt < 500;
    }
}

void PostShare::Leave(ID3D11DeviceContext* c)
{
    static std::atomic<ULONGLONG> nextLog{ 0 };
    const ULONGLONG now = GetTickCount64(); ULONGLONG due = nextLog.load();
    if (now >= due && nextLog.compare_exchange_strong(due, now + 60000)) { if (due) LOGI("PostShare: {}", Status()); }
    if (t_Depth <= 0) return;
    if (t_Depth > 4) { t_Depth--; return; }
    Task& t = t_Stack[t_Depth - 1];
    if (t.c && t.what == 1)
    {
        if (t.predicated) t.c->SetPredication(nullptr, FALSE);
        else if (t.begun && !t.ended) t.c->End(PredFor(t.c, t.eye));
    }
    t_Depth--;
}

bool PostShare::AtDraw(ID3D11DeviceContext* c)
{
    Task* t = Cur();
    if (!t || t->what != 1 || c != t->c || t->eye < 0 || t->eye > 1) return false;
    ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr);
    const uint64_t h = HashOf(ps);
    if (ps) ps->Release();
    if (h == kPsMask)
    {   // an object to outline: counted (a new group after a finished one starts again)
        ID3D11Predicate* p = PredFor(c, t->eye);
        if (!p) return false;
        if (t->predicated) { c->SetPredication(nullptr, FALSE); t->predicated = false; }
        if (!t->begun || t->ended) { c->Begin(p); t->begun = true; t->ended = false; }
        return false;
    }
    if (h == kPsBlur || h == kPsCompose)
    {
        if (!t->begun) { g_OutNoMask.fetch_add(1, std::memory_order_relaxed); return true; }   // nothing drawn into the mask
        ID3D11Predicate* p = PredFor(c, t->eye);
        if (!p) return false;
        if (!t->ended) { c->End(p); t->ended = true; }
        if (!t->predicated) { c->SetPredication(p, FALSE); t->predicated = true; g_OutPred.fetch_add(1, std::memory_order_relaxed); }
        return false;
    }
    if (t->predicated) { c->SetPredication(nullptr, FALSE); t->predicated = false; }   // anything else: never predicated
    return false;
}

bool PostShare::AtDispatch(ID3D11DeviceContext* c)
{
    Task* t = Cur();
    if (!t || t->what != 2 || c != t->c || t->eye < 0 || t->eye > 1) return false;
    ID3D11ComputeShader* cs = nullptr; c->CSGetShader(&cs, nullptr, nullptr);
    const uint64_t h = HashOf(cs);
    if (h != kCsAdapt) { if (cs) cs->Release(); }
    if (t->eye == 1 && t->skipHist && (h == kCsHist || h == kCsHistMerge)) { g_ExpSkipped.fetch_add(1, std::memory_order_relaxed); return true; }
    if (h != kCsAdapt) return false;
    ID3D11Resource* in = AdaptInput(c, cs);
    if (cs) cs->Release();
    if (!in) return false;
    std::lock_guard lk(g_ExpM);
    if (t->eye == 0)
    {
        if (g_LeftHist != in) { if (g_LeftHist) g_LeftHist->Release(); g_LeftHist = in; in->AddRef(); }
        g_LeftAt = GetTickCount64();
    }
    else if (!t->skipHist)
    {   // learn the right eye's: the same resource (nothing to copy) or one of the same shape (copied into)
        if (g_LeftHist && !g_Bad)
        {
            if (in == g_LeftHist || SameShape(in, g_LeftHist)) { if (!g_RightOk) LOGI("PostShare: StereoExposureReuse: the eyes' histograms {}", in == g_LeftHist ? "are one resource" : "are separate: the left's is copied into the right's"); g_RightHist = in; g_RightOk = true; }
            else { g_Bad = true; LOGI("PostShare: StereoExposureReuse: the right eye's histogram is not shaped like the left's: left alone"); }
        }
    }
    else if (in == g_LeftHist) g_ExpShared.fetch_add(1, std::memory_order_relaxed);
    else if (SameShape(in, g_LeftHist)) { c->CopyResource(in, g_LeftHist); g_ExpCopies.fetch_add(1, std::memory_order_relaxed); }
    else { g_Bad = true; g_RightOk = false; LOGI("PostShare: StereoExposureReuse: the right eye's histogram changed shape: left alone from now on"); }
    in->Release();
    return false;
}

std::string PostShare::Status()
{
    return fmt::format("postshare outlines {} predicated {} no-mask skips {} | exposure {} right histograms skipped {} copied {} shared {}{}",
                       g_Outlines.load() ? "on" : "off", g_OutPred.load(), g_OutNoMask.load(), g_Exposure.load() ? "on" : "off",
                       g_ExpSkipped.load(), g_ExpCopies.load(), g_ExpShared.load(), g_Bad ? " (disabled: shapes differ)" : "");
}
