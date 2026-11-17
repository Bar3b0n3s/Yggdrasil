#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/AssetPipeline/Graph/GraphDocument.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The graph transpiler (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decisions 7 and 8; stream G-B): one graph to
// one behaviour's Luau text, which GraphImporter compiles through the script pipeline, so a graph runs as any behaviour
// runs, deterministic in every configuration, and Dist needs no compiler for it (ADR 0028 decision 6).
//
// The output contract (decision 7), frozen:
//   - One text per graph: a pure function of the document, the library and `path`, byte-identical in every configuration
//     (no pointer, no unordered iteration, no locale; numbers in the shortest form that reads back to the same double,
//     strings in double quotes with Luau escapes for '"', '\' and the control bytes and every other byte kept), LF line
//     ends and a final newline.
//   - Its layout, in order: "--!strict"; the comment line "-- Generated from <path> by the graph transpiler: edit the
//     graph, not this text."; "local Graph = {}"; "Graph.Fields = { ... }" with one "<Name> = Field.<Kind>(<default>,
//     <options>)," line per field in the graph's (byte-wise) order, Array fields as Field.Array(<element constructor>);
//     "Graph.RunsWhileWorldPaused = true" when the graph opts in (M39; nothing otherwise);
//     "function Graph.OnCreate(self: any)", always, which first sets "self.__GraphLive = {}" (ScriptLiveValuesKey,
//     Scripting/ScriptEngine.h), then each variable to its Default in name order, then runs the OnCreate event nodes' flows;
//     then one "function Graph.<Callback>(self: any, <the callback's parameters>)" per other callback the graph has an event
//     node of, in byte-wise callback order, each running its event nodes' flows in id order; then (M39) one
//     "function Graph.<Method>(self: any, p_<parameter>: <type>, ...)" per declared method in name order, running its
//     Method.<Name> nodes' flows in id order, whether or not it has any; and last "return Script.Define("<Name>", Graph)".
//     Nothing else is global: no require, no upvalue across functions.
//   - Names, stable under edits that do not touch them: a statement's outputs are the locals "n<Id>_<Pin>"; a pure node's
//     value computed for one statement is "v<Id>_<Pin>" inside that statement's own do ... end block (so a function's
//     active locals stay below Luau's 200); an event's data outputs are its callback's parameters, a Method node's its
//     method's parameters "p_<Name>" (so no parameter shadows a global or a generated local); a flow that several exec links
//     enter is the local function "flow<Id>" of its event function; Script.Set's and Script.Call's object is the local
//     "o<Id>: any". Each node's code starts with the comment line "-- <Id>: <Kind>".
//   - Semantics: a statement runs when its "In" is reached and continues at "Then" (Flow's outputs as decision 6 states);
//     a pure node is evaluated again for every statement input it feeds (in pin order, depth first), so a value is always
//     current; Flow.Delay continues at "Then" at once and at "Later" from Task.Delay(<Seconds>, function() ... end);
//     Flow.IfNotNil continues at "True" when its Value is not nil (its Result is the Value) and at "False" otherwise; a
//     statement's output read later in its flow is the value of its last run. M39
//     (Docs/Decisions/0056-m39-graphs-behaviours.md decisions 2 and 4): Script.Get is "(<object> :: any)[<member>]",
//     Script.Set "o<Id>[<member>] = <value>", Script.Call "n<Id>_Result = (o<Id>:<Method>(<Arg1..Arg4>) :: any)" for a
//     literal Method that is a Luau name, else "(o<Id>[<method>](o<Id>, ...) :: any)", trailing unset arguments left out,
//     Self.Instance "self". A Return ends the function that runs it, Luau's return, always in a do ... end block of its
//     own: "return <Value>" ("return" without a value); a merge whose flow reaches a Return is "local function
//     flow<Id>(): boolean", in which a Return sets the function's local "result" (declared "local result: any") and
//     returns true and which ends with "return false" unless it always returns, and each of its calls is "if flow<Id>()
//     then return result end" ("return" without a value; "return true" inside another flow function). A function that
//     returns a value (a method with a result, a callback's with a Return of a value) ends with "return nil" unless its
//     body always returns, judged as Luau's UnreachableCode lint judges a block, so every path returns one value and the
//     text adds no unreachable line of its own. A graph without methods, Returns or the opt-in has exactly M24's text.
//   - Values and types: a literal is written in Luau's spelling of its pin's type (a vector as vector.create(x, y, z), a
//     Color as Color.New(r, g, b, a), a Quat as Quat.New(x, y, z, w), an Enum's choice as its string); an Asset literal (an
//     AssetHandle) as (Assets.Load("<16 hex digits>") :: AssetRef), an AssetRef as a required Asset input takes, nil at run
//     time only for an asset that no longer exists (a located runtime error where it is passed on); an Any pin's value is
//     typed any (an Any output's local is annotated ": any"), so an Any input takes any value, nil included, and an Any
//     output feeds every data input. With AreGraphPinsCompatible's optional rule, the text of every graph that validates
//     passes the strict checker.
//   - Live values (decision 14): every value the text computes, a pure node's and a statement's data outputs, is also
//     written to "self.__GraphLive[<key>]", the key GetGraphProbeKey's "<Id>.<Pin>" as a string constant, so the editor
//     reads a node's last value between frames (ScriptEngine::ReadInstanceValues) without running Luau. The writes change
//     no state the state hash covers: instance tables are outside it, as every script's are (ADR 0028's phase-3 ledger).
//   - Source mapping (decision 8): LineNodes names the node whose code each line is, so a compile, type or runtime error at a
//     line of the text maps back to its node (FindGraphNodeAtLine).
//   - The text type-checks without an error in the engine's strict checker for every graph that validates.

