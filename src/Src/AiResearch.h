#pragma once
#include <string>

//! Research for the gestures at people that the game does not answer yet (2026-11-05): how the game makes people
//! greet, panic, cower and flee, and what guards hear. Installed only with the log on ([Logging.Files] WriteLog), and
//! logging only.
//!  - Every entity message of the AI's kinds posted (exe+0x8112C0(queue, type, payload size, entity id): the post that
//!    returns the payload to fill) -- ARK_MSG_AI_QUEUE_STIM 0x23, AI_PANIC_INDUCED 0x24, AI_HEARD_SOUND 0x87, AI_PANIC
//!    0x8B, AI_PANIC_CLEAR 0x8C, AI_GREETINGS_START 0x8F / FINISHED 0x90, ENTITY_HEARD_BY_AI 0xFB, AI_PICKPOCKETED_ON /
//!    OFF 0x1C4 / 0x1C5, AI_PANIC_FINISHED 0x1B0 -- with who it is for and the code that posted it (a few frames of the
//!    call stack); its payload is read on the next game tick (it is filled after the post returns).
//!  - Every sound an NPC hears (exe+0x5EBF80(hearing component, source entity, the sound, bool): it sends AI_HEARD_SOUND
//!    and, for sound kinds 0x1A / 0x1B, AI_PANIC_INDUCED): the hearer, the source, the sound's first 0x40 bytes and the
//!    call stack -- what a bottle's crash is to a guard, and who raises it (a knock to lure would raise the same).
namespace AiResearch
{
    void Install();
    void Tick();   //!< the game thread, once a frame: the payloads posted since
    std::string Status();
}
