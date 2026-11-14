# Graphs

A reference page of the `game-building` skill (`skills/game-building`), for its step "4. Scripts": visual-scripting graphs, behaviours built from nodes and links with the `graph_*` tools; the node library; the findings and the limits a graph keeps; live values in play; and debugging. A graph is another way to write a behaviour, not another kind of script: Luau behaviours, modules and suites are the `luau-gameplay` skill's.

## What a graph is

- **One behaviour.** A graph (`Assets/Scripts/Door.graph`) is one behaviour drawn as nodes and links. Its import transpiles it to one strict Luau text and compiles that as a `.luau` behaviour is compiled, so it is a `Script` asset (importer `Graph`): a script slot takes it as it takes a Luau behaviour (`entity_update {entity, components: {Script: {Scripts: [{Script: "Assets/Scripts/Door.graph", Fields: {Player: "/Player"}, ExecutionOrder: 0}]}}}`), its fields are the slot's overrides, and play, hot reload, suites, replays and the Release and Dist exports run it as any behaviour, alike in every run mode; its instance's state stays outside the state hash, as every script's does. `asset_list {type: "Script"}` lists graphs with the Luau scripts, and `engine_call {method: "asset.info"}` names the importer.
- **Graph tools only.** `script_create`, `script_read`, `script_write` and `script_check` refuse a `.graph` path, naming the graph tools; never write a graph file by hand (the ground rules). Its format is "Graph" in `reference/FileFormats` and `reference/Schemas/Graph`; the tools take and return the document in the file's spelling (`Nodes`, `Links`, `{Node, Pin}`).
- **Self-contained.** A graph requires no module, and no script requires it. A Luau behaviour or a suite reaches its instance by its behaviour name, `entity:GetScript("Door")`, typed `any`, and reads its fields and variables as members (`door.Opened`).
- **Its events.** Its event nodes are the Behaviour callbacks (`Event.OnStart`, `Event.OnFixedUpdate`, `Event.OnTriggerEnter`, ...); several event nodes of one callback run in id order. An event returns nothing, so a graph's `OnUiClick` and the other `OnUi*` events never consume the UI event, which bubbles on to the parent's behaviours: write a behaviour that must stop it in Luau.

## Building one

