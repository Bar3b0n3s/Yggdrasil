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
