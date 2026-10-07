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

Environment (all optional): ENGINE_MCP_TRANSCRIPT redirects the transcript (transcript.py); ENGINE_MCP_USER_DATA_DIR
replaces the OS user-data root the bridge passes to the editors it launches and scans for session files (the bridge's
tests keep session files, logs and crash reports in a temporary directory with it); ENGINE_AUTOMATION_CONFIG names the
one build configuration whose editor editor_launch starts (default: Release, then Debug).
"""

from __future__ import annotations

import dataclasses
import json
import logging
import os
import sys
from pathlib import Path
from typing import Any, Callable

import engine_client
from engine_mcp.connection import EditorConnection, EditorCrashed
from engine_mcp.launcher import EditorAlreadyOpen, Launcher, LaunchRequest

CATALOG_PATH = Path(__file__).resolve().parent.parent / "catalog.json"
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
SERVER_NAME = "engine"
SERVER_VERSION = "1.0"
MAX_TEXT_BYTES = 48 * 1024
LOCAL_TOOL_NAMES = (
    "editor_launch", "editor_attach", "editor_status", "editor_shutdown", "engine_methods", "engine_call",
)
USER_DATA_ENVIRONMENT_VARIABLE = "ENGINE_MCP_USER_DATA_DIR"
CATALOG_FORMAT = "McpCatalog"
CATALOG_VERSION = 1
DEFAULT_TIMEOUT_SECONDS = 60
NOT_CONNECTED = "no editor is connected: call editor_launch (or editor_attach) first"

LOGGER = logging.getLogger("engine_mcp")


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
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ValueError(f"cannot read the tool catalogue {path}: {error}") from error
    if not isinstance(document, dict) or document.get("Format") != CATALOG_FORMAT:
        raise ValueError(f"{path} is not an {CATALOG_FORMAT} file")
    if document.get("Version") != CATALOG_VERSION:
        raise ValueError(f"{path} has catalogue version {document.get('Version')!r}; this bridge reads version "
                         f"{CATALOG_VERSION} (regenerate it with Editor --dump-reference)")
    entries = document.get("Tools")
    if not isinstance(entries, list):
        raise ValueError(f"{path}: \"Tools\" is not a list")
    tools: list[CatalogTool] = []
    for index, entry in enumerate(entries):
        try:
            tool = CatalogTool(name=_typed(entry, "name", str), method=_typed(entry, "method", str),
                               description=_typed(entry, "description", str),
                               input_schema=_typed(entry, "inputSchema", dict), mutates=_typed(entry, "mutates", bool),
                               supports_dry_run=_typed(entry, "supportsDryRun", bool),
                               timeout_seconds=_typed(entry, "timeoutSeconds", int))
        except (TypeError, KeyError) as error:
            raise ValueError(f"{path}: tool {index}: {error}") from None
        if tool.name in LOCAL_TOOL_NAMES or any(existing.name == tool.name for existing in tools):
            raise ValueError(f"{path}: tool name '{tool.name}' is taken")
        tools.append(tool)
    return tools


def _typed(entry: Any, key: str, expected: type) -> Any:
    if not isinstance(entry, dict) or key not in entry:
        raise KeyError(f"'{key}' is missing")
    value = entry[key]
    if not isinstance(value, expected) or (expected is int and isinstance(value, bool)):
        raise TypeError(f"'{key}' is not of type {expected.__name__}")
    return value


def _object_schema(properties: dict[str, Any], required: tuple[str, ...] = ()) -> dict[str, Any]:
    schema: dict[str, Any] = {"type": "object", "properties": properties, "additionalProperties": False}
    if required:
        schema["required"] = list(required)
    return schema


def local_tool_schemas() -> dict[str, dict[str, Any]]:
    """The inputSchema of each bridge-local tool, by name."""
    return {
        "editor_launch": _object_schema({
            "project": {"type": "string", "description": "The project directory or its .eproj file"},
            "create": {"type": "boolean", "description": "Create the project first when it does not exist"},
            "template": {"type": "string", "enum": ["Empty"], "description": "The template of a created project"},
            "headless": {"type": "boolean", "description": "Run without a window (default true)"},
            "renderer": {"type": "string", "enum": ["vulkan", "none"], "description": "Default none"},
        }, ("project",)),
        "editor_attach": _object_schema({
            "project": {"type": "string", "description": "Attach to the editor serving this project"},
            "pid": {"type": "integer", "minimum": 1, "description": "Attach to the editor with this process id"},
        }),
        "editor_status": _object_schema({}),
        "editor_shutdown": _object_schema({
            "save": {"type": "boolean", "description": "Save the open scene first"},
            "force": {"type": "boolean", "description": "Exit even with unsaved changes"},
        }),
        "engine_methods": _object_schema({
            "domain": {"type": "string", "description": "Only this domain's methods (scene, entity, ...)"},
        }),
        "engine_call": _object_schema({
            "method": {"type": "string", "description": "A method name such as project.upgrade"},
            "params": {"type": "object", "description": "The method's params; see rpc.discover"},
        }, ("method",)),
    }


_LOCAL_TOOL_DESCRIPTIONS = {
    "editor_launch": "Starts a headless editor on a project, or attaches to the editor already serving it; creates the "
                     "project first with create: true.",
    "editor_attach": "Attaches to a running editor with automation, by project or process id.",
    "editor_status": "Reports the connected editor: process, project, versions and whether it crashed.",
    "editor_shutdown": "Shuts down an editor this bridge launched (an attached editor is only disconnected).",
    "engine_methods": "Lists the editor's automation methods with their flags (all of them, not only the tools).",
    "engine_call": "Calls any automation method by name with raw params (for example project.upgrade).",
}


def format_result(response: dict[str, Any]) -> tuple[dict[str, Any], str]:
    """The structuredContent and the text summary (with the _meta delta, truncated at MAX_TEXT_BYTES) of a response."""
    error = response.get("error")
    if isinstance(error, dict):
        data = error.get("data") if isinstance(error.get("data"), dict) else {}
        structured = {"error": data.get("errorCode") or "Error", "code": error.get("code"),
                      "message": data.get("detail", error.get("message", "")), "data": data}
        lines = [f"Error {error.get('code')} {data.get('errorCode', '')}: {data.get('detail', error.get('message'))}"]
        if data.get("hint"):
            lines.append(f"hint: {data['hint']}")
        for issue in data.get("issues", [])[:20]:
            if isinstance(issue, dict):
                hint = f" ({issue['hint']})" if issue.get("hint") else ""
                lines.append(f"  {issue.get('pointer', '')}: {issue.get('message', '')}{hint}")
        extra = {key: value for key, value in data.items()
                 if key not in ("errorCode", "detail", "hint", "issues", "_meta", "contexts", "location")}
        if extra:
            lines.append(json.dumps(extra, ensure_ascii=False))
        if "_meta" in data:
            lines.append(f"_meta: {json.dumps(data['_meta'], ensure_ascii=False)}")
        return structured, _limit("\n".join(lines))
    result = response.get("result")
    structured = result if isinstance(result, dict) else {"result": result}
    lines = []
    rest = {key: value for key, value in structured.items() if key != "_meta"}
    if structured.get("truncated") is True and isinstance(structured.get("path"), str):
        lines.append(f"The result is large; the editor wrote it to {structured['path']}")
    if isinstance(rest.get("text"), str):
        lines.append(rest.pop("text"))
    if rest:
        lines.append(json.dumps(rest, ensure_ascii=False))
    lines.append(f"_meta: {json.dumps(structured.get('_meta', {}), ensure_ascii=False)}")
    return structured, _limit("\n".join(lines), structured.get("_meta"))


def _limit(text: str, meta: Any = None) -> str:
    """`text`, cut at MAX_TEXT_BYTES of UTF-8 with a pointer to the complete result; the _meta delta is kept."""
    encoded = text.encode("utf-8")
    if len(encoded) <= MAX_TEXT_BYTES:
        return text
    tail = "\n... (text truncated at 48 KB; the complete result is in structuredContent"
    tail += f")\n_meta: {json.dumps(meta, ensure_ascii=False)}" if meta is not None else ")"
    budget = MAX_TEXT_BYTES - len(tail.encode("utf-8"))
    return encoded[:max(0, budget)].decode("utf-8", errors="ignore") + tail


class Bridge:
    """The bridge's state: the catalogue, the launcher and the current editor connection. Tool calls run one at a time
    on a worker thread (the server serializes them), so nothing here is shared between threads at once."""

    def __init__(self, repository_root: Path, catalog: list[CatalogTool], launcher: Launcher) -> None:
        self.repository_root = repository_root
        self.catalog = {tool.name: tool for tool in catalog}
        self.launcher = launcher
        self.connection: EditorConnection | None = None

    def call_tool(self, name: str, arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
        """Runs one tool: (structuredContent, text, isError)."""
        local: dict[str, Callable[[dict[str, Any]], tuple[dict[str, Any], str, bool]]] = {
            "editor_launch": self.editor_launch,
            "editor_attach": self.editor_attach,
            "editor_status": self.editor_status,
            "editor_shutdown": self.editor_shutdown,
            "engine_methods": self.engine_methods,
            "engine_call": self.engine_call,
        }
        try:
            if name in local:
                return local[name](arguments)
            tool = self.catalog.get(name)
            if tool is None:
                return _failure("UnknownTool", f"no tool named '{name}'")
            return self.proxy(tool.method, arguments, tool.timeout_seconds)
        except EditorCrashed as crashed:
            return crashed.to_json(), f"EditorCrashed: {crashed}\n" + "\n".join(crashed.last_log_lines[-10:]), True
        except EditorAlreadyOpen as already_open:
            return already_open.to_json(), str(already_open), True
        except engine_client.ConnectionClosed as error:
            return _failure("ConnectionClosed", str(error))
        except TimeoutError as error:
            return _failure("Timeout", str(error))
        except (OSError, LookupError, ValueError, RuntimeError, engine_client.EngineError,
                engine_client.ProtocolError) as error:
            return _failure(type(error).__name__, str(error))

    def proxy(self, method: str, params: dict[str, Any], timeout: float) -> tuple[dict[str, Any], str, bool]:
        if self.connection is None:
            return _failure("NotConnected", NOT_CONNECTED)
        response = self.connection.call(method, params, timeout=timeout)
        structured, text = format_result(response)
        return structured, text, "error" in response

    def editor_launch(self, arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
        project = arguments.get("project")
        if not isinstance(project, str) or not project:
            return _failure("InvalidParams", "editor_launch needs project (a directory or .eproj path)")
        if self.connection is not None and self.connection.is_alive():
            return _failure("InvalidState", f"already connected to an editor ({self.connection.describe()['project']}"
                                            f"); call editor_shutdown first")
        self.drop_connection()
        request = LaunchRequest(project=Path(project), create=bool(arguments.get("create", False)),
                                template=str(arguments.get("template", "Empty")),
                                headless=bool(arguments.get("headless", True)),
                                renderer=str(arguments.get("renderer", "none")))
        self.connection = self.launcher.launch(request)
        status = self.connection.describe()
        verb = "Launched" if self.connection.supervised else "Attached to the running"
        return status, f"{verb} editor (pid {status['pid']}) on {status['project'] or 'no project'}", False

    def editor_attach(self, arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
        if self.connection is not None and self.connection.is_alive():
            return _failure("InvalidState", "already connected to an editor; call editor_shutdown first")
        project = arguments.get("project")
        pid = arguments.get("pid")
        self.drop_connection()
        self.connection = self.launcher.attach(project=Path(project) if isinstance(project, str) else None,
                                               pid=pid if isinstance(pid, int) and not isinstance(pid, bool) else None)
        status = self.connection.describe()
        return status, f"Attached to the editor (pid {status['pid']}) on {status['project'] or 'no project'}", False

    def editor_status(self, arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
        if self.connection is None:
            return {"connected": False}, NOT_CONNECTED, False
        status = self.connection.describe()
        if self.connection.is_alive():
            response = self.connection.call("session.info", {})
            if "result" in response:
                status["session"] = response["result"]
        else:
            self.connection.check_editor()
        return status, json.dumps(status, ensure_ascii=False), False

    def editor_shutdown(self, arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
        if self.connection is None:
            return {"disconnected": False}, NOT_CONNECTED, False
        connection = self.connection
        result = connection.shutdown(save=bool(arguments.get("save", False)), force=bool(arguments.get("force", False)))
        if result.get("disconnected"):
            self.connection = None
            return result, json.dumps(result, ensure_ascii=False), False
        structured, text = format_result(result["response"])
        return structured, text, True

    def engine_methods(self, arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
        domain = arguments.get("domain")
        if self.connection is None or not self.connection.is_alive():
            tools = [tool for tool in self.catalog.values()
                     if not isinstance(domain, str) or tool.method.split(".", 1)[0] == domain]
            methods = [{"name": tool.method, "tool": tool.name, "description": tool.description,
                        "mutates": tool.mutates, "supportsDryRun": tool.supports_dry_run} for tool in tools]
            structured = {"methods": methods, "source": "catalog.json (connect an editor for every method)"}
            return structured, json.dumps(structured, ensure_ascii=False), False
        response = self.connection.call("rpc.discover", {"domain": domain} if isinstance(domain, str) else {})
        if "error" in response:
            structured, text = format_result(response)
            return structured, text, True
        # The unfiltered catalogue is over the offload threshold, so the editor answers with the file it wrote (§13.4
        # bounded output; ADR 0008 decision 31); the bridge runs on the editor's machine and reads it.
        inline = response["result"]
        catalogue = engine_client.load_offloaded(inline)
        keys = ("name", "description", "exposeAsTool", "mutates", "supportsDryRun", "availableInLauncher", "pending",
                "requiredParams")
        methods = [{key: method[key] for key in keys if key in method}
                   for method in catalogue.get("methods", []) if isinstance(method, dict)]
        structured = {"methods": methods, "_meta": inline.get("_meta", {})}
        return structured, _limit(json.dumps(structured, ensure_ascii=False)), False

    def engine_call(self, arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
        method = arguments.get("method")
        params = arguments.get("params", {})
        if not isinstance(method, str) or not method:
            return _failure("InvalidParams", "engine_call needs method")
        if not isinstance(params, dict):
            return _failure("InvalidParams", "engine_call's params must be an object")
        tool = next((tool for tool in self.catalog.values() if tool.method == method), None)
        return self.proxy(method, params, tool.timeout_seconds if tool is not None else DEFAULT_TIMEOUT_SECONDS)

    def drop_connection(self) -> None:
        """Forgets a connection whose editor is gone (crashed or shut down) before another one is made."""
        if self.connection is not None:
            self.connection.disconnect()
            self.connection = None

    def close(self) -> None:
        """When the MCP client goes away: a launched editor with nothing unsaved is shut down; one with unsaved changes
        keeps running (the next editor_launch attaches to it), so no work is lost."""
        connection, self.connection = self.connection, None
        if connection is None:
            return
        if connection.supervised and connection.is_alive():
            try:
                result = connection.shutdown()
                if not result.get("disconnected"):
                    LOGGER.warning("the launched editor has unsaved changes and keeps running; editor_launch attaches "
                                   "to it again")
            except (engine_client.ConnectionClosed, engine_client.ProtocolError, TimeoutError, EditorCrashed) as error:
                LOGGER.warning("shutting down the launched editor failed: %s", error)
        connection.disconnect()


def _failure(error: str, message: str) -> tuple[dict[str, Any], str, bool]:
    return {"error": error, "message": message}, f"{error}: {message}", True


def create_bridge(repository_root: Path) -> Bridge:
    """The bridge with the committed catalogue and a launcher configured from the environment (module comment)."""
    user_data = os.environ.get(USER_DATA_ENVIRONMENT_VARIABLE, "")
    launcher = Launcher(repository_root, Path(user_data).resolve() if user_data else None,
                        engine_client.configurations_from_environment())
    return Bridge(repository_root, load_catalog(), launcher)


def build_server(repository_root: Path) -> Any:
    """The SDK server with every tool registered (the SDK's low-level Server; typed Any so this module imports without
    the SDK for the Lint step)."""
    return _build_server(create_bridge(repository_root))[0]


def _build_server(bridge: Bridge) -> tuple[Any, Bridge]:
    import anyio
    import mcp_types as types
    from mcp.server import Server, ServerRequestContext

    schemas = local_tool_schemas()
    tools = [types.Tool(name=name, description=_LOCAL_TOOL_DESCRIPTIONS[name], input_schema=schemas[name])
             for name in LOCAL_TOOL_NAMES]
    tools += [types.Tool(name=tool.name, description=tool.description, input_schema=tool.input_schema)
              for tool in bridge.catalog.values()]
    lock = anyio.Lock()

    async def list_tools(context: ServerRequestContext, params: types.PaginatedRequestParams | None) -> Any:
        return types.ListToolsResult(tools=tools)

    async def call_tool(context: ServerRequestContext, params: types.CallToolRequestParams) -> Any:
        arguments = dict(params.arguments or {})
        async with lock:
            # The editor connection blocks on its socket (each call bounded by its method's timeoutSeconds), so the
            # call runs on a worker thread and the event loop keeps serving the MCP session meanwhile.
            structured, text, is_error = await anyio.to_thread.run_sync(bridge.call_tool, params.name, arguments)
        return types.CallToolResult(content=[types.TextContent(type="text", text=text)], structured_content=structured,
                                    is_error=is_error)

    server = Server(SERVER_NAME, version=SERVER_VERSION, on_list_tools=list_tools, on_call_tool=call_tool)
    return server, bridge


async def _serve(repository_root: Path) -> None:
    import anyio
    from mcp.server.stdio import stdio_server

    server, bridge = _build_server(create_bridge(repository_root))
    try:
        async with stdio_server() as (read_stream, write_stream):
            await server.run(read_stream, write_stream, server.create_initialization_options())
    finally:
        await anyio.to_thread.run_sync(bridge.close)


def main() -> int:
    """Serves MCP on stdio until the client disconnects; returns the process exit code."""
    import anyio

    logging.basicConfig(stream=sys.stderr, level=logging.INFO, format="engine-mcp: %(levelname)s: %(message)s")
    try:
        anyio.run(_serve, REPOSITORY_ROOT)
    except ValueError as error:
        LOGGER.error("%s", error)
        return 1
    except KeyboardInterrupt:
        return 0
    return 0
