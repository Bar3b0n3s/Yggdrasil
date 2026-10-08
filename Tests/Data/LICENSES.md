# Test data licenses

Every file under `Tests/Data/` is first-party test data of this repository, under the same terms as its first-party
code, unless a section below names another origin. Generated fixtures are committed together with the generator that
produces them (AGENTS.md "Tests"), so each one can be reproduced and checked byte for byte.

A fixture of third-party origin (downloaded or derived from someone else's work) needs a section here before it is
committed: its source, version or commit, license, checksum and the files it covers (AGENTS.md "Vendored code").

## Generated glTF fixtures (`Assets/Gltf/`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Gltf/**` (the `.gltf`, `.glb`, `.bin` and `.png` files) |
| Origin | First-party: written by `Tests/Data/Generate/MakeGltfFixtures.py` from the glTF 2.0 specification, with the Python standard library only. No third-party model, texture or tool output is included. |
| License | First-party: the same terms as the repository's code; no third-party rights |
| Reproduce | `python Tests/Data/Generate/MakeGltfFixtures.py` writes them; `--check` verifies that the committed files are byte-identical to a fresh generation |

The images are procedural (checkers and solid colours), encoded by the generator as PNG with stored deflate blocks; the
geometry is a unit cube and unit quads computed in the script.

## Generated texture fixtures (`Assets/Textures/`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Textures/**` (the `.png`, `.jpg`, `.bmp` and `.tga` files) |
| Origin | First-party: written by `Tests/Data/Generate/MakeTextureFixtures.py` with the Python standard library only (PNG with stored deflate blocks, a baseline JPEG encoder of its own, uncompressed BMP and TGA, RLE TGA). No third-party image or tool output is included. |
| License | First-party: the same terms as the repository's code; no third-party rights |
| Reproduce | `python Tests/Data/Generate/MakeTextureFixtures.py` writes them; `--check` verifies that the committed files are byte-identical to a fresh generation |

`Truncated.png` is deliberately cut short, so the texture importer's failure path has a fixture.

## Hand-written asset fixtures (`Assets/Materials/`, `Assets/Scenes/`, `Assets/Prefabs/`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Materials/**`, `Tests/Data/Assets/Scenes/**`, `Tests/Data/Assets/Prefabs/**` |
| Origin | First-party: hand-written canonical JSON documents (materials, a scene and a prefab) |
| License | First-party: the same terms as the repository's code; no third-party rights |

## Generated audio fixtures (`Assets/Audio/Tone.wav`, `Assets/Audio/Tone.flac`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Audio/Tone.wav`, `Tests/Data/Assets/Audio/Tone.flac` |
| Origin | First-party: written by `Tests/Data/Generate/MakeAudioFixtures.py` from the RIFF/WAVE and FLAC specifications, with the Python standard library only (PCM WAV; FLAC with verbatim subframes). The tones are computed in the script; no third-party recording or tool output is included. |
| License | First-party: the same terms as the repository's code; no third-party rights |
| Reproduce | `python Tests/Data/Generate/MakeAudioFixtures.py` writes them; `--check` verifies that the committed files are byte-identical to a fresh generation |

`Tone.wav` is 0.1 s of 44.1 kHz stereo (440 Hz left, 660 Hz right) and `Tone.flac` 0.1 s of 48 kHz mono (440 Hz), both 16-bit.

## Ogg Vorbis fixture (`Assets/Audio/Tone.ogg`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Audio/Tone.ogg` |
| Origin | Wikimedia Commons, [File:1000Hz.ogg](https://commons.wikimedia.org/wiki/File:1000Hz.ogg), uploaded to the English Wikipedia by User:Denelson83; downloaded unmodified from `https://upload.wikimedia.org/wikipedia/commons/1/17/1000Hz.ogg` on 2026-10-08 and renamed |
| Version | The file revision whose SHA-1 is `f7e762fcf90a7c98c41ab521d71940c041802bd7` (the SHA-1 Commons lists for it) |
| License | Public domain (as stated on the file's Commons page) |
| SHA-256 | `f6d213190ddd88313ea689fadcbce10083932fa2779243ca28d0bafb30d3ba8f` (5,490 bytes) |

A 1000 Hz tone: Ogg Vorbis, 22,050 Hz mono, 44,100 frames (2 s), three Ogg pages, the last with the end-of-stream flag.

## MP3 fixture (`Assets/Audio/Tone.mp3`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Audio/Tone.mp3` |
| Origin | Wikimedia Commons, [File:Sputnik - Beep.mp3](https://commons.wikimedia.org/wiki/File:Sputnik_-_Beep.mp3), a NASA recording (credited source `https://www.nasa.gov/mp3/578626main_sputnik-beep.mp3`); downloaded unmodified from `https://upload.wikimedia.org/wikipedia/commons/7/75/Sputnik_-_Beep.mp3` on 2026-10-08 and renamed |
| Version | The file revision whose SHA-1 is `b2f534a4ff115e97499d145722cc7b65b2ae15c8` (the SHA-1 Commons lists for it) |
| License | Public domain: a work of NASA, a United States government agency (as stated on the file's Commons page) |
| SHA-256 | `84ab44b58396913c75ba0d8f5c29b18bca382269a10527bcf90a39c5c1460215` (70,272 bytes) |

Beeps: MPEG-1 Layer III, 48 kHz joint stereo, 128 kbit/s constant bit rate with CRC-protected frames, 4.39 s.
