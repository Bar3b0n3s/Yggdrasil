"""MCP tool registration and the stdio server (Docs/Architecture.md §13.8), on the official MCP Python SDK's low-level
server API (dynamic tool registration).

Tools:
  - bridge-local: editor_launch {project, create?, template?, headless?, renderer?, recover?} (template: the catalogue's
    project_create templates), editor_attach {project?, pid?} (a relative project path is resolved against the
    repository root, never the bridge's working directory; every attach and every launch sends a transcribed
    session.info, a launch right after its project.open or project.create, whose result it reports as "session" with
    the open scene; a launch waits for a running gate and a rendering editor holds the machine-wide GPU lock until it
    exits, LockUnavailable naming the holder after the wait: launcher.py),
    editor_status (without a connection: "editors", the editors running with automation, each with its pid, project and
    renderer, for editor_attach {pid}; Launcher.running_editors), editor_shutdown, engine_methods {domain?},
    engine_call {method, params} (the escape hatch to every method, such as project.refreshAssets, each bounded by its
    own timeoutSeconds: Bridge.method_timeout), result_read
    {path, pointer?, offset?, length?} (reads a file the editor wrote to an offload folder, the connected project's
    Library/Automation/Out/ or the user-data Automation/Out/, so an agent with only these tools can read an offloaded
    result or a test report: one page of its text, or the JSON value at a pointer; the location is checked as text
    before the file system is touched, and again once symbolic links are resolved; never transcribed, since it reads
    nothing of the editor's);
  - proxied: every method of Tools/MCP/catalog.json (methods flagged exposeAsTool, named domain_verb), listed before any
    editor runs, each with the catalogue's compact inputSchema (component and asset values are free-form objects, so the
    tool list does not flood the agent's context; full schemas stay available through rpc.discover and
    component_schema).
Arguments of a proxied tool (Docs/Decisions/0026-m16-contract.md, friction S3-02): a client whose tool list predates the
current catalogue sends an argument its cached schema lacks as a string ("true", "0"), which the editor refuses. Before
forwarding, the bridge converts each string argument whose parameter the current catalogue declares with another JSON
type (boolean, integer, number, object or array) when the string is exactly a JSON literal of that type
(coerce_arguments); any other value, and every parameter declared as a string or not declared at all, goes to the editor
unchanged, so the editor's validation still reports what is wrong. The transcript records the converted params, which
are what the editor received. The bridge-local tools' arguments are converted the same way against their own schemas,
and a boolean parameter then refuses anything but a JSON boolean (boolean_argument; the milestone review's R34), so the
string "false" never counts as true.
Results return as structuredContent plus a short text summary that always includes the _meta delta; text over 48 KB is
truncated with a pointer to the offloaded file. Screenshots (viewport_screenshot, editor_screenshot: results with a
"mimeType" of image/png and the "path" of the PNG the editor wrote, Docs/Decisions/0009-m5-decisions.md decision 33)
also return the image as an image content block, read from that file (the bridge runs on the editor's machine) unless
the result carries it inline ("data"); neither the text summary nor the structuredContent repeats the base64 data, and
the summary names the file. A screenshot result is never offloaded: the editor leaves out inline data that would make
it so.

Tool list refresh (Docs/Decisions/0026-m16-contract.md, predicted friction P10): the server declares tools.listChanged.
On a handshake-era connection it sends notifications/tools/list_changed once after the client's
notifications/initialized, so a client that kept the tool list of an earlier bridge process fetches it again.
editor_launch and editor_attach first reread catalog.json when its bytes changed since it was read
(Bridge.reload_catalog); a changed tool list is listed from then on, the result reports toolListChanged (a failed launch
or attach too, and catalogError for a catalogue that could not be reloaded), and before returning the server sends the
notification on the session of a handshake-era connection and publishes it to the subscriptions/listen streams of a
2026-07-28 one, the only place that protocol carries change notifications (the server serves subscriptions/listen, so
the SDK advertises listChanged there too). A call that is cancelled or fails still notifies, and a change whose
notification could not be sent is sent after the next call.

Environment (all optional): ENGINE_MCP_TRANSCRIPT redirects the transcript (transcript.py); ENGINE_MCP_USER_DATA_DIR
replaces the OS user-data root the bridge passes to the editors it launches and scans for session files (the bridge's
tests keep session files, logs and crash reports in a temporary directory with it); ENGINE_AUTOMATION_CONFIG names the
one build configuration whose editor editor_launch starts (default: Release, then Debug).
"""

from __future__ import annotations

