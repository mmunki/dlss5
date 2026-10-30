# DLSS5 for Android arm64

A root plugin that turns a chosen game into an **ultra** build of itself. While
one of the games you picked is in the foreground, DLSS5 forces anti-aliasing,
pins the whole SoC (GPU *and* every CPU cluster) to its performance governors,
locks the display at the panel's peak refresh rate, shifts the size of the
game's interface, renders fewer pixels only when the phone is under load and
upscales through the display pipeline, and calibrates colour. A control-centre
(Quick Settings) tile switches the whole engine on and off, the app lets you
pick exactly which games are affected, and the app draws a small pass-through
HUD over the game while it is being boosted.

Two deliverables, one project:

| File | What it is |
| --- | --- |
| `DLSS5-<version>-arm64.zip` | Magisk module with the static arm64 daemon |
| `DLSS5-<version>.apk` | Control centre app: tile, game picker, in-game HUD, live status |

## What it actually does (per selected game, only while it is in front)

1. **Anti-aliasing force (MSAA)** — while the game runs the daemon sets
   `setprop debug.egl.msaa <4|8>` and `debug.egl.force_msaa 1`; on GLES 1/2
   titles most drivers then draw with 4/8 samples, so edges and hair visibly
   smooth over. The original property values are saved and restored (the
   props were unset before, `resetprop -d` deletes them again).
2. **CPU + GPU performance boost** — the GPU governor (Adreno `kgsl`, Mali
   kbase, generic `devfreq` are discovered at runtime) *and* every CPU cluster
   (`/sys/devices/system/cpu/cpufreq/policy*/scaling_governor`) are pinned to
   `performance` while the game is in front. The original values are saved and
   written back when the game closes, when the tile is switched off, when the
   screen goes off, or when the daemon stops.
3. **Peak refresh-rate lock** — `settings put system peak_refresh_rate/
   min_refresh_rate <hertz>` holds the panel at its top rate (60/90/120/144)
   while you play and restores your previous settings afterwards. On a panel
   that cannot reach the target, Android simply plays at its top rate - the
   command is always safe.
4. **Game UI mode** — a per-game density shift (`wm density <base±delta>`)
   changes the size of the game interface: smaller for a compact, sharper look
   with more world on screen, or bigger for easier touch targets. It is
   relative to the density the game currently sees and is restored (override
   or reset) when the game exits. The game re-creates its activity once at the
   moment of the switch, that is standard Android behaviour for a density
   change.
5. **Render scale** — `cmd game set --downscale <0.30…0.90> --fps <n> <pkg>`
   only when there is thermal/GPU headroom pressure. The scale is chosen from
   Android's documented ladder, so the system accepts it and the game keeps a
   normal, full-size UI. The 2.0 default is quality first: 0.90 with a 90 fps
   cap, and the adaptive controller may raise it to native whenever the phone
   is cool. The frame cap is only lowered when the phone is hot or the GPU is
   saturated.
6. **Colour calibration** — `cmd color_display set-layer-saturation <level>
   com.mmunki.dlss5 <pkg>` applies a saturation transform to the layers of
   that one package. There is no extra frame, no overlay and no measurable
   latency. Android caps the transform at 100 = native, so this stage can only
   pull an over-saturated panel back towards neutral, never push it beyond
   native.
7. **Adaptive controller** — a small neural network (5 inputs, 8 tanh hidden
   units, 2 outputs) picks the render scale and frame cap from the maximum
   thermal-zone temperature, GPU busy percentage, battery level and SoC class.
   It is blended with an explicit monotonic prior, and the daemon only
   re-applies when the scale moves by at least half a ladder step. The weights
   are fitted by `tools/train_model.py` and committed as `native/model.h`.
8. **The DLSS5 game HUD** — the control app adds a `TYPE_APPLICATION_OVERLAY`
   window (granted explicitly, pass-through with `FLAG_NOT_TOUCHABLE`, so it
   can never intercept a game touch) that shows the current render scale, the
   frame cap, MSAA state, CPU/GPU boost state, the locked hertz and the UI
   density while the boost is active, and disappears when it is released.
9. **Hard limits on what can be touched** — the daemon only accepts
   third-party packages (`pm list packages -3`), verifies the package-name
   grammar before every shell call, validates every value it ever echoes into
   a command against a number grammar, and never touches a system app. The
   app's picker applies the same rule (`ApplicationInfo.FLAG_SYSTEM` apps are
   not listed). Restore data for sysfs paths is accepted only from paths that
   literally name a `scaling_governor`.

## What it is not

DLSS5 is **not NVIDIA DLSS** and does not pretend to be. Real DLSS-style
reconstruction needs the game engine to hand over motion vectors, depth and
jittered projection matrices, and needs tensor cores to run the network. No
root module can inject that into an arbitrary closed-source game, and any
project claiming otherwise is either lying or hooking the renderer of one
specific engine.

What is possible without engine integration is exactly what this project does:
force the anti-aliasing on, spend the frame time on the whole SoC running at
its performance governors, hold the panel at its peak refresh rate, change the
interface size of the game to taste, render fewer pixels only under load and
upscale them in the display pipeline. The shadow/colour grading that a real
DLSS 5 pipeline performs inside the engine cannot be reproduced from outside
the engine; the colour stage above is the honest, latency-free part that
Android exposes.

## Requirements

* Root (Magisk 20.4+ or a root manager that runs Magisk modules) on an
  `arm64-v8a` device.
* Android **11+** for the colour stage (`cmd color_display`), **12+** for
  render scaling and frame caps (`cmd game set`). MSAA, CPU boost, refresh
  lock, the UI shift and the HUD work on **8.0+**. Everything degrades one
  stage at a time on older releases; each stage is independent.
* The games must be normal user-installed apps. System apps, including
  pre-installed games, are rejected by design.
* For render scaling the game should declare `android:appCategory="game"`;
  Android's `cmd game set` refuses everything else and DLSS5 then falls back
  to the performance-mode command. The other stages apply regardless. The
  picker marks game-category apps with `*`.
* The HUD needs the "draw over other apps" permission, which the app asks
  for when you turn it on.

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
also reloads on `SIGHUP`. Version 2 config files (every key is optional, a 1.x
file still loads):

```json
{
  "version": 2,
  "enabled": true,
  "ai": true,
  "gpu_boost": true,
  "cpu_boost": true,
  "msaa": 4,
  "refresh_lock": true,
  "refresh_rate": 90,
  "ui_density_delta": 0,
  "hud": true,
  "downscale": 0.90,
  "target_fps": 90,
  "saturation": 100,
  "games": ["com.mojang.minecraftpe", "com.example.game"]
}
```

* `msaa` — `0`, `4` or `8`; the forced anti-aliasing applied while a selected
  game runs. `0` leaves the driver's choice alone. If the game or the driver
  ignores it, nothing happens at all.
* `cpu_boost` — pin every cpufreq cluster to `performance` during play.
* `refresh_lock` + `refresh_rate` — `0` = leave the panel alone, otherwise the
  hertz target while playing.
* `ui_density_delta` — `-200…+200` dpi shift of the game interface. `0` = don't
  change the game's own size.
* `downscale` — the quality floor. With `ai` on, the controller may raise the
  scale up to native when there is headroom but never goes below this value.
* `target_fps` — `0` leaves the panel alone.
* `saturation` — `100` is native; lower values desaturate that one game.
* `hud` — round-tripped for the app; set `false` to stop the in-game overlay.

Commands:

```sh
su -c dlss5d --status          # the JSON the app and the HUD read
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
