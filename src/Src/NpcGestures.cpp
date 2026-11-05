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
    // wave
    constexpr float kWaveAmp = 0.08f, kWaveAbove = -0.25f, kWaveAngle = 12.f, kWaveMin = 1.2f, kWaveMax = 8.f, kTalkMax = 3.0f;
    constexpr double kWaveWindow = 1.4;
    // a weapon held on someone
    constexpr float kAimR = 0.22f, kAimMax = 5.f, kBladeR = 0.40f, kBladeMax = 1.5f;
    constexpr double kAimHold = 0.7;
    // touches
    constexpr float kTapR = 0.12f, kBeltR = 0.20f, kPullBack = 0.06f;

    std::atomic<bool> g_Pick{ true }, g_Talk{ true };
    std::atomic<unsigned long long> g_Waves{ 0 }, g_Talks{ 0 }, g_Threats{ 0 }, g_Taps{ 0 }, g_Picks{ 0 };

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
    struct Wave { bool valid = false; float lastX = 0, ext = 0; int dir = 0, swings = 0; double firstAt = 0; double lastWave = -10; } g_Wave[2];
    struct Threat { uint32_t id = 0; double since = -1; double toldAt = -10; } g_Threat[2];   // 0 the gun, 1 the blade
    std::unordered_map<uint32_t, double> g_TapAt, g_ThreatAt;
    struct Pick { bool armed = false; Person who; float joint[3] = {}; float d0 = 0; double at = 0; bool gripWas = false; double toldAt = -10; } g_Pk;
    NpcSkel::Skel g_Sk;
    float g_PrevHand[2][3] = {}; bool g_PrevOk[2] = {}; double g_PrevT = 0;

    std::string Who(const Person& p) { return fmt::format("{:#x} ({})", p.id, Collision::Describe(p.ent)); }
}

void NpcGestures::SetPickpocket(bool on) { g_Pick = on; LOGI("NpcGestures: pickpocket by hand {}", on ? "on: sneaking, your hand at someone's belt from behind, grip and pull back (PickpocketByHand)" : "off"); }
void NpcGestures::SetWaveToTalk(bool on) { g_Talk = on; LOGI("NpcGestures: a wave {}", on ? "at someone within 3 m you look at talks to him, where the game lets you (WaveToTalk)" : "does not talk"); }