import base64
import dataclasses
import hashlib
import json
import logging
import math
import os
import re
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
# The MIME types of screenshot results, which return image content (image_content).
IMAGE_MIME_TYPES = ("image/png",)
LOCAL_TOOL_NAMES = (
    "editor_launch", "editor_attach", "editor_status", "editor_shutdown", "engine_methods", "engine_call",
    "result_read",
)
# The tools that reread catalog.json first (Bridge.reload_catalog) and report what that found, failed calls included.
CATALOG_RELOADING_TOOL_NAMES = ("editor_launch", "editor_attach")
# Bounds sending a tool-list change to the client, which goes ahead even when the call that made it is cancelled.
NOTIFY_TIMEOUT_SECONDS = 5.0
# The most bytes of text one result_read page holds (docs.get's page size, DocsPageBytes).
RESULT_PAGE_BYTES = 32 * 1024
# The project-relative offload folder (Engine/Automation/Protocol/ResultOffload.h, OffloadDirectory).
OFFLOAD_DIRECTORY = Path("Library") / "Automation" / "Out"
USER_DATA_ENVIRONMENT_VARIABLE = "ENGINE_MCP_USER_DATA_DIR"
CATALOG_FORMAT = "McpCatalog"
CATALOG_VERSION = 1
DEFAULT_TIMEOUT_SECONDS = 60
NOT_CONNECTED = "no editor is connected: call editor_launch (or editor_attach) first"
# The exact JSON literals coerce_arguments converts (RFC 8259 numbers, no surrounding whitespace).
_JSON_INTEGER = re.compile(r"-?(?:0|[1-9][0-9]*)")
_JSON_NUMBER = re.compile(r"-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?")
# coerce_string's answer for a string it leaves as it is.
_UNCHANGED = object()

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
    """The tools of catalog.json ({"Format": "McpCatalog", "Version": 1, "Tools": [...]}). Raises ValueError for a file
    that cannot be read and for another format or version."""
    return read_catalog(path)[0]


def read_catalog(path: Path = CATALOG_PATH) -> tuple[list[CatalogTool], str]:
    """load_catalog's tools and the SHA-256 (hexadecimal) of the bytes they were parsed from, read once, so the digest
    always belongs to the tools (Bridge.reload_catalog compares it with the file's)."""
    try:
        data = path.read_bytes()
    except OSError as error:
        raise ValueError(f"cannot read the tool catalogue {path}: {error}") from error
    return parse_catalog(data, path), hashlib.sha256(data).hexdigest()


def parse_catalog(data: bytes, path: Path) -> list[CatalogTool]:
    """The tools of the catalogue `data` read from `path` (named in errors); raises ValueError as load_catalog does."""
    try:
        document = json.loads(data.decode("utf-8"))
    except ValueError as error:
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


def project_templates(catalog: list[CatalogTool]) -> list[str]:
    """The templates project.create accepts, read from the catalogue's project_create schema, so editor_launch's
    template can never disagree with the editor's; ["Empty"] for a catalogue without that tool."""
    tool = next((tool for tool in catalog if tool.method == "project.create"), None)
    template = tool.input_schema.get("properties", {}).get("template") if tool is not None else None
    values = template.get("enum") if isinstance(template, dict) else None
    names = [value for value in values if isinstance(value, str)] if isinstance(values, list) else []
    return names or ["Empty"]


def local_tool_schemas(templates: list[str] | None = None) -> dict[str, dict[str, Any]]:
    """The inputSchema of each bridge-local tool, by name; `templates` are editor_launch's (project_templates of the
    committed catalogue when omitted)."""
    if templates is None:
        templates = project_templates(load_catalog())
    return {
        "editor_launch": _object_schema({
            "project": {"type": "string", "description": "The project directory or its .eproj file; a relative "
                                                         "path is relative to the repository root "
                                                         "(Tests/Projects/Breakout)"},
            "create": {"type": "boolean", "description": "Create the project first when it does not exist"},
            "template": {"type": "string", "enum": list(templates), "description": "The template of a created project"},
            "headless": {"type": "boolean", "description": "Run without a window (default true)"},
            "renderer": {"type": "string", "enum": ["vulkan", "none"],
                         "description": "Default none; vulkan for the screenshot tools (needs a GPU)"},
            "recover": {"type": "boolean",
                        "description": "Adopt the newest valid autosave of every autosaved scene, each as an "
                                       "unsaved open scene (project.open {recover: true}), as after EditorCrashed "
                                       "with autosaveAvailable; the result reports recoveryAvailable and recovered"},
        }, ("project",)),
        "editor_attach": _object_schema({
            "project": {"type": "string", "description": "Attach to the editor serving this project (relative to the "
                                                         "repository root unless absolute)"},
            "pid": {"type": "integer", "minimum": 1, "description": "Attach to the editor with this process id"},
        }),
        "editor_status": _object_schema({}),
        "editor_shutdown": _object_schema({
            "save": {"type": "boolean", "description": "Save every open scene with unsaved changes first"},
            "force": {"type": "boolean", "description": "Exit even with unsaved changes"},
        }),
        "engine_methods": _object_schema({
            "domain": {"type": "string", "description": "Only this domain's methods (scene, entity, ...)"},
        }),
        "engine_call": _object_schema({
            "method": {"type": "string", "description": "A method name such as project.refreshAssets"},
            "params": {"type": "object", "description": "The method's params; see rpc.discover"},
        }, ("method",)),
        "result_read": _object_schema({
            "path": {"type": "string", "description": "The file: the path an offloaded result (truncated: true) or a "
                                                      "report (jsonPath, junitPath) names, or its file name"},
            "pointer": {"type": "string", "description": "A JSON pointer (/cases/0) to read one value of a JSON file"},
            "offset": {"type": "integer", "minimum": 0,
                       "description": "Where the page starts, in bytes: 0, or the previous page's nextOffset"},
            "length": {"type": "integer", "minimum": 1, "maximum": RESULT_PAGE_BYTES,
                       "description": f"The most bytes of text to return (default {RESULT_PAGE_BYTES})"},
        }, ("path",)),
    }


