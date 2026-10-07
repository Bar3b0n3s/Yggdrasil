# Golden images

Reference images of the golden tests (Architecture §15.4, `Docs/Decisions/0009-m5-decisions.md` decision 14).

- **Layout:** `Tests/Golden/<DeviceClass>/<Name>.png`, one directory per device class. The device class is `GraphicsDeviceInfo::DeviceClass`: the vendor and the driver's major version with its last digit replaced by `x` (`nvidia-58x`), or the whole major version when it has one digit (`amd-2`). Images are 8-bit RGBA PNGs of the display-encoded values.
- **Comparison:** a golden test fails when more than 0.1 % of the pixels differ by more than 2/255 in any channel, or any pixel by more than 24/255. The failure writes `<Name>-actual.png`, `<Name>-expected.png` and `<Name>-diff.png` into `bin/TestResults/Golden/`.
- **Smoke mode:** on a device class without a directory here, each golden test checks only that the image rendered (a valid image with at least two colours and a mean luminance in [0.01, 0.99]) and reports "goldens missing" as a warning, never as a pass of the comparison. There is no cross-device comparison and no software-rasterizer golden.
- **Updating:** `python Scripts/Test.py --suite golden --update-golden` writes every golden test's image of this machine's device class as the new candidate. Review the candidates in the git diff, image by image, before committing them; an unreviewed golden makes the comparison meaningless.
