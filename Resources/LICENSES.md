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
  license: `Resources/Licenses/Inter-OFL.txt` is that text, byte-identical to `LICENSE.txt` of the archive (SHA-256
  `262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a`). An exported game ships the font inside
  `Engine.pak` (§7.6), so every export ships this text with it in `THIRD_PARTY_NOTICES.txt` (Docs/Decisions/
  0024-m15-contract.md decision 8).

## Vulkan SDK components of macOS bundles

An exported macOS game is a bundle that carries the Vulkan loader and MoltenVK in `Contents/Frameworks` (Architecture
§14.1, Docs/Decisions/0024-m15-contract.md decision 5). `Scripts/Build.py` stages both from the pinned Vulkan SDK of
`Scripts/Lib/Toolchain.json` only, and its report records each staged file's SHA-256. Both are Apache-2.0; their license
texts, committed under `Resources/Licenses/`, ship in the macOS notices only (Shipped `macOS` below).

- **Vulkan Loader:** https://github.com/KhronosGroup/Vulkan-Loader, the loader of the pinned SDK (tag
  `vulkan-sdk-1.4.350.0`). `Resources/Licenses/Vulkan-Loader-LICENSE.txt` is `LICENSE.txt` at that tag, byte-identical
  (SHA-256 `43c0a37e6a0fa7ff3c843b3ec5a4fac84b712558ddac103fbd4c1649662a9ece`).
- **MoltenVK:** https://github.com/KhronosGroup/MoltenVK, the MoltenVK of the pinned SDK. `Resources/Licenses/
  MoltenVK-LICENSE.txt` is `LICENSE` of the releases `v1.4.1` and `v1.4.2`, which are byte-identical (SHA-256
  `cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30`).

## Notices

The rows `Scripts/Lib/notices.py` turns into `THIRD_PARTY_NOTICES.txt` together with the `## Notices` sections of
`Vendor/*/VENDOR.md`: one per engine resource in `Engine.pak` and per Vulkan SDK component of macOS bundles. License files
are relative to `Resources/`; `-` means no notice text is required (CC0).

| Name | Upstream | Version | License | Shipped | License files |
|---|---|---|---|---|---|
| Inter | https://github.com/rsms/inter | 4.1 | OFL-1.1 | yes | `Licenses/Inter-OFL.txt` |
| Poly Haven Kloofendal 48d Partly Cloudy (Pure Sky) | https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky | 1k HDR | CC0-1.0 | yes | - |
| Poly Haven Studio Small 09 | https://polyhaven.com/a/studio_small_09 | 1k HDR | CC0-1.0 | yes | - |
| Vulkan Loader | https://github.com/KhronosGroup/Vulkan-Loader | Vulkan SDK 1.4.350.0 | Apache-2.0 | macOS | `Licenses/Vulkan-Loader-LICENSE.txt` |
| MoltenVK | https://github.com/KhronosGroup/MoltenVK | Vulkan SDK 1.4.350.0 | Apache-2.0 | macOS | `Licenses/MoltenVK-LICENSE.txt` |
