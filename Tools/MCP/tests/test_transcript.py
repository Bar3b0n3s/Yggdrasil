"""The always-on transcript (Docs/Architecture.md §13.8 "Transcript", Roadmap M4)."""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

MCP_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MCP_ROOT))

from engine_mcp import transcript  # noqa: E402


class TranscriptTests(unittest.TestCase):
    def make_directory(self) -> Path:
        """A temporary directory that is removed after the test."""
        directory = Path(tempfile.mkdtemp(prefix="EngineTranscript-"))
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        return directory

    def test_transcript_written_without_env_var(self) -> None:
        directory = self.make_directory()
        environment = {key: value for key, value in os.environ.items()
                       if key != transcript.TRANSCRIPT_ENVIRONMENT_VARIABLE}
        with mock.patch.dict(os.environ, environment, clear=True):
            log = transcript.Transcript.open(directory)
            self.assertEqual(log.path, directory / "Automation" / "BuildLog.jsonl")
            first = log.append_request(1, "entity.create", {"name": "Board"})
            log.append_response(1, first, {"jsonrpc": "2.0", "id": 1, "result": {"entity": {"id": "5d1c9a7e33b04f12"},
                                                                                  "_meta": {"revision": 3}}})
            second = log.append_request(2, "scene.save", {})
            self.assertEqual((first, second), (1, 3))
            lines = [json.loads(line) for line in log.path.read_text(encoding="utf-8").splitlines()]
            self.assertEqual(lines[0]["type"], "request")
            self.assertEqual(lines[0]["method"], "entity.create")
            self.assertEqual(lines[0]["client"], transcript.CLIENT_NAME)
            self.assertEqual(lines[1]["requestLine"], 1)
            self.assertTrue(lines[1]["ok"])
            self.assertIn("5d1c9a7e33b04f12", lines[1]["summary"])
            # A reopened transcript continues the numbering.
            self.assertEqual(transcript.Transcript.open(directory).append_request(3, "scene.tree", {}), 4)

    def test_environment_variable_redirects_the_transcript(self) -> None:
        directory = self.make_directory()
        redirected = directory / "elsewhere.jsonl"
        with mock.patch.dict(os.environ, {transcript.TRANSCRIPT_ENVIRONMENT_VARIABLE: str(redirected)}):
            self.assertEqual(transcript.transcript_path(directory / "Project"), redirected)

    def test_hello_params_never_reach_the_transcript(self) -> None:
        self.assertIn("session.hello", transcript.UNRECORDED_METHODS)
        error = {"code": -32001, "message": "no entity", "data": {"errorCode": "NotFound", "detail": "no entity"}}
        summary = transcript.summarize({"jsonrpc": "2.0", "id": 1, "error": error})
        self.assertIn("-32001", summary)
        self.assertLessEqual(len(summary), 200)


if __name__ == "__main__":
    unittest.main()
