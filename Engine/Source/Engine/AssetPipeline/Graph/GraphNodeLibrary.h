#pragma once

#include "Engine/AssetPipeline/Graph/GraphDocument.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The graph node library (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decision 6; stream G-B): what each node kind of
// a graph is, its pins and their types, and the rules a graph must keep against it (ValidateGraph). The library grows from
// the script API instead of by hand: GraphNodeLibrary::Create derives a node from every member of the frozen ScriptApiRegistry
// whose signature it can model (the Behaviour callbacks' events, module functions, type methods, properties, constants and
// constructors, and the record aliases' fields), and adds the hand-written built-ins of decision 6 (flow control, members,
// operators). A binding added to the API therefore adds its node, and a binding's change changes its node; the importer's
// cache follows through the script checker's environment hash, which the same registry's declarations make (decision 9).
//
// Kinds (decision 6):
//   - Generated: "Event.<Callback>" (the Behaviour type's callbacks), "Call.<Group>.<Member>" (a function, method or
//     constructor of a module or type), "Get.<Group>.<Member>" and "Set.<Group>.<Member>" (a readable or writable property or
//     component proxy field, or a record alias's field, read only), "Const.<Group>.<Member>" (a constant).
//   - Built-in: "Flow.Branch", "Flow.Sequence", "Flow.ForRange", "Flow.ForEach", "Flow.Delay", "Flow.IfNotNil"; "Self.Entity";
//     "Self.Get.<Name>" and "Self.Set.<Name>" for each field and variable of the graph (Resolve, never in GetNodes);
//     "Number.Add", "Number.Subtract", "Number.Multiply", "Number.Divide", "Number.Modulo", "Number.Negate"; "Compare.Equal",
//     "Compare.NotEqual", "Compare.Less", "Compare.LessEqual", "Compare.Greater", "Compare.GreaterEqual"; "Logic.And",
//     "Logic.Or", "Logic.Not"; "Vector.Make", "Vector.Split", "Vector.Add", "Vector.Subtract", "Vector.Scale", "Vector.Dot",
//     "Vector.Cross", "Vector.Length", "Vector.Normalize"; "Text.Concat".
//   - Eligible members: available in every run mode (RunModes::All: a graph runs alike in the editor, Release and Dist) and
//     at run time (ScriptApiEnvironment::Runtime), whose signature is a single function type (no generic, no overload
//     intersection) of parameters and results the pin types model, a trailing variadic parameter ("...any") becoming one
//     optional input "Values" passed as its one argument; every other member has no node. A union of types other than an
//     enum's strings and nil ("boolean | number", OnUiValueChanged's value) is an Any pin.
//   - Optional values: a Luau "T?" input is optional (unlinked and unset, it passes nil), and a "T?" output (a result, a
//     property, a constant or a callback parameter: Assets.Load's Result, an Entity's component proxies, Scene.FindByName's
//     Result) may give nil, so it feeds only an optional input, an Any input, or Flow.IfNotNil, whose Result (Any, the value on
//     its True flow) feeds any data input: AreGraphPinsCompatible refuses a link whose transpiled text the strict checker
//     would refuse ("value of type T? could be nil"). A member's own value (Self.Get) is typed by its kind, never optional.
//   - Events: an Event node's function returns nothing, so a UI event (OnUiClick and the other OnUi* callbacks, whose Luau
//     result true consumes the event) never consumes it and always bubbles on (decision 23's kept limit).
//   - Pin names: "In" and "Then" for the exec pins of a statement node (decision 6 names Flow's others), the Luau parameter
//     names for data inputs ("self" for a method's or a property's object), "Value" for a property's and a member's value,
//     "Result" for a single result and "Result1", "Result2", ... for several.
//   - Purity: a member registered with Mutates false that returns a value, a getter, a constant and an operator is pure (no
//     exec pins, evaluated where each statement needs its outputs); every other node, a member that returns nothing (Log.Info)
//     included, is a statement (an "In" and a "Then" exec pin, run in the order the exec links give).

namespace Engine {

	class ScriptApiRegistry;

	// Registry enum "GraphPinType": what a pin carries.
	enum class GraphPinType : uint8_t
	{
		Exec,   // control flow, no value
		Any,    // Luau's any: links to every data pin
		Bool,   // boolean
		Number, // number
		String, // string
		Vector, // vector
		Color,  // Color
		Quat,   // Quat
		Entity, // Entity
		Asset,  // AssetRef
		Enum,   // a registered enum's string union; TypeName names it
		Object  // any other named type (an extern type such as a component proxy or TaskHandle, or a record alias such as
				// CollisionContact); TypeName names it
	};