_LOCAL_TOOL_DESCRIPTIONS = {
    "editor_launch": "Starts a headless editor on a project, or attaches to the editor already serving it; creates the "
                     "project first with create: true; reports its session, the active scene included, and an "
                     "available autosave, which recover: true adopts (every autosaved scene, each as an unsaved open "
                     "scene). A start waits while a gate (CI.py) runs and, with renderer vulkan, while another GPU run "
                     "holds the machine-wide GPU lock, which the bridge then holds until the editor exits or the "
                     "bridge itself exits (an editor left running with unsaved changes holds none); after 30 min it "
                     "fails with LockUnavailable, naming the holder.",
    "editor_attach": "Attaches to a running editor with automation, by project or process id; reports its session, the "
                     "active scene included.",
    "editor_status": "Reports the connected editor: process, project, active scene, versions and whether it crashed. "
                     "Without a connection it lists the editors running with automation (pid, project, renderer), "
                     "which editor_attach {pid} connects to.",
    "editor_shutdown": "Shuts down an editor this bridge launched (an attached editor is only disconnected).",
    "engine_methods": "Lists the editor's automation methods with their flags (all of them, not only the tools).",
    "engine_call": "Calls any automation method by name with raw params (for example project.refreshAssets).",
    "result_read": "Reads a file the editor wrote to the project's Library/Automation/Out/ (or its user-data "
                   "Automation/Out/): an offloaded result (a response with truncated: true names its path) or a test "
                   "report; one page of the text with nextOffset, or the JSON value at pointer.",
}


def describe_scene(session: dict[str, Any]) -> str:
    """For a launch or attach summary, the open scene of a session.info result (Docs/Decisions/0026-m16-contract.md,
    friction S2-03): its path and whether it has unsaved changes, or, in a project without an open scene, how to open
    one; "" without an open project."""
    project = session.get("project")
    if not isinstance(project, dict) or project.get("open") is not True:
        return ""
    scene = session.get("scene")
    if not isinstance(scene, dict):
        return "; no scene is open: scene_open {path} opens one, such as the project's StartScene"
    path = scene.get("path") if isinstance(scene.get("path"), str) and scene.get("path") else "an unsaved scene"
    return f"; scene {path}" + (" (unsaved changes)" if scene.get("dirty") is True else "")


def describe_running_editors(editors: list[dict[str, Any]]) -> str:
    """editor_status's text without a connection (the native test game's friction NT-04): NOT_CONNECTED, then each
    editor running with automation (Launcher.running_editors) with its pid, project and renderer, and how to attach to
    one."""
    if not editors:
        return f"{NOT_CONNECTED}; no editor with automation is running"
    described = [f"pid {editor['pid']} ({editor['project'] or 'no project open'}, renderer "
                 f"{editor['renderer'] or 'unknown'})" for editor in editors]
    return (f"{NOT_CONNECTED}. Running with automation: {'; '.join(described)}. editor_attach {{pid}} connects to one "
            f"of them as it is, with its project and renderer")


def coerce_arguments(arguments: dict[str, Any], input_schema: dict[str, Any]) -> dict[str, Any]:
    """`arguments` with each string whose parameter `input_schema` (a catalogue tool's) declares with a JSON type other
    than string converted to that type, when the string is exactly a JSON literal of it (coerce_string); every other
    argument as it is (module comment; Docs/Decisions/0026-m16-contract.md, friction S3-02)."""
    properties = input_schema.get("properties") if isinstance(input_schema, dict) else None
    if not isinstance(properties, dict):
        return arguments
    coerced = dict(arguments)
    for name, value in arguments.items():
        schema = properties.get(name)
        if isinstance(value, str) and isinstance(schema, dict):
            converted = coerce_string(value, schema.get("type"))
            if converted is not _UNCHANGED:
                coerced[name] = converted
    return coerced


def coerce_string(text: str, declared: Any) -> Any:
    """`text` as the JSON type `declared` (a JSON Schema "type": one name or a list of names) when it is exactly a JSON
    literal of that type: "true" or "false" for boolean, an integer literal for integer, a finite number literal for
    number, "null" for null, and the JSON text of an object or an array for object and array (no NaN or Infinity
    inside, and no number literal that overflows a double). _UNCHANGED when the declared types include string or name
    none of these, or when `text` is not exactly a literal of any of them (an integer literal too long for Python to
    convert included)."""
    kinds = [declared] if isinstance(declared, str) else declared if isinstance(declared, list) else []
    if not kinds or "string" in kinds:
        return _UNCHANGED
    for kind in kinds:
        value = _convert_literal(text, kind)
        if value is not _UNCHANGED:
            return value
    return _UNCHANGED


