# Test data licenses

Every file under `Tests/Data/` is first-party test data of this repository, under the same terms as its first-party
code, unless a section below names another origin. Generated fixtures are committed together with the generator that
produces them (AGENTS.md "Tests"), so each one can be reproduced and checked byte for byte.

A fixture of third-party origin (downloaded or derived from someone else's work) needs a section here before it is
committed: its source, version or commit, license, checksum and the files it covers (AGENTS.md "Vendored code").

`Projects/FeatureTest/Scaffold/Sources/` holds byte-identical copies of some of these fixtures, which the FeatureTest
scaffolds import into the project's `Assets/` (Docs/Decisions/0022-m14-contract.md decision 16), so `Assets/` holds a
second byte-identical copy of each imported file. Each copy, in either place, is covered by the section of its fixture
here. The third-party copies are those of the pinned fixtures below: every copy of the OpenType font fixture
(`Inter-Regular.otf`) and of the Noto font fixtures (SIL OFL 1.1, whose full texts their sections hold) in
`Scaffold/Sources/Fonts/` or imported from there into `Assets/` (today `Assets/Formats/Fonts/`, `Assets/Ui/Fonts/`,
`Assets/Ui/FallbackFonts/` and `Assets/Fonts/GoldenUiScripts/`), `Projects/FeatureTest/Assets/Formats/Audio/Tone.mp3` and
`Projects/FeatureTest/Assets/Formats/Audio/Tone.ogg` (public domain) and `Projects/FeatureTest/Assets/Formats/Audio/Water.opus`
(CC0 1.0, the Ogg Opus fixture), with their `Scaffold/Sources/Audio/` counterparts; every other copy is first-party.

## Pinned third-party fixtures (`Scripts/FetchAssets.py fixture`)

`python Scripts/FetchAssets.py fixture` downloads each of these files from its official source when it is missing or does
not match its pin, and verifies it against the SHA-256 and size below, which mirror `FIXTURE_FILES` in
`Scripts/FetchAssets.py`; `python Scripts/FetchAssets.py fixture --check` (a static check of the commit gate) verifies the
committed files and this table without downloading anything. A pin changes only together with the file and this table, in
one reviewed change. The sections below give each file's origin and license.

| File | Origin | License | Size (bytes) | SHA-256 |
|---|---|---|---|---|
| `Tests/Data/Assets/Audio/Tone.mp3` | Wikimedia Commons `File:Sputnik - Beep.mp3` (NASA) | Public domain | 70272 | `84ab44b58396913c75ba0d8f5c29b18bca382269a10527bcf90a39c5c1460215` |
| `Tests/Data/Assets/Audio/Tone.ogg` | Wikimedia Commons `File:1000Hz.ogg` | Public domain | 5490 | `f6d213190ddd88313ea689fadcbce10083932fa2779243ca28d0bafb30d3ba8f` |
| `Tests/Data/Assets/Audio/Water.opus` | Wikimedia Commons `File:Toberbreedia_water_bubbling.opus` (User:A.-K. D.) | CC0 1.0 | 339139 | `063b8a1ed2e1958d8a7e79a13c182f8aa9e132afa3a4207139395b14f80965ec` |
| `Tests/Data/Assets/Fonts/Inter-Regular.otf` | Inter 4.1, Regular (`extras/otf/Inter-Regular.otf` of the release archive) | SIL OFL 1.1 | 609600 | `d4f2b9e148059a15f014cb0f0b8fea8cd11bfa447dd483bedf1b0adc0e2ba799` |
| `Tests/Data/Assets/Fonts/NotoSansArabic-Regular.ttf` | Noto Sans Arabic 2.013, Regular (`NotoSansArabic/unhinted/ttf/NotoSansArabic-Regular.ttf` of the release archive) | SIL OFL 1.1 | 142140 | `bd86ca02f087d7f3c3788ba458fb6b73744c7639ed276b8d870dba6def6c40d0` |
| `Tests/Data/Assets/Fonts/NotoSansHebrew-Regular.ttf` | Noto Sans Hebrew 3.001, Regular (`NotoSansHebrew/unhinted/ttf/NotoSansHebrew-Regular.ttf` of the release archive) | SIL OFL 1.1 | 16836 | `04272f5600d0ec816d31d0df73b23aa8d3501ea359ebe820da31c11ffcf00853` |
| `Tests/Data/Assets/Fonts/NotoSansJP-Regular.otf` | Noto Sans CJK 2.004, Japanese subset, Regular (`Sans/SubsetOTF/JP/NotoSansJP-Regular.otf` at tag `Sans2.004` of noto-cjk) | SIL OFL 1.1 | 4533028 | `dff723ba59d57d136764a04b9b2d03205544f7cd785a711442d6d2d085ac5073` |

## M13 Luau fixtures (`TypeChecker/`)

All files in `Tests/Data/TypeChecker/` are original first-party test data under the repository's code license. `Tests/Data/Generate/MakeScriptFixtures.py` generates them, including expanded copies of the first-party script templates. Run the generator with `--check` to verify the committed bytes. No third-party scripts or recordings are included.

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
| Files | `Tests/Data/Assets/Textures/**` (the `.png`, `.jpg`, `.jpeg`, `.bmp` and `.tga` files) |
| Origin | First-party: written by `Tests/Data/Generate/MakeTextureFixtures.py` with the Python standard library only (PNG with stored deflate blocks, a baseline JPEG encoder of its own, uncompressed BMP and TGA, RLE TGA). No third-party image or tool output is included. |
| License | First-party: the same terms as the repository's code; no third-party rights |
| Reproduce | `python Tests/Data/Generate/MakeTextureFixtures.py` writes them; `--check` verifies that the committed files are byte-identical to a fresh generation |

`Truncated.png` is deliberately cut short, so the texture importer's failure path has a fixture.

## Generated heightmap fixtures (`Assets/Heightmaps/`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Heightmaps/**` (today `Ramp16.png`) |
| Origin | First-party: written by `Tests/Data/Generate/MakeHeightmapFixtures.py` with the Python standard library only (a 16-bit greyscale PNG with stored deflate blocks). No third-party image or tool output is included. |
| License | First-party: the same terms as the repository's code; no third-party rights |
| Reproduce | `python Tests/Data/Generate/MakeHeightmapFixtures.py` writes them; `--check` verifies that the committed files are byte-identical to a fresh generation |

`Ramp16.png` is a 17 x 17 ramp whose sample (x, z) is x * 4000 + z (Docs/Decisions/0037-m26-contract.md decision 20).

## Hand-written asset fixtures (`Assets/Materials/`, `Assets/Scenes/`, `Assets/Prefabs/`, `Assets/SpriteSheets/`, `Assets/Tilemaps/`, `Assets/StringTables/`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Materials/**`, `Tests/Data/Assets/Scenes/**`, `Tests/Data/Assets/Prefabs/**`, `Tests/Data/Assets/SpriteSheets/**`, `Tests/Data/Assets/Tilemaps/**`, `Tests/Data/Assets/StringTables/**` |
| Origin | First-party: hand-written canonical JSON documents (materials, a scene, prefabs, sprite sheets, a tilemap and string tables) |
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

## Ogg Opus fixture (`Assets/Audio/Water.opus`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Audio/Water.opus` |
| Origin | Wikimedia Commons, [File:Toberbreedia_water_bubbling.opus](https://commons.wikimedia.org/wiki/File:Toberbreedia_water_bubbling.opus), "The bubbling sound of Toberbreedia, a holy well in County Kilkenny", own work of User:A.-K. D., 2024-09-09; downloaded unmodified from `https://upload.wikimedia.org/wikipedia/commons/c/cd/Toberbreedia_water_bubbling.opus` on 2026-10-29 and renamed (M23, `Docs/Decisions/0034-m23-contract.md` decision 16; approved in `Docs/Decisions/0001-approvals.md`) |
| Version | The file revision whose SHA-1 is `e6dd2f04d2d244432cf5ec2cb32ce889954baab6` (the SHA-1 Commons lists for it) |
| License | CC0 1.0 Universal public domain dedication (`{{self|cc-zero}}` on the file's Commons page) |
| SHA-256 | `063b8a1ed2e1958d8a7e79a13c182f8aa9e132afa3a4207139395b14f80965ec` (339,139 bytes) |

Water bubbling: Ogg Opus (encoded by libopus 1.4), stereo, 48 kHz, channel mapping family 0, a 312-frame pre-skip, 82
Ogg pages of one logical stream (20 ms CELT packets, about 14 a page), the last with the end-of-stream flag; it decodes to
1,066,531 frames (22.22 s).

## MP3 fixture (`Assets/Audio/Tone.mp3`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Audio/Tone.mp3` |
| Origin | Wikimedia Commons, [File:Sputnik - Beep.mp3](https://commons.wikimedia.org/wiki/File:Sputnik_-_Beep.mp3), a NASA recording (credited source `https://www.nasa.gov/mp3/578626main_sputnik-beep.mp3`); downloaded unmodified from `https://upload.wikimedia.org/wikipedia/commons/7/75/Sputnik_-_Beep.mp3` on 2026-10-08 and renamed |
| Version | The file revision whose SHA-1 is `b2f534a4ff115e97499d145722cc7b65b2ae15c8` (the SHA-1 Commons lists for it) |
| License | Public domain: a work of NASA, a United States government agency (as stated on the file's Commons page) |
| SHA-256 | `84ab44b58396913c75ba0d8f5c29b18bca382269a10527bcf90a39c5c1460215` (70,272 bytes) |

Beeps: MPEG-1 Layer III, 48 kHz joint stereo, 128 kbit/s constant bit rate with CRC-protected frames, 4.39 s.

## Generated format fixtures (`Assets/Environments/Gradient.hdr`, `Gradient.exr`, `GradientZip.exr`, `GradientBlocks*.exr`, `Assets/Fonts/Glyphs.ttf`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Environments/Gradient.hdr`, `Tests/Data/Assets/Environments/Gradient.exr`, `Tests/Data/Assets/Environments/GradientZip.exr`, `Tests/Data/Assets/Environments/GradientBlocksNone.exr`, `GradientBlocksRle.exr`, `GradientBlocksZips.exr`, `GradientBlocksZip.exr`, `GradientBlocksPiz.exr`, `GradientBlocksPxr24.exr`, `Tests/Data/Assets/Fonts/Glyphs.ttf` |
| Origin | First-party: written by `Tests/Data/Generate/MakeFormatFixtures.py` from the Radiance RGBE, OpenEXR and TrueType (sfnt) specifications, with the Python standard library only (the OpenEXR ZIP, ZIPS and PXR24 data deflated by the script's own encoder, and the PIZ data coded by its own wavelet and Huffman encoders). The texels and glyph outlines are computed in the script; no third-party image, font or tool output is included. |
| License | First-party: the same terms as the repository's code; no third-party rights |
| Reproduce | `python Tests/Data/Generate/MakeFormatFixtures.py` writes them; `--check` verifies that the committed files are byte-identical to a fresh generation |

`Gradient.hdr` is a 32 x 16 equirectangular sky gradient in flat (not run-length encoded) RGBE scanlines; `Gradient.exr`
(uncompressed) and `GradientZip.exr` (ZIP) hold the same texels as half-float OpenEXR scanlines (M23, ADR 0034
decision 14), and the `GradientBlocks*.exr` images the same texels each repeated over 4 x 4 texels (128 x 64) in several
scanline blocks of each compression the environment importer reads: NONE, RLE, ZIPS, ZIP, PIZ and PXR24 (decision 21's
review). `Glyphs.ttf` is
a TrueType font with quadratic outlines for `.notdef`, space, `A`, `O` and `V` (`.notdef`, `A` and `O` with counters), a
format 4 Unicode character map and a legacy `kern` table that kerns `A` and `V`.

## Generated LUT fixtures (`Assets/Luts/`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Luts/Warm.cube` |
| Origin | First-party: written by `Tests/Data/Generate/MakeLutFixtures.py` from the Adobe/IRIDAS `.cube` format, with the Python standard library only. The table's entries are exact rational functions of the grid coordinates computed in the script; no third-party LUT or tool output is included. |
| License | First-party: the same terms as the repository's code; no third-party rights |
| Reproduce | `python Tests/Data/Generate/MakeLutFixtures.py` writes them; `--check` verifies that the committed files are byte-identical to a fresh generation |

`Warm.cube` is a 9 x 9 x 9 warm grade (red lifted, blue lowered) over the domain [0, 1], with a title, a comment and both
domain lines (M26, Docs/Decisions/0037-m26-contract.md decision 15).

## Generated particle fixtures (`Assets/Particles/`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Particles/Flame.png` |
| Origin | First-party: written by `Tests/Data/Generate/MakeParticleFixtures.py` from the PNG specification, with the Python standard library only (stored deflate blocks). Every texel is an integer function of its frame and position computed in the script; no third-party image or tool output is included. |
| License | First-party: the same terms as the repository's code; no third-party rights |
| Reproduce | `python Tests/Data/Generate/MakeParticleFixtures.py` writes it; `--check` verifies that the committed file is byte-identical to a fresh generation |

`Flame.png` is a 128 x 128 RGBA flipbook of sixteen 32 x 32 flame frames in a 4 x 4 grid, its tip swaying and flickering
from frame to frame (M33, Docs/Decisions/0048-m33-particle-look.md decision 8); FeatureTest's scaffolds import a copy from
`Projects/FeatureTest/Scaffold/Sources/Particles/`.

## Recorded soak samples (`Hardening/`)

| | |
|---|---|
| Files | `Tests/Data/Hardening/SoakStreamC.json`, `Tests/Data/Hardening/SoakIntegration.json` |
| Origin | First-party: subjects copied verbatim from two `bin/TestResults/Soak.json` reports that `Scripts/Hardening.py --steps soak` wrote on the M15 reference machine: stream C's full soak, whose `Callbacks.scene` and `Input.scene` scripts still appended a record every tick (both leak), and the integration run after the scaffolds bounded those histories (Docs/Decisions/0024-m15-contract.md "The soak fix"). `SoakStreamC.json` keeps the two leaking scenes, every scene whose script heap is a sawtooth and one script-less scene (`Physics.scene`, a constant heap); `SoakIntegration.json` keeps both editor scenes of the integration run. Recorded data, so no generator reproduces it; `Tests/Automation/test_hardening.py` evaluates it with the soak rules |
| License | First-party: the same terms as the repository's code; no third-party rights |

## Recorded fuzz report (`Hardening/FuzzM17.xml`)

| | |
|---|---|
| Files | `Tests/Data/Hardening/FuzzM17.xml` |
| Origin | First-party: the `bin/TestResults/Fuzz-Release-Asan.xml` that `Scripts/Hardening.py`'s fuzz step wrote in M17's milestone gate (`CI.py --milestone`, 2026-10-19) on the reference machine, copied verbatim except for LF line endings: the 35 targets of `Tests/Data/Fuzz/Targets.json` at 1,000,000 iterations each in 18 shards, each target's time summed over the shards (58,583.848 s in all; the step took 3,312 s). Recorded data, so no generator reproduces it; `Tests/Automation/test_hardening.py` sizes the fuzz step from it, and `Scripts/Hardening.py`'s fuzz step reads it until the machine keeps a full, passing run's measurement of its own (`Docs/Decisions/0028-backlog-program.md` decision 16, T-FUZZMODEL) |
| License | First-party: the same terms as the repository's code; no third-party rights |

## OpenType font fixture (`Assets/Fonts/Inter-Regular.otf`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Fonts/Inter-Regular.otf` |
| Origin | The member `extras/otf/Inter-Regular.otf` of the release archive `https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip` (33707794 bytes, SHA-256 `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`), tag `v4.1` of https://github.com/rsms/inter, byte-identical; the same archive as the engine's `Resources/Fonts/Inter-Regular.ttf` (`Resources/LICENSES.md`) |
| Version | Inter 4.1 |
| License | SIL Open Font License 1.1 (the text below) |
| SHA-256 | `d4f2b9e148059a15f014cb0f0b8fea8cd11bfa447dd483bedf1b0adc0e2ba799` (609,600 bytes) |

A static OpenType font with CFF outlines (`OTTO`, a `CFF ` table, no `CFF2` and no `fvar`), so the font importer reads the
CFF path that the TrueType fixtures do not. Condition 2 of the license requires that every copy carries the copyright
notice and the license: this section is that text, copied verbatim from `LICENSE.txt` of the archive (SHA-256
`262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a`), and the font's own `name` table carries the notice
and the license reference in every copy, including the FeatureTest project's.

```text
Copyright (c) 2016 The Inter Project Authors (https://github.com/rsms/inter)

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
http://scripts.sil.org/OFL

-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION AND CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

## Noto Sans Arabic font fixture (`Assets/Fonts/NotoSansArabic-Regular.ttf`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Fonts/NotoSansArabic-Regular.ttf` |
| Origin | The member `NotoSansArabic/unhinted/ttf/NotoSansArabic-Regular.ttf` of the release archive `https://github.com/notofonts/arabic/releases/download/NotoSansArabic-v2.013/NotoSansArabic-v2.013.zip` (18777381 bytes, SHA-256 `1301aceaea84c501cf2e6dcfb3182e2328c8eae5725817fcb239672bda7154f1`), release `NotoSansArabic-v2.013` (2025-10-15) of https://github.com/notofonts/arabic, byte-identical |
| Version | Noto Sans Arabic 2.013 |
| License | SIL Open Font License 1.1 (the text below) |
| SHA-256 | `bd86ca02f087d7f3c3788ba458fb6b73744c7639ed276b8d870dba6def6c40d0` (142,140 bytes) |

A TrueType font of the Arabic script whose GSUB substitutes each letter's contextual forms (isolated, initial, medial
and final) and whose GPOS places its marks: the fallback chain's and the shaper's Arabic fixture
(Docs/Decisions/0033-m22-contract.md decisions 42 and 43). Condition 2 of the license requires that every copy carries
the copyright notice and the license: this section is that text, copied verbatim from `OFL.txt` of the archive (SHA-256
`a7a5a25eb188bf1cd96982030d53e23c33485c69b1044a562254226857ee13af`, 4,382 bytes), and the font's own `name` table
carries the notice and the license reference in every copy, including the FeatureTest project's.

```text
Copyright 2022 The Noto Project Authors (https://github.com/notofonts/arabic)

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
https://openfontlicense.org


-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded, 
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

## Noto Sans Hebrew font fixture (`Assets/Fonts/NotoSansHebrew-Regular.ttf`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Fonts/NotoSansHebrew-Regular.ttf` |
| Origin | The member `NotoSansHebrew/unhinted/ttf/NotoSansHebrew-Regular.ttf` of the release archive `https://github.com/notofonts/hebrew/releases/download/NotoSansHebrew-v3.001/NotoSansHebrew-v3.001.zip` (3734185 bytes, SHA-256 `df0a71814b4e63644cf40fcc4529111b61266b7a2dafbe95068b29a7520cc3cb`), release `NotoSansHebrew-v3.001` (2024-06-03) of https://github.com/notofonts/hebrew, byte-identical |
| Version | Noto Sans Hebrew 3.001 |
| License | SIL Open Font License 1.1 (the text below) |
| SHA-256 | `04272f5600d0ec816d31d0df73b23aa8d3501ea359ebe820da31c11ffcf00853` (16,836 bytes) |

A TrueType font of the Hebrew script, a right-to-left script without contextual forms: the fallback chain's and the
bidirectional algorithm's Hebrew fixture (Docs/Decisions/0033-m22-contract.md decisions 42 and 43). Condition 2 of the
license requires that every copy carries the copyright notice and the license: this section is that text, copied
verbatim from `OFL.txt` of the archive (SHA-256 `9b9fe028b5ba74d231659a1bbaf0ed09b11e759d1ca6a070999e16d151616b47`,
4,382 bytes), and the font's own `name` table carries the notice and the license reference in every copy, including the
FeatureTest project's.

```text
Copyright 2022 The Noto Project Authors (https://github.com/notofonts/hebrew)

This Font Software is licensed under the SIL Open Font License, Version 1.1.
This license is copied below, and is also available with a FAQ at:
https://scripts.sil.org/OFL


-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font creation
efforts of academic and linguistic communities, and to provide a free and
open framework in which fonts may be shared and improved in partnership
with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded, 
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply
to any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software components as
distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to, deleting,
or substituting -- in part or in whole -- any of the components of the
Original Version, by changing formats or by porting the Font Software to a
new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed, modify,
redistribute, and sell modified and unmodified copies of the Font
Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components,
in Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the corresponding
Copyright Holder. This restriction only applies to the primary font name as
presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created
using the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```

## Noto Sans CJK JP font fixture (`Assets/Fonts/NotoSansJP-Regular.otf`)

| | |
|---|---|
| Files | `Tests/Data/Assets/Fonts/NotoSansJP-Regular.otf` |
| Origin | The file `Sans/SubsetOTF/JP/NotoSansJP-Regular.otf` of https://github.com/notofonts/noto-cjk at tag `Sans2.004` (commit `523d033d6cb47f4a80c58a35753646f5c3608a78`; its git blob `4af556abb9cc42e585aab777b01857246a110746`), downloaded from `https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/Sans/SubsetOTF/JP/NotoSansJP-Regular.otf`, byte-identical |
| Version | Noto Sans CJK 2.004, the Japanese region subset (the smallest of the five region subsets) |
| License | SIL Open Font License 1.1 (the text below) |
| SHA-256 | `dff723ba59d57d136764a04b9b2d03205544f7cd785a711442d6d2d085ac5073` (4,533,028 bytes) |

A CID-keyed OpenType font with CFF outlines covering Japanese kana and ideographs: the fallback chain's CJK fixture,
whose texts bake only the codepoints their `Ranges` name (Docs/Decisions/0033-m22-contract.md decision 42). Condition 2
of the license requires that every copy carries the copyright notice and the license: this section is that text, copied
verbatim from `LICENSE` of the repository at the same tag
(`https://raw.githubusercontent.com/notofonts/noto-cjk/Sans2.004/LICENSE`, SHA-256
`6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2`, 4,301 bytes), and the font's own `name` table
carries the notice and the license reference in every copy, including the FeatureTest project's.

```text
This Font Software is licensed under the SIL Open Font License,
Version 1.1.

This license is copied below, and is also available with a FAQ at:
http://scripts.sil.org/OFL

-----------------------------------------------------------
SIL OPEN FONT LICENSE Version 1.1 - 26 February 2007
-----------------------------------------------------------

PREAMBLE
The goals of the Open Font License (OFL) are to stimulate worldwide
development of collaborative font projects, to support the font
creation efforts of academic and linguistic communities, and to
provide a free and open framework in which fonts may be shared and
improved in partnership with others.

The OFL allows the licensed fonts to be used, studied, modified and
redistributed freely as long as they are not sold by themselves. The
fonts, including any derivative works, can be bundled, embedded,
redistributed and/or sold with any software provided that any reserved
names are not used by derivative works. The fonts and derivatives,
however, cannot be released under any other type of license. The
requirement for fonts to remain under this license does not apply to
any document created using the fonts or their derivatives.

DEFINITIONS
"Font Software" refers to the set of files released by the Copyright
Holder(s) under this license and clearly marked as such. This may
include source files, build scripts and documentation.

"Reserved Font Name" refers to any names specified as such after the
copyright statement(s).

"Original Version" refers to the collection of Font Software
components as distributed by the Copyright Holder(s).

"Modified Version" refers to any derivative made by adding to,
deleting, or substituting -- in part or in whole -- any of the
components of the Original Version, by changing formats or by porting
the Font Software to a new environment.

"Author" refers to any designer, engineer, programmer, technical
writer or other person who contributed to the Font Software.

PERMISSION & CONDITIONS
Permission is hereby granted, free of charge, to any person obtaining
a copy of the Font Software, to use, study, copy, merge, embed,
modify, redistribute, and sell modified and unmodified copies of the
Font Software, subject to the following conditions:

1) Neither the Font Software nor any of its individual components, in
Original or Modified Versions, may be sold by itself.

2) Original or Modified Versions of the Font Software may be bundled,
redistributed and/or sold with any software, provided that each copy
contains the above copyright notice and this license. These can be
included either as stand-alone text files, human-readable headers or
in the appropriate machine-readable metadata fields within text or
binary files as long as those fields can be easily viewed by the user.

3) No Modified Version of the Font Software may use the Reserved Font
Name(s) unless explicit written permission is granted by the
corresponding Copyright Holder. This restriction only applies to the
primary font name as presented to the users.

4) The name(s) of the Copyright Holder(s) or the Author(s) of the Font
Software shall not be used to promote, endorse or advertise any
Modified Version, except to acknowledge the contribution(s) of the
Copyright Holder(s) and the Author(s) or with their explicit written
permission.

5) The Font Software, modified or unmodified, in part or in whole,
must be distributed entirely under this license, and must not be
distributed under any other license. The requirement for fonts to
remain under this license does not apply to any document created using
the Font Software.

TERMINATION
This license becomes null and void if any of the above conditions are
not met.

DISCLAIMER
THE FONT SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO ANY WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
OF COPYRIGHT, PATENT, TRADEMARK, OR OTHER RIGHT. IN NO EVENT SHALL THE
COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
INCLUDING ANY GENERAL, SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL
DAMAGES, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF THE USE OR INABILITY TO USE THE FONT SOFTWARE OR FROM
OTHER DEALINGS IN THE FONT SOFTWARE.
```
