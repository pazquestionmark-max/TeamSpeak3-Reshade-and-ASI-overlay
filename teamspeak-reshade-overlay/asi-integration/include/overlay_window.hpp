// SPDX-License-Identifier: MIT
// The overlay's own window, layered over the game. No hooks of any kind.
//
// This replaces an earlier design that intercepted IDXGISwapChain::Present. That drew correctly
// and FiveM killed the process anyway: an execute trap at FiveM_b3751_GTAProcess.exe+0x1028
// with "dllcrashed": "TeamSpeakOverlay.asi" in its crash metadata, and adhesive.dll -- its
// integrity component -- on the faulting thread. Inline patching tripped it; replacing the
// vtable pointer instead tripped it identically, which is the useful part: what FiveM objects
// to is a foreign hand in its present path at all, not the technique used to get there.
//
// So there is no hand in it. A transparent, click-through, always-on-top window is created,
// sized and positioned to the game's client area and kept there, with a DirectComposition
// swap chain of our own so per-pixel alpha composites over the game properly. The game's
// device, swap chain, window procedure and render loop are untouched, and the plugin is
// indistinguishable from any other application that happens to have a window open.
//
// What this costs, stated honestly: it cannot draw over an exclusive-fullscreen game, because
// nothing outside that game can. Borderless and windowed are unaffected, which is what FiveM
// uses by default and what most people run.
#ifndef TSRO_OVERLAY_WINDOW_HPP
#define TSRO_OVERLAY_WINDOW_HPP

#include <windows.h>

namespace tsro::overlay { class OverlayHost; }

namespace tsro::asi {

/// Starts the UI thread: creates the window, the device and the ImGui context, then renders
/// until stop(). Returns false only if the thread could not be created.
bool start_overlay_window(tsro::overlay::OverlayHost* host);

/// Signals the UI thread to tear everything down and waits briefly for it.
void stop_overlay_window();

}  // namespace tsro::asi

#endif  // TSRO_OVERLAY_WINDOW_HPP
