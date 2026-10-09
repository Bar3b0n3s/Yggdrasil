"""M10 contract scaffolds; each skip is removed when its implementation stream lands."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase


class Basic3dTests(AutomationTestCase):
    """Acceptance checks through the real editor host."""

    @unittest.skip("M10 contract scaffold")
    def test_basic3d_template_validates_clean(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")