def _convert_literal(text: str, kind: Any) -> Any:
    """coerce_string for one declared type."""
    if kind == "boolean":
        return {"true": True, "false": False}.get(text, _UNCHANGED)
    if kind == "null":
        return None if text == "null" else _UNCHANGED
    if kind == "integer":
        return _parse_integer(text) if _JSON_INTEGER.fullmatch(text) else _UNCHANGED
    if kind == "number":
        if _JSON_INTEGER.fullmatch(text):
            return _parse_integer(text)
        if not _JSON_NUMBER.fullmatch(text):
            return _UNCHANGED
        number = float(text)
        return number if math.isfinite(number) else _UNCHANGED
    if kind in ("object", "array"):
        try:
            # An integer literal too long for int() raises ValueError inside json.loads too.
            value = json.loads(text, parse_constant=_reject_constant, parse_float=_parse_finite_float)
        except (ValueError, RecursionError):
            return _UNCHANGED
        return value if isinstance(value, dict if kind == "object" else list) else _UNCHANGED
    return _UNCHANGED


def _parse_integer(text: str) -> Any:
    """An integer literal's value; _UNCHANGED for one Python refuses to convert (more than sys.int_info's
    default_max_str_digits digits), which then goes to the editor as the string it was."""
    try:
        return int(text)
    except ValueError:
        return _UNCHANGED


def _reject_constant(name: str) -> Any:
    """json.loads' parse_constant: NaN, Infinity and -Infinity are not JSON."""
    raise ValueError(f"{name} is not JSON")


class InvalidParams(Exception):
    """A bridge-local tool's argument of the wrong type; run_tool answers it as an InvalidParams failure."""


def boolean_argument(arguments: dict[str, Any], name: str, default: bool) -> bool:
    """A bridge-local tool's boolean argument: `default` when absent, else a JSON boolean (run_tool has already
    converted the exact strings "true" and "false" with coerce_arguments, as for a proxied tool). Anything else raises
    InvalidParams, never Python's truthiness, under which the string "false" is true (the milestone review's R34)."""
    value = arguments.get(name, default)
    if not isinstance(value, bool):
        raise InvalidParams(f"{name} must be a boolean (true or false), not {json.dumps(value, ensure_ascii=False)}")
    return value


def _parse_finite_float(text: str) -> float:
    """json.loads' parse_float: a number literal that overflows a double (1e400) is refused, so the converted value
    never holds an infinity that json.dumps would write as the non-JSON Infinity (the milestone review's R32)."""
    value = float(text)
    if not math.isfinite(value):
        raise ValueError(f"{text} overflows a double")
    return value


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
        lines.append(f"The result is large; the editor wrote it to {structured['path']} (result_read reads it, a page "
                     "or a JSON pointer at a time)")
    if structured.get("mimeType") in IMAGE_MIME_TYPES:
        # The image itself travels as image content (image_content); the summary names its file.
        rest.pop("data", None)
        lines.append(f"Screenshot {structured.get('width')}x{structured.get('height')}: {structured.get('path')}")
    if isinstance(rest.get("text"), str):
        lines.append(rest.pop("text"))
    if rest:
        lines.append(json.dumps(rest, ensure_ascii=False))
    lines.append(f"_meta: {json.dumps(structured.get('_meta', {}), ensure_ascii=False)}")
    return structured, _limit("\n".join(lines), structured.get("_meta"))


def image_content(structured: dict[str, Any]) -> tuple[str, str] | None:
    """The base64 data and MIME type of a screenshot result: its inline "data", or else the PNG at its "path", which the
    editor wrote on this machine. None for any other result, and for a file that cannot be read (the text summary still
    names it)."""
    mime_type = structured.get("mimeType")
    if mime_type not in IMAGE_MIME_TYPES:
        return None
    data = structured.get("data")
    if isinstance(data, str) and data:
        return data, mime_type
    path = structured.get("path")
    if not isinstance(path, str) or not path:
        return None
    try:
        return base64.b64encode(Path(path).read_bytes()).decode("ascii"), mime_type
    except OSError as error:
        LOGGER.warning("cannot read the screenshot %s: %s", path, error)
        return None


def tool_content(structured: dict[str, Any], is_error: bool) -> tuple[dict[str, Any], tuple[str, str] | None]:
    """The structuredContent of a tool result and its image content (image_content; None for an error and for a result
    without an image). When the image travels as image content, the structuredContent leaves out the inline "data", so
    the base64 is sent once."""
    image = None if is_error else image_content(structured)
    if image is None:
        return structured, None
    return {key: value for key, value in structured.items() if key != "data"}, image


