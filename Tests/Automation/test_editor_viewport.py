"""M10 contract scaffolds; each skip is removed when its implementation stream lands."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase


class EditorViewportTests(AutomationTestCase):
    """Acceptance checks through the real editor host."""

    @unittest.skip("M10 contract scaffold")
    def test_viewport_camera_get_set_and_invalid_pose(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")

    @unittest.skip("M10 contract scaffold")
    def test_viewport_frame_fits_entities(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")

    @unittest.skip("M10 contract scaffold")
    def test_viewport_set_options_preserves_unspecified_members(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")

    @unittest.skip("M10 contract scaffold")
    def test_viewport_controls_without_renderer(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")
