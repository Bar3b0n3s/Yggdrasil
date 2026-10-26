# Golden images

Reference images of the golden tests (Architecture §15.4, `Docs/Decisions/0009-m5-decisions.md` decision 14).

- **Layout:** `Tests/Golden/<DeviceClass>/<Name>.png`, one directory per device class. The device class is `GraphicsDeviceInfo::DeviceClass`: the vendor and the driver's major version with its last digit replaced by `x` (`nvidia-58x`), or the whole major version when it has one digit (`amd-2`). Images are 8-bit RGBA PNGs of the display-encoded values. The goldens are those of `nvidia-61x` only, and the NVIDIA driver's major version is held for the backlog program (`Docs/Decisions/0028-backlog-program.md` decision 9).
- **Comparison:** a golden test fails when more than 0.1 % of the pixels differ by more than 2/255 in any channel, or any pixel by more than 24/255. The failure writes `<Name>-actual.png`, `<Name>-expected.png` and `<Name>-diff.png` into `bin/TestResults/Golden/`.
- **Smoke mode:** on a device class without a directory here, each golden test checks only that the image rendered (a valid image with at least two colours and a mean luminance in [0.01, 0.99]) and reports "goldens missing" as a warning, never as a pass of the comparison. There is no cross-device comparison and no software-rasterizer golden.
- **The device-class check:** when the image has no golden for the machine's device class but another class of the same vendor has one (`nvidia-62x` next to `nvidia-61x`), the driver's major version changed, and the test fails with "device class changed", naming the classes that have it, instead of smoke-testing (`Test::GoldenOutcome::DeviceClassChanged`; the golden run's detail names the images). Go back to the held driver, or rename the goldens as below.
- **Updating:** `python Scripts/Test.py --suite golden --update-golden` writes every golden test's image of this machine's device class as the new candidate. Review the candidates in the git diff, image by image, before committing them; an unreviewed golden makes the comparison meaningless.
- **Scenes:** most goldens render a golden scene of `Projects/FeatureTest`: the scenes `Scaffold/Golden.jsonl` keeps in `Assets/Scenes/Golden/`, and the scenes of the golden scaffolds that `Scaffold/Scaffolds.json` lists with their `GoldenDirectories` (`Test::ListGoldenScenes`; the golden-images skill describes adding one). Every golden scene has its image in every device class directory here (a unit test checks it).

## Renaming a device class

The driver is held (ADR 0028 decision 9), so a new device class is an accident until the product owner decides to take the new driver. Only then, on the machine with the new driver:

1. Rename the directory with history: `git mv Tests/Golden/<old class> Tests/Golden/<new class>` (the images keep their history, and the old class's directory goes: no machine renders it any more).
2. Run the golden suite: `python Scripts/Test.py --suite golden --config Release --junit --require-gpu`. Every image that matches stays as it is, byte for byte.
3. Review each mismatch's `actual`, `expected` and `diff` images in `bin/TestResults/Golden/` as for any rendering change: a difference the driver explains (edge pixels, the last bits of a filter) is regenerated with `--update-golden --filter "<test case>"`, the failing test case's name as the golden run reports it, and reviewed in the diff; anything else is a bug to fix first. A scene golden's case is named after its image (`Golden: <Name>`), the editor-design cases of `Tests/Source/Golden/EditorDesignTests.cpp` are not (`EditorDesignSelected2560x1440-2x.png` is "Golden: EditorDesign selected object fits 2560x1440 at 2x").
4. Record the rename in the change: both driver versions, the goldens that changed and why (the commit's record, as every golden change).
5. Update the documents that name the class: Architecture §15.4 and §17, this file, the golden-images skill, `Docs/Backlog.md` §6 and §8, and ADR 0028 decision 9 through a ledger amendment on main.
