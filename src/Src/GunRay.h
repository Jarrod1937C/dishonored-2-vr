#pragma once
#include <cstdint>
#include <string>
#include <sstream>

//! One ray for the ranged weapon in the hand: from the MUZZLE of the weapon as it is DRAWN in the hand, along its
//! barrel. Everything that has to agree about a shot takes it from here (2026-10-04, the development notes):
//!   - the game's own aim sensor (arkCpntPlayerSensorRangedTarget: SUBMIT exe+0x3C3FD0 posts an asynchronous ray from
//!     the camera eye along the aim component's direction, RESULTS exe+0x3C3E10 stores the hit at sensor+0x54/+0x58)
//!     traces THIS ray instead of the head's, so "what the weapon points at" is decided by the engine with the flat
//!     game's own mask and range;
//!   - the shot: the projectile leaves from the drawn muzzle (ComputeAimTarget places it at the weapon component's
//!     entity, this+0x10, transform * the muzzle joint this+0xF4 -- before, that entity was where the FLAT game holds
//!     the weapon, beside the head) and flies along the barrel to the sensor's hit;
//!   - the reticle: the line leaves the muzzle and ends on that hit, so the ring is where the bolt lands.
//! Before this the three disagreed: the bolt left from beside the head towards a point on the controller ray at the
//! distance the HEAD's ray had found, and the reticle was a fixed 8 m along the controller -- so close up and to the
//! side, a ring sitting on a bottle meant nothing (user report, Espacio 01 13:10 save).
namespace GunRay
{
    bool Install();
    void Uninstall();

    //! Bit mask, default 63: 1 = the aim sensor traces the gun ray, 2 = the aim point is on the gun ray,
    //! 4 = the projectile leaves from the drawn muzzle (and the fire's camera reads the gun ray), 8 = the reticle
    //! draws the gun ray up to the sensor's hit, 16 = the pistol's blast (its m_spreadArea explosion, a 30 deg cone) is
    //! fired along the barrel instead of from the head, 32 = everything the engine places on the weapon (muzzle flash,
    //! smoke, sparks, the muzzle entity) sits on the weapon as drawn -- except the MODELS bound to a
    //! weapon joint (the cartridge): their draws already ride the weapon's GPU delta, so their bind keeps the flat pose;
    //! 64 = move those too (research: drawn twice over, an old bug kept for A/B).
    //! 15 = part 31 only, 0 = the behaviour before 2026-10-04 (for A/B).
    void SetMode(int mask);
    int  Mode();
    //! 0 (default) = the shot goes exactly along the barrel. 1 = keep the game's own spread: the angle the game's
    //! spread step (exe+0x639120, done through the HEAD camera's projection) turned the aim by, re-applied about the
    //! barrel.
    void SetSpread(int on);

    //! The gun ray NOW, for the controller as it is this instant, placed where the next view will draw it (callable
    //! from the game thread). muzzle = world point, axis = idMat3 rows (row 0 = along the barrel).
    bool Pose(int hand, float muzzle[3], float axis[9]);
    //! The gun ray as DRAWN in the last recorded view (the reticle's, on the Present thread).
    bool DrawnRay(int hand, float muzzle[3], float fwd[3]);
    //! The engine's answer for the gun ray: distance along it to what it hits (`hit`), or the sensor's range when it
    //! hits nothing. False when the sensor has not traced the gun ray recently.
    bool SensorHit(float& dist, bool& hit);
    //! For the reticle's flicker log: what SensorHit answered from. `fromTrace`: the frame's own trace (else the aim
    //! sensor alone, the trace being stale); the trace's static sweep (t, hit) and whether the aim sensor's hit (dynamic
    //! world: props, characters) shortened it; the aim sensor's own last answer and its point; ages in ms.
    struct TraceDbg { bool valid = false, fromTrace = false, hit = false, sweepHit = false, sensorUsed = false, sensorHas = false;
                      float t = 0, sweepT = 0, sensorT = 0; float sensorPoint[3] = {}; unsigned traceAgeMs = 0, sensorAgeMs = 0; };
    TraceDbg LastTrace();

    //! ComputeAimTarget integration (HandAimHook): what the fire should read while the engine computes the spawn.
    struct Shot
    {
        bool valid = false;
        void* weapon = nullptr; uint8_t* ent = nullptr;
        float muzzle[3] = {}, axis[9] = {};   // the gun ray
        float entOrg[3] = {}, entAxis[9] = {};// the entity pose that puts the engine's muzzle on `muzzle`
        float eye[3] = {};                    // the camera eye the fire reads (behind the muzzle on the ray)
        float aim[3] = {}; bool aimSet = false;
        float aimIn[3] = {};                  // the aim point as the game handed it in
        float dist = 0; bool sensorHit = false;
    };
    //! True when this shot goes along the gun ray (fills `s`); false = leave the shot to the old path.
    bool Prepare(void* weapon, int hand, const float* aimPointIn, Shot& s);
    //! After the engine computed the spawn: put it back on the muzzle unless the weapon really pokes through something.
    void FixSpawn(const Shot& s, float* spawn);
    //! One log line per shot, both paths: where it left, where the gun ray and the reticle were, and the miss.
    void Report(void* weapon, int hand, const Shot* s, const float* spawn, const float* aimPoint,
                const float* oldOrg, const float* oldAxis);

    //! Which weapon components are guns/crossbows (the only ones this applies to; throwables keep their path).
    bool IsGun(void* weapon);

    void Log(int results);       //!< log the next n sensor results
    void SetTrack(int on);       //!< research: log each gun projectile's flight (GunFlight lines)
    void SetNoLock(int on);      //!< research: drop the fire's target lock (exe+0x63A8B0)
    void NearEntities(float radius); //!< research: every entity within radius of the eye (the entity table)
    void ProbeKids(float radius);    //!< research: entities near the eye that reference a held weapon entity (how parts are parented)
    void ProbeAttachment(int n); //!< research: the gun's attachment entity (+0x158, the cartridge) and its binds
    void FreezeAfterShot(int ms); //!< research: g_stopTime 1 this many ms after the next shot
    void BlockFx(int k);          //!< research: the k-th FX the next shot creates is never started
    void LogFx(int n);           //!< research: log the FX created after the next shot (callers, where they sit)
    void WatchFx(int hits);      //!< research: write watchpoint on the next muzzle fx instance's origin
    void BurstNextShot(int frames); //!< test: the next gun shot captures `frames` consecutive frames from its launch
    //! Once a frame on the game thread: sweep the projectile's shape along the gun ray (exe+0x638800, the engine's own
    //! "is the shot's path clear" check) -- the reticle's end and the shot's distance.
    void Tick();
    void Probe();                //!< research: log ray / sweep results along the gun ray for several collision masks
    void SetSweep(uint32_t mask, float maxRange);
    //! Test instrument: the world point under a screen point (fractions of the frame, as in a capture): the aim sensor's
    //! hit along the eye's ray through that pixel (props included). It becomes the TARGET every later shot is measured against (GunShot
    //! `target miss`), and the log says how far the gun ray must turn to reach it.
    void Pixel(float fx, float fy);
    void ClearTarget();
    //! The reticle reports where it drew the gun ring (fractions of the frame), so a shot's log line says where to look.
    void NoteRing(float fx, float fy);
    bool Command(const std::string& what, std::istringstream& ss);   //!< the `vr gun...` commands; false = not one of them
    std::string Stats();
}
