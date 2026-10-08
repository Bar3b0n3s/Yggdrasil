# miniaudio (vendored)

| Field          | Value |
|----------------|-------|
| Upstream       | https://github.com/mackron/miniaudio |
| Version / tag  | `0.11.25` (latest stable 0.11.x release at vendoring time) |
| Commit         | `9634bedb5b5a2ca38c1ee7108a9358a4e233f14d` (2026-03-04) |
| Date vendored  | 2026-10-05 |
| License        | `Unlicense OR MIT-0` (public domain or MIT No Attribution, your choice; see `LICENSE`) |
| Build          | Static library, premake project `miniaudio` (`premake5.lua`), language C |

## What was kept / removed

Kept (byte-identical to upstream, LF line endings as stored upstream):

- `miniaudio.h`: the complete library. The API is always visible; the implementation is compiled only
  when `MINIAUDIO_IMPLEMENTATION` is defined.
- `miniaudio.c`: upstream's implementation translation unit (`#define MINIAUDIO_IMPLEMENTATION` +
  `#include "miniaudio.h"`). Kept for reference and for updates; since M12 it is not compiled (see "Local
  additions").
- `LICENSE`

Removed: `extras/` (split build, optional libvorbis/libopus decoders, extra nodes, stb_vorbis, osaudio),
`external/`, `examples/`, `tests/`, `tools/`, `research/`, `resources/`, `data/`, `website/`, `camal/`,
`CMakeLists.txt`, `miniaudio.pc.in`, `README.md`, `CHANGES.md`, `CONTRIBUTING.md`, `.github/`, git metadata.

## Local modifications

None: every upstream file is byte-identical to the release.

## Local additions

- `miniaudio_vorbis.c` (M12, `Docs/Decisions/0015-m12-decisions.md`): the implementation translation unit the
  project compiles instead of `miniaudio.c`. It is the route `miniaudio.h` documents for Ogg Vorbis ("Vorbis"):
  `#define STB_VORBIS_HEADER_ONLY` + `#include "stb_vorbis.c"`, then `#define MINIAUDIO_IMPLEMENTATION` +
  `#include "miniaudio.h"` (exactly `miniaudio.c`'s content), then `#undef STB_VORBIS_HEADER_ONLY` + `#include
  "stb_vorbis.c"` for the decoder's implementation. Because `STB_VORBIS_INCLUDE_STB_VORBIS_H` is defined while the
  implementation compiles, miniaudio defines `MA_HAS_VORBIS` and registers its stb_vorbis decoding backend, so
  `ma_decoder` and the resource manager read `.ogg` (Vorbis) data like WAV, FLAC and MP3.
- `stb_vorbis.c` itself is not taken from upstream's `extras/`: the approved source is github.com/nothings/stb
  (`Docs/Decisions/0001-approvals.md`), vendored unmodified in `Vendor/stb/` (`Vendor/stb/VENDOR.md`: v1.22 at the
  pinned stb commit, SHA-256 `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`). That file is
  byte-identical to miniaudio 0.11.25's `extras/stb_vorbis.c` (same SHA-256, compared when it was vendored), so the
  backend sees exactly the decoder miniaudio ships with.
- `MA_HAS_VORBIS` is decided inside the implementation section of `miniaudio.h` (after the `MINIAUDIO_IMPLEMENTATION`
  guard at line 11552), so no struct a consumer sees changes and consumers need no new define.

## Build configuration (`premake5.lua`)

Mirrors upstream `add_library(miniaudio miniaudio.c miniaudio.h)`:

- Files: `miniaudio_vorbis.c` (the local implementation translation unit, above) and `miniaudio.h`, the same on every
  platform (the backend is chosen inside the header). Include directories: `.` and `../stb` (for `stb_vorbis.c`).
- C dialect: compiler default, as upstream CMake does (no `-std=` flag; GCC 14 / Clang 18 default to
  gnu17). Do not force `-std=c89`/`-std=c99`: miniaudio warns this can break `timespec`/`timeval` on Linux.
- Configuration defines:
  - `MA_NO_ENCODING` (all platforms): the engine never writes audio files. This only removes the
    WAV encoder (`ma_encoder_*`).
  - `MA_NO_RUNTIME_LINKING` (macOS only): link CoreAudio, AudioToolbox and CoreFoundation directly
    instead of `dlopen()`-ing them from hard-coded framework paths. Upstream recommends this for apps
    that use the hardened runtime and notarization (miniaudio.h, section "2.2. macOS and iOS"). This
    is a deliberate change from upstream's defaults.
- Everything else stays enabled: device IO with all of the platform's backends, decoding (built-in
  WAV, FLAC and MP3 decoders, plus Ogg Vorbis through stb_vorbis), the resource manager, the node graph, the high-level engine, generation
  (`ma_waveform`, `ma_noise`), threading, and SSE2/AVX2/NEON paths (chosen at runtime on MSVC; GCC and
  Clang use what the target enables by default, so do not add `-mavx2`).
- Linux: `pic "On"`.
- MSVC: warning C4244 is disabled for `miniaudio_vorbis.c` only (one harmless upstream conversion; see Notes).

Backends compiled in: Windows uses WASAPI, then DirectSound, then WinMM. Linux uses PulseAudio
(PipeWire's pulse server), then ALSA, then JACK. macOS uses CoreAudio. Every platform also gets the
Null and custom backends. On Windows and Linux the backends load their system libraries at runtime,
so no SDKs or `-dev` packages are needed to build.

## How consumers use it

```lua
includedirs { "%{wks.location}/Vendor/miniaudio" }       -- #include <miniaudio.h>
defines { "MA_NO_ENCODING" }                              -- keep in sync with premake5.lua
links { "miniaudio" }
filter "system:linux"
    links { "dl", "pthread", "m" }
filter "system:macosx"
    defines { "MA_NO_RUNTIME_LINKING" }                   -- only matters to the implementation; mirrored for consistency
    links { "CoreFoundation.framework", "CoreAudio.framework", "AudioToolbox.framework" }
filter {}
```

Never define `MINIAUDIO_IMPLEMENTATION` in engine code: the implementation lives only in this library.
Feature defines must be identical in this project and in every consumer. Some of them
(`MA_NO_DEVICE_IO`, `MA_NO_THREADING`, `MA_NO_RESOURCE_MANAGER`) change struct layouts such as
`ma_engine_config`, `ma_engine` and `ma_resource_manager`, so a mismatch silently corrupts memory. The others
(`MA_NO_ENCODING`, `MA_NO_DECODING`, `MA_NO_GENERATION`, `MA_NO_NODE_GRAPH`, `MA_NO_ENGINE`) remove
declarations, so a mismatch shows up as compile or link errors.

Link requirements per platform (upstream miniaudio.h, section "2. Building"):

| Platform | Link |
|----------|------|
| Windows  | nothing (WASAPI, DirectSound, WinMM and COM DLLs are loaded at runtime) |
| Linux    | `dl`, `pthread`, `m` (32-bit ARM would also need `atomic`; not a target) |
| macOS    | `CoreFoundation.framework`, `CoreAudio.framework`, `AudioToolbox.framework` (needed because of `MA_NO_RUNTIME_LINKING`) |

Notes:

- miniaudio does not guarantee ABI compatibility between releases, even bug-fix ones. Always link it
  statically (as here) and rebuild everything after an update.
- Ogg Vorbis is built in since M12 through stb_vorbis (see "Local additions"); Ogg Opus is not (it would need
  libopus and libopusfile, which are not vendored).
- Upstream's `miniaudio.c` has one MSVC level-3 warning: C4244 in `ma_dr_wav__read_smpl_to_metadata_obj`
  (the dr_wav `smpl` chunk parser), where `(pChunkHeader->sizeInBytes - MA_DR_WAV_SMPL_BYTES) /
  MA_DR_WAV_SMPL_LOOP_BYTES` (64-bit) is assigned to the 32-bit `calculatedLoopCount`. It is harmless: the
  value is only compared with the chunk's own 32-bit loop count, and a mismatch makes the parser skip the
  chunk. The source stays unmodified, so `premake5.lua` disables C4244 for the implementation translation unit
  only (`filter { "toolset:msc*", "files:miniaudio_vorbis.c" } disablewarnings { "4244" }`), which keeps the
  workspace build free of warnings. stb_vorbis, compiled in the same translation unit, adds no MSVC warning at the
  default level: with the suppression removed, the Release build reports exactly the one dr_wav C4244 (checked when
  stb_vorbis was vendored, MSVC 14.51). No other warning is disabled, and consumer translation units that
  include `miniaudio.h` compile cleanly at `/W4 /WX`. Re-check after every update: remove the
  suppression if upstream fixes the conversion, and never widen it to other warnings or files.

M12 (stb_vorbis): the library builds with zero warnings in Debug, Release and Dist with `miniaudio_vorbis.c` (MSVC
14.51 / v145); decoding Ogg Vorbis is covered by the engine's `AudioDecoder` and `AudioImporter` tests.

Verified on Windows (MSVC 14.51 / v145, Debug/Release/Dist, C++23 consumer) by a smoke test:

- An in-memory WAV decoded through `ma_decoder_init_memory` (bit-exact).
- A headless `ma_engine` (`noDevice = MA_TRUE`, 2 ch @ 48 kHz) playing that WAV through the resource
  manager (`ma_resource_manager_register_encoded_data` + `ma_sound_init_from_file`), plus an
  `ma_waveform` routed through an `ma_lpf_node` in the node graph, pulled with
  `ma_engine_read_pcm_frames`.
- An `ma_device` on the Null backend whose data callback ran on miniaudio's device thread.

## Updating

1. Find the latest release: `git ls-remote --tags https://github.com/mackron/miniaudio`.
2. `git clone --depth 1 --branch <tag> https://github.com/mackron/miniaudio <tmp>` and export with the
   original line endings: `git -C <tmp> -c core.autocrlf=false archive HEAD | tar -x -C <tmp2>`.
3. Replace `miniaudio.h`, `miniaudio.c` and `LICENSE`. Check that `miniaudio.c` still only defines
   `MINIAUDIO_IMPLEMENTATION` and includes `miniaudio.h` (`miniaudio_vorbis.c` repeats that between the stb_vorbis
   includes), that `miniaudio.h` still documents the stb_vorbis route, and compare upstream's `extras/stb_vorbis.c`
   with `Vendor/stb/stb_vorbis.c` (update the stb copy, per `Vendor/stb/VENDOR.md`, when they differ).
4. Read `CHANGES.md` and the "Building" / "Build Options" sections of `miniaudio.h`, and diff upstream
   `CMakeLists.txt` (the `miniaudio` target, its defines and `COMMON_LINK_LIBRARIES`). Update
   `premake5.lua` and this file to match.
5. Rebuild Debug/Release/Dist, run the engine's audio tests, and update this file (tag, commit, date).
