#pragma once
#include <cstdint>
#include <string>
#include <vector>

//! Direct access to Void Engine cvars by static address (id Tech 5 idCVar
//! objects live in .data; the int value sits at +0x28 of the object).
//! The objects are found by scanning for their NAMES (the RVAs of the community dump, docs/cvars-1.77.9.0.txt, do
//! not match this exe: only its names and defaults are trusted), and only when GameInfo::IsSupportedBuild().
namespace CVars
{
    struct Entry { const char* name; uint32_t rva; };

    void Init();                                  //!< validates layout, logs a dump
    bool Available();
    const std::vector<Entry>& Known();
    int  GetInt(const char* name, int fallback = 0);
    float GetFloat(const char* name, float fallback = 0.f);
    std::string GetString(const char* name);     //!< live string value (idStr)
    bool SetInt(const char* name, int v);          //!< writes the int value slot
    bool SetFloat(const char* name, float v);
    bool SetIntModified(const char* name, int v);  //!< the int value and the engine's "modified" flag (the renderer reacts)
    uintptr_t Address(const char* name);          //!< object address or 0
    void DumpObject(const char* name);            //!< research: log the raw object + pointed-to blocks
}