void NpcGestures::Tick()
{
    if (!Log::Enabled() && !g_Pick.load() && !g_Talk.load()) return;
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
    const float fwd[3] = { ax[0], ax[1], ax[2] }, right[3] = { -ax[3], -ax[4], -ax[5] };
    const float dt = (float)std::clamp(now - g_PrevT, 1e-3, 0.1); g_PrevT = now;
    float hp[2][3]; bool hok[2];
    for (int h = 0; h < 2; h++) { float r[9]; hok[h] = GpuTrace::RawHandPose(h, hp[h], r); }

    // ---- a wave: an empty hand raised and waved side to side (three strokes within 1.4 s, 8 cm each) ----
    for (int h = 0; h < 2; h++)
    {
        Wave& W = g_Wave[h];
        if (!hok[h] || GpuTrace::HandItemDraws(h) > 0 || hp[h][2] - eye[2] < kWaveAbove) { W.valid = false; W.swings = 0; continue; }
        const float rel[3] = { hp[h][0] - eye[0], hp[h][1] - eye[1], hp[h][2] - eye[2] };
        const float x = Dot(rel, right);
        if (!W.valid) { W = Wave{ true, x, x, 0, 0, 0, W.lastWave }; continue; }
        const int d = x > W.lastX + 0.004f ? 1 : x < W.lastX - 0.004f ? -1 : 0;
        if (d && d != W.dir)
        {
            if (W.dir && fabsf(W.lastX - W.ext) >= kWaveAmp)
            {
                if (W.swings == 0 || now - W.firstAt > kWaveWindow) { W.swings = 0; W.firstAt = now; }
                W.swings++;
            }
            W.ext = W.lastX; W.dir = d;
        }
        W.lastX = x;
        if (W.swings >= 3 && now - W.lastWave > 3.0)
        {
            W.lastWave = now; W.swings = 0; g_Waves++;
            const Person* best = nullptr; float bestA = 1e9f, bestD = 0.f;
            for (const Person& p : g_People)
            {
                const float c[3] = { p.org[0] - eye[0], p.org[1] - eye[1], p.org[2] + 1.5f - eye[2] };
                const float dl = Len(c); if (dl < kWaveMin || dl > kWaveMax) continue;
                const float a = acosf(std::clamp(Dot(c, fwd) / dl, -1.f, 1.f)) * 57.29578f;
                if (a < bestA) { bestA = a; best = &p; bestD = dl; }
            }
            if (!best || bestA > kWaveAngle)
            {
                LOGI("NpcGestures: {} hand waved, at no one ({})", h ? "right" : "left",
                     best ? fmt::format("the nearest to your gaze, {}, is {:.0f} deg off it (needs under {:.0f})", Who(*best), bestA, kWaveAngle) : std::string("nobody within 8 m"));
                continue;
            }
            const float fc = Facing(*best, player);
            const bool talk = g_Talk.load() && bestD <= kTalkMax && fc > 0.3f;   // (facing you: from behind, the interact would be a pickpocket)
            LOGI("NpcGestures: {} hand WAVED at {}, {:.1f} m away, {:.0f} deg off your gaze, {} you{}", h ? "right" : "left", Who(*best), bestD, bestA,
                 fc > 0.5f ? "facing" : fc > -0.3f ? "side on to" : "with his back to",
                 talk ? ": interact on him (talks, if the game lets you talk to him)" : " (a greeting: research, the game does nothing yet)");
            if (talk)
            {
                const float pt[3] = { best->org[0], best->org[1], best->org[2] + 1.35f };
                Collision::RequestUse(h, best->id, pt); g_Talks++;
                VRRuntime::Vibrate(h, 0.2f, 0.03f);
            }
        }
    }

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

    // ---- touches: the hands at someone's shoulder / upper back (a tap) or belt (a pickpocket) ----
    const int gh = GrabHand::Hand();
    const bool sneaking = PlayerState::Running("Sneaking") || PlayerState::Running("AutoCrouch");
    const bool grip = KbmInject::GripIdle(gh);
    Pick& P = g_Pk;
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
            float bestT = 1e9f, bestB = 1e9f; int jt = -1, jb = -1;
            for (int j = 0; j < g_Sk.n; j++)
            {
                const float r = (g_Sk.p[j][2] - lo) / tall;
                const float d[3] = { hp[h][0] - g_Sk.p[j][0], hp[h][1] - g_Sk.p[j][1], hp[h][2] - g_Sk.p[j][2] };
                const float l = Len(d);
                if (r > 0.72f && r < 0.92f && l < bestT) { bestT = l; jt = j; }
                if (r > 0.45f && r < 0.64f && l < bestB) { bestB = l; jb = j; }
            }
            // a tap: a hand arriving gently at the shoulder / upper back, from behind (research)
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
            // a pickpocket: sneaking, behind or beside him, the grabbing hand at his belt, the grip closed there
            if (g_Pick.load() && h == gh && jb >= 0 && bestB < kBeltR && !P.armed && grip && !P.gripWas)
            {
                if (!sneaking || fc > 0.3f)
                {
                    if (now - P.toldAt > 2.0) { P.toldAt = now; LOGI("NpcGestures: grip at {}'s belt, but no pickpocket: {}", Who(p), !sneaking ? "you are not sneaking (crouch)" : "he faces you"); }
                }
                else
                {
                    P.armed = true; P.who = p; memcpy(P.joint, g_Sk.p[jb], 12); P.d0 = bestB; P.at = now;
                    VRRuntime::Vibrate(h, 0.15f, 0.02f);
                    LOGI("NpcGestures: {} hand at {}'s belt ({:.0f} cm), grip: pull back to pickpocket", h ? "right" : "left", Who(p), bestB * 100.f);
                }
            }
        }
    }
    if (P.armed)
    {
        const int h = gh;
        const float d[3] = { hp[h][0] - P.joint[0], hp[h][1] - P.joint[1], hp[h][2] - P.joint[2] };
        const float away = Len(d) - P.d0;
        if (!grip) { LOGI("NpcGestures: the grip opened at {}'s belt without a pull: no pickpocket", Who(P.who)); P.armed = false; }
        else if (away >= kPullBack)
        {
            Collision::RequestUse(h, P.who.id, P.joint); g_Picks++;
            VRRuntime::Vibrate(h, 0.3f, 0.04f);
            LOGI("NpcGestures: {} hand pulled back from {}'s belt ({:.0f} cm): interact on him (the game's pickpocket, if it offers one)", h ? "right" : "left", Who(P.who), away * 100.f);
            P.armed = false;
        }
        else if (now - P.at > 2.0) { LOGI("NpcGestures: no pull from {}'s belt within 2 s: no pickpocket", Who(P.who)); P.armed = false; }
    }
    P.gripWas = grip;
    for (int h = 0; h < 2; h++) { if (hok[h]) memcpy(g_PrevHand[h], hp[h], 12); g_PrevOk[h] = hok[h]; }
}

std::string NpcGestures::Status()
{
    return fmt::format("people: waves {} (to talk {}), weapons held on someone {}, taps {}, pickpockets {}", g_Waves.load(), g_Talks.load(), g_Threats.load(), g_Taps.load(), g_Picks.load());
}
