# Where this copy of MinHook came from

Upstream: https://github.com/TsudaKageyu/minhook (BSD-2-Clause, Copyright (C) 2009-2017
Tsuda Kageyu). The Hacker Disassembler Engine in `src/hde/` is Copyright (C) 2008-2009
Vyacheslav Patkov, also BSD-2-Clause. Both notices are in the file headers and in this
repository's LICENSE.

Pinned to upstream commit **`d94c64d32ea37bc4f5ee47d580709f70c6fb6080`** (2026-06-13).

Nine of the ten files are byte for byte that commit:

    include/MinHook.h   src/buffer.c   src/buffer.h   src/trampoline.c   src/trampoline.h
    src/hde/hde64.c     src/hde/hde64.h   src/hde/table64.h   src/hde/pstdint.h

`src/hook.c` is the only file that differs, and the only change is that the thread-freeze code
is deleted. The comment at the top of that file says what went and why. In short: upstream
suspends every other thread while it writes the patch, FiveM blocks the call that lists threads
so it never worked here, and the patch goes in before the game's entry point runs so there is
nothing to freeze. Deleting it also drops eight imports that antivirus heuristics weight heavily
(`CreateToolhelp32Snapshot`, `Thread32First`, `Thread32Next`, `OpenThread`, `SuspendThread`,
`ResumeThread`, `GetThreadContext`, `SetThreadContext`).

## Check it yourself

    git clone https://github.com/TsudaKageyu/minhook.git /tmp/minhook
    cd /tmp/minhook && git checkout d94c64d32ea37bc4f5ee47d580709f70c6fb6080
    diff -r --strip-trailing-cr src include <this repo>/minhook/src <this repo>/minhook/include

That reports one differing file, `src/hook.c`. Anything else means this copy has been altered
and you should not trust it. The build workflow runs the same check on every push.

## Why this is vendored rather than a git submodule

Asked by a contributor, answered here so it does not have to be re-argued.

- The copy is a fork on purpose. Upstream still has the thread-freeze code, so a submodule
  tracking upstream puts those eight imports straight back. Keeping the change would mean
  maintaining a fork of MinHook as a second repository.
- GitHub's source zip and tarball do not include submodule contents. Anyone who downloads the
  source archive would get an empty `minhook/` and a build that fails. The antivirus section of
  the README asks people to read the source, build it themselves and compare the hash, and the
  people who most need that are the ones using the zip.
- Reproducible builds are load-bearing here: the workflow builds twice and compares SHA-256. A
  second repository in that chain means its availability gates every release.
- This copy is newer than the newest upstream release. v1.3.4 (2025-03-28) does not have the
  signed relative offset fix in the trampoline (`INT32 operand` rather than `UINT32`) that this
  commit has, so pinning a submodule to the latest tag would be a downgrade.

## Updating it

Every build prints whether upstream has newer commits touching these files, so drifting behind
is visible rather than silent. It never fails the build: being behind is information, not a
problem.

To take an update, copy the nine unmodified files from the new upstream commit, re-apply the
`hook.c` deletion, update the commit hash above, then run `tools\run_tests.bat`. `tools/test_minhook.cpp` hooks a
function inside the test and checks the patch goes in and comes back out, so a bad merge fails
there rather than in the game.

## What this fork requires of code that uses it (TeamSpeak overlay)

This copy came from `blancodagoat/texoverride`, and the sections above are that project's
reasoning, kept verbatim so the divergence from upstream stays documented. One consequence
applies to *any* caller, so it is written down here as well.

Because the thread-freeze is gone, `MH_EnableHook` writes over the first bytes of the target
function without suspending anything. That is safe only while no other thread can be executing
those bytes. texoverride gets that for free: it patches from `DllMain` before the game's entry
point has run.

`asi-integration/` relies on the same property, and states it in `src/asi_main.cpp`: the DXGI
hooks are the very first thing its bootstrap thread does, before it reads a profile or opens the
IPC client, so they go in while the process is still starting and no frame has been presented.
The corollary is that injecting `TeamSpeakOverlay.asi` into a game that is already running is
**not supported** — an ASI loader must load it at startup. `docs/asi-plugin.md` says so to users.

If a future caller needs to hook at an arbitrary moment, this copy is the wrong tool and
upstream's freeze has to come back with it.
