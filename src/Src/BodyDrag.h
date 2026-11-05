#pragma once
#include <cstdint>
#include <string>

//! Bodies dragged and lifted by hand ([Gameplay.Interaction] DragBodies). The grip closed on a body (dead or
//! unconscious: the interaction sensor's carry pick, within reach of the hand) takes hold of the nearest part of it and
//! drags it with the hand -- along the floor, into a corner, out of the light. Bring the hand up over your shoulder and
//! the body goes on your back: the game's own carry (the long press of interact that a keyboard player makes), so
//! carrying, dropping and everything the game does with a carried body is unchanged. Letting go of the grip lets go.
//!
//! How it pulls: the body's joints as the engine poses them this frame (NpcSkel: a ragdoll writes them where its body
//! is), the grabbed joint pulled towards the hand by the game's impulse message (0xFA {impulse, world point}, what
//! Windblast and the hands' pushes post) each game tick, as a damped spring with some slack. How heavy the part is (how
//! much speed an impulse gives it) is not known beforehand: it is measured from the joint's response to the impulses
//! already sent, and the next impulse sized by it.
namespace BodyDrag
{
    void SetEnabled(bool on);
    bool Enabled();
    //! KbmInject (its thread): the grip of `hand` closed on body `id` within reach: drag it.
    void Start(int hand, uint32_t id);
    void Stop(const char* why);
    bool Active();
    //! KbmInject: the hand dragging it came up over the shoulder -- once: the body's id and a point on it (for the
    //! interaction sensor to be aimed at while the game's carry is asked for). Dragging has stopped.
    bool TakeShoulder(uint32_t* id, float point[3]);
    //! The game thread, once a frame (after Collision::Tick).
    void Tick();
    std::string Status();
}
