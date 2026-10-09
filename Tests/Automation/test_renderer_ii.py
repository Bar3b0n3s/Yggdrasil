"""M9 contract acceptance. Replace failing bodies and remove every skip at integration."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase


@unittest.skip("M9 contract scaffold")
class RendererIITests(AutomationTestCase):
    """CPU viewport queries, per-view statistics and capture annotations."""

    def test_stats_report_pass_timings(self) -> None:
        """Read delayed per-pass GPU timings and renderer-none CPU counts in both hosts."""
        self.fail("Implement stats.get acceptance against a rendering editor and exported Runtime")

    def test_viewport_pick_deterministic(self) -> None:
        """Known scene/game pixels resolve identically without a graphics device."""
        self.fail("Implement viewport.pick acceptance across repeated calls and play interpolation")

    def test_scene_raycast_exported_runtime(self) -> None:
        """Known triangles, masks and located invalid rays in the exported Runtime."""
        self.fail("Implement scene.raycast acceptance through the standard client")

    def test_screenshot_annotations_are_capture_local(self) -> None:
        """Collider alpha, label IDs, bounds and axes never change subsequent clean captures."""
        self.fail("Implement annotated screenshot acceptance and option validation")

    def test_viewport_pick_camera_clip_interval(self) -> None:
        """Off-axis far geometry and near-clipped foreground in both projections."""
        self.fail("Implement camera-clipped viewport.pick cases without a GPU")

    def test_annotation_label_selection_and_explicit_ids(self) -> None:
        """Selection aliases, paths/prefixes, empty list, indexed errors and Runtime IDs."""
        self.fail("Implement annotation wire protocol and unchanged editor selection assertions")

    def test_render_validation_warnings(self) -> None:
        """Both render codes are stable scene warnings; fallback and emission avoid false positives."""
        self.fail("Implement project.validate against open and saved scenes, including nine shadowed spots")

    def test_renderer_debug_outputs_and_wireframe(self) -> None:
        """AO, cascade colors, overdraw palette and wireframe with unchanged solid picking."""
        self.fail("Implement GPU-gated screenshots plus headless Unsupported and located enum errors")