	// The enumerator name ("Number"); "Unknown" for a value outside the enumeration. Pure.
	[[nodiscard]] std::string_view GraphPinTypeToString(GraphPinType type);

	// Registry enum "GraphPinDirection".
	enum class GraphPinDirection : uint8_t
	{
		Input,
		Output
	};

	// Registry enum "GraphNodeCategory": where the canvas's node menu lists a kind.
	enum class GraphNodeCategory : uint8_t
	{
		Event,    // Event.*
		Flow,     // Flow.*
		Member,   // Self.*
		Operator, // Number.*, Compare.*, Logic.*, Vector.*, Text.*
		Call,     // Call.*
		Property, // Get.*, Set.*
		Constant  // Const.*
	};

	// One pin of a node kind (registry struct "GraphPinInfo", graph.nodes' wire form).
	struct GraphPinDescription
	{
		std::string Name{};
		GraphPinDirection Direction = GraphPinDirection::Input;
		GraphPinType Type = GraphPinType::Any;
		// Enum and Object: the type's name; Asset: the AssetType a member restricts it to, or empty; otherwise empty.
		std::string TypeName{};
		// An input whose Luau type is optional (T?): left unlinked and without a literal, it passes nil. A required input of
		// type Bool, Number, String, Vector, Color, Quat or Enum without a link or a literal takes Default; one of type Entity,
		// Asset, Object or Any must be linked or (Asset, Any) hold a literal, or the graph is incomplete. An output whose Luau
		// type is optional (T?) may give nil: it feeds only an optional input or an Any input (AreGraphPinsCompatible), such as
		// Flow.IfNotNil's Value.
		bool Optional = false;
		VariantValue Default{}; // the literal an unlinked required input takes (false, 0, "", [0,0,0], white, identity, an enum's first value); null otherwise

		bool operator==(const GraphPinDescription&) const = default;
	};

	// One node kind (registry struct "GraphNodeInfo", graph.nodes' wire form). Pins are ordered: the exec input, the data
	// inputs in signature order, the exec outputs, the data outputs in result order.
	struct GraphNodeDescription
	{
		std::string Kind{};
		std::string Title{}; // the canvas's caption ("Get Axis", "Branch")
		GraphNodeCategory Category = GraphNodeCategory::Call;
		std::string Description{}; // the member's registered description, or the built-in's
		bool Pure = false;
		std::vector<GraphPinDescription> Pins{};

		bool operator==(const GraphNodeDescription&) const = default;
	};

	// Whether a link from `from` (an output) to `to` (an input) carries a compatible value, so that the transpiled text
	// type-checks: exec to exec; Any to or from any data pin; an optional output (T?) only into an optional input; then a data
	// pin to a data pin of the same type (Enum and Object also of the same TypeName), and an Enum output to a String input.
	// So Assets.Load's Result (an optional Asset) feeds Animator.Play's optional clip and Flow.IfNotNil's Value, never
	// Audio.PlayOneShot's required clip. Directions are not checked here. Pure.
	[[nodiscard]] bool AreGraphPinsCompatible(const GraphPinDescription& from, const GraphPinDescription& to);

	// The validation codes of graphs (project.validate, Architecture §13.7), appended after M23's ANIMATION_SKELETON_MISMATCH in
	// both of ProjectValidator.cpp's tables (ADR 0028's phase-3 ledger), each with its Tests/Data/Validation/<CODE>/ fixture and
	// its row in the game-debugging skill's validation-codes page (stream G-D):
	//   - GRAPH_INVALID (Error, not fixable): one per ValidateGraph issue of a graph, located at its JSON pointer and node; the
	//     graph does not import (no artifact; the import's failure is this code's, never a second ASSET_IMPORT_FAILED).
	//   - GRAPH_UNREACHABLE_NODE (Warning, not fixable): a statement node that no event's exec flow reaches, so it never runs.
	inline constexpr std::string_view GraphInvalidCode = "GRAPH_INVALID";
	inline constexpr std::string_view GraphUnreachableNodeCode = "GRAPH_UNREACHABLE_NODE";

	// One ValidateGraph finding.
	struct GraphIssue
	{
		std::string JsonPointer{}; // into the .graph document: "/Nodes/4/Kind", "/Links/2", "/Nodes/4/Values/speed"
		uint32_t Node = 0;         // the node it concerns; 0 for the graph's members or name
		std::string Message{};
		// An issue of an unfinished graph that the edit methods accept on the way to a finished one (a required input not
		// linked yet; an output read where its statement may not have run, or Flow.IfNotNil's Result where its Value may be
		// nil); every other issue is refused by the method that would make it. The import refuses both kinds.
		bool Incomplete = false;

