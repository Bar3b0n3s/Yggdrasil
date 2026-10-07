# Engine resource licenses

The third-party files under `Resources/`: engine resources that development builds mount as `engine://` and exported games
pack into `Engine.pak` (Architecture §4.10, §7.6). `Resources/EngineAssets.json` names them as built-in assets (§7.1).
Everything else under `Resources/` (shaders, project templates, `EngineAssets.json`) is first-party.

Each item was approved in `Docs/Decisions/0001-approvals.md`. `python Scripts/FetchAssets.py defaults` downloads them
again from their official sources and verifies them against the pins below, which mirror `DEFAULT_RESOURCES` in
`Scripts/FetchAssets.py`; `python Scripts/FetchAssets.py defaults --check` verifies the committed files without
downloading anything. A pin changes only together with the file and this table, in one reviewed change.

| File | Asset | License | Size (bytes) | SHA-256 |
|---|---|---|---|---|
| `Resources/Environments/Studio.hdr` | Poly Haven "Studio Small 09" (`studio_small_09`), 1k Radiance HDR | CC0 1.0 | 1615248 | `e7cfda5f4e98e623db12b8bfd0184e048488e4855d9c83e2751fb44a32e80c45` |
| `Resources/Environments/Sky.hdr` | Poly Haven "Kloofendal 48d Partly Cloudy (Pure Sky)" (`kloofendal_48d_partly_cloudy_puresky`), 1k Radiance HDR | CC0 1.0 | 1435119 | `fd94c84997b8a3c353b62c2125a9b44e19509956986a126e472684432a02d798` |
| `Resources/Fonts/Inter-Regular.ttf` | Inter 4.1, Regular (`extras/ttf/Inter-Regular.ttf` of the release archive) | SIL OFL 1.1 | 411640 | `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82` |

## Poly Haven HDRIs

- **Studio.hdr:** https://polyhaven.com/a/studio_small_09 by Sergej Majboroda. Listed by
  `https://api.polyhaven.com/files/studio_small_09` and downloaded from
  `https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/1k/studio_small_09_1k.hdr` (md5 `d5d7eb9d26d341d6aa4d11c1a54fd97c`,
  as the API lists it). The built-in `engine://Environments/Studio`.
- **Sky.hdr:** https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky by Greg Zaal (original) and Jarod Guest (sky
  edits). Listed by `https://api.polyhaven.com/files/kloofendal_48d_partly_cloudy_puresky` and downloaded from
  `https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/1k/kloofendal_48d_partly_cloudy_puresky_1k.hdr` (md5
  `c69498687e876bf68d2a8b8a62234eb9`). The built-in `engine://Environments/Sky`.
- **License:** both are released under CC0 1.0 Universal (public domain dedication,
  https://creativecommons.org/publicdomain/zero/1.0/; https://polyhaven.com/license). No attribution is required; the
  authors are credited here anyway. The files are committed byte-identical to the downloads, renamed only.

## Inter

- **Source:** the release archive `https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip` (33707794 bytes,
  SHA-256 `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`), tag `v4.1` of https://github.com/rsms/inter.
  `Resources/Fonts/Inter-Regular.ttf` is its member `extras/ttf/Inter-Regular.ttf`, byte-identical. The built-in
  `engine://Fonts/Default` (FontImporter bakes its SDF atlas, §7.4).
- **License:** SIL Open Font License 1.1. Condition 2 requires that every copy carries the copyright notice and the
  license; this section is that text, copied verbatim from `LICENSE.txt` of the archive (SHA-256
  `262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a`). An exported game ships the font inside
  `Engine.pak` (§7.6), so the export must ship this text with it.

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