namespace Engine {

	// One value the text writes for the live values: a data output of a node, keyed "<Node>.<Pin>".
	struct GraphProbe
	{
		uint32_t Node = 0;
		std::string Pin{};
		std::string Key{}; // GetGraphProbeKey(Node, Pin)

		bool operator==(const GraphProbe&) const = default;
	};

	// One graph's Luau text with its maps.
	struct GraphTranspileResult
	{
		std::string Luau{};
		// LineNodes[i] is the node whose code line i + 1 is, 0 for the layout's own lines; one entry per line of Luau.
		std::vector<uint32_t> LineNodes{};
		// Every value the text writes to its live-value table, sorted by (Node, Pin).
		std::vector<GraphProbe> Probes{};

		bool operator==(const GraphTranspileResult&) const = default;
	};

	// Transpiles `graph`, which must pass ValidateGraphStructure, against `library`, naming `path` (the project-relative
	// .graph path, "Assets/Scripts/Door.graph") in the header comment. Pure and thread-safe. Errors: Validation carrying every
	// ValidateGraph issue (Incomplete ones included) as located issues, when there is any: only a complete, valid graph has a
	// text.
	[[nodiscard]] Result<GraphTranspileResult> TranspileGraph(const GraphDocument& graph, const GraphNodeLibrary& library, std::string_view path);

	// The node whose code line `line` (1-based) of `result`'s text is; 0 for a layout line and for a line outside the text.
	// Pure.
	[[nodiscard]] uint32_t FindGraphNodeAtLine(const GraphTranspileResult& result, uint32_t line);

	// The live-value key of a node's output pin: "<node>.<pin>" ("12.Result"). Pure.
	[[nodiscard]] std::string GetGraphProbeKey(uint32_t node, std::string_view pin);
	// The node and pin of a live-value key; nullopt for a string GetGraphProbeKey does not make (a node id of 0 or out of
	// range, an empty pin, no dot). Pure.
	[[nodiscard]] std::optional<GraphPinRef> ParseGraphProbeKey(std::string_view key);

	// What a graph names by handle, which its importer lists in Import.Dependencies (ADR 0028's phase-3 ledger: every asset a
	// node names by handle): every non-null Asset literal of its nodes' inputs, each once, sorted, as a component's asset
	// references are listed. A literal path a node loads (Call.Assets.Load's "reference") is not among them: the exporter
	// finds it, with the computed loads, by scanning the graph's transpiled text as it scans a .luau source (decision 24), so
	// a graph's loads behave exactly as a Luau script's.
	struct GraphAssetLiterals
	{
		std::vector<AssetHandle> Handles{};

		bool operator==(const GraphAssetLiterals&) const = default;
	};

	// GraphAssetLiterals of `graph` (which passes ValidateGraphStructure) against `library`. Pure.
	[[nodiscard]] GraphAssetLiterals FindGraphAssetLiterals(const GraphDocument& graph, const GraphNodeLibrary& library);

}
