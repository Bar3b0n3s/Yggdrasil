---
name: golden-images
description: Use this skill to run, update and review the engine's golden-image tests: when a rendering or editor-UI change makes a golden comparison fail, when a new golden scene or image is added, when the golden device class changes, or when a candidate image must be reviewed before it is committed. It covers the device classes and the device-class check, thresholds, smoke mode, --update-golden, the review, Golden.jsonl's kept scenes, golden scaffolds and the record a change keeps of which goldens changed and why.
---

# Golden images

Golden tests render fixed scenes and compare the images with committed references (Architecture §15.4; `Tests/Golden/README.md`; ADR 0009 decision 14). They catch unintended changes to rendering and to the editor's UI. They do not prove that an image looks good: **a passing comparison is not design quality** (ADR 0020). Every candidate is reviewed by eye before it is committed.

## What is compared

- **Scenes.** The golden scenes of `Projects/FeatureTest` (`Test::ListGoldenScenes`, `Tests/Source/Support/GoldenScene.h`): the eight M8 scenes `Scaffold/Golden.jsonl` keeps in `Assets/Scenes/Golden/` (material grid, IBL only, tonemappers, bloom, alpha modes, debug draw, SDF text, an imported glTF) and the scenes of the golden scaffolds (below), the M9 ones of `Scaffold/GoldenRendererII.jsonl` in `Assets/Scenes/GoldenRendererII/` (near and far shadow cascades, orthographic shadows, spot shadows, GTAO on, off and orthographic, selection outline, an annotated screenshot), rendered headless at 640 × 360 at fixed ticks with FXAA and fixed noise; plus the editor's UI (`editor.screenshot`: the default layout and the design captures). The tests are in `Tests/Source/Golden/` (doctest suite `Golden`).
- **References.** `Tests/Golden/<DeviceClass>/<Name>.png`, 8-bit RGBA, display-encoded. The device class is the vendor plus the driver's major version with its last digit replaced by `x` (`nvidia-61x`), or the whole major version when it has one digit (`amd-2`).
- **Thresholds.** A test fails when more than 0.1 % of the pixels differ by more than 2/255 in any channel, or any pixel differs by more than 24/255. A test may tune them with a stated reason; never loosen them to make a change pass.
- **Smoke mode.** A device class without a directory checks only that the image rendered (valid, at least two colours, mean luminance within [0.01, 0.99]) and reports "goldens missing" as a warning, never as a pass. There are no software-rasterizer goldens (Lavapipe runs in smoke mode) and no cross-device comparison.
- **The device-class check.** The committed goldens are `nvidia-61x`'s, and the NVIDIA driver's major version is held (ADR 0028 decision 9). When an image has no golden for the machine's class but a class of the same vendor has one, the driver changed: the test fails with "device class changed" instead of smoke-testing, and the golden run names the images. Go back to the held driver; only the product owner decides to take a new one, and then the goldens are renamed by `Tests/Golden/README.md`'s "Renaming a device class" (a `git mv`, the suite, a reviewed regeneration of the images the driver changed, the record and the documents that name the class).

## Running

```
python Scripts/Test.py --suite golden --config Release --junit --require-gpu
bin/Release-windows-x86_64/Tests/Tests.exe --test-suite=Golden -tc="Golden: *Tonemappers*" --require-gpu
```

A mismatch writes `<Name>-actual.png`, `<Name>-expected.png` and `<Name>-diff.png` into `bin/TestResults/Golden/` and names the image. Every golden device is created with validation and synchronization validation on; a validation message fails the test too (see `build-and-test`).

## Updating after an intended change

