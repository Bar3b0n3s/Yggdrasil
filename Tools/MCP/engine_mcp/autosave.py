"""Read-only publication/integrity probe for EditorCore/Autosave's version-1 disk format.

Manifest.json is the commit point; stray payloads, metadata and temporary writes never advertise recovery. The editor
still validates scene semantics and opaque C++ file-clock fingerprints when offering recovery. Those timestamps are
not Unix times and must not be ordered or converted here. No writes, recovery actions or editor ownership changes.
"""

from __future__ import annotations

import json
import os
import stat
import struct
from pathlib import Path
from typing import Any

_MAX_METADATA = 1024 * 1024
_MAX_PAYLOAD = 256 * 1024 * 1024
_MAX_GENERATIONS = 32
_MASK = (1 << 64) - 1
_PRIME1 = 0x9E3779B185EBCA87
_PRIME2 = 0xC2B2AE3D27D4EB4F
_PRIME3 = 0x165667B19E3779F9
_PRIME4 = 0x85EBCA77C2B2AE63
_PRIME5 = 0x27D4EB2F165667C5


def _rotate(value: int, count: int) -> int:
    value &= _MASK
    return ((value << count) | (value >> (64 - count))) & _MASK


def _round(accumulator: int, lane: int) -> int:
    return (_rotate(accumulator + lane * _PRIME2, 31) * _PRIME1) & _MASK


def xxh64(data: bytes) -> int:
    """Seed-zero XXH64, matching Engine/Core/Hash.cpp; no additional Python dependency."""
    offset = 0
    if len(data) >= 32:
        lanes = [(_PRIME1 + _PRIME2) & _MASK, _PRIME2, 0, (-_PRIME1) & _MASK]
        while offset + 32 <= len(data):
            for index, word in enumerate(struct.unpack_from("<4Q", data, offset)):
                lanes[index] = _round(lanes[index], word)
            offset += 32
        result = sum(_rotate(value, shift) for value, shift in zip(lanes, (1, 7, 12, 18))) & _MASK
        for value in lanes:
            result = ((result ^ _round(0, value)) * _PRIME1 + _PRIME4) & _MASK
    else:
        result = _PRIME5
    result = (result + len(data)) & _MASK
    while offset + 8 <= len(data):
        result ^= _round(0, struct.unpack_from("<Q", data, offset)[0])
        result = (_rotate(result, 27) * _PRIME1 + _PRIME4) & _MASK
        offset += 8
    if offset + 4 <= len(data):
        result ^= struct.unpack_from("<I", data, offset)[0] * _PRIME1
        result = (_rotate(result, 23) * _PRIME2 + _PRIME3) & _MASK
        offset += 4
    for byte in data[offset:]:
        result ^= byte * _PRIME5
        result = (_rotate(result, 11) * _PRIME1) & _MASK
    result = ((result ^ (result >> 33)) * _PRIME2) & _MASK
    result = ((result ^ (result >> 29)) * _PRIME3) & _MASK
    return result ^ (result >> 32)


def _uint(value: Any) -> int:
    if type(value) is not int or not 0 <= value <= _MASK:
        raise ValueError("expected uint64")
    return value


def _object(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError("expected object")
    return value


def _header(value: Any, name: str) -> dict[str, Any]:
    result = _object(value)
    if result.get("Format") != name or _uint(result.get("Version")) != 1:
        raise ValueError("unsupported autosave format")
    return result


def _invalid_constant(value: str) -> None:
    raise ValueError(f"invalid JSON constant: {value}")


def _json(data: bytes) -> Any:
    return json.loads(data.decode("utf-8"), parse_constant=_invalid_constant)


def _confined(root: Path, path: Path) -> None:
    relative = path.relative_to(root)
    current = root
    for part in relative.parts:
        current /= part
        info = current.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise ValueError("linked autosave path")
    if path.resolve(strict=True) != path:
        raise ValueError("redirected autosave path")


def _read(root: Path, path: Path, limit: int) -> bytes:
    _confined(root, path)
    # Nonblocking avoids a substituted pipe on POSIX; O_NOFOLLOW rejects a substituted final symlink where supported.
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0)
    with os.fdopen(os.open(path, flags), "rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_size > limit:
            raise ValueError("autosave file exceeds its bound or is not regular")
        data = stream.read(before.st_size + 1)
        after = os.fstat(stream.fileno())
    if (len(data) != before.st_size or before.st_size != after.st_size
            or before.st_mtime_ns != after.st_mtime_ns):
        raise ValueError("autosave file changed during read")
    _confined(root, path)
    current = path.stat()
    if (current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns) != (
            after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
        raise ValueError("autosave file was replaced during read")
    return data


def _fingerprint(value: Any) -> dict[str, Any]:
    result = _object(value)
    if type(result.get("Exists")) is not bool:
        raise ValueError("invalid fingerprint existence")
    for key in ("Hash", "Size", "ModificationTime"):
        _uint(result.get(key))
    if result["Size"] > _MAX_PAYLOAD or (not result["Exists"] and any(result[key] for key in (
            "Hash", "Size", "ModificationTime"))):
        raise ValueError("invalid fingerprint")
    return result


def _matches(root: Path, path: Path, fingerprint: dict[str, Any]) -> bool:
    try:
        data = _read(root, path, _MAX_PAYLOAD)
    except FileNotFoundError:
        return not fingerprint["Exists"]
    return (fingerprint["Exists"] and len(data) == fingerprint["Size"]
            and xxh64(data) == fingerprint["Hash"])


def _scene_path(root: Path, name: str) -> Path:
    # VfsPath's portable segment rules: no traversal, Windows device aliases, trailing dot/space or alternate streams.
    reserved = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10))}
    name.encode("utf-8")
    for part in name.split("/"):
        if (not part or part in (".", "..") or part.endswith((".", " "))
                or part.split(".")[0].rstrip(" ").upper() in reserved
                or any(ord(char) < 32 or char in '\\:<>"|?*' for char in part)):
            raise ValueError("noncanonical scene path")
    if not name.endswith(".scene"):
        raise ValueError("invalid scene extension")
    return root / name


