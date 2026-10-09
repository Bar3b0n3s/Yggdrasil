"""M10 contract scaffolds; each skip is removed when its implementation stream lands."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase


class AutosaveTests(AutomationTestCase):
    """Acceptance checks through the real editor host."""

    @unittest.skip("M10 contract scaffold")
    def test_autosave_and_recovery_after_kill(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")

    @unittest.skip("M10 contract scaffold")
    def test_autosave_read_only_writes_nothing(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")

    @unittest.skip("M10 contract scaffold")
    def test_recovery_refuses_replaced_project_and_escaping_paths(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")

    @unittest.skip("M10 contract scaffold")
    def test_device_loss_autosaves_current_dirty_scene(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")

    @unittest.skip("M10 contract scaffold")
    def test_recovery_freshness_survives_restart_with_equal_timestamps(self) -> None:
        self.fail("Assert dirty derived bytes recover across restart without timestamp ordering")

    @unittest.skip("M10 contract scaffold")
    def test_project_open_recover_false_is_not_an_inspection_only_call(self) -> None:
        self.fail("Assert one open retains the lock and a second open fails with InvalidState")
