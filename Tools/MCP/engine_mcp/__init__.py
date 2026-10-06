"""The engine's MCP bridge (Docs/Architecture.md §13.8): a thin Python layer over the editor's JSON-RPC automation
protocol, built on the official MCP Python SDK (pinned with hashes in Tools/MCP/requirements.lock and installed into
Tools/MCP/.venv by Scripts/Setup.py).

Modules:
  server      MCP tool registration (catalogue tools, bridge-local tools) and the stdio server
  connection  the connection to one editor: handshake, calls with timeouts, crash detection (reuses
              Tools/Automation/engine_client.py)
  launcher    launch or attach: session files, the project lock, supervision of a launched editor
  transcript  the always-on transcript <Project>/Automation/BuildLog.jsonl
"""