1. `graph_create {path: "Assets/Scripts/Door.graph"}` writes an empty graph and its `.meta`; the behaviour's name is the file's stem (`name` gives another Luau identifier).
2. `graph_set_members {graph, fields?, variables?, name?, rename?, remove?}` declares the members. A **field** is a behaviour field (serialized, overridden per script slot, shown in the inspector); a **variable** is instance state, set to its default in `OnCreate`. Spell each member in full, as the file does: `{"Name": "Speed", "Kind": "Number", "Default": 2, "Element": "Number", "AssetType": "", "EnumValues": [], "Options": {"Min": 0, "Max": 10}}`, without `Options` for a variable. `Kind` is a Luau `Field` constructor: `Number`, `Integer`, `Bool`, `String`, `Vector` (`[x, y, z]`), `Color` (`[r, g, b, a]`), `Quat` (`[x, y, z, w]`, not zero), `Entity` and `Asset` (default `null`; an Asset names its `AssetType`), `Enum` (its `EnumValues`, the default one of them) or `Array` (default `[]`, its `Element` kind). `Options` (`Min`, `Max`, `Step`, `Tooltip`) exist only for the kinds whose constructor takes them (not Entity, Asset, Enum or Array), and a default lies within `Min` and `Max`. A member given again replaces the member of its name, `rename` rewrites the member's nodes, and `remove` refuses a member a node still uses (naming the nodes).
3. `graph_nodes {prefix?, kinds?, graph?}` lists node kinds with their `pins` (each with `name`, `direction`, `type`, `optional` and `default`); `graph` adds that graph's `Self.Get.<Name>` and `Self.Set.<Name>` kinds. Ask by prefix (`"Flow."`, `"Event."`, `"Call.Input."`) rather than for the whole library.
4. `graph_add_node {graph, kind, position?, values?}` adds a node and returns its id (`node`: one past the highest, and an id never changes). `position` is layout only; without it the node goes right of the rightmost one. `values` sets input literals by pin name.
5. `graph_connect {graph, from: {Node, Pin}, to: {Node, Pin}}` links an output to an input. A data input and an exec output hold one link, so a new one replaces the old (`replaced`); an exec input takes any number. `graph_disconnect` removes a link, `graph_remove_nodes {graph, nodes}` nodes with their links, and `graph.moveNodes {graph, positions: [{node, position}]}` (layout, through `engine_call`) moves nodes.
6. `graph_set_values {graph, values: [{node, pin, value}]}` sets input literals (`null` removes one, also one on a pin the node's kind no longer has); a linked input keeps its literal, and the link wins.
7. `graph_read {graph}` until its `diagnostics` is empty; `luau: true` adds the transpiled text and its `lineNodes`. Then attach the graph to a slot and check it as any behaviour (steps 5 to 7 of the skill): `play_start {lockstep: true}`, `play_step`, `script_eval {context: "Play", entity, script: "Door", code: "return self.Opened"}`, a suite and a replay.

**Every edit** reads the file strictly, writes its canonical text (members, nodes and links sorted, so a diff stays small) as one undo step that `edit_undo` takes back byte for byte, is recorded in provenance like every write, and takes `dryRun: true`. A refused edit writes nothing and names its place in your params (`/from/Node`, `/to/Pin`, `/values/0/value`, `/fields/1/Kind`) and the node. The graph is imported again after each write; a lockstep, recording, replay or test session sees the change only after it ends (`play_stop`, then play again).

**One batch.** Every graph tool works inside `edit_batch`, so a graph's nodes and links go in as one undo step: give an op a `name` and write the node it adds as `{"$ref": "<name>.node"}`. The Door's nodes, after `graph_create` and a `graph_set_members` of its `Player` Entity field and `Opened` Bool variable (the door opens when its Player enters its trigger):

```json
{"label": "Door nodes", "ops": [
	{"name": "enter", "method": "graph.addNode", "params": {"graph": "Assets/Scripts/Door.graph", "kind": "Event.OnTriggerEnter"}},
	{"name": "same", "method": "graph.addNode", "params": {"graph": "Assets/Scripts/Door.graph", "kind": "Compare.Equal"}},
	{"name": "player", "method": "graph.addNode", "params": {"graph": "Assets/Scripts/Door.graph", "kind": "Self.Get.Player"}},
	{"name": "branch", "method": "graph.addNode", "params": {"graph": "Assets/Scripts/Door.graph", "kind": "Flow.Branch"}},
	{"name": "open", "method": "graph.addNode", "params": {"graph": "Assets/Scripts/Door.graph", "kind": "Self.Set.Opened", "values": {"Value": true}}},
	{"name": "log", "method": "graph.addNode", "params": {"graph": "Assets/Scripts/Door.graph", "kind": "Call.Log.Info", "values": {"Values": "Door opened"}}},
	{"method": "graph.connect", "params": {"graph": "Assets/Scripts/Door.graph", "from": {"Node": {"$ref": "enter.node"}, "Pin": "Then"}, "to": {"Node": {"$ref": "branch.node"}, "Pin": "In"}}},
	{"method": "graph.connect", "params": {"graph": "Assets/Scripts/Door.graph", "from": {"Node": {"$ref": "enter.node"}, "Pin": "other"}, "to": {"Node": {"$ref": "same.node"}, "Pin": "A"}}},
	{"method": "graph.connect", "params": {"graph": "Assets/Scripts/Door.graph", "from": {"Node": {"$ref": "player.node"}, "Pin": "Value"}, "to": {"Node": {"$ref": "same.node"}, "Pin": "B"}}},
	{"method": "graph.connect", "params": {"graph": "Assets/Scripts/Door.graph", "from": {"Node": {"$ref": "same.node"}, "Pin": "Result"}, "to": {"Node": {"$ref": "branch.node"}, "Pin": "Condition"}}},
	{"method": "graph.connect", "params": {"graph": "Assets/Scripts/Door.graph", "from": {"Node": {"$ref": "branch.node"}, "Pin": "True"}, "to": {"Node": {"$ref": "open.node"}, "Pin": "In"}}},
	{"method": "graph.connect", "params": {"graph": "Assets/Scripts/Door.graph", "from": {"Node": {"$ref": "open.node"}, "Pin": "Then"}, "to": {"Node": {"$ref": "log.node"}, "Pin": "In"}}}]}
```

The windowed editor's Graph Editor (the content browser's double-click on a graph, or a script slot's Open graph in the inspector) draws the same graph and acts through these same methods, but its edits are the user's: provenance records them as `ui` writes, which a game's provenance audit rejects. As with every windowed UI, use it to look, not to author a game.

## The transpiled text

`graph_read {graph, luau: true}` shows the Luau each import compiles. The Door above gives:

```luau path=Assets/Scripts/Door.luau
--!strict
-- Generated from Assets/Scripts/Door.graph by the graph transpiler: edit the graph, not this text.
local Graph = {}

Graph.Fields = {
	Player = Field.Entity(),
}

function Graph.OnCreate(self: any)
	self.__GraphLive = {}
	self.Opened = false
end

function Graph.OnTriggerEnter(self: any, other: Entity)
	-- 4: Flow.Branch
	do
		-- 3: Self.Get.Player
		local v3_Value = self.Player
		self.__GraphLive["3.Value"] = v3_Value
		-- 2: Compare.Equal
		local v2_Result = other == v3_Value
		self.__GraphLive["2.Result"] = v2_Result
		if v2_Result then
			-- 5: Self.Set.Opened
			self.Opened = true
			-- 6: Call.Log.Info
			Log.Info("Door opened")
		end
	end
end

return Script.Define("Door", Graph)
```

Each node's code starts with the comment `-- <Id>: <Kind>`, a pure node's value is the local `v<Id>_<Pin>` and a statement's output `n<Id>_<Pin>`, and `lineNodes[N - 1]` is the node of line N (0 for the layout's own lines). Every computed value is also stored in the instance's live-value table, `self.__GraphLive["<Id>.<Pin>"]`, which the editor reads ("Live values"); like every script table it is outside the state hash. The text is the same bytes in every editor configuration, and exports run its compiled bytecode. Edit the graph, never the text.

