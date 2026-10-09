# 0001 — Operator approvals

- **Status:** accepted
- **Date:** 2026-10-05
- **Decided by:** product owner
- **Context:** Roadmap M0 "Approvals task" — every download and vendoring decision the roadmap needs is settled up front so no milestone stalls on it (Roadmap rule 6).

## Decision

All requested items are approved. The product owner also granted standing permission to download whatever else the engine needs from official sources.

| Item | Source | License | Destination | Needed by |
|---|---|---|---|---|
| Poly Haven HDRI `studio_small_09` (1k) | `api.polyhaven.com` / `dl.polyhaven.org` | CC0 | `Resources/Environments/Studio.hdr` (committed in M6; SHA-256 and size in `DEFAULT_RESOURCES` of `Scripts/FetchAssets.py`, mirrored in `Resources/LICENSES.md`) | M6 |
| Poly Haven HDRI `kloofendal_48d_partly_cloudy_puresky` (1k) | `api.polyhaven.com` / `dl.polyhaven.org` | CC0 | `Resources/Environments/Sky.hdr` (committed in M6; SHA-256 and size in `DEFAULT_RESOURCES` of `Scripts/FetchAssets.py`, mirrored in `Resources/LICENSES.md`) | M6 |
| Inter font (`Inter-Regular.ttf`, pinned release) | github.com/rsms/inter releases | SIL OFL 1.1 | `Resources/Fonts/Inter-Regular.ttf` (Inter 4.1, committed in M6; archive and font SHA-256 in `Scripts/FetchAssets.py`, mirrored in `Resources/LICENSES.md` with the license text) | M6 |
| MikkTSpace (`mikktspace.c/.h`) | github.com/mmikk/MikkTSpace | zlib | `Vendor/MikkTSpace/` (vendored by the M6 contract; commit and SHA-256 in `Vendor/MikkTSpace/VENDOR.md`) | M6 |
| stb_vorbis (`stb_vorbis.c`) | github.com/nothings/stb (pinned commit) | MIT OR Unlicense | `Vendor/stb/` (vendored by the M12 contract at the stb commit pinned in `Vendor/stb/VENDOR.md`, which records its version and SHA-256; built into miniaudio, `Vendor/miniaudio/VENDOR.md`) | M12 |
| Format fixtures (FLAC, MP3, OTF) | pinned upstream sources, recorded in `Tests/Data/LICENSES.md` | permissive (per fixture) | `Tests/Data/` | M14 |
| Audio format fixtures `Tone.ogg` (Wikimedia Commons `File:1000Hz.ogg`) and `Tone.mp3` (NASA `File:Sputnik - Beep.mp3` via Wikimedia Commons) | commons.wikimedia.org (unmodified downloads; neither codec project publishes a small public-domain sample at a stable address) | public domain | `Tests/Data/Assets/Audio/` (SHA-256 and Commons revision SHA-1 in `Tests/Data/LICENSES.md`) | M12 — approved by the product owner on 2026-10-08 (ADR 0015 decision 25) |

Exact sizes and checksums are not part of this record because they were not known when the items were approved. They are pinned when each item is first fetched, as part of that milestone's change:

- **Assets:** the SHA-256 or md5 and the size go in `Scripts/FetchAssets.py`, and the entry is mirrored in `Resources/LICENSES.md` or `Tests/Data/LICENSES.md`.
- **Vendored code:** the commit SHA goes in `Vendor/<Lib>/VENDOR.md`.

The commit that adds an item must update this table with a pointer to where its pin lives.

## Rules that still apply

The standing permission removes the need to ask. It does not change how downloads are handled:

- Download only from the official upstream source, pin the exact version or commit, and verify a checksum.
  - Assets: the checksum lives in `Scripts/FetchAssets.py`.
  - Vendored code: the commit SHA lives in `VENDOR.md`.
- Commit the license with every asset or library. Assets are listed in `Resources/LICENSES.md` or `Tests/Data/LICENSES.md`; libraries keep their license in `Vendor/<Lib>/`.
- Every new third-party library follows the vendoring rules in `AGENTS.md`: unmodified upstream sources, a `VENDOR.md`, and ABI defines checked by `Scripts/CheckBuildConfig.py`.

## Repository remote and CI

- **Remote:** `origin` = https://github.com/Bar3b0n3s/Yggdrasil.
- **Push policy:** push only after a milestone (or task) has passed its review and its commit gate (Architecture §15.9).
- **CI:** GitHub Actions compiles and runs the unit tests on Windows, Linux (Ubuntu 24.04) and macOS (arm64). This replaces the earlier "generation-checked only" status of Linux and macOS (Architecture §1.1 G6, §16). A platform counts as verified once its CI job is green.

## Consequences

- The fallbacks in Architecture Appendix C for declined items are not needed: no approximate tangents, no OGG rejection, no bitmap-only text, no missing format coverage.
- Linux and macOS compile errors now show up in CI, so first-party code must actually compile on GCC 14 or Clang 18+ and Apple Clang, not just generate project files.
