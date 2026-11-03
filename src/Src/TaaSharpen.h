#pragma once
#include <d3d11.h>
#include <string>

//! Sharpening of the game's TAA output in the headset ([Display.Picture] TaaSharpening).
//! The game's TAA comes out soft, and more so in a headset (the head never holds still, so its history is resampled
//! every frame). After each TAA dispatch its RESULT (rw_taaresult, what the rest of the frame reads) is sharpened by a
//! contrast-adaptive sharpen (AMD FidelityFX CAS's filter, made scale-free for HDR). The TAA's HISTORY is left alone:
//! sharpening that would compound frame after frame into halos and shimmer.
namespace TaaSharpen
{
    //! Right before a TAA dispatch, on the context that records it (any thread): when the sharpen is on and ready, the
    //! dispatch's rw_taaresult view (slot `resultSlot`) is swapped for the mod's texture. True then, with the engine's view
    //! and the mod's texture handed out (AddRef'd) for AfterTaa -- which must be called after the dispatch, once the
    //! engine's view is bound back. `historySlot`: the history write target, never sharpened.
    bool BeforeTaa(ID3D11DeviceContext* c, int resultSlot, int historySlot, ID3D11UnorderedAccessView** engineResult, ID3D11ShaderResourceView** taaOut);
    //! After that dispatch: the sharpen, from the mod's texture into the engine's result.
    void AfterTaa(ID3D11DeviceContext* c, ID3D11UnorderedAccessView* engineResult, ID3D11ShaderResourceView* taaOut);
    //! At Present (the game's render thread): makes / remakes the shader and the copy texture, takes the setting.
    void OnPresent(IDXGISwapChain* sc);
    //! For the StereoTaa health line: on, strength, the neighbours' distance, passes made / waiting for the copy texture.
    std::string Status();
}
