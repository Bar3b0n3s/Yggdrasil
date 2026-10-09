"""M10 contract scaffolds; each skip is removed when its implementation stream lands."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase


class ThumbnailsTests(AutomationTestCase):
    """Acceptance checks through the real editor host."""

    @unittest.skip("M10 contract scaffold")
    def test_thumbnails_generated_headless(self) -> None:
        self.fail("Implement the M10 acceptance scenario before removing this skip")
