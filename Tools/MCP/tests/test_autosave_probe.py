"""Crash recovery publication probes on generated temporary files; no editor, SDK, or user projects required."""

from __future__ import annotations

import copy
import json
import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Any
from unittest import mock

MCP_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MCP_ROOT))
sys.path.insert(0, str(MCP_ROOT.parents[1] / "Tools" / "Automation"))

from engine_mcp import autosave  # noqa: E402
from engine_mcp.connection import EditorConnection  # noqa: E402


class AutosaveProbeTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory(prefix="EngineRecoveryProbe-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.project = self.root / "Game.eproj"
        self.project.write_bytes(b'{"Format":"Project","Version":1,"Name":"Game"}\n')
        self.source = self.root / "Assets" / "Scenes" / "Main.scene"
        self.source.parent.mkdir(parents=True)
        self.source.write_bytes(self.scene_bytes(1))
        self.folder = self.root / "Library" / "Autosave"
        self.folder.mkdir(parents=True)
        self.manifest_path = self.folder / "Manifest.json"
        self.payload = self.scene_bytes(2)

    @staticmethod
    def scene_bytes(seed: int) -> bytes:
        return json.dumps({"Format": "Scene", "Version": 1, "Name": "Main", "Seed": seed,
                           "ComponentVersions": {}, "Entities": []}).encode("utf-8")

    @staticmethod
    def fingerprint(data: bytes | None) -> dict[str, Any]:
        return {"Exists": data is not None, "Hash": autosave.xxh64(data) if data is not None else 0,
                "Size": len(data) if data is not None else 0,
                "ModificationTime": (1 << 64) - 1 if data is not None else 0}

    @staticmethod
    def write_json(path: Path, value: Any) -> None:
        path.write_text(json.dumps(value), encoding="utf-8")

    def prepare(self, sequence: int = 1, source: str = "Assets/Scenes/Main.scene") -> tuple[Path, dict[str, Any]]:
        directory = self.folder / f"g-{sequence:016x}"
        directory.mkdir(exist_ok=True)
        (directory / "Scene.json").write_bytes(self.payload)
        metadata = {"Format": "AutosaveGeneration", "Version": 1, "Sequence": sequence,
                    "ProjectFile": self.project.as_posix(), "ProjectFingerprint": self.fingerprint(self.project.read_bytes()),
                    "SourceFingerprint": self.fingerprint(self.source.read_bytes() if source and self.source.exists() else None),
                    "ScenePath": source, "SceneName": "Main", "UntitledToken": "" if source else "ab" * 16,
                    "DirtyRevision": 8, "Dirty": True, "Reason": 2,
                    "PayloadSize": len(self.payload), "PayloadHash": autosave.xxh64(self.payload)}
        self.write_json(directory / "Metadata.json", metadata)
        return directory, metadata

    def publish(self, *sequences: int) -> dict[str, Any]:
        manifest = {"Format": "AutosaveManifest", "Version": 1, "Sequence": max(sequences, default=0),
                    "Generations": [{"Generation": f"g-{index:016x}", "Sequence": index} for index in sequences]}
        self.write_json(self.manifest_path, manifest)
        return manifest

    def test_xxh64_matches_engine_reference_vectors(self) -> None:
        # Same independent expected values as Tests/Source/Engine/Core/HashTests.cpp, including all tail paths.
        generator = 2654435761
        buffer = bytearray()
        for _ in range(2367):
            buffer.append(generator >> 56)
            generator = (generator * 11400714785074694797) & ((1 << 64) - 1)
        vectors = {0: 0xEF46DB3751D8E999, 1: 0xE934A84ADB052768, 14: 0x8282DCC4994E35C8,
                   100: 0x4BFE019CD91D9EA4, 222: 0xB641AE8CB691C174, 2367: 0xA82418DDEC0EA581}
        for size, expected in vectors.items():
            with self.subTest(size=size):
                self.assertEqual(autosave.xxh64(bytes(buffer[:size])), expected)
        self.assertEqual(autosave.xxh64(b"abc"), 0x44BC2CF5AD770999)

    def test_only_manifest_publication_makes_a_complete_generation_available(self) -> None:
        connection = EditorConnection(self.root, supervised=False)
        self.assertFalse(connection.autosave_available())
        directory, _ = self.prepare()
        self.assertFalse(connection.autosave_available())
        self.manifest_path.with_suffix(".json.tmp").write_text("partial", encoding="utf-8")
        self.assertFalse(connection.autosave_available())
        self.publish(1)
        self.assertTrue(connection.autosave_available())
        (directory / "Scene.json").unlink()
        self.assertFalse(connection.autosave_available())

    def test_unpublished_partial_generation_does_not_hide_the_previous_commit(self) -> None:
        self.prepare(1)
        self.publish(1)
        partial = self.folder / "g-0000000000000002"
        partial.mkdir()
        (partial / "Scene.json").write_bytes(b"partial")
        self.assertTrue(autosave.has_published_autosave(self.root))

    def test_invalid_manifest_identities_formats_and_types_are_unavailable(self) -> None:
        self.prepare()
        valid = self.publish(1)
        changes = [("Format", "Scene"), ("Version", 2), ("Version", True), ("Sequence", -1),
                   ("Sequence", 1.0), ("Sequence", 1 << 64), ("Generations", {}), ("Generations", [None]),
                   ("Generations", valid["Generations"] * 2), ("Generations", valid["Generations"] * 33),
                   ("Generations", [{"Generation": "../escape", "Sequence": 1}]),
                   ("Generations", [{"Generation": "g-0000000000000002", "Sequence": 1}]),
                   ("Generations", [{"Generation": "g-0000000000000002", "Sequence": 2}])]
        for key, value in changes:
            with self.subTest(key=key, value=value):
                changed = copy.deepcopy(valid)
                changed[key] = value
                self.write_json(self.manifest_path, changed)
                self.assertFalse(autosave.has_published_autosave(self.root))

    def test_missing_malformed_oversized_and_nonregular_files_are_unavailable(self) -> None:
        directory, metadata = self.prepare()
        self.publish(1)
        path = directory / "Metadata.json"
        for data in (b"{", b"\xff", b"[]", b'{"Version": NaN}', b"[" * 2000, b" " * (1024 * 1024 + 1)):
            with self.subTest(length=len(data)):
                path.write_bytes(data)
                self.assertFalse(autosave.has_published_autosave(self.root))
        self.write_json(path, metadata)
        self.assertTrue(autosave.has_published_autosave(self.root))
        path.unlink()
        path.mkdir()
        self.assertFalse(autosave.has_published_autosave(self.root))

    def test_generation_checks_metadata_identity_paths_dirty_state_and_integrity(self) -> None:
        directory, valid = self.prepare()
        self.publish(1)
        changes = [("Version", 2), ("Sequence", 2), ("Dirty", False), ("Dirty", 1), ("DirtyRevision", 0),
                   ("ProjectFile", (self.root.parent / "Elsewhere.eproj").as_posix()),
                   ("PayloadSize", len(self.payload) + 1), ("PayloadSize", 256 * 1024 * 1024 + 1),
                   ("PayloadHash", valid["PayloadHash"] ^ 1), ("ProjectFingerprint", {}),
                   ("SourceFingerprint", {**valid["SourceFingerprint"], "ModificationTime": -1}),
                   ("ScenePath", "../Main.scene"), ("ScenePath", "/Main.scene"), ("ScenePath", "Assets/NUL.scene"),
                   ("ScenePath", "Assets\\Main.scene"), ("ScenePath", "Assets//Main.scene"),
                   ("ScenePath", "Assets/Main.scene:stream"), ("ScenePath", "Assets/Main.scene "),
                   ("ScenePath", "Assets/Main.scene\u0000"), ("UntitledToken", "a" * 32)]
        for key, value in changes:
            with self.subTest(key=key, value=value):
                changed = copy.deepcopy(valid)
                changed[key] = value
                self.write_json(directory / "Metadata.json", changed)
                self.assertFalse(autosave.has_published_autosave(self.root))

    def test_truncated_and_same_length_corrupt_payloads_are_unavailable(self) -> None:
        directory, _ = self.prepare()
        self.publish(1)
        for data in (self.payload[:-1], b"x" + self.payload[1:], self.payload + b"x"):
            (directory / "Scene.json").write_bytes(data)
            self.assertFalse(autosave.has_published_autosave(self.root))

    def test_checksummed_payload_still_needs_a_scene_document(self) -> None:
        for payload in (b"{", b"[]", self.payload.replace(b'"Version": 1', b'"Version": 2'),
                        self.payload.replace(b'"Entities": []', b'"Entities": null')):
            with self.subTest(payload=payload):
                self.payload = payload
                self.prepare()
                self.publish(1)
                self.assertFalse(autosave.has_published_autosave(self.root))

    def test_project_or_source_replacement_and_identical_payload_are_unavailable(self) -> None:
        project_bytes, source_bytes = self.project.read_bytes(), self.source.read_bytes()
        for kind in ("project", "source", "identical"):
            with self.subTest(kind=kind):
                self.project.write_bytes(project_bytes)
                self.source.write_bytes(source_bytes)
                self.payload = source_bytes if kind == "identical" else self.scene_bytes(2)
                self.prepare()
                self.publish(1)
                if kind != "identical":
                    (self.project if kind == "project" else self.source).write_bytes(b"replaced")
                self.assertFalse(autosave.has_published_autosave(self.root))

    def test_untitled_and_not_yet_saved_scenes_are_complete_candidates(self) -> None:
        directory, metadata = self.prepare(source="")
        self.publish(1)
        self.assertTrue(autosave.has_published_autosave(self.root))
        metadata["UntitledToken"] = "../bad"
        self.write_json(directory / "Metadata.json", metadata)
        self.assertFalse(autosave.has_published_autosave(self.root))
        self.source.unlink()
        self.prepare()
        self.assertTrue(autosave.has_published_autosave(self.root))

    def test_generation_order_is_manifest_sequence_not_file_timestamp(self) -> None:
        self.prepare(1)
        self.payload = self.source.read_bytes()
        latest, _ = self.prepare(2)
        self.publish(1, 2)
        os.utime(latest / "Metadata.json", ns=(0, 0))
        # A clean newest generation is skipped; the retained dirty one is still available. Opaque times are not Unix times.
        self.assertTrue(autosave.has_published_autosave(self.root))
        (latest / "Metadata.json").write_bytes(b"broken")
        self.assertFalse(autosave.has_published_autosave(self.root))

    def test_read_errors_links_and_changed_publication_do_not_escape_the_probe(self) -> None:
        directory, _ = self.prepare()
        self.publish(1)
        with mock.patch("engine_mcp.autosave.os.open", side_effect=PermissionError("denied")):
            self.assertFalse(autosave.has_published_autosave(self.root))
        original_lstat = Path.lstat

        def linked(path: Path) -> Any:
            return SimpleNamespace(st_mode=stat.S_IFLNK) if path == directory else original_lstat(path)

        with mock.patch.object(Path, "lstat", linked):
            self.assertFalse(autosave.has_published_autosave(self.root))
        original_hash = autosave.xxh64

        def republish(data: bytes) -> int:
            if data == self.payload:
                self.publish()
            return original_hash(data)

        with mock.patch("engine_mcp.autosave.xxh64", side_effect=republish):
            self.assertFalse(autosave.has_published_autosave(self.root))

    def test_crash_diagnostic_uses_the_probe_and_attached_shutdown_only_disconnects(self) -> None:
        editor = SimpleNamespace(process=SimpleNamespace(returncode=4), output_tail=["last log"], join_readers=mock.Mock())
        supervised = EditorConnection(self.root, supervised=True, editor=editor)
        self.prepare()
        self.assertFalse(supervised.make_crashed().to_json()["autosaveAvailable"])
        self.publish(1)
        self.assertTrue(supervised.make_crashed().to_json()["autosaveAvailable"])
        client = mock.Mock()
        attached = EditorConnection(self.root, supervised=False, client=client)
        self.assertTrue(attached.autosave_available())
        outcome = attached.shutdown(force=True)
        self.assertTrue(outcome["disconnected"])
        self.assertFalse(outcome["shutDown"])
        client.close.assert_called_once()
        client.request.assert_not_called()
        self.assertTrue(self.manifest_path.is_file())
        self.assertFalse(EditorConnection(None, supervised=False).autosave_available())
