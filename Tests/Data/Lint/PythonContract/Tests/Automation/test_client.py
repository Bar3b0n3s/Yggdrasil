"""Seeded defects for the contract step's Python skip rule (Scripts/Lint.py, Tests/Data/Lint/Fixtures.json)."""

from __future__ import annotations

import unittest
from unittest import skip


class ClientTests(unittest.TestCase):
    @unittest.skip("contract stub")
    def test_decorated(self) -> None:
        self.assertTrue(True)

    @skip("a bare name")
    def test_bare(self) -> None:
        self.assertTrue(True)

    @unittest.skipIf(True, "a condition")
    def test_conditional(self) -> None:
        self.assertTrue(True)

    def test_runtime(self) -> None:
        self.skipTest("at run time")

    def test_raised(self) -> None:
        raise unittest.SkipTest("raised")

    def test_control(self) -> None:
        # The allowed controls: unittest.skip named in a comment, and a string that spells "@unittest.skip".
        self.assertEqual("@unittest.skip", "@unittest.skip")


@unittest.expectedFailure
class FailingTests(unittest.TestCase):
    def test_fails(self) -> None:
        self.assertTrue(False)


class PytestTests(unittest.TestCase):
    def test_imperative(self) -> None:
        import pytest

        pytest.skip("at run time")

    def test_import_or_skip(self) -> None:
        import pytest

        pytest.importorskip("numpy")
