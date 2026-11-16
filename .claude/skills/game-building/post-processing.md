# Post-processing

A reference page of the `game-building` skill (`skills/game-building`), for its step "3. Scenes, assets and prefabs": the `PostProcess` component's exposure, colour grading, reflections and anti-aliasing (version 2, M26). Lights, shadows, glow, GTAO and framing are on `skills/game-building/look`; every field's range and default is in `reference/Components`.

- **Defaults render as before.** A scene's one `PostProcess` (or its absence) gives manual exposure at `ExposureEV` 0, AgX, no LUT, SSR off and FXAA (`AntiAliasing` `"Fxaa"`). Each effect below is opt-in; set it before recording replays, since the `PostProcess` is hashed like every look component, so changing one later means re-recording (`skills/game-building/replays`).
- **The order.** The HDR image is anti-aliased (TAA), bloomed and metered (auto-exposure), then exposed, tone-mapped, graded by the LUT, encoded and dithered; FXAA runs on the encoded image. `stats.get`'s pass rows show what ran: `SSR`, `TAA`, `Exposure`, `Tonemap` and `FXAA`.

## Automatic exposure

- `ExposureMode` `"Auto"` meters each frame: a 64-bin histogram of the image's luminance, whose pixels between the 50th and 95th percentiles (bright highlights such as emissive lamps left out) are exposed to middle grey (0.18), clamped to [`AutoExposureMinEV`, `AutoExposureMaxEV`] (-6 and 12 by default). `ExposureEV` stays a compensation on top: +1 is one stop brighter than the metered exposure.
- The exposure adapts at `AutoExposureSpeedUp` EV per second towards a brighter target and `AutoExposureSpeedDown` towards a darker one (3 and 1 by default), so walking from a dark room into the sun takes a moment to settle: six stops take two seconds at the default `AutoExposureSpeedUp`. It converges at once whenever the view's history is reset: a view's first frame, play started or stopped, a scene opened or swapped, a camera cut (another camera, projection kind or viewport size) and every screenshot.
- A narrow range (`AutoExposureMinEV` equal to `AutoExposureMaxEV`) fixes the exposure; a maximum below the minimum is refused, as a validation error at `AutoExposureMaxEV`.
- Pixel-art mode (a `PixelPerfectCamera`, `skills/game-building/sprites-2d`) ignores it and exposes at 1, so sprite texels reach the screen unchanged; the wireframe and the debug views expose manually at `ExposureEV`.

## Colour-grading LUTs

- `asset_import` a `.cube` file (Adobe/IRIDAS 3D tables of 2 to 64 entries per side, with `DOMAIN_MIN`/`DOMAIN_MAX` or `LUT_3D_INPUT_RANGE`; 1D tables are refused), then set `Lut` to its path. `LutIntensity` (0 to 1, default 1) blends the graded colour with the ungraded one. A LUT is never block-compressed and has no import settings.
- The table is addressed by the display-encoded (sRGB) colour after the tone mapping curve and returns a display-encoded colour, as grading tools author them: export a LUT for an sRGB display from a grading tool, not a log or scene-linear one.
- `engine://Luts/Identity` is a built-in identity table of 33 entries per side; it changes an image only by rounding (at most one level), so it is the neutral starting point for an in-game grade switched by script (`(entity.PostProcess :: PostProcess).Lut = Assets.Load("engine://Luts/Identity")`).
- Pixel-art mode never grades (as above). A LUT that fails to load grades nothing, and the error is logged once.

## Screen-space reflections

- `SsrEnabled` makes glossy opaque surfaces reflect what is on screen, replacing the environment's (or reflection probe's) reflection where a ray finds a surface and keeping it where it misses. With SSR off nothing is recorded and the image is exactly as without SSR.
- A pixel traces when its material's roughness is at most `SsrMaxRoughness` (0.6): surfaces up to half of it take the traced reflection whole, and it fades back to the probes' reflection over the upper half; raise `SsrMaxRoughness` for blurrier floors, lower it to keep SSR on mirrors and polished metal. `SsrIntensity` (0 to 1) scales how much of the probes' reflection is replaced.
- `SsrMaxDistance` (50 m) bounds the ray; `SsrThickness` (0.2 m) is how thick a surface is assumed behind its depth: too thin and rays slip through thin pillars, too thick and a ray that passes behind an object hits it, stretching the object in the reflection. Reflections fade over the screen's outer edge and the last quarter of the distance.
- **Limits.** One mirror ray per pixel, with no roughness blur: a glossy surface's traced reflection is as sharp as a mirror's and fades to the probes' prefiltered one rather than blurring. Screen space only: what is off screen, hidden behind a surface thicker than `SsrThickness` or seen from its back falls back to the probes, so a reflection's top edge can fade where the reflected object leaves the screen. Transparent (`Blend`) surfaces are neither traced nor reflected. Place a baked `ReflectionProbe` (`lighting_bake`) for what SSR cannot see.

## Anti-aliasing

- `AntiAliasing` `"Fxaa"` (the default) smooths edges on the final image in one pass; `"Taa"` jitters the camera by a sub-pixel offset every frame and resolves each frame against the view's history, reprojected by each pixel's motion and clamped to its neighbourhood, which smooths thin and moving edges and specular shimmer better; `"None"` keeps hard edges.
- TAA needs a few frames after a reset to resolve: the first frame after play starts, a scene change or a camera cut shows the plain jittered image. Particles write the motion they were drawn with where they are at least half opaque, so they stay sharp under TAA and only their faint fringes ghost slightly; a terrain carries the camera's motion but not its own, so only a moving terrain ghosts.
- Pixel-art mode ignores TAA (it acts as `"None"`), as it ignores FXAA.

## Screenshots, goldens and tests

- `viewport_screenshot`, `editor_screenshot`, `Test.Screenshot` and the editor's thumbnails of a scene with TAA or auto-exposure reset the view's history and render the same frame eight times (the jitter sequence) before keeping the last image, so two screenshots of one scene and tick are byte-identical; a scene without either renders once. `editor_screenshot` renders its Scene and Game views that way in the frame it captures, and the live views go on from there. Expect such captures to take several times longer.
- In play, the game view keeps its history between frames, so a screenshot taken during play still starts from a reset history and may differ slightly from what the window showed at that tick (the window's TAA had resolved over earlier, moving frames). Compare screenshots with screenshots, never with frames read back from the window.
- After a GPU device loss the renderer starts its temporal history afresh: the first frames after the recovery may differ from an uninterrupted run, while screenshots stay identical.
