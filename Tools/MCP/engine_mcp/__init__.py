"""The engine's MCP bridge (Docs/Architecture.md §13.8): a thin Python layer over the editor's JSON-RPC automation
protocol, built on the official MCP Python SDK (pinned with hashes in Tools/MCP/requirements.lock and installed into
Tools/MCP/.venv by Scripts/Setup.py).

Modules:
  server      MCP tool registration (catalogue tools, bridge-local tools) and the stdio server
  connection  the connection to one editor: handshake, calls with timeouts, crash detection (reuses
              Tools/Automation/engine_client.py)
  launcher    launch or attach: session files, the project lock, supervision of a launched editor
  transcript  the always-on transcript <Project>/Automation/BuildLog.jsonl

Importing the package puts Tools/Automation on sys.path, so its modules import the shared engine_client.
"""

import sys
from pathlib import Path

AUTOMATION_ROOT = Path(__file__).resolve().parents[2] / "Automation"
if str(AUTOMATION_ROOT) not in sys.path:
    sys.path.insert(0, str(AUTOMATION_ROOT))
