# CLAUDE.md

Guidance for working in this repo. Keep it short; the code is well-commented — prefer pointing at it over duplicating it here.

## What this is

Blipscope: ESP32-S3 firmware for round touch LCDs — a desk flight radar fed by OpenSky (cloud) or a local dump1090/readsb `aircraft.json`. PlatformIO + Arduino (pioarduino platform). The original ESP32-C3 Kit (1.28" GC9A01 + CST816T) is retired; the codebase is **S3-only** now (its single-core guards stay in the tree, inert — see below). See [README.md](README.md) for the product/user side.

## Build / flash / monitor

Multi-SKU: one PlatformIO env per hardware variant (see [platformio.ini](platformio.ini)). The S3 1.46" (`blipscope-s3-146`) is the default; the original C3 Kit is retired (S3-only).

```sh
pio run                                       # build the default (S3 1.46") env
pio run -e blipscope-s3-146 -t upload         # flash over USB-C (esptool)
pio device monitor -b 115200                  # serial at 115200
pio run -e <env> -t upload -t monitor         # build+flash+monitor a specific SKU
pio run -e blipscope-eam-s3-146 -t upload     # flash the EAM monitor (same S3 board; see FEATURE_EAM)
```

In VS Code, the PlatformIO toolbar buttons do the same. If upload fails to auto-reset: hold **BOOT**, tap **RESET**, release **BOOT**.

- Partitions: `min_spiffs.csv` (firmware is large; OTA needs the room).
- A pre-build script ([scripts/patch_async_buff.py](scripts/patch_async_buff.py)) re-applies a guard to ESPAsyncWebServer in `.pio/` (gitignored) so `-DASYNC_RESPONCE_BUFF_SIZE=1024` survives a fresh lib install. If the config web page silently stops sending after a clean `.pio/`, that patch didn't take.

## Variants / multi-SKU

Blipscope is several boards from one codebase. A `-DBLIPSCOPE_VARIANT_*` flag (set per env) selects a header in [include/variants/](include/variants/) defining pins, the display/touch driver (`BLIPSCOPE_PANEL_*` / `BLIPSCOPE_TOUCH_*`), capability flags (`BANDED_RENDER`, `ENRICH_ALWAYS`, `HAS_AUDIO`, `HAS_IMU`), and `SLUG`/`NAME`. Shared code never hardcodes hardware: geometry comes from [Layout.h](include/Layout.h) (from `variant::SCREEN_SIZE`), behaviour from `variant::*`, and display config from those macros in [LGFX.h](include/LGFX.h) (add a panel via an `#if` block). **Add a SKU = a variant header + an `[env:*]` + a CI matrix row** ([RELEASING.md](RELEASING.md)). Don't reintroduce hardcoded `240`/pins.

The radar SKUs are all S3: `blipscope-s3-146` (Waveshare ESP32-S3-Touch-LCD-1.46B — SPD2010 412×412 QSPI; the default) and `blipscope-pro-s3-21` (Waveshare ESP32-S3-Touch-LCD-2.1 — first **RGB-bus** panel, an ST7701 480×480). The original `blipscope-kit-c3-128` is retired; its variant header stays in the tree, inert. The S3-2.1 also has two board-specific wrinkles the model doesn't share: its panel/touch reset and the ST7701 init chip-select hang off a **TCA9554 I²C IO expander**, and it carries an IMU + buzzer. Both are handled behind the variant: `variant::BoardPreInit()` (a hook called in [setup()](src/main.cpp) before `tft.init()`; a no-op on the C3) drives the expander, and the IMU/buzzer live in [src/board/board_s3_touch21.cpp](src/board/board_s3_touch21.cpp) behind `board::*` (no-ops elsewhere via [Board.h](include/Board.h)). All board I²C uses LovyanGFX's `lgfx::i2c` (same owner as touch) on the loop task — don't reach for Arduino `Wire`.

## FEATURE_EAM — a second product from this codebase

`-DFEATURE_EAM` (set on the `blipscope-eam-*` envs) swaps the radar app for an **HFGCS EAM (Emergency Action Message) monitor** built from the same boards and the same shared infra (display, Wi-Fi, web config, NVS, HTTP/TLS, OTA, ntfy). It compiles **no** radar/aircraft/ADS-B code: the EAM envs' `build_src_filter` drops the radar-only TUs (`AircraftManager`, `MqttPublisher`, `SpecialAircraft`, `AircraftInfoFields`, `Logbook`, `models/`), `[common]` drops `src/eam/` from the radar builds, and [main.cpp](src/main.cpp) picks `EamManager` vs `AircraftManager` at compile time (same `Initialise/Update/Draw` surface). Everything EAM lives in [src/eam/](src/eam/).

- **Data:** the device talks only to one backend ("valar-eam-feed"; base URL is the runtime config `eam-base-url`, defaulting to the `EAM_FEED_BASE` build flag) over its normalized endpoints. [EamFeedClient](src/eam/EamFeedClient.h) runs **one** worker task (reusing the shared TLS client, like the radar's fetch task) with per-endpoint interval/backoff/dedupe/retention; all state stays on the loop task. Shapes + parsers are in [EamModels.h](src/eam/EamModels.h). The one exception to "feed-agnostic" is the optional **command-post watch** ([AbncpProvider.h](src/eam/AbncpProvider.h)): the "OpenSky — your account" source queries OpenSky **directly from the device** with the user's own OAuth creds (reusing [OpenSkyAuthTokenHandler](src/OpenSkyAuthTokenHandler.h)) — never via the backend, **never a baked-in key**, inert until creds are entered.
- **UI:** seven screens (ticker / tempo / codewords / ABNCP / propagation / ICBM / Zulu clock) on a dwell-timed rotation that skips empty feeds; [EamManager](src/eam/EamManager.h) + [EamScreens.cpp](src/eam/EamScreens.cpp), with a real 7-segment clock in [SevenSegment.cpp](src/eam/SevenSegment.cpp). Same C3 touch/TLS serialization (`TryAcquireBus`) and solar auto-dim as the radar.
- **Persistence + alerts:** [EamLogbook](src/eam/EamLogbook.h) (own NVS namespace `eam-log`) tracks seen EAMs/codewords; ntfy alerts reuse the radar's `ntfy-topic` + POST pattern on three toggleable triggers.
- **OTA channel:** `-DFW_OTA_PREFIX="eam-"` makes [OtaUpdater](src/OtaUpdater.cpp) fetch `firmware-eam-<slug>.bin`, so an EAM device never pulls a radar image for the same board. The shared `version.txt` gate is unchanged.
- The config web page is feature-gated in [ConfigurationWebServer.cpp](src/ConfigurationWebServer.cpp) (`#ifdef FEATURE_EAM`): the EAM form + its NVS keys instead of the radar form; the shell (mDNS, `/reset-wifi`, save flag, secret masking) is shared. Adding an EAM SKU = a variant header + an `[env:*]` (with `-DFEATURE_EAM`, the `build_src_filter`, and `FW_OTA_PREFIX`) + a CI row whose slug is `eam-<board>`.

## FEATURE_SPACE — a third product (Spacescope)

`-DFEATURE_SPACE` (set on the `blipscope-space-*` envs) swaps the radar app for **Spacescope** — a desk window onto live space data (ISS, rocket launches, space weather, deep-space probes) — built from the same boards and the same shared infra as the radar and EAM. It compiles **no** radar/aircraft or EAM code: the SPACE env's `build_src_filter` drops the radar-only TUs **and** `src/eam/`, `[common]` drops `src/space/` from the radar builds (and the EAM env drops it too), and [main.cpp](src/main.cpp) picks `SpaceManager` vs `EamManager` vs `AircraftManager` at compile time (same `Initialise/Update/Draw` surface). Everything Spacescope lives in [src/space/](src/space/). The config page gains a `#elif defined(FEATURE_SPACE)` branch alongside the EAM one in [ConfigurationWebServer.cpp](src/ConfigurationWebServer.cpp). OTA channel: `-DFW_OTA_PREFIX="space-"` (its own `firmware-space-<slug>.bin`). It pulls directly from free public space APIs and **bakes in no backend**; the optional `valar-space-feed` backend is the runtime `space-base-url` config (default empty, from the `SPACE_FEED_BASE` flag). **Status: Stage-1 skeleton** — the product gate, config form, OTA channel, and the rotation/touch/brightness shell with a splash + UTC clock are in; the feed client + data screens land in later stages (each = a feed + a `DrawX()` + a `HasData()` case).

## FEATURE_SEISMIC — a fourth product (Seismic edition)

`-DFEATURE_SEISMIC` (set on the `blipscope-seismic-*` envs) swaps the radar app for the **Seismic edition** — a desk earthquake radar fed by the keyless **USGS** feed — built from the same boards and shared infra as the radar/EAM/Space apps. Like them it compiles **no** radar/aircraft, EAM, or Space code: the SEISMIC env's `build_src_filter` drops the radar-only TUs **and** `src/eam/` **and** `src/space/`, `[common]` already drops `src/seismic/`'s siblings, and [main.cpp](src/main.cpp) picks `SeismicManager` vs `SpaceManager` vs `EamManager` vs `AircraftManager` at compile time (same `Initialise/Update/Draw` surface). Everything lives in [src/seismic/](src/seismic/). The config page gains a `#elif defined(FEATURE_SEISMIC)` branch alongside the EAM/Space ones in [ConfigurationWebServer.cpp](src/ConfigurationWebServer.cpp). OTA channel: `-DFW_OTA_PREFIX="seismic-"` (its own `firmware-seismic-<slug>.bin`).

Unlike Space/EAM (rotating screens), the Seismic UI mirrors the **Aviation radar**: three swipe-able screens (Radar / List / Stats) with a tap-to-inspect detail-card overlay ([SeismicManager](src/seismic/SeismicManager.h) + [SeismicScreens.cpp](src/seismic/SeismicScreens.cpp)). Its quake radar is **static range rings**, not the aircraft PPI sweep, so [main.cpp](src/main.cpp) gates the sweep block out of the FEATURE_SEISMIC build (alongside EAM/Space). The data layer mirrors `SpaceFeedClient`: [SeismicFeedClient](src/seismic/SeismicFeedClient.h) runs one worker over the **USGS FDSN event query API** (a worldwide "recent" query + a radius-bounded "near me" query, both bounded by `limit` so the JSON stays small), with shapes + parsers in [SeismicModels.h](src/seismic/SeismicModels.h). It bakes in **no backend** (the optional `se-base-url` config is empty by default). ntfy alerts reuse the shared `ntfy-topic` on three toggles (big quake worldwide / quake near you / tsunami-flagged), edge-seeded at boot so the backlog never fires.

## The C3's three hard constraints — read before changing memory, networking, or touch

These applied to the now-**retired C3 Kit** SKU: a **single-core RISC-V** chip with a tight, fragmenting heap — most non-obvious shared code exists to live within that. The board is gone, but its guards stay in the tree (inert on S3, gated by capability flags), so this section stays as reference and a C3 could be revived: **don't "simplify" the guards away.** The S3 SKUs (dual-core + PSRAM) relax all three (full framebuffer, enrichment always-on, touch and network on separate cores) via the variant's capability flags rather than by deleting the guards:

1. **Contiguous heap is scarce (~24–28 KB largest block).** A TLS handshake needs a big contiguous block. That's why the radar renders in two 240×120 bands instead of one 240×240 sprite ([BandCanvas](include/BandCanvas.h), [main.cpp](src/main.cpp)), why feeds are parsed straight off the socket (`GetJson`), and why adsbdb enrichment is **heap-gated** (skipped, not forced, when the largest block is small — `ENRICH_TLS_HEAP_FLOOR`). Do not shrink the shared TLS buffers.
2. **Touch I2C must never overlap a TLS handshake.** On the single core, an overlapping CST816 transfer wedges the controller off the bus until reboot. Touch is serialized against network via the HTTP client's request mutex — `HttpRequestManager::TryAcquireBus/ReleaseBus`, used in `AircraftManager::HandleTouch` (gated on the radar view, ungated on the detail card so close-taps are instant). Background enrichment also pauses briefly after a touch. See PR #8 / commit `56a3df2` for the full diagnosis. There is intentionally **no** RST/bit-bang touch watchdog — it thrashed on auto-sleep and never recovered a real wedge.
3. **A slow TLS handshake can starve the watchdog.** The Task-WDT is raised to 10 s in [setup()](src/main.cpp) because a blocking OpenSky/adsbdb handshake on a background task can keep `async_tcp` from being fed and reboot the board.

## Architecture in three sentences

- [AircraftManager](src/AircraftManager.h) owns essentially all state and runs on the Arduino **loop task**; all `trackedAircraft` mutation happens there.
- Three background FreeRTOS tasks do the blocking network work and never hold pointers into shared state — OpenSky/local **fetch**, adsbdb/photo **enrich**, and **MQTT** publish — handing parsed results back to the loop via queues; they share one HTTP client ([HttpRequestManager](src/HttpRequestManager.h)) because there isn't heap for a second TLS context.
- The UI is three swipe-able screens (Radar / List / Stats) with a detail card overlay; config is an async web page ([ConfigurationWebServer](src/ConfigurationWebServer.cpp)) reachable at `http://<device-name>.local`.

## Conventions

- Branching: feature/fix branches → PR into `main`. OTA is **per-SKU**: bump `FW_VERSION` ([OtaUpdater.h](src/OtaUpdater.h)) once, then a GitHub Release builds and publishes `firmware-<slug>.bin` for every SKU — see [RELEASING.md](RELEASING.md). A device only ever downloads its own slug's binary.
- The C3 SKU is single-core (no true parallelism); treat anything touching the I2C/SPI buses or the heap as timing-sensitive. Put per-variant behaviour behind `variant::` capability flags, not `#ifdef`s scattered through the logic.
- `credentials.json` (OpenSky client secret) is a user secret — never read, commit, or log it.
