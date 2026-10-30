# DLSS5 for Android arm64

A root plugin that makes a chosen game render at a lower internal resolution,
upscales it through the display pipeline, pins the GPU to its performance
governor and calibrates colour — all only while that game is in the
foreground. A control-centre (Quick Settings) tile switches the whole engine
on and off, and the app lets you pick exactly which games are affected.

Two deliverables, one project:

| File | What it is |
| --- | --- |
| `DLSS5-<version>-arm64.zip` | Magisk module with the static arm64 daemon |
| `DLSS5-<version>.apk` | Control centre app: tile, game picker, live status |

## What it actually does

Android's `GameManager` can set a per-game render scale, a frame-rate cap and
a performance mode. That is the same mechanism the system game dashboard
uses, and it really does make the app render fewer pixels and let
SurfaceFlinger scale the result up to the panel. DLSS5 drives that mechanism
per game:

1. **Render scale** — `cmd game set --downscale <0.30…0.90> --fps <n> <pkg>`.
   The scale is chosen from Android's documented ladder, so the system accepts
   it and the game keeps a normal, full-size UI.
2. **GPU boost** — while the selected game is in front, the GPU governor is
   set to `performance` (Adreno `kgsl`, Mali kbase, generic `devfreq` are
   discovered at runtime). The original value is saved and written back when
   the game closes, when the tile is switched off, when the screen goes off,
   or when the daemon stops.
3. **Colour calibration** — `cmd color_display set-layer-saturation
   <level> com.mmunki.dlss5 <pkg>` applies a saturation transform to the
   layers of that one package. It is AOSP's own per-app colour transform, so
   there is no extra frame, no overlay and no measurable latency. Android caps
   the transform at 100 = native, so this stage can only pull an over-saturated
   panel back towards neutral, never push it beyond native.
4. **Adaptive controller** — a small neural network (5 inputs, 8 tanh hidden
   units, 2 outputs) picks the render scale and frame cap from the maximum
   thermal-zone temperature, GPU busy percentage, battery level and SoC class.
   It is blended with an explicit monotonic prior, and the daemon only
   re-applies when the scale moves by at least half a ladder step, so it does
   not oscillate. The weights are fitted by `tools/train_model.py` and
   committed as `native/model.h`.
5. **Hard limits on what can be touched** — the daemon only accepts
   third-party packages (`pm list packages -3`), verifies the package-name
   grammar before every shell call, and never touches a system app. The app's
   picker applies the same rule (`ApplicationInfo.FLAG_SYSTEM` apps are not
   listed).

## What it is not

DLSS5 is **not NVIDIA DLSS** and does not pretend to be. Real DLSS-style
reconstruction needs the game engine to hand over motion vectors, depth and
jittered projection matrices, and needs tensor cores to run the network. No
root module can inject that into an arbitrary closed-source game, and any
project claiming otherwise is either lying or hooking the renderer of one
specific engine.

What is possible without engine integration is exactly what this project does:
render fewer pixels, upscale them in the display pipeline, spend the saved
frame time on a stable frame rate, and keep the GPU out of its power-saving
governor. The result is a sharper-looking, steadier game at the same panel
resolution. The shadow/colour grading that a real DLSS 5 pipeline performs
inside the engine cannot be reproduced from outside the engine; the colour
stage above is the honest, latency-free part that Android exposes.

## Requirements

* Android **11+** for the colour stage (`cmd color_display`), **12+** for
  render scaling and frame caps (`cmd game set`). Everything degrades one
  stage at a time on older releases.
* `arm64-v8a` device, Magisk 20.4+ (or a root manager that runs Magisk
  modules).
* The game must be a normal user-installed app. System apps, including
  pre-installed games, are rejected by design.
* For render scaling the game should declare `android:appCategory="game"`;
  Android's `cmd game set` refuses everything else and DLSS5 then falls back
  to the performance-mode command. The picker marks those apps with `*`.

## Install

1. Flash `DLSS5-<version>-arm64.zip` in Magisk and reboot.
2. Install `DLSS5-<version>.apk`.
3. Open **DLSS5** once, grant root when asked, tick the games you want.
4. Add the **DLSS5** tile to the control centre. One tap turns the engine on
   or off; the game in the foreground is boosted (or released) immediately.

The daemon is started by the module's `service.sh` after boot. The app can
restart it from the main screen if it was killed.

## Configuration

The app writes `/data/adb/dlss5/config.json`; the daemon watches its mtime and
also reloads on `SIGHUP`.

