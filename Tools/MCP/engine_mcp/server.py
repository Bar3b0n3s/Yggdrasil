"""MCP tool registration and the stdio server (Docs/Architecture.md §13.8), on the official MCP Python SDK's low-level
server API (dynamic tool registration).

Tools:
  - bridge-local: editor_launch {project, create?, template?, headless?, renderer?}, editor_attach {project?, pid?},
    editor_status, editor_shutdown, engine_methods {domain?}, engine_call {method, params} (the escape hatch to every
    method, including project.upgrade);
  - proxied: every method of Tools/MCP/catalog.json (methods flagged exposeAsTool, named domain_verb), listed before any
    editor runs, each with the catalogue's compact inputSchema (component and asset values are free-form objects, so the
    tool list does not flood the agent's context; full schemas stay available through rpc.discover and
    component_schema).
Results return as structuredContent plus a short text summary that always includes the _meta delta; text over 48 KB is
truncated with a pointer to the offloaded file; screenshots (M5) return image content.

Contract stubs (M4 stream D).
"""

from __future__ import annotations

import dataclasses
from pathlib import Path
from typing import Any

CATALOG_PATH = Path(__file__).resolve().parent.parent / "catalog.json"
SERVER_NAME = "engine"
MAX_TEXT_BYTES = 48 * 1024
LOCAL_TOOL_NAMES = (
    "editor_launch", "editor_attach", "editor_status", "editor_shutdown", "engine_methods", "engine_call",
)


@dataclasses.dataclass(frozen=True)
class CatalogTool:
    """One proxied tool of catalog.json."""

    name: str
    method: str
    description: str
    input_schema: dict[str, Any]
    mutates: bool
    supports_dry_run: bool
    timeout_seconds: int


def load_catalog(path: Path = CATALOG_PATH) -> list[CatalogTool]:
    """The tools of catalog.json ({"Format": "McpCatalog", "Version": 1, "Tools": [...]}). Raises ValueError for another
    format or version."""
    raise NotImplementedError("contract stub: server.load_catalog (M4 stream D)")


def local_tool_schemas() -> dict[str, dict[str, Any]]:
    """The inputSchema of each bridge-local tool, by name."""
    raise NotImplementedError("contract stub: server.local_tool_schemas (M4 stream D)")


def format_result(response: dict[str, Any]) -> tuple[dict[str, Any], str]:
    """The structuredContent and the text summary (with the _meta delta, truncated at MAX_TEXT_BYTES) of a response."""
    raise NotImplementedError("contract stub: server.format_result (M4 stream D)")


def build_server(repository_root: Path) -> Any:
    """The SDK server with every tool registered (the SDK's low-level Server; typed Any so this module imports without
    the SDK for the Lint step)."""
    raise NotImplementedError("contract stub: server.build_server (M4 stream D)")


def main() -> int:
    """Serves MCP on stdio until the client disconnects; returns the process exit code."""
    raise NotImplementedError("contract stub: server.main (M4 stream D)")
