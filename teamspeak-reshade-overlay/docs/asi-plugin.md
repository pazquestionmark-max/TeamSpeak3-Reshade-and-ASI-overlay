# The `.asi` plugin

The overlay has two front ends. They draw the same HUD, read the same profiles and talk to the
same TeamSpeak plugin over the same pipe. The only difference is how they get a frame to draw
into:

| | ReShade add-on | `.asi` plugin |
|---|---|---|
| File | `TeamSpeakOverlay.addon64` | `TeamSpeakOverlay.asi` |
| Needs ReShade | Yes, an add-on-enabled build | No |
| Gets its frame from | ReShade, which already has one | Hooking the game's DXGI swap chain |
| Settings window | ReShade's menu → TeamSpeak Overlay | **Insert** (configurable) |
| Graphics APIs | Whatever ReShade supports | Direct3D 11 |
| Architectures | x64 and x86 | x64 |
| Risk | Whatever ReShade's is | Higher — see below |

**Install one or the other.** Both loaded at once means two copies of the overlay drawing two
copies of the HUD, one on top of the other.

## Ban risk, stated plainly

The `.asi` build has no host to borrow a frame from, so it makes one: it replaces three entries
in the DXGI swap chain's vtable (`Present`, `Present1`, `ResizeBuffers`) and draws in the
Present hook.

It writes **pointers**, not code. No byte of anybody's executable memory is modified, which is
both safer around other overlays and less visible to an integrity scan than rewriting a function
prologue would be. It is still an interception in a game process, and **a scanner has no way to
tell this overlay from something that is not an overlay**. Intent is not visible to a scanner; a
replaced vtable entry is.

Concretely:

* **FiveM servers in "pure mode"** block `.asi` plugins outright. Pure mode level 1 and above
  refuses unapproved files; the plugin simply will not load, and that is the server's decision,
  not a bug here.
* **Games with kernel-level anti-cheat** (EAC, BattlEye, Vanguard and the like) should be
  assumed to treat this as a violation. Do not install it into one.
* **The ReShade add-on is the lower-risk of the two.** ReShade is widely allowlisted; a
  home-grown hook is not. If ReShade is an option for you, use it.

This front end exists so that the overlay does not *require* ReShade. It is not a claim that
hooking is safe. You are choosing to run it.

## Game builds (FiveM)

From build 2189 onward, FiveM's ASI loader refuses any plugin that does not declare support for
the build being run. It looks for a resource inside the DLL:

    FindResource(module, L"FX_ASI_BUILD", MAKEINTRESOURCE(GetGameBuild()))

`TeamSpeakOverlay.asi` declares the whole known ladder — 1604 through 3889 — so it loads on any
current build and on the older ones servers still pin.

The declaration is about the *loader*, not the renderer. The overlay hooks DXGI and nothing
game-specific, so it does not care which build of GTA V it is inside; the list exists only
because FiveM asks for it.

**When FiveM adds a build we have not claimed**, the loader prints:

> Unable to load ...\TeamSpeakOverlay.asi - this ASI plugin does not claim to support game build
> N. If you have access to its source code, add `FX_ASI_BUILD N BEGIN "\0" END` to the .rc file
> when building this plugin. If not, contact its maintainer.

That refusal happens before `DllMain`, so the plugin never runs and writes nothing to the log —
it looks exactly like "it does nothing". The fix is one line in
`asi-integration/TeamSpeakOverlay.rc.in` and a new build. A line for a build that does not exist
costs nothing; a missing one fails in silence, so the list is deliberately generous.

## Other overlays in the same process

A game process is a crowded place. ReShade installs itself as a proxy `dxgi.dll`, ENBSeries as a
proxy `d3d11.dll`, and both sit in front of the functions this plugin replaces. The plugin names
every one it finds in the log at startup, because a crash in that arrangement is otherwise a
guessing game:

    asi: another graphics mod is loaded as dxgi.dll: ...\FiveM.app\plugins\dxgi.dll
    asi: IDXGISwapChain::Present is dxgi.dll+0x8A120  (...\FiveM.app\plugins\dxgi.dll)

**If one of them is ReShade, use the add-on instead.** It is the same overlay, it hooks nothing
at all, and it cannot conflict with the thing it is running inside. The plugin says so in the
log when it detects ReShade's add-on exports. Running both a proxy overlay and this one is
supported only in the sense that it is not prevented.

## What it does not do

* It does not read or write game memory, and does not scan the game's code for patterns. The
  only thing it touches is the swap chain's vtable, which it finds by creating a throwaway
  device of its own rather than by searching the game.
* It does not open a network socket. Neither component does.
* It does not inject into anything. An ASI loader loads it; there is no separate injector.
* It does not take game input unless the settings window is open, and even then it forwards
  everything ImGui is not using.

## Installing