```json
{
  "version": 1,
  "enabled": true,
  "ai": true,
  "gpu_boost": true,
  "downscale": 0.70,
  "target_fps": 60,
  "saturation": 100,
  "games": ["com.mojang.minecraftpe", "com.example.game"]
}
```

* `downscale` — the quality floor. With `ai` on, the controller may raise the
  scale up to native when there is headroom but never goes below this value.
* `target_fps` — `0` leaves the panel alone.
* `saturation` — `100` is native; lower values desaturate that one game.

Commands:

```sh
su -c dlss5d --status          # the JSON the app reads
su -c dlss5d --once --package com.example.game --dry-run
su -c dlss5d --once --package com.example.game   # apply, then restore
su -c dlss5d --selftest
```

The Magisk action button prints the same status.

## How it is put together

```
native/
  dlss5.h        shared types and the small core API
  util.c         file/sysfs helpers, command runner, bionic-compatible glob
  json.c         minimal JSON reader
  config.c       config defaults, validation, atomic save
  tune.c         the adaptive controller (model.h + monotonic prior)
  sysinfo.c      thermal, GPU load, battery, SoC class, governor discovery
  profile.c      apply/restore: GameManager, colour, GPU governor
  device.c       foreground package, third-party cache, screen state, status
  daemon.c       the polling loop, retune hysteresis, CLI
  selftest.c     312 checks, runs on the host and on the device
scripts/integration-test.sh   stub-driven end-to-end test of apply/restore
  model.h        weights generated by tools/train_model.py
app/
  src/com/mmunki/dlss5/        MainActivity, tile service, root shell, catalog
  src/com/mmunki/dlss5/core/   Config, MiniJson, GameFilter, Status (pure Java)
  tests/CoreTests.java         59 checks, plain javac/java
module/                        Magisk module: service.sh, customize.sh, action.sh
scripts/build.sh               cross compile, test, package, sign, checksums
```

The daemon runs one thread and polls the foreground activity every ~1 s
(0.7 s while a game is boosted). It reads the config only when it changes,
re-reads `applied.json` at startup to undo a profile left behind by a crash,
and restores everything on `SIGTERM`. Thermal/GPU/battery samples are taken
every 4 s, and the decision is re-applied only when it changed by half a
ladder step, which keeps the polling overhead well below one percent of a
frame.

## Build and test

```sh
./scripts/build.sh
```

No Gradle: aapt2 + javac + d8 + zipalign + apksigner, all from `ANDROID_HOME`,
plus the NDK clang for the daemon. Output lands in `dist/` with `SHA256SUMS`.

The build runs

* the deterministic model check (`tools/train_model.py --check`),
* 312 native checks on the host build,
* the daemon integration test: stub `pm`/`cmd`/`dumpsys` on the PATH, then
  assert that a selected game gets the render-scale and colour commands, that
  a non-installed package and a disabled master switch do nothing, and that
  the loop restores everything on `SIGTERM`,
* a static `aarch64-linux-android26` daemon, verified as an AArch64 ELF,
* the same arm64 binary under `qemu-aarch64` when `DLSS5_QEMU` points at a
  static qemu (the CI does this),
* 59 plain-Java checks of the config, JSON, package-filter and status logic,
* APK build, alignment, signing and `apksigner verify`,
* the Magisk zip and the checksums.

Test matrix:

| ABI | build | selftest |
| --- | --- | --- |
| arm64-v8a | static ELF, NDK 27 clang | `--selftest` under qemu-aarch64 |
| host x86_64 | glibc build of the same sources | `--selftest` |

Device verification (needs a rooted arm64 phone):

```sh
su -c dlss5d --status
su -c cmd game set --downscale 0.70 --fps 60 com.example.game
su -c cmd color_display set-layer-saturation 95 com.mmunki.dlss5 com.example.game
```

## Safety rules

* A package is only acted on if it is in the user's selection, is listed by
  `pm list packages -3`, and matches `[A-Za-z0-9._]` with at least one dot.
  Nothing from the config ever reaches a shell unvalidated.
* `applied.json` records exactly what was changed, so a crash or a reboot does
  not leave a GPU governor pinned to `performance`.
* Switching the tile off, leaving the game, turning the screen off, or killing
  the daemon all restore the game mode, the colour transform and the governor.
* No file in this project is deleted or disabled on the device.

## Signing key

`keys/dlss5-test.jks` is a **throwaway test key** (password `dlss5test`) so
that successive DLSS5 builds install as updates of each other. It is not a
secret and it is not suitable for Play Store publishing. Build with
`KEYSTORE_PATH=...` to use your own.

## Licence

MIT. See `LICENSE`.