1. Make the change, then run the golden suite and look at each mismatch's `actual`, `expected` and `diff` images in `bin/TestResults/Golden/`. Decide for each image whether the difference is the intended effect of the change. An unexplained difference is a bug: fix it instead of updating.
2. Write candidates for the intended ones: `python Scripts/Test.py --suite golden --config Release --update-golden --require-gpu`. Every golden test of this machine's device class writes its image into `Tests/Golden/<DeviceClass>/` (the run lists the candidates written).
3. **Review the candidates in the git diff, image by image** (`git diff --stat Tests/Golden`, then view each changed PNG beside its previous version). Revert every candidate the change was not meant to touch: an update run rewrites images whose differences are only noise, and those must not be committed.
4. Run the golden suite again without `--update-golden`: it must pass.
5. **Record what changed.** The commit message (or its review record) lists each golden that changed and why ("Tonemappers: AgX curve fix shifts mid-greys"; "EditorDefaultLayout: new panel spacing"). A golden that changed without a recorded reason blocks the commit (`commit-review`).

Never update goldens to silence a failure you have not explained, and never commit candidates from a device class other than the one whose directory you are updating.

## Adding a golden

- **A new scene goes in a golden scaffold** (M18's mechanism, ADR 0028's ledger "Golden scenes"), never into `Golden.jsonl`:
  1. Write `Projects/FeatureTest/Scaffold/Golden<Name>.jsonl`, batch requests like every scaffold (`asset.create`, `scene.new`, `edit.batch`, `scene.save`; imports from `Scaffold/Sources/`, byte-identical copies of `Tests/Data/Assets/`, which a test checks), ending with `project.refreshAssets`. It writes only below directories of its own, outside `Golden.jsonl`'s kept ones: its scenes below `Assets/Scenes/Golden<Name>/` (so the soak and the perf capture, which play every scene below `Assets/Scenes/`, reach them) and its materials and textures below `Assets/Materials/Golden<Name>/` and the like.
  2. List it in `Scaffold/Scaffolds.json` with an IdSeed from the milestone's range (the phase's ledger; `Scaffold.py` refuses a seed twice) and `"GoldenDirectories": [...]`, before `Suites.jsonl`. `Scaffold.py` refuses a directory that overlaps a kept one or another golden scaffold's, and fails the run of a golden scaffold that writes outside its directories, of any scaffold that writes into them and of any that writes below a kept directory.
  3. Run `python Scripts/Scaffold.py`, then `--check` (both under the GPU lock): the scenes, their `.meta` files and their `Automation/Provenance.json` entries come from the seeded run, byte for byte on every regeneration.
  4. The scene is in the golden tests' scene list at once (`Test::ListGoldenScenes` reads `Scaffolds.json`): add its test in `Tests/Source/Golden/` with `Test::RenderGoldenScene("<Scene>")`, which compares the image named after it, and commit the reference image (below); a unit test fails while a golden scene has no image in a committed device class.
- **Golden.jsonl's kept scenes** (the eight M8 scenes, their materials, models and textures) are exactly the files `Scaffold/Baseline/Provenance.json` records, with the bytes it records (`Scaffold.py --check` compares them). They change only through a recorded regeneration: move the requests concerned into a golden scaffold, as M18's T-FTPROV moved the M9 part into `GoldenRendererII.jsonl`, remove their baseline entries, and regenerate; the goldens that change are a reviewed regeneration named in the milestone's record (ADR 0028's ledger: M23 owns it in phase 2).
- **A shadow-casting light** changes with `Rendering.ShadowMapSize`; render such scenes through the fixture that pins the size (`RendererIIGoldenFixture`), so the project's setting never changes a golden.
- **The reference image.** Run the new test once with `--update-golden` on the device class whose directory exists, review the candidate, and commit it with the test.

## Pitfalls

- **Nondeterministic inputs.** Goldens need fixed ticks, fixed noise and fixed camera poses; a golden that changes between two runs on one machine has an unseeded or time-dependent input. Fix the input, never the threshold.
- **Asset changes.** Any change to a golden scene, its materials or imported fixtures changes the images: treat it as a rendering change with a recorded reason.
- **Entity ids in an image.** `AnnotatedScreenshot` labels its entities with the first six hex digits of their UUIDs, which its golden scaffold's IdSeed and request order fix: a change to either changes the labels' pixels (M18's recorded regeneration did), and the test's pinned UUIDs.
- **New device class.** A machine whose driver crosses a version decade gets a new device class: next to the same vendor's goldens the device-class check fails the run (above); another vendor's class runs in smoke mode until someone commits a reviewed set for it.