def resolve_json_pointer(document: Any, pointer: str) -> Any:
    """The value at an RFC 6901 JSON pointer ("" is the whole document). Raises ValueError for a pointer that does not
    start with "/" and LookupError for one that names nothing."""
    if pointer == "":
        return document
    if not pointer.startswith("/"):
        raise ValueError(f"the JSON pointer '{pointer}' must be empty or start with '/'")
    value = document
    for token in pointer[1:].split("/"):
        token = token.replace("~1", "/").replace("~0", "~")
        if isinstance(value, dict) and token in value:
            value = value[token]
        elif (isinstance(value, list) and token.isdigit() and (token == "0" or not token.startswith("0"))
              and int(token) < len(value)):
            value = value[int(token)]
        else:
            raise LookupError(f"the JSON pointer '{pointer}' names nothing (no '{token}')")
    return value


def page_text(text: str, offset: int, length: int) -> tuple[str, int, int, int]:
    """One page of `text` as docs.get pages: at most `length` UTF-8 bytes from `offset` (moved forward to the start of a
    character), ending after the last line break inside it when the text goes on, else at a character boundary, and
    never empty before the end. Returns (page, offset, next offset or 0 at the end, total bytes)."""
    data = text.encode("utf-8")
    total = len(data)

    def is_continuation(index: int) -> bool:
        return index < total and (data[index] & 0xC0) == 0x80

    begin = min(offset, total)
    while is_continuation(begin):
        begin += 1
    end = min(begin + length, total)
    if end < total:
        line_break = data.rfind(b"\n", begin, end)
        if line_break >= 0:
            end = line_break + 1
        else:
            while end > begin and is_continuation(end):
                end -= 1
    if end == begin and begin < total:
        end += 1
        while is_continuation(end):
            end += 1
    return data[begin:end].decode("utf-8"), begin, end if end < total else 0, total


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
    on a worker thread (the server serializes them), so nothing here is shared between threads at once, except that
    the server lists the tools of `catalog` on its event loop meanwhile: reload_catalog replaces that dictionary,
    never changes it, so a listing reads one catalogue whole."""

    def __init__(self, repository_root: Path, catalog: list[CatalogTool], launcher: Launcher,
                 catalog_path: Path | None = None, catalog_digest: str = "") -> None:
        """`catalog_path` is the file reload_catalog rereads, with `catalog_digest` the SHA-256 of the bytes `catalog`
        was read from (read_catalog); without a path the catalogue never changes."""
        self.repository_root = repository_root
        self.catalog = {tool.name: tool for tool in catalog}
        self.launcher = launcher
        self.connection: EditorConnection | None = None
        self.catalog_path = catalog_path
        self.catalog_digest = catalog_digest
        # How many times reload_catalog changed the tool list (the server notifies the client when a call changed it),
        # and why the last reload kept the loaded catalogue ("" after a successful or unneeded one).
        self.catalog_revision = 0
        self.catalog_error = ""

    def call_tool(self, name: str, arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
        """Runs one tool: (structuredContent, text, isError). A launch or attach result, a failed one too, reports what
        its reload of catalog.json found (with_catalog_status)."""
        revision = self.catalog_revision
        structured, text, is_error = self.run_tool(name, arguments)
        if name in CATALOG_RELOADING_TOOL_NAMES:
            return self.with_catalog_status(structured, text, is_error, self.catalog_revision != revision)
        return structured, text, is_error

    def run_tool(self, name: str, arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
        """call_tool's dispatch, with the bridge's and the editor's expected failures mapped to error results."""
        local: dict[str, Callable[[dict[str, Any]], tuple[dict[str, Any], str, bool]]] = {
            "editor_launch": self.editor_launch,
            "editor_attach": self.editor_attach,
            "editor_status": self.editor_status,
            "editor_shutdown": self.editor_shutdown,
            "engine_methods": self.engine_methods,
            "engine_call": self.engine_call,
            "result_read": self.result_read,
        }
        try:
            if name in local:
                # The local tools' own schemas convert stringified booleans and integers as the catalogue's do for a
                # proxied tool; each tool then refuses a value of the wrong type (boolean_argument).
                schemas = local_tool_schemas(project_templates(list(self.catalog.values())))
                return local[name](coerce_arguments(arguments, schemas[name]))
            tool = self.catalog.get(name)
            if tool is None:
                return _failure("UnknownTool", f"no tool named '{name}'")
            # The current catalogue's types, which a client with a stale tool list may have sent as strings.
            return self.proxy(tool.method, coerce_arguments(arguments, tool.input_schema), tool.timeout_seconds)
        except InvalidParams as error:
            return _failure("InvalidParams", f"{name}: {error}")
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
        self.reload_catalog()
        project = arguments.get("project")
        if not isinstance(project, str) or not project:
            return _failure("InvalidParams", "editor_launch needs project (a directory or .eproj path)")
        if self.connection is not None and self.connection.is_alive():
            return _failure("InvalidState", f"already connected to an editor ({self.connection.describe()['project']}"
                                            f"); call editor_shutdown first")
        self.drop_connection()
        request = LaunchRequest(project=self.launcher.resolve_project(Path(project)),
                                create=boolean_argument(arguments, "create", False),
                                template=str(arguments.get("template", "Empty")),
                                headless=boolean_argument(arguments, "headless", True),
                                renderer=str(arguments.get("renderer", "none")),
                                recover=boolean_argument(arguments, "recover", False))
        self.connection = self.launcher.launch(request)
        status = self.connection.describe()
        verb = "Launched" if self.connection.supervised else "Attached to the running"
        text = f"{verb} editor (pid {status['pid']}) on {status['project'] or 'no project'}"
        if self.connection.session_info is not None:
            status["session"] = self.connection.session_info
            text += describe_scene(self.connection.session_info)
        if status.get("recovered"):
            text += "; recovered every autosaved scene as an unsaved open scene (scene_list lists them)"
        elif status.get("recoveryAvailable"):
            text += ("; an autosave is available: call editor_shutdown, then editor_launch with recover: true to adopt "
                     "it")
        return status, text, False

    def editor_attach(self, arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
        self.reload_catalog()
        if self.connection is not None and self.connection.is_alive():
            return _failure("InvalidState", "already connected to an editor; call editor_shutdown first")
        project = arguments.get("project")
        pid = arguments.get("pid")
        self.drop_connection()
        self.connection = self.launcher.attach(project=self.launcher.resolve_project(Path(project))
                                               if isinstance(project, str) else None,
                                               pid=pid if isinstance(pid, int) and not isinstance(pid, bool) else None)
        status = self.connection.describe()
        text = f"Attached to the editor (pid {status['pid']}) on {status['project'] or 'no project'}"
        if self.connection.session_info is not None:
            status["session"] = self.connection.session_info
            text += describe_scene(self.connection.session_info)
        return status, text, False

    def reload_catalog(self) -> bool:
        """Rereads catalog.json when its bytes changed since the catalogue was read from it, as editor_launch and
        editor_attach do first: the editor they connect to may be a rebuilt one, whose catalogue GenerateDocs.py
        regenerated while this bridge ran. The SHA-256 of the bytes decides, not the modification time, so a rewrite
        within the file system's timestamp resolution is not missed and a rewrite with the same content changes nothing.
        Returns whether the tool list changed (catalog_revision counts it; the server then notifies the client). A file
        that cannot be read or is not a valid catalogue keeps the loaded tools, sets catalog_error and is read again at
        the next launch or attach."""
        if self.catalog_path is None:
            return False
        try:
            data = self.catalog_path.read_bytes()
        except OSError as error:
            return self.keep_catalog(f"cannot read the tool catalogue {self.catalog_path}: {error}")
        digest = hashlib.sha256(data).hexdigest()
        if digest == self.catalog_digest:
            self.catalog_error = ""
            return False
        try:
            tools = parse_catalog(data, self.catalog_path)
        except ValueError as error:
            return self.keep_catalog(str(error))
        self.catalog_digest = digest
        self.catalog_error = ""
        if tools == list(self.catalog.values()):
            return False
        self.catalog = {tool.name: tool for tool in tools}
        self.catalog_revision += 1
        LOGGER.info("reloaded the tool catalogue %s: %d proxied tools", self.catalog_path, len(tools))
        return True

    def keep_catalog(self, error: str) -> bool:
        """reload_catalog's outcome when catalog.json cannot be reloaded: the loaded tools stay listed."""
        self.catalog_error = error
        LOGGER.warning("%s; the tool list is unchanged", error)
        return False

    def with_catalog_status(self, structured: dict[str, Any], text: str, is_error: bool,
                            tools_changed: bool) -> tuple[dict[str, Any], str, bool]:
        """A launch or attach result, successful or not, with what its reload_catalog found: toolListChanged when the
        tool list changed, catalogError when catalog.json could not be reloaded."""
        structured = dict(structured)
        if tools_changed:
            structured["toolListChanged"] = True
            text += (f"; catalog.json changed, so the tool list changed ({len(LOCAL_TOOL_NAMES) + len(self.catalog)} "
                     f"tools) and the client was notified (notifications/tools/list_changed)")
        if self.catalog_error:
            structured["catalogError"] = self.catalog_error
            text += f"; the tool list is unchanged: {self.catalog_error}"
        return structured, text, is_error

    def editor_status(self, arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
        if self.connection is None:
            editors = self.launcher.running_editors()
            return {"connected": False, "editors": editors}, describe_running_editors(editors), False
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
        result = connection.shutdown(save=boolean_argument(arguments, "save", False),
                                     force=boolean_argument(arguments, "force", False))
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
        return self.proxy(method, params, self.method_timeout(method))

    def offload_directories(self) -> list[Path]:
        """The folders result_read may read: the connected project's Library/Automation/Out/ and the user-data
        Automation/Out/ (where a launcher-state or read-only editor writes; the parent of the sessions directory)."""
        directories = []
        if self.connection is not None and self.connection.project_root is not None:
            directories.append(self.connection.project_root / OFFLOAD_DIRECTORY)
        directories.append(self.launcher.sessions_directory().parent / "Out")
        return directories

    def resolve_result_path(self, path: str) -> Path:
        """The file result_read reads: `path` absolute, or relative to the project root or to an offload folder (a bare
        file name). The location is checked lexically before anything touches the file system (the milestone review's
        R33): a path with a ".." part, or one with no interpretation whose normalized parent is an offload folder,
        raises PermissionError without a stat, so a UNC path opens no network connection and the answer never tells
        whether a file outside the folders exists. Then FileNotFoundError when no interpretation inside names a file,
        and PermissionError for a file whose resolved location is not directly inside an offload folder (symbolic links
        resolved, so none leads outside)."""
        directories = self.offload_directories()
        folders = ", ".join(item.as_posix() for item in directories)
        candidate = Path(path).expanduser()
        if ".." in candidate.parts:
            raise PermissionError(f"result_read reads only the files of {folders}, and a path with '..' is refused")
        if candidate.is_absolute():
            candidates = [candidate]
        else:
            roots = [self.connection.project_root] if self.connection and self.connection.project_root else []
            candidates = [root / candidate for root in [*roots, *directories]]
        allowed = {_lexical_path(directory) for directory in directories}
        inside = [item for item in candidates if _lexical_path(item.parent) in allowed]
        if not inside:
            raise PermissionError(f"result_read reads only the files of {folders}, not {path}")
        existing = next((item for item in inside if item.is_file()), None)
        if existing is None:
            raise FileNotFoundError(f"no file {path} in {folders}")
        resolved = existing.resolve()
        for directory in directories:
            try:
                if resolved.parent == directory.resolve():
                    return resolved
            except OSError:
                continue
        raise PermissionError(f"result_read reads only the files of {folders}, and {path} leads outside them")

    def result_read(self, arguments: dict[str, Any]) -> tuple[dict[str, Any], str, bool]:
        path = arguments.get("path")
        pointer = arguments.get("pointer")
        offset = arguments.get("offset", 0)
        length = arguments.get("length", RESULT_PAGE_BYTES)
        if not isinstance(path, str) or not path:
            return _failure("InvalidParams", "result_read needs path (the file an offloaded result or a report names)")
        if pointer is not None and not isinstance(pointer, str):
            return _failure("InvalidParams", "result_read's pointer must be a string such as /cases/0")
        if not isinstance(offset, int) or isinstance(offset, bool) or offset < 0:
            return _failure("InvalidParams", "result_read's offset must be an integer of at least 0")
        if not isinstance(length, int) or isinstance(length, bool) or not 1 <= length <= RESULT_PAGE_BYTES:
            return _failure("InvalidParams", f"result_read's length must be 1 to {RESULT_PAGE_BYTES}")
        try:
            file = self.resolve_result_path(path)
        except FileNotFoundError as error:
            return _failure("NotFound", str(error))
        except PermissionError as error:
            return _failure("PermissionDenied", str(error))
        try:
            text = file.read_bytes().decode("utf-8")
        except UnicodeDecodeError:
            return _failure("NotText", f"{file.as_posix()} is not UTF-8 text (screenshots return their image through "
                                       "the screenshot tools)")
        if pointer is not None:
            try:
                document = json.loads(text)
            except ValueError as error:
                return _failure("NotJson", f"{file.as_posix()}: a pointer reads a JSON file: {error}")
            try:
                value = resolve_json_pointer(document, pointer)
            except ValueError as error:
                return _failure("InvalidParams", f"{file.as_posix()}: {error}")
            except LookupError as error:
                return _failure("NotFound", f"{file.as_posix()}: {error}")
            text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False, indent=1)
        if offset > len(text.encode("utf-8")):
            return _failure("InvalidParams", f"offset {offset} is beyond the end of the text "
                                             f"({len(text.encode('utf-8'))} bytes)")
        content, begin, next_offset, total = page_text(text, offset, length)
        structured = {"path": file.as_posix(), "pointer": pointer or "", "offset": begin, "totalBytes": total,
                      "nextOffset": next_offset, "content": content}
        where = f"{file.as_posix()}{' ' + pointer if pointer else ''}"
        more = f"; next page at offset {next_offset}" if next_offset else ""
        header = f"{where}: bytes {begin}-{begin + len(content.encode('utf-8'))} of {total}{more}"
        return structured, _limit(f"{header}\n{content}"), False

    def method_timeout(self, method: str) -> float:
        """How long engine_call waits for `method` (Docs/Decisions/0026-m16-contract.md decision 13): the method's own
        timeoutSeconds as the connected editor declares it (rpc.discover), else the catalogue's, else
        DEFAULT_TIMEOUT_SECONDS; so a long project.refreshAssets or test.run through engine_call is not cut off at 60 s
        (a proxied tool, project_upgrade's included, always waits for the catalogue's timeoutSeconds)."""
        declared = self.connection.method_timeout(method) if self.connection is not None else None
        if declared is not None:
            return declared
        tool = next((tool for tool in self.catalog.values() if tool.method == method), None)
        return float(tool.timeout_seconds) if tool is not None else float(DEFAULT_TIMEOUT_SECONDS)

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


def _lexical_path(path: Path) -> str:
    """`path` made absolute and normalized as text alone (os.path.abspath, os.path.normcase), never through the file
    system, for result_read's lexical confinement check."""
    return os.path.normcase(os.path.abspath(path))


def create_bridge(repository_root: Path, catalog_path: Path = CATALOG_PATH) -> Bridge:
    """The bridge with the catalogue of `catalog_path` (the committed one by default, which editor_launch and
    editor_attach reread when it changed) and a launcher configured from the environment (module comment)."""
    user_data = os.environ.get(USER_DATA_ENVIRONMENT_VARIABLE, "")
    launcher = Launcher(repository_root, Path(user_data).resolve() if user_data else None,
                        engine_client.configurations_from_environment())
    catalog, digest = read_catalog(catalog_path)
    return Bridge(repository_root, catalog, launcher, catalog_path, digest)


def build_server(repository_root: Path) -> Any:
    """The SDK server with every tool registered (the SDK's low-level Server; typed Any so this module imports without
    the SDK for the Lint step)."""
    return _build_server(create_bridge(repository_root))[0]


def _build_server(bridge: Bridge) -> tuple[Any, Bridge]:
    import anyio
    import mcp_types as types
    from mcp.server import InitializationOptions, NotificationOptions, Server, ServerRequestContext
    from mcp.server.subscriptions import InMemorySubscriptionBus, ListenHandler, ToolsListChanged

    class BridgeServer(Server):
        """The SDK's low-level server, whose initialization options declare tools.listChanged whoever creates them (the
        stdio server and the SDK's in-memory transport alike): the SDK takes the list-changed flags only as
        create_initialization_options' NotificationOptions."""

        def create_initialization_options(self, notification_options: NotificationOptions | None = None,
                                          experimental_capabilities: dict[str, dict[str, Any]] | None = None,
                                          extensions: dict[str, dict[str, Any]] | None = None) -> InitializationOptions:
            return super().create_initialization_options(
                notification_options or NotificationOptions(tools_changed=True), experimental_capabilities, extensions)

    # A 2026-07-28 connection has no handshake and carries change notifications only on its subscriptions/listen
    # streams, which the listen handler serves from this bus (and which make the SDK advertise listChanged there).
    bus = InMemorySubscriptionBus()
    lock = anyio.Lock()
    # The catalogue revision the client last heard about: a call compares it with the bridge's, so a change is sent
    # once, and a change whose notification could not be sent goes out after the next call.
    notified_revision = bridge.catalog_revision

    def tool_list() -> list[Any]:
        # One read of the catalogue: a launch or attach on the worker thread may replace it meanwhile.
        catalog = bridge.catalog
        schemas = local_tool_schemas(project_templates(list(catalog.values())))
        tools = [types.Tool(name=name, description=_LOCAL_TOOL_DESCRIPTIONS[name], input_schema=schemas[name])
                 for name in LOCAL_TOOL_NAMES]
        return tools + [types.Tool(name=tool.name, description=tool.description, input_schema=tool.input_schema)
                        for tool in catalog.values()]

    async def list_tools(context: ServerRequestContext, params: types.PaginatedRequestParams | None) -> Any:
        # Immediately stale (ttlMs 0 on a 2026-07-28 connection): a launch or attach may change the tool list.
        return types.ListToolsResult(tools=tool_list(), ttl_ms=0)

    async def call_tool(context: ServerRequestContext, params: types.CallToolRequestParams) -> Any:
        nonlocal notified_revision
        arguments = dict(params.arguments or {})
        async with lock:
            try:
                # The editor connection blocks on its socket (each call bounded by its method's timeoutSeconds), so the
                # call runs on a worker thread and the event loop keeps serving the MCP session meanwhile.
                structured, text, is_error = await anyio.to_thread.run_sync(bridge.call_tool, params.name, arguments)
            finally:
                # Also when the client cancelled the call (the worker thread still finished the reload) or it raised,
                # shielded from that cancellation and bounded. Before returning, the server sends the notification on
                # the session of a handshake-era connection and publishes it to the listen streams of a 2026-07-28 one
                # (the SDK drops the session's copy there by design).
                revision = bridge.catalog_revision
                if revision != notified_revision:
                    with anyio.move_on_after(NOTIFY_TIMEOUT_SECONDS, shield=True):
                        await bus.publish(ToolsListChanged())
                        await context.session.send_tool_list_changed()
                        notified_revision = revision
        content: list[Any] = [types.TextContent(type="text", text=text)]
        structured, image = tool_content(structured, is_error)
        if image is not None:
            content.append(types.ImageContent(type="image", data=image[0], mime_type=image[1]))
        return types.CallToolResult(content=content, structured_content=structured, is_error=is_error)

    async def initialized(context: ServerRequestContext, params: types.NotificationParams) -> None:
        # The SDK runs this once the session is initialized and can carry notifications. A client that kept the tool
        # list of an earlier bridge process (restarted after a change to the bridge or its catalogue) fetches it again.
        await context.session.send_tool_list_changed()

    server = BridgeServer(SERVER_NAME, version=SERVER_VERSION, on_list_tools=list_tools, on_call_tool=call_tool,
                          on_subscriptions_listen=ListenHandler(bus))
    server.add_notification_handler("notifications/initialized", types.NotificationParams, initialized)
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
