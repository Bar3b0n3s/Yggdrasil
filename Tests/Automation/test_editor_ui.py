"""M10 contract scaffolds; each skip is removed when its implementation stream lands."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase


class EditorUiTests(AutomationTestCase):
    """Acceptance checks through the real editor host."""

    @unittest.skip("M10 contract scaffold")
    def test_attach_to_windowed_editor_with_automation_allowed(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")

    @unittest.skip("M10 contract scaffold")
    def test_editor_state_tracks_panels_selection_and_lockstep(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")

    @unittest.skip("M10 contract scaffold")
    def test_editor_screenshot_after_pipelined_mutation_is_fresh(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")

    @unittest.skip("M10 contract scaffold")
    def test_scene_changed_on_disk_banner_requires_reload(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")
