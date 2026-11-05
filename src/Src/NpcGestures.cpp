#include "pch.h"
#include "NpcGestures.h"
#include "BodyHide.h"
#include "Collision.h"
#include "GameInfo.h"
#include "GpuTrace.h"
#include "GrabHand.h"
#include "GunRay.h"
#include "HandAimHook.h"
#include "KbmInject.h"
#include "NpcSkel.h"
#include "PlayerState.h"
#include "SwordContact.h"
#include "ViewMatrixHook.h"
#include "VRRuntime.h"
#include <atomic>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr uint32_t kGamePtr = 0x39A5290, kEntTable = 0x3130B0, kEntSlots = 0x4000;
    constexpr size_t kEntId = 0xA8, kEntFlagsD1 = 0xD1, kEntOrigin = 0x300, kEntAxis = 0x30C, kEntHidden = 0xD0;
    constexpr float kScanR = 10.f;
    // a weapon held on someone
    constexpr float kAimR = 0.22f, kAimMax = 5.f, kBladeR = 0.40f, kBladeMax = 1.5f;
    constexpr double kAimHold = 0.7;
    // touches
    constexpr float kTapR = 0.12f;

    std::atomic<unsigned long long> g_Threats{ 0 }, g_Taps{ 0 };

    struct Person { void* ent = nullptr; uint32_t id = 0; float org[3] = {}; float fwd[3] = {}; };
    std::vector<Person> g_People; double g_ScanAt = -1;

    double Now() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
    float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    float Len(const float* a) { return sqrtf(Dot(a, a)); }
    bool Rd(void* d, const void* s, size_t n) { __try { memcpy(d, s, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
    float RayDist(const float o[3], const float f[3], const float p[3], float* along)
    {
        const float d[3] = { p[0] - o[0], p[1] - o[1], p[2] - o[2] };
        const float t = Dot(d, f); *along = t;
        if (t < 0.f) return Len(d);
        const float q[3] = { d[0] - f[0] * t, d[1] - f[1] * t, d[2] - f[2] * t };
        return Len(q);
    }
    //! alive, on his feet, his pose read: origin, forward
    bool Read(void* e, Person& p)
    {
        uint8_t d1 = 0, hid = 0; float ax[9];
        if (!Rd(&d1, (uint8_t*)e + kEntFlagsD1, 1) || (d1 & 0x28) || !Rd(&hid, (uint8_t*)e + kEntHidden, 1) || (hid & 1)) return false;
        if (!Rd(p.org, (uint8_t*)e + kEntOrigin, 12) || !Rd(ax, (uint8_t*)e + kEntAxis, 36) || !Rd(&p.id, (uint8_t*)e + kEntId, 4)) return false;
        p.ent = e; memcpy(p.fwd, ax, 12);
        return std::isfinite(p.org[0] + p.org[1] + p.org[2]);
    }
    void Scan(const float eye[3])
    {
        g_People.clear();
        uint8_t* game = nullptr;
        if (!Rd(&game, (void*)(GameInfo::Base() + kGamePtr), 8) || !game) return;
        uint8_t** table = (uint8_t**)(game + kEntTable);
        BodyHide::Anchor an[BodyHide::aCount]; BodyHide::GetAnchors(an);
        for (uint32_t i = 0; i < kEntSlots; i++)
        {
            void* e = nullptr;
            if (!Rd(&e, table + i, 8) || !e) continue;
            bool own = false; for (int k = 0; k < BodyHide::aCount; k++) if (an[k].valid && an[k].ent == e) own = true;
            if (own) continue;
            float o[3]; if (!Rd(o, (uint8_t*)e + kEntOrigin, 12)) continue;
            const float d[3] = { o[0] - eye[0], o[1] - eye[1], o[2] - eye[2] };
            if (Dot(d, d) > kScanR * kScanR) continue;
            if (!Collision::IsCharacterEnt(e)) continue;
            Person p; if (Read(e, p)) g_People.push_back(p);
        }
    }
    //! the player behind / beside him: his forward against the way to the player (cos; -1 = straight behind)
    float Facing(const Person& p, const float player[3])
    {
        const float tp[2] = { player[0] - p.org[0], player[1] - p.org[1] };
        const float tl = sqrtf(tp[0] * tp[0] + tp[1] * tp[1]), fl = sqrtf(p.fwd[0] * p.fwd[0] + p.fwd[1] * p.fwd[1]);
        return tl > 1e-3f && fl > 1e-3f ? (tp[0] * p.fwd[0] + tp[1] * p.fwd[1]) / (tl * fl) : 0.f;
    }

    // ---- state (game thread) ----
    struct Threat { uint32_t id = 0; double since = -1; double toldAt = -10; } g_Threat[2];   // 0 the gun, 1 the blade
    std::unordered_map<uint32_t, double> g_TapAt;
    NpcSkel::Skel g_Sk;
    float g_PrevHand[2][3] = {}; bool g_PrevOk[2] = {}; double g_PrevT = 0;

    std::string Who(const Person& p) { return fmt::format("{:#x} ({})", p.id, Collision::Describe(p.ent)); }
}

void NpcGestures::Tick()
{
    if (!Log::Enabled()) return;   // research only
    float eye[3], ax[9];
    if (!HandAimHook::RealCamera(eye, ax)) return;
    {   // the real head, where the view puts it
        float vo[3], go[3], hm[3];
        if (ViewMatrixHook::ViewAnchor(vo, go, hm)) for (int i = 0; i < 3; i++) { const float d = vo[i] - go[i]; if (fabsf(d) < 1.5f) eye[i] += d; }
    }
    if (PlayerState::Cinematic() || KbmInject::Possessing()) return;
    const double now = Now();
    if (g_ScanAt < 0 || now - g_ScanAt > 0.3) { Scan(eye); g_ScanAt = now; }
    else for (size_t i = 0; i < g_People.size();)
    {   // this frame's pose (or gone: dead, down, hidden)
        if (Read(g_People[i].ent, g_People[i])) i++; else { g_People[i] = g_People.back(); g_People.pop_back(); }
    }
    BodyHide::Anchor an[BodyHide::aCount]; BodyHide::GetAnchors(an);
    float player[3] = { eye[0], eye[1], eye[2] - 1.6f };
    if (an[BodyHide::aPlayer].valid) memcpy(player, an[BodyHide::aPlayer].org, 12);
    const float dt = (float)std::clamp(now - g_PrevT, 1e-3, 0.1); g_PrevT = now;
    float hp[2][3]; bool hok[2];
    for (int h = 0; h < 2; h++) { float r[9]; hok[h] = GpuTrace::RawHandPose(h, hp[h], r); }

    // ---- a weapon held on someone (research: who would cower) ----
    for (int w = 0; w < 2; w++)
    {
        Threat& T = g_Threat[w];
        float o[3], f[3], tip[3]; bool have = false;
        if (w == 0) have = GpuTrace::HandItemDraws(0) > 0 && GunRay::DrawnRay(0, o, f);
        else { float g[3]; have = SwordContact::Blade(g, tip); }
        const Person* hit = nullptr; float hitD = 0.f, hitMiss = 0.f;
        if (have)
            for (const Person& p : g_People)
            {
                const float head[3] = { p.org[0], p.org[1], p.org[2] + 1.6f }, chest[3] = { p.org[0], p.org[1], p.org[2] + 1.3f };
                const float c[3] = { chest[0] - eye[0], chest[1] - eye[1], chest[2] - eye[2] };
                const float dist = Len(c);
                float miss;
                if (w == 0)
                {
                    float a1, a2; miss = std::min(RayDist(o, f, head, &a1), RayDist(o, f, chest, &a2));
                    if (dist > kAimMax || miss > kAimR || a1 < 0.f) continue;
                }
                else
                {
                    const float d1[3] = { tip[0] - head[0], tip[1] - head[1], tip[2] - head[2] }, d2[3] = { tip[0] - chest[0], tip[1] - chest[1], tip[2] - chest[2] };
                    miss = std::min(Len(d1), Len(d2));
                    if (dist > kBladeMax + 0.6f || miss > kBladeR) continue;
                }
                if (!hit || miss < hitMiss) { hit = &p; hitD = dist; hitMiss = miss; }
            }
        if (!hit) { T.id = 0; T.since = -1; continue; }
        if (T.id != hit->id) { T.id = hit->id; T.since = now; }
        if (now - T.since >= kAimHold && now - T.toldAt > 5.0)
        {
            T.toldAt = now; g_Threats++;
            const float fc = Facing(*hit, player);
            LOGI("NpcGestures: the {} HELD ON {} for {:.1f} s ({:.1f} m away, {:.0f} cm from his head / chest; he is {} you): he would cower (research: the game does nothing yet)",
                 w == 0 ? "left hand's gun" : "sword", Who(*hit), now - T.since, hitD, hitMiss * 100.f, fc > 0.5f ? "facing" : fc > -0.3f ? "side on to" : "turned away from");
        }
    }

    // ---- a tap: a hand arriving gently at someone's shoulder / upper back, from behind ----
    for (const Person& p : g_People)
    {
        bool close = false;
        for (int h = 0; h < 2; h++) if (hok[h]) { const float d[3] = { hp[h][0] - p.org[0], hp[h][1] - p.org[1], 0.f }; if (Len(d) < 0.9f) close = true; }
        if (!close || !NpcSkel::Get(p.ent, g_Sk) || g_Sk.n < 5) continue;
        float lo = 1e9f, hi = -1e9f; for (int j = 0; j < g_Sk.n; j++) { lo = std::min(lo, g_Sk.p[j][2]); hi = std::max(hi, g_Sk.p[j][2]); }
        const float tall = hi - lo; if (tall < 0.8f) continue;
        const float fc = Facing(p, player);
        for (int h = 0; h < 2; h++)
        {
            if (!hok[h]) continue;
            float bestT = 1e9f; int jt = -1;
            for (int j = 0; j < g_Sk.n; j++)
            {
                const float r = (g_Sk.p[j][2] - lo) / tall;
                const float d[3] = { hp[h][0] - g_Sk.p[j][0], hp[h][1] - g_Sk.p[j][1], hp[h][2] - g_Sk.p[j][2] };
                const float l = Len(d);
                if (r > 0.72f && r < 0.92f && l < bestT) { bestT = l; jt = j; }
            }
            if (jt >= 0 && bestT < kTapR && fc < -0.2f && g_PrevOk[h])
            {
                const float v[3] = { (hp[h][0] - g_PrevHand[h][0]) / dt, (hp[h][1] - g_PrevHand[h][1]) / dt, (hp[h][2] - g_PrevHand[h][2]) / dt };
                const float sp = Len(v);
                auto it = g_TapAt.find(p.id);
                if (sp > 0.15f && sp < 2.5f && (it == g_TapAt.end() || now - it->second > 2.0))
                {
                    if (g_TapAt.size() > 64) g_TapAt.clear();
                    g_TapAt[p.id] = now; g_Taps++;
                    LOGI("NpcGestures: {} hand TAPPED {} on the shoulder / back from behind ({:.1f} m/s, {:.0f} cm from the joint at {:.0f}% of his height): he would turn (research: the game does nothing yet)",
                         h ? "right" : "left", Who(p), sp, bestT * 100.f, (g_Sk.p[jt][2] - lo) / tall * 100.f);
                }
            }
        }
    }
    for (int h = 0; h < 2; h++) { if (hok[h]) memcpy(g_PrevHand[h], hp[h], 12); g_PrevOk[h] = hok[h]; }
}

std::string NpcGestures::Status()
{
    return fmt::format("people (research): weapons held on someone {}, taps {}", g_Threats.load(), g_Taps.load());
}