## The node library

`graph_nodes` is the catalogue. It is generated from the script API (`reference/ScriptAPI`), so a binding the engine adds brings its node.

- **Generated kinds.** `Event.<Callback>` for each Behaviour callback (outputs `Then` and the callback's parameters by name: `other`, `contact`); `Call.<Group>.<Member>` for a function, method or constructor (`Call.Input.GetAxis`, `Call.Log.Info`, `Call.Transform.Translate`, whose object is the input `self`); `Get.<Group>.<Member>` and `Set.<Group>.<Member>` for a property or a component proxy (`Get.Entity.Transform`, `Get.Transform.Translation`, `Set.Transform.Translation`), and `Get` for a record's field (`Get.CollisionContact.Normal`); `Const.<Group>.<Member>` for a constant (`Const.Math.Pi`). Only a member available in every run mode and at run time, whose signature is one plain function type, has a node: `OnHotReload`, `Test.*`, `Script.Define`, `Task.Spawn` and `Task.Delay` have none (`Flow.Delay` delays a flow).
- **Built-in kinds:**

| Kinds | Pins (inputs → outputs) |
|---|---|
| `Flow.Branch` | `In`, `Condition` → `True` or `False` |
| `Flow.Sequence` | `In` → `Then1` to `Then4`, run in order |
| `Flow.ForRange` | `In`, `First`, `Last` → `Body` with `Index` for each number, then `Completed` |
| `Flow.ForEach` | `In`, `Array` → `Body` with `Element` and `Index` for each element, then `Completed` |
| `Flow.Delay` | `In`, `Seconds` → `Then` at once, and `Later` after the delay (`Task.Delay`'s ticks) |
| `Flow.IfNotNil` | `In`, `Value` → `True` with `Result` (the value) when it is not nil, else `False` |
| `Self.Entity` | → `Value`, the behaviour's entity |
| `Self.Get.<Name>`; `Self.Set.<Name>` | → `Value` of a field or variable; `In`, `Value` → `Then` |
| `Number.Add`, `Subtract`, `Multiply`, `Divide`, `Modulo`; `Number.Negate` | `A`, `B` → `Result`; `Value` → `Result` |
| `Compare.Equal`, `NotEqual` (any values); `Less`, `LessEqual`, `Greater`, `GreaterEqual` (numbers) | `A`, `B` → `Result` |
| `Logic.And`, `Logic.Or`; `Logic.Not` | `A`, `B` → `Result`; `Value` → `Result` |
| `Vector.Make`; `Vector.Split` | `X`, `Y`, `Z` → `Result`; `Value` → `X`, `Y`, `Z` |
| `Vector.Add`, `Subtract`, `Cross`, `Dot`; `Vector.Scale`; `Vector.Length`, `Normalize` | `A`, `B` → `Result`; `Value`, `Factor` → `Result`; `Value` → `Result` |
| `Text.Concat` | `A`, `B` (any values, each `tostring`ed) → `Result` |

- **Statements and pure nodes.** A statement has the exec pins `In` and `Then` (the flow nodes name theirs above) and runs when its `In` is reached. A pure node (an operator, a getter, a constant, a member that only reads and returns a value) has no exec pins and is computed again for every statement input it feeds, so its value is always current. Data inputs are named after the Luau parameters (`name`, `value`, `self`), a single result is `Result` (several: `Result1`, `Result2`, ...), and a trailing variadic (`Log.Info`'s) is one input, `Values`.
- **Types.** Exec links only exec. A data link joins pins of one type (`Number`, `Bool`, `String`, `Vector`, `Color`, `Quat`, `Entity`, `Asset`, an enum or an object type by name); an enum output also feeds a `String` input, and an `Any` pin (a table, a union) links with every data pin. An optional output (Luau `T?`: `Get.Entity.RigidBody`, the `Result` of `Call.Assets.Load` and of `Call.Scene.FindByName`) may give nil, so it feeds only an optional or `Any` input: link it to `Flow.IfNotNil`'s `Value` and use its `Result` on the `True` flow, where every input takes it. `graph_connect` refuses an incompatible link, its hint naming `Flow.IfNotNil` for an optional one.
- **Unlinked inputs.** An optional input left alone passes nil. A required `Bool`, `Number`, `String`, `Vector`, `Color`, `Quat` or enum input without a link or literal takes its `default` (false, 0, "", the zero vector, white, identity, the first choice); an `Entity` or object input must be linked (it takes no literal), and an `Asset` or `Any` one linked or set.
- **Literals** (`values`, `graph_set_values`): a boolean, a number, a string, `[x, y, z]` for a vector, `[r, g, b, a]` for a Color, `[x, y, z, w]` for a Quat (not all zero), an enum's choice as its string, and an asset as its 16-digit hex `id`, as `asset_list` gives it (the graph's import lists it as a dependency, which a pruned export keeps). A fractional number must be one a float holds exactly as written (`0.3`, `2.5`, `0.70710677`): `0.30000001` is refused, the message naming the float to write.
- **Flow order.** Statements run in the order of their exec links, and a statement's output read later in its flow holds its last run's value. Read a statement's or an event's output only from the statements its flow leads to (that it dominates), `Flow.IfNotNil`'s `Result` only where its `True` flow alone leads, and a loop's `Index` or `Element` into an input that needs a value only where its `Body` flow alone leads (on `Completed` they are nil when the loop ran zero times; an optional or `Any` input takes that nil); elsewhere the read leaves the graph incomplete.

## Findings and limits

`graph_read`'s `diagnostics` (each `{severity, code, message, node, jsonPointer, line, incomplete}`) and `project_validate` report:
- **`GRAPH_INVALID`** (Error): a problem of the graph, at its JSON pointer (`/Nodes/<i>/Values/<pin>`, `/Links/<i>`) and node. An `incomplete` one (a required input neither linked nor set, an output read where it may not have run, `Flow.IfNotNil`'s `Result` read off its `True` flow, a loop's `Index` or `Element` read off its `Body` flow into an input that needs a value) is accepted while you build, but the graph does not import while any is left, so its slots run nothing and play, suites and exports report its import failure. The edits refuse every other problem.
- **`GRAPH_UNREACHABLE_NODE`** (Warning): a statement no event's flow reaches, which never runs.
- **`SCRIPT_TYPE_ERROR`** and **`SCRIPT_COMPILE_ERROR`**: the import's check of the transpiled text, with its `line` and node. A graph that validates type-checks, so these come only from Luau's own limits (below).

The limits a graph keeps:
- At most 4,096 nodes, 16,384 links, 128 fields, 128 variables, 64 literals a node and 64 enum choices; names of 1 to 64 bytes (a Luau identifier that is no reserved word, does not start with `__`, and for a member is not `Entity`, `Fields` or a callback's name), kinds of at most 256 bytes, strings of 4,096 and tooltips of 256; positions within ±10^6.
- A statement's code starts at most 64 blocks deep in the text (each branch, loop body, delayed flow, `Flow.IfNotNil` flow and statement block nests one), and the text holds at most 8 MiB. Passing either is a `GRAPH_INVALID` at the statement that passes it, which the edit that would pass it refuses. Flatten a deep flow with `Flow.Sequence`, or link several exec outputs into one node.
- Every pure value a statement reads is computed again for that statement, so a long chain of values read by many statements grows the text: store it once in a variable (`Self.Set`) and read the variable.
- Luau allows 200 locals a function: a statement that reads more than about 200 values validates, but its import fails with `SCRIPT_COMPILE_ERROR` at its node's line. Split it.
- A kind whose binding left the script API (renamed or removed) makes its graphs `GRAPH_INVALID` until they are edited, as a Luau script gets a type error; every graph is imported again whenever the script API changes. A parameter the binding renamed or removed leaves its node's link or literal on a pin the kind no longer has: `graph_disconnect` removes the link and `graph_set_values` with `null` the literal (the Graph Editor shows such a pin in the error colour).

## Live values

During play, `graph_live_values {entity, script?}` reads a graph behaviour's instance in the play scene (`script`: the graph's current behaviour name, the first of two slots of one name; without it the entity's first graph slot) between frames, a test run's session included, running no Luau and allocating nothing in the script VM, so it changes nothing the state hash covers and a lockstep session goes on exactly as it would without the read.
- `values`: each node output's last value, `{node, pin, type, value}`, sorted by node and pin. A pure node's value is the one computed for the last statement that read it; a node that has not run yet has no entry.
- `members`: the graph's fields and variables, `{name, type, value}`.
- `errors`: the instance's retained script errors raised by the graph's own code, each with its node (`{node, line, message, count}`).
- `tick`, the session's tick, and `truncated`, true when a list was cut at 16,384 entries.

Values come in reflection's JSON spelling (a number, a boolean, a string, `[x, y, z]`, `[r, g, b, a]`, `[x, y, z, w]`, 16 hex digits for an entity or an asset); a table, a function, a component proxy, a non-finite number or a string that is not valid UTF-8 is `null`, its `type` naming it. Without a play session, or in a simulation, the call is `InvalidState`. In a lockstep session call it between `play_step`s. Of two graph slots of one behaviour name on an entity, only the first can be read: give the graphs different names (`graph_set_members {name}`) to read both. The editor's Graph Editor shows the same values under its nodes and beside its members while a graph is open during play, a test run's included, and the inspector beside a graph slot's members.

## Debugging a graph

- **It does nothing.** `graph_read {graph}`: any `GRAPH_INVALID`, an `incomplete` one included, keeps it from importing. `project_validate` reports the same for every graph, and `GRAPH_UNREACHABLE_NODE` names the statements no event reaches (link them into an event's flow).
- **A runtime error.** `script_errors` names `<path>.graph:<line>`, a line of the transpiled text: `graph_read {graph, luau: true}`'s `lineNodes` gives its node, as the `-- <Id>: <Kind>` comment above it does, and `graph_live_values`' `errors` maps it for you. An exported game's log names the line too. The usual cause is a required `Entity` or `Asset` value that is nil at run time (an unset field read through `Self.Get`, an asset that no longer exists): test it with `Flow.IfNotNil` first.
- **A wrong value.** Step in lockstep and read `graph_live_values`: the first node whose value differs from what you expect is the bug. `script_eval {context: "Play", entity, script: "<Name>", code: "return self.<Member>"}` reads a member too.
- **A refused link.** The error names the place in your params and the reason: types that do not link (`Text.Concat` turns any value into text), an optional output into a required input (put `Flow.IfNotNil` between), a cycle, or a flow nested too deep. A second link out of an exec output replaces the first: use `Flow.Sequence` to run several statements in order.
- **An edit that play does not show.** A lockstep, recording, replay or test session sees a graph edit only after it ends: `play_stop`, then play again.
- **Loads.** The export finds a graph's `Call.Assets.Load` as it finds a Luau script's: a literal `reference` keeps its asset in a pruned export, while a linked one computes its path, which `project_validate` warns about as `SCRIPT_COMPUTED_ASSET_LOAD` under `Export.PruneUnreferenced` (a pruned export leaves out an asset only it names): set the path as a literal, or load the asset through an `Asset` field.
