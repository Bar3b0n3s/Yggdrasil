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
  `#include "miniaudio.h"`). Releases since 0.11.22 ship this file, so no local implementation file
  was added.
- `LICENSE`

Removed: `extras/` (split build, optional libvorbis/libopus decoders, extra nodes, stb_vorbis, osaudio),
`external/`, `examples/`, `tests/`, `tools/`, `research/`, `resources/`, `data/`, `website/`, `camal/`,
`CMakeLists.txt`, `miniaudio.pc.in`, `README.md`, `CHANGES.md`, `CONTRIBUTING.md`, `.github/`, git metadata.

## Local modifications

None.

## Build configuration (`premake5.lua`)

Mirrors upstream `add_library(miniaudio miniaudio.c miniaudio.h)`:

- Files: `miniaudio.c`, `miniaudio.h` (the same on every platform; the backend is chosen inside the header).
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
  WAV, FLAC and MP3 decoders), the resource manager, the node graph, the high-level engine, generation
  (`ma_waveform`, `ma_noise`), threading, and SSE2/AVX2/NEON paths (chosen at runtime on MSVC; GCC and
  Clang use what the target enables by default, so do not add `-mavx2`).
- Linux: `pic "On"`.

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
- Ogg Vorbis/Opus are not built in. If `.ogg` support is needed, the supported route is upstream's
  `extras/stb_vorbis.c` (public domain), included before the implementation in a custom
  implementation `.c` file. miniaudio then registers a Vorbis decoding backend automatically
  (`STB_VORBIS_INCLUDE_STB_VORBIS_H`).
- Upstream's `miniaudio.c` has one MSVC level-3 warning (C4244, a 64-to-32-bit conversion in the
  dr_wav `smpl` chunk parser). It is harmless and left untouched. Consumer translation units that
  include `miniaudio.h` compile cleanly at `/W4`.

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
3. Replace `miniaudio.h`, `miniaudio.c` and `LICENSE`.
4. Read `CHANGES.md` and the "Building" / "Build Options" sections of `miniaudio.h`, and diff upstream
   `CMakeLists.txt` (the `miniaudio` target, its defines and `COMMON_LINK_LIBRARIES`). Update
   `premake5.lua` and this file to match.
5. Rebuild Debug/Release/Dist, run the engine's audio tests, and update this file (tag, commit, date).
