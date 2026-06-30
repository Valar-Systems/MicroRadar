# Releasing Blipscope firmware

Blipscope ships several hardware SKUs from one codebase (see `include/variants/`). All SKUs
are versioned and released **together** from a single commit, and each device self-updates to
**its own** binary over OTA. This doc is the release checklist.

## How OTA works

- A device checks `releases/latest/download/version.txt` — a single integer, the latest
  firmware version, shared by all SKUs.
- If that integer is greater than the device's compiled `FW_VERSION`, the device downloads
  **`firmware-<slug>.bin`**, where `<slug>` is its own `variant::SLUG` (e.g. `s3-146`). Each
  SKU only ever downloads its own image.
- The flow lives in [src/OtaUpdater.cpp](src/OtaUpdater.cpp); `FW_VERSION` is in
  [src/OtaUpdater.h](src/OtaUpdater.h).

## Cutting a release

1. **Bump the version:** edit `FW_VERSION` in `src/OtaUpdater.h` (one number, all SKUs).
2. Commit + merge to `main`.
3. **Create a GitHub Release** with a tag (e.g. `v5`). Publishing it triggers
   [.github/workflows/firmware.yml](.github/workflows/firmware.yml), which:
   - builds every SKU in the matrix,
   - attaches each as `firmware-<slug>.bin`,
   - attaches a `version.txt` containing `FW_VERSION`.
4. Devices pick up the update on their next daily check (or reboot).

> Don't hand-upload assets — the workflow names them so they match what devices request.

## Adding a new SKU to releases

A SKU's slug must be identical in **three** places (keep them in sync):

1. `variant::SLUG` in its `include/variants/<sku>.h`
2. its `[env:*]` in `platformio.ini`
3. its `{ env, slug }` row in the `matrix.include` of `.github/workflows/firmware.yml`

Add all three, and the next release automatically builds and publishes that SKU's binary.

## EAM builds — a separate OTA channel

The `blipscope-eam-*` envs build a different **product** (the EAM monitor, `-DFEATURE_EAM`; see
[CLAUDE.md](CLAUDE.md)) from the same boards. So an EAM device never pulls a radar image for its
board, the EAM envs set `-DFW_OTA_PREFIX="eam-"`, and their CI slug is prefixed to match:

| env | slug (CI + OTA asset) |
| --- | --- |
| `blipscope-eam-s3-146` | `eam-s3-146` → `firmware-eam-s3-146.bin` |

These ride the **same** `version.txt` gate (one `FW_VERSION` bump releases radar and EAM together),
and a device only ever downloads its own `<prefix><slug>` binary. Releasing is otherwise identical
— the matrix rows are already in [.github/workflows/firmware.yml](.github/workflows/firmware.yml).

## Spacescope builds — another separate OTA channel

The `blipscope-space-*` envs build a third **product** (Spacescope, `-DFEATURE_SPACE`; see
[CLAUDE.md](CLAUDE.md)) from the same boards. Same arrangement as EAM: `-DFW_OTA_PREFIX="space-"`,
and a CI slug prefixed to match so a Spacescope device never pulls a radar or EAM image:

| env | slug (CI + OTA asset) |
| --- | --- |
| `blipscope-space-s3-146` | `space-s3-146` → `firmware-space-s3-146.bin` |

Same `version.txt` gate (one `FW_VERSION` bump releases radar, EAM, and Spacescope together).

## Seismic builds — another separate OTA channel

The `blipscope-seismic-*` envs build a fourth **product** (the Seismic edition, `-DFEATURE_SEISMIC`; see
[CLAUDE.md](CLAUDE.md)) from the same boards. Same arrangement as EAM/Space: `-DFW_OTA_PREFIX="seismic-"`,
and a CI slug prefixed to match so a Seismic device never pulls a radar, EAM, or Space image:

| env | slug (CI + OTA asset) |
| --- | --- |
| `blipscope-seismic-s3-146` | `seismic-s3-146` → `firmware-seismic-s3-146.bin` |

Same `version.txt` gate (one `FW_VERSION` bump releases radar, EAM, Spacescope, and Seismic together).

## Legacy note: the retired C3

The original ESP32-C3 Kit is retired — Blipscope is S3-only going forward. The workflow no
longer builds a `c3-128` slug, and the plain `firmware.bin` alias it used to publish for the
very first (pre-per-SKU-naming) devices is gone, so those C3 units no longer receive OTA
updates. The variant header (`include/variants/c3_128.h`) and the single-core guards it drove
stay in the tree, inert behind their capability flags, so the board can be revived by
re-adding its `[env:*]` and CI matrix row.