def _generation(root: Path, name: str, sequence: int) -> bool:
    directory = root / "Library" / "Autosave" / name
    metadata = _header(_json(_read(root, directory / "Metadata.json", _MAX_METADATA)), "AutosaveGeneration")
    identity = metadata.get("ProjectFile")
    if not isinstance(identity, str):
        raise ValueError("missing project identity")
    project = Path(identity)
    if (not project.is_absolute() or project.parent != root or project.suffix != ".eproj"
            or project.as_posix() != identity or _uint(metadata.get("Sequence")) != sequence):
        raise ValueError("generation belongs to another project or sequence")
    project_fingerprint = _fingerprint(metadata.get("ProjectFingerprint"))
    source_fingerprint = _fingerprint(metadata.get("SourceFingerprint"))
    if (metadata.get("Dirty") is not True or not _uint(metadata.get("DirtyRevision"))
            or not project_fingerprint["Exists"]):
        raise ValueError("generation has no captured dirty revision")
    scene_name, source, token = (metadata.get(key) for key in ("SceneName", "ScenePath", "UntitledToken"))
    if not all(isinstance(value, str) for value in (scene_name, source, token)):
        raise ValueError("invalid scene identity")
    if source:
        if token or not _matches(root, _scene_path(root, source), source_fingerprint):
            raise ValueError("scene source changed")
    elif source_fingerprint["Exists"] or len(token) != 32 or any(char not in "0123456789abcdef" for char in token):
        raise ValueError("invalid untitled identity")
    size, checksum = _uint(metadata.get("PayloadSize")), _uint(metadata.get("PayloadHash"))
    if size > _MAX_PAYLOAD:
        raise ValueError("payload exceeds its bound")
    payload = _read(root, directory / "Scene.json", size)
    if len(payload) != size or xxh64(payload) != checksum:
        raise ValueError("incomplete or corrupt payload")
    scene = _header(_json(payload), "Scene")
    if (scene.get("Name") != scene_name or not isinstance(scene.get("Entities"), list)
            or not isinstance(scene.get("ComponentVersions"), dict) or _uint(scene.get("Seed")) > 0xFFFFFFFF):
        raise ValueError("invalid scene document")
    if not _matches(root, project, project_fingerprint):
        raise ValueError("project changed")
    return not source_fingerprint["Exists"] or (size, checksum) != (source_fingerprint["Size"], source_fingerprint["Hash"])


def has_published_autosave(project_root: Path) -> bool:
    """True for a complete, dirty committed candidate with matching project/source bytes; errors return false.

    Opaque engine ModificationTime values are validated as uint64 only. Final freshness and scene-schema validation
    belongs to Autosave::FindRecovery/Recover; this crash diagnostic cannot authorize or perform recovery.
    """
    try:
        root = project_root.resolve(strict=True)
        manifest_path = root / "Library" / "Autosave" / "Manifest.json"
        published = _read(root, manifest_path, _MAX_METADATA)
        manifest = _header(_json(published), "AutosaveManifest")
        sequence = _uint(manifest.get("Sequence"))
        entries = manifest.get("Generations")
        if not isinstance(entries, list) or len(entries) > _MAX_GENERATIONS:
            return False
        generations: dict[int, str] = {}
        for entry in entries:
            entry = _object(entry)
            index = _uint(entry.get("Sequence"))
            if not 0 < index <= sequence or index in generations or entry.get("Generation") != f"g-{index:016x}":
                return False
            generations[index] = entry["Generation"]
        for index in sorted(generations, reverse=True):
            if _generation(root, generations[index], index):
                # An atomic manifest replacement during the probe must not advertise an already-retired candidate.
                return _read(root, manifest_path, _MAX_METADATA) == published
        return False
    except (OSError, ValueError, RuntimeError, MemoryError):
        return False