1. **The TeamSpeak plugin**, exactly as for the add-on: `plugin\tsro_overlay_win64.dll` into
   `%APPDATA%\TS3Client\plugins\`, then TeamSpeak → Tools → Options → Addons → Plugins and
   enable it. See [installation.md](installation.md).

2. **The `.asi`** into your ASI loader's plugins folder:

   | Game / loader | Folder |
   |---|---|
   | FiveM | `%LOCALAPPDATA%\FiveM\FiveM.app\plugins\` |
   | GTA V with an ASI loader | next to `GTA5.exe` |
   | Other loaders | wherever that loader looks; most use a `plugins` folder |

   FiveM's own loader picks up any `.asi` in that folder at startup. Nothing else is needed.

3. **Fonts**, optionally: copy the `fonts` folder next to the `.asi`, or drop `.ttf` files into
   `%APPDATA%\TeamSpeakReShadeOverlay\fonts\`. The settings window lists both paths and what it
   found in them.

4. Start the game and press **Insert**.

## First launch

For the first 15 seconds after the game starts, the plugin prints a line across the top of the
screen naming itself and the key that opens its settings. It then gets out of the way, and does
not come back unless you reinstall.

That exists because the shipped profile hides the HUD entirely while TeamSpeak is closed or its
plugin is not enabled — right for everyday use, wrong for a first launch, when a working overlay
and a broken one would otherwise look identical: nothing on screen either way.

If you do not see that line, the plugin is not running. Go to
[When nothing appears](#when-nothing-appears).

## The settings window

**Insert** opens and closes it. Insert always works, whatever the profile says — it is polled
directly every frame, so it does not depend on the profile loading, on the window hook going in,
or on the plugin having picked the right window. If you bind a different key, that key works
*as well as* Insert, never instead of it. While it is open the mouse and keyboard go to the window rather
than to the game; everything the window is not using is passed straight through, so the game
does not lose keys it still needs.

The key is a setting, not a constant: *Settings window (.asi plugin only)* in the settings, or
`general.menu_key` in the profile. It can be Insert, Home, End, Delete, Pause, Scroll Lock or
F1–F12. That list is deliberately short — a profile has no business binding a key you drive
with. The ReShade add-on ignores this setting entirely.

## Profiles

Identical to the add-on's, in the same place: `%APPDATA%\TeamSpeakReShadeOverlay\profiles\`. A
profile saved from one front end is loaded by the other, and per-executable profile selection
works the same way. See [configuration.md](configuration.md).

On a first run, with no `default` profile yet, one is written from the copy built into the
binary (generated from `examples/profiles/default.json`). A `profiles\default.json` placed next
to the `.asi` takes precedence, which is how you would ship a house look. An existing profile is
never overwritten.

## When nothing appears

Work through [troubleshooting.md](troubleshooting.md) first — most symptoms are the same for
both front ends. These are the ones specific to this build:

**No overlay at all, and no log.** The `.asi` was not loaded, so nothing of ours ever ran. Check
FiveM's own console first — it says which of these it is:

* *"does not claim to support game build N"* — FiveM moved to a build this release does not
  declare. See [Game builds](#game-builds-fivem) above; it needs a new build of the plugin.
* Nothing at all about the file — it is in the wrong folder, or the loader is not present.
* On a pure-mode server, `.asi` plugins are blocked outright. That is the server's decision.

**A log, but nothing on screen.** Look in `%APPDATA%\TeamSpeakReShadeOverlay\tsro-overlay.log`
for `asi`:

* `DXGI hooks installed` absent → the hooks never went in; the message before it says why.
* `Dear ImGui attached to the game's D3D11 swap chain` absent → the game is not Direct3D 11.
  D3D12 and Vulkan are not supported by this front end; use the ReShade add-on.

**Insert does nothing.** The key is polled every frame while the game is the window in front, so
this is nearly always one of three things, and the log says which:

* No `settings window opened` line → the press never reached us. `menu key ignored: the
  foreground window is not ours` means the game did not have focus.
* No `bound to swap chain` or `Dear ImGui attached` line → nothing is being drawn at all, so the
  window would be invisible even if it opened.
* Something else has taken Insert. Bind another key in the profile (`general.menu_key`); it works
  alongside Insert rather than replacing it.

**Two overlays.** Both front ends are installed. Delete one.

## Removing it

Delete `TeamSpeakOverlay.asi`. That is the whole of it — nothing is installed elsewhere, no
registry keys are written, and the game's files are not modified. Configuration stays in
`%APPDATA%\TeamSpeakReShadeOverlay\` until you delete that too. See
[uninstallation.md](uninstallation.md).

## For maintainers

* Built by `asi-integration/`, which compiles `TSRO_OVERLAY_SOURCES` — the same renderer,
  settings window, icons, font engine and host object the add-on uses — **without**
  `TSRO_HOST_RESHADE`. That one absence is what makes their ImGui calls bind to a real Dear
  ImGui rather than to ReShade's inline forwarders.
* `cmake -DTSRO_BUILD_ASI=ON`. x64 only.
* `scripts/check-asi-link.sh` cross-builds it to a linked DLL with MinGW. That matters more here
  than for the add-on: this target links a real ImGui, so an unresolved symbol is its most
  likely failure and the syntax-only check cannot see one.
* The hooks themselves cannot be tested without Windows and a running game. CI proves the code
  compiles and links; it does not prove a frame draws.