		bool operator==(const GraphIssue&) const = default;
	};

	// The node kinds of decision 6, built once from a frozen script API registry. Immutable after Create, so its const members
	// are thread-safe (import workers share none: each import builds its own, as the load-time VM builds its registry).
	class GraphNodeLibrary
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class GraphNodeLibrary;
		};

		explicit GraphNodeLibrary(ConstructionKey key);
		~GraphNodeLibrary();

		GraphNodeLibrary(const GraphNodeLibrary&) = delete;
		GraphNodeLibrary& operator=(const GraphNodeLibrary&) = delete;

		// Derives the generated kinds from `api` (frozen; InvalidState otherwise) by the eligibility rule of the file comment
		// and adds the built-ins. Pure function of the registry: the same registry gives the same library in every
		// configuration. Errors: InvalidState for an unfrozen registry; Validation when two members would give one kind.
		[[nodiscard]] static Result<Scope<GraphNodeLibrary>> Create(const ScriptApiRegistry& api);

		// Every kind but the graph-dependent Self.Get and Self.Set ones, sorted by Kind byte-wise.
		[[nodiscard]] std::span<const GraphNodeDescription> GetNodes() const;
		// The kind `kind` of GetNodes, or nullptr.
		[[nodiscard]] const GraphNodeDescription* Find(std::string_view kind) const;
		// Find, or "Self.Get.<Name>" and "Self.Set.<Name>" for a field or variable of `graph` (its Value pin typed by the
		// member's kind). Errors: NotFound naming the kind, with up to three close kinds as suggestions.
		[[nodiscard]] Result<GraphNodeDescription> Resolve(std::string_view kind, const GraphDocument& graph) const;
		// The Self.Get and Self.Set kinds of `graph`'s members, sorted by Kind.
		[[nodiscard]] std::vector<GraphNodeDescription> GetMemberNodes(const GraphDocument& graph) const;
		// XXH64 of the canonical listing of every kind, for tests and logs.
		[[nodiscard]] uint64_t GetFingerprint() const;
		// The choices of the registered enum an Enum pin names (its TypeName), in declaration order; empty for a name the
		// registry does not hold. ValidateGraph checks an Enum literal against them.
		[[nodiscard]] std::span<const std::string> GetEnumChoices(std::string_view enumName) const;
	private:
		struct State;
		Scope<State> m_State{};
	};

	// Every rule a graph keeps against `library`, beyond ValidateGraphStructure's (which `graph` passes): every kind resolves;
	// every value names a data input of its node and fits its type (Bool, Number, String, Vector, Color, a nonzero Quat, an
	// Enum's choice, an Asset handle or null, an Any scalar; an Entity or Object input takes no literal); every link joins an
	// output to an input of compatible pins (AreGraphPinsCompatible); a data input and an exec output take at most one link
	// (an exec input any number); no cycle of data links and no exec cycle; a member node names a member of the graph; and,
	// as Incomplete issues, every required input without a Default is linked or holds a literal, every data link from a
	// statement's output reaches only nodes that statement dominates in its event's flow (it has run whenever they do, in
	// the same event function, Flow.Delay's later flow included), and Flow.IfNotNil's Result reaches only nodes its True
	// flow alone enters (where its Value is not nil). When every other issue is Incomplete, no statement's code starts more
	// than 64 blocks deep in the transpiled text (TranspileGraph's layout, where a branch, a loop's body, Flow.Delay's later
	// flow and a statement's block of pure values each nest one level), reported at the statement that first passes the
	// limit, and the text holds at most 8 MiB, reported at the statement whose code passes it, so a graph transpiles exactly
	// when it validates (Docs/Decisions/0035-m24-contract.md decision 7 and risk 1). Several event nodes of one callback are
	// valid and run in id order. The issues are in document order. Pure.
	[[nodiscard]] std::vector<GraphIssue> ValidateGraph(const GraphDocument& graph, const GraphNodeLibrary& library);

	// The statement nodes that no event's flow reaches (GRAPH_UNREACHABLE_NODE), sorted by id. Pure; `graph` validates.
	[[nodiscard]] std::vector<uint32_t> FindUnreachableGraphNodes(const GraphDocument& graph, const GraphNodeLibrary& library);

}
