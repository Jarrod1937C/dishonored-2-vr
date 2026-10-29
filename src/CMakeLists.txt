cmake_minimum_required(VERSION 3.25)
project(DishonoredVR LANGUAGES CXX)

if((NOT MSVC) OR (NOT (CMAKE_SIZEOF_VOID_P EQUAL 8)))
    message(FATAL_ERROR "Build with MSVC x64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CONFIGURATION_TYPES Release RelWithDebInfo)
# The C/C++ runtime is linked INTO both DLLs (/MT, vcpkg triplet x64-windows-static). With /MD the mod used the
# msvcp140/vcruntime140 installed on the player's machine: a machine with an older Visual C++ redistributable than
# this toolset expects can crash at load (the 2024 std::mutex change), and the game shares ucrtbase with us, so a
# setlocale() by anyone in the process would change how our fprintf/fscanf write and read decimals.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

# The mod's version: the release number below plus the commit it was built from (shown in the log and in the DLLs'
# file properties, so a tester's log says exactly which build it came from).
set(DVR_VERSION_MAJOR 0)
set(DVR_VERSION_MINOR 5)
set(DVR_VERSION_PATCH 0)
set(DVR_VERSION "${DVR_VERSION_MAJOR}.${DVR_VERSION_MINOR}.${DVR_VERSION_PATCH}")
set(DVR_GIT "nogit")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --abbrev=7
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}" OUTPUT_VARIABLE DVR_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT DVR_GIT)
        set(DVR_GIT "nogit")
    endif()
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/Src/Version.h.in" "${CMAKE_BINARY_DIR}/generated/Version.h" @ONLY)
configure_file("${CMAKE_CURRENT_LIST_DIR}/Src/Version.rc.in" "${CMAKE_BINARY_DIR}/generated/Version.rc" @ONLY)
include_directories("${CMAKE_BINARY_DIR}/generated")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi /Gy")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /debug /OPT:REF /OPT:ICF")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/CMake")
find_package(Detours MODULE REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(OpenXR CONFIG REQUIRED)

set(OUT_DIR "${CMAKE_BINARY_DIR}/bin")

# --- dinput8 proxy: tiny, forwards DirectInput8Create and loads the mod DLL ---
add_library(dinput8 SHARED Proxy/dinput8.cpp Proxy/dinput8.def "${CMAKE_BINARY_DIR}/generated/Version.rc")
target_compile_definitions(dinput8 PRIVATE DVR_RC_PROXY)
set_target_properties(dinput8 PROPERTIES OUTPUT_NAME dinput8 PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUT_DIR}")
# The DLLs name their PDB by file name only, not by the absolute path of the build machine (which ends up in every
# shipped binary otherwise); dbghelp finds it in the folder it is pointed at (tools/symbolize.py).
# The name is written out: "%_PDB%" went through the Visual Studio generator's escaping and the DLLs carried
# "%%%DishonoredVR.pdb%%%", a name dbghelp never finds (tools/symbolize.py resolved nothing; measured 2026-10-23).
target_link_options(dinput8 PRIVATE "/PDBALTPATH:dinput8.pdb")

# --- the mod ---
set(MOD_SOURCES
    Src/Main.cpp
    Src/Log.cpp        Src/Log.h
    Src/GameInfo.cpp   Src/GameInfo.h
    Src/CrashLog.cpp   Src/CrashLog.h
    Src/Watchdog.cpp   Src/Watchdog.h
    Src/CVars.cpp      Src/CVars.h
    Src/D3D11Hook.cpp  Src/D3D11Hook.h
    Src/RenderRes.cpp  Src/RenderRes.h
    Src/Overlay.cpp    Src/Overlay.h
    Src/WristHud.cpp   Src/WristHud.h
    Src/StereoTaa.cpp  Src/StereoTaa.h
    Src/StereoSync.cpp Src/StereoSync.h
    Src/MetaAsw.cpp Src/MetaAsw.h
    Src/ScreenMode.cpp Src/ScreenMode.h
    Src/BinkVideo.cpp  Src/BinkVideo.h
    Src/XRProbe.cpp    Src/XRProbe.h
    Src/XRSession.cpp  Src/XRSession.h
    Src/XRCapture.cpp  Src/XRCapture.h
    Src/VRRuntime.cpp  Src/VRRuntime.h
    Src/Commands.cpp   Src/Commands.h
    Src/Game.cpp       Src/Game.h
    Src/CameraHook.cpp Src/CameraHook.h
    Src/PlayerViewHook.cpp Src/PlayerViewHook.h
    Src/ViewMatrixHook.cpp Src/ViewMatrixHook.h
    Src/XInputHook.cpp Src/XInputHook.h
    Src/PadProbe.cpp   Src/PadProbe.h
    Src/KbmInject.cpp  Src/KbmInject.h
    Src/HandAimHook.cpp Src/HandAimHook.h
    Src/BodyHide.cpp   Src/BodyHide.h
    Src/WeaponPose.cpp Src/WeaponPose.h
    Src/HandFx.cpp     Src/HandFx.h
    Src/Watchpoint.cpp Src/Watchpoint.h
    Src/GpuTrace.cpp   Src/GpuTrace.h
    Src/Reticle.cpp    Src/Reticle.h
    Src/PlayerState.cpp Src/PlayerState.h
    Src/MeleeAim.cpp   Src/MeleeAim.h
    Src/SwordGesture.cpp Src/SwordGesture.h
    Src/SwordContact.cpp Src/SwordContact.h
    Src/Collision.cpp  Src/Collision.h
    Src/NpcSkel.cpp Src/NpcSkel.h
    Src/ThrowGesture.cpp Src/ThrowGesture.h
    Src/GrabHand.cpp   Src/GrabHand.h
    Src/PointerRay.cpp Src/PointerRay.h
    Src/GunRay.cpp     Src/GunRay.h
    Src/BodyView.cpp   Src/BodyView.h
    Src/PipeTrace.cpp  Src/PipeTrace.h
    Src/TestAids.cpp   Src/TestAids.h
    Src/Settings.cpp   Src/Settings.h
    Src/VrTweaks.cpp   Src/VrTweaks.h
    Src/KeyBinds.cpp   Src/KeyBinds.h
    Src/SysInfo.cpp    Src/SysInfo.h
    Src/Seeds.cpp      Src/Seeds.h
    "${CMAKE_BINARY_DIR}/generated/Version.rc"
    Src/VRMath.h
    Src/pch.h
)
add_library(DishonoredVR SHARED ${MOD_SOURCES})
target_precompile_headers(DishonoredVR PRIVATE Src/pch.h)
target_link_libraries(DishonoredVR PRIVATE
    fmt::fmt imgui::imgui OpenXR::openxr_loader
    Detours::Detours
    d3d11 dxgi version windowscodecs shell32 ole32)
set_target_properties(DishonoredVR PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}" RUNTIME_OUTPUT_DIRECTORY_RELEASE "${OUT_DIR}")
target_link_options(DishonoredVR PRIVATE "/PDBALTPATH:DishonoredVR.pdb")   # (see dinput8 above)
