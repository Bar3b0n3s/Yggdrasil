---
name: golden-images
description: Use this skill to run, update and review the engine's golden-image tests: when a rendering or editor-UI change makes a golden comparison fail, when a new golden scene or image is added, or when a candidate image must be reviewed before it is committed. It covers the device classes, thresholds, smoke mode, --update-golden, the review, Golden.jsonl and the record a change keeps of which goldens changed and why.
---

# Golden images

Golden tests render fixed scenes and compare the images with committed references (Architecture §15.4; `Tests/Golden/README.md`; ADR 0009 decision 14). They catch unintended changes to rendering and to the editor's UI. They do not prove that an image looks good: **a passing comparison is not design quality** (ADR 0020). Every candidate is reviewed by eye before it is committed.

## What is compared

- **Scenes.** `Projects/FeatureTest/Assets/Scenes/Golden/*.scene` (17 scenes: material grid, IBL only, near and far shadow cascades, orthographic shadows, spot shadows, GTAO on, off and orthographic, tonemappers, bloom, alpha modes, debug draw, selection outline, SDF text, an imported glTF, an annotated screenshot), rendered headless at 640 × 360 at fixed ticks with FXAA and fixed noise; plus the editor's UI (`editor.screenshot`: the default layout and the design captures). The tests are in `Tests/Source/Golden/` (doctest suite `Golden`).
- **References.** `Tests/Golden/<DeviceClass>/<Name>.png`, 8-bit RGBA, display-encoded. The device class is the vendor plus the driver's major version with its last digit replaced by `x` (`nvidia-61x`), or the whole major version when it has one digit (`amd-2`).
- **Thresholds.** A test fails when more than 0.1 % of the pixels differ by more than 2/255 in any channel, or any pixel differs by more than 24/255. A test may tune them with a stated reason; never loosen them to make a change pass.
- **Smoke mode.** A device class without a directory checks only that the image rendered (valid, at least two colours, mean luminance within [0.01, 0.99]) and reports "goldens missing" as a warning, never as a pass. There are no software-rasterizer goldens (Lavapipe runs in smoke mode) and no cross-device comparison.

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

- **A new scene.** Golden scenes are written by the committed scaffold `Projects/FeatureTest/Scaffold/Golden.jsonl`, through automation like every FeatureTest file. Append the new scene's requests at its end; do not change or reorder existing lines: the M9 regression test pins 30 of them and the golden scenes' ids depend on the order. The fixtures it imports live in `Projects/FeatureTest/Scaffold/Sources/`, byte-identical copies of `Tests/Data/Assets/` (a test checks that). Then add the test in `Tests/Source/Golden/` with `Test::RenderGoldenScene` and the scene name in `Tests/Source/Support/GoldenSceneTests.cpp`.
- **A shadow-casting light** changes with `Rendering.ShadowMapSize`; render such scenes through the fixture that pins the size (`RendererIIGoldenFixture`), so the project's setting never changes a golden.
- **The reference image.** Run the new test once with `--update-golden` on the device class whose directory exists, review the candidate, and commit it with the test.

## Pitfalls

- **Nondeterministic inputs.** Goldens need fixed ticks, fixed noise and fixed camera poses; a golden that changes between two runs on one machine has an unseeded or time-dependent input. Fix the input, never the threshold.
- **Asset changes.** Any change to a golden scene, its materials or imported fixtures changes the images: treat it as a rendering change with a recorded reason.
- **New device class.** A machine whose driver crosses a version decade gets a new device class and runs in smoke mode until someone commits a reviewed set for it.
