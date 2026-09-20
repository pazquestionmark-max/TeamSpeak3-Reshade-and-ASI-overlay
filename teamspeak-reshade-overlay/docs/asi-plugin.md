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
in the DXGI swap chain's vtable (`Present`, `Present1`, `ResizeBuffers`) using MinHook, and
draws in the Present hook.

That is a hook in a game process. A generic code-integrity or anti-cheat scan looks for exactly
that, and **it has no way to tell this overlay from something that is not an overlay**. Intent
is not visible to a scanner; a patched vtable is.

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

## It must be loaded at startup, not injected

The hooks go in as the very first thing the plugin does, while the process is still starting and
before the game has presented a frame. That is deliberate: the vendored MinHook writes its patch
without suspending other threads (see `third_party/minhook/UPSTREAM.md`), which is safe only
while nothing can be executing the bytes being replaced.

An ASI loader loads plugins at startup, so this holds. **Injecting `TeamSpeakOverlay.asi` into a
game that is already running is not supported** and may crash it. There is no injector in this
project and none is needed.

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

## The settings window

**Insert** opens and closes it. While it is open the mouse and keyboard go to the window rather
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

**No overlay at all, and no log.** The `.asi` was not loaded. Check it is in the right folder
and that the loader is present. On FiveM, a pure-mode server is the usual answer.

**A log, but nothing on screen.** Look in `%APPDATA%\TeamSpeakReShadeOverlay\tsro-overlay.log`
for `asi`:

* `DXGI hooks installed` absent → the hooks never went in; the message before it says why.
* `Dear ImGui attached to the game's D3D11 swap chain` absent → the game is not Direct3D 11.
  D3D12 and Vulkan are not supported by this front end; use the ReShade add-on.

**Insert does nothing.** If the log says the window procedure could not be hooked, the key is
being polled instead and only works while the game window is in the foreground. Check nothing
else has bound Insert.

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
* `cmake -DTSRO_BUILD_ASI=ON`. x64 only: the vendored MinHook carries the 64-bit half of its
  length-disassembler.
* `scripts/check-asi-link.sh` cross-builds it to a linked DLL with MinGW. That matters more here
  than for the add-on: this target links a real ImGui, so an unresolved symbol is its most
  likely failure and the syntax-only check cannot see one.
* The hooks themselves cannot be tested without Windows and a running game. CI proves the code
  compiles and links; it does not prove a frame draws.
