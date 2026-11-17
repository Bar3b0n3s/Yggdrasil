#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Reflection/VariantValue.h"

#include <glm/vec2.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// The visual-scripting graph (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decisions 2 to 5): the .graph file, format
// version 2 (M39, Docs/Decisions/0056-m39-graphs-behaviours.md decision 1: methods, the world-pause opt-in and the Any kind),
// and the document behind it. A graph is one behaviour. GraphImporter (AssetPipeline/Importers/GraphImporter.h)
// transpiles it to Luau (AssetPipeline/Graph/GraphTranspiler.h) and compiles that text through the script pipeline, so a
// graph is a Script asset whose importer is "Graph" (decision 2), never an AssetType of its own: the Script component's
// slots, the script engine, hot reload, the export's paks and Dist see only its compiled behaviour. This header holds the
// document and the rules that need no node library; the rules that do (kinds, pins, types, links, completeness) are
// ValidateGraph's (AssetPipeline/Graph/GraphNodeLibrary.h). The graph code lives in AssetPipeline, editor-only, which the
// Runtime never links and the Dist map check's AssetPipeline family keeps out of the Dist Runtime (decision 3).
//
// Conventions, which every reader, writer and method follows (decision 4):
//   - Names. The behaviour's Name (its Script.Define name), and every field's and variable's Name, is a Luau identifier
//     ([A-Za-z_][A-Za-z0-9_]*) of 1 to MaxGraphNameBytes bytes that is not one of Luau's reserved words ("and", "break", "do",
//     "else", "elseif", "end", "false", "for", "function", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then",
//     "true", "until", "while"; the transpiler writes "<Name> = Field..." and "self.<Name>") and does not start with "__"
//     (IsGraphIdentifier). A field's, variable's or method's name is also not a reserved script field name
//     (IsReservedScriptFieldName, Asset/ScriptData.h: Entity, Fields and the callbacks), since it becomes an instance or class
//     member, and is unique across the three lists; a method's is also not RunsWhileWorldPaused, the class key Script.Define
//     reads as a boolean.
//   - Nodes. Each has an Id in [1, MaxGraphNodeId], unique in the graph and stable for the node's life (graph.addNode gives
//     GetNextGraphNodeId's), and a Kind of 1 to MaxGraphKindBytes bytes: dot-separated non-empty parts of [A-Za-z0-9_]
//     ("Flow.Branch", "Call.Input.GetAxis", "Self.Get.Speed"). Position is the canvas place of the node's top-left corner,
//     finite, each component within MaxGraphPosition; layout only: the transpiler never reads it. Values are literals of the
//     node's input pins, keyed by pin name (a Luau identifier of at most MaxGraphNameBytes bytes), at most MaxGraphNodeValues
//     of them, each JSON null, a boolean, a finite number, a string of at most MaxGraphStringBytes bytes of valid UTF-8, or an
//     array of 3 or 4 finite numbers (a vector, a colour, a quaternion). A literal whose pin is also linked is kept, and the
//     link wins (unlinking restores the literal). A finite number is one JsonWriter writes back as itself: an integer, or a
//     fractional number in float's range that is the float it rounds to (the writer writes every fractional number as that
//     float, -0 as 0), so a file and its canonical re-save hold the same numbers and transpile to the same text; 0.30000001
//     or 1e-50, which the writer would write as 0.3 or 0, is refused, the message naming the float to write.
//   - Links. From names an output pin and To an input pin (the library checks the directions); both nodes exist and differ;
//     each (From, To) once. A link's nodes are checked once every node id is valid and in order; until then the ids' issues
//     stand for them.
//   - Members. A field (a behaviour field: serialized, set per script slot in the inspector) or a variable (instance state,
//     created at OnCreate) has a Kind (GraphValueKind) and a Default in the JSON spelling of that kind: Number a finite
//     number, Integer an int32, Bool a boolean, String a string of at most MaxGraphStringBytes bytes of valid UTF-8, Vector
//     [x, y, z], Color [r, g, b, a] (linear), Quat [x, y, z, w] (finite, nonzero), Entity, Asset and Any null, Enum one of
//     EnumValues, Array []. A field is never of kind Any, which no Field constructor holds. Element is an Array's element kind
//     (any kind but Array and Any) and Number for every other kind; AssetType is an Asset's (or an Array of Asset's) AssetType
//     name, other than None, and empty otherwise; EnumValues are an Enum's (or an Array of Enum's) 1 to MaxGraphEnumValues
//     unique choices, each 1 to MaxGraphNameBytes bytes of valid UTF-8 without NUL, and empty otherwise. A field's Options are the Luau FieldOptions it is declared with, so only a kind whose Field
//     constructor takes FieldOptions has any (Number, Integer, Bool, String, Vector, Color, Quat; never Entity, Asset, Enum or
//     Array): "Min", "Max" and "Step" (finite numbers, for Number, Integer and Vector only, Min <= Max, Step > 0) and "Tooltip"
//     (a string of at most MaxGraphTooltipBytes bytes of valid UTF-8 without NUL); a variable has none. A field's Default
//     also meets what its Field constructor requires of a default: a Number's, Integer's or Vector's (each component) within
//     Min and Max, compared as the field's reflected type compares them (a float against the bound rounded to float), and a
//     Quat's of unit length (within UnitQuaternionTolerance, Reflection/FieldType.h).
//   - Methods (M39, decision 1). A method has a Name, at most MaxGraphMethodParameters Parameters (each a Name, an
//     IsGraphIdentifier other than "self" and "Then" unique in its method, a Kind and an AssetType) and at most
//     MaxGraphMethodResults Results (each a Kind and an AssetType; none for a method that returns nothing). A parameter's or result's Kind is one
//     IsGraphMethodValueKind accepts (not Enum, not Array); its AssetType is empty, or for an Asset an AssetType name other
//     than None. RunsWhileWorldPaused true makes the behaviour run while the world is paused (ADR 0050 decision 2).
//   - Versions. A version 1 file reads as version 2 with no methods and RunsWhileWorldPaused false; in it Methods and
//     RunsWhileWorldPaused are unknown members and a variable of kind Any is refused.
//   - Canonical order. Fields, Variables and Methods sorted by Name, Nodes by Id, Links by (From.Node, From.Pin, To.Node,
//     To.Pin), all byte-wise, so the file and its edits are canonical (CanonicalizeGraph) and every write of one graph is
//     byte-identical.
//   - Limits. The counts and sizes below bound what external input controls, so a hostile file cannot exhaust memory.

namespace Engine {

	class TypeRegistry;

	// The .graph document header (decision 4).
	inline constexpr std::string_view GraphFormatName = "Graph";
	inline constexpr uint32_t GraphFormatVersion = 2;
	// The extension GraphImporter claims (Asset/SourceFormats.h's row: importer "Graph", ".graph", main type Script).
	inline constexpr std::string_view GraphFileExtension = ".graph";

	// The limits ValidateGraphStructure enforces (the file comment).
	inline constexpr size_t MaxGraphNodes = 4096;
	inline constexpr size_t MaxGraphLinks = 16384;
	inline constexpr size_t MaxGraphFields = 128;
	inline constexpr size_t MaxGraphVariables = 128;
	inline constexpr size_t MaxGraphMethods = 64;         // M39
	inline constexpr size_t MaxGraphMethodParameters = 4; // M39: Script.Call's Arg1 to Arg4
	inline constexpr size_t MaxGraphMethodResults = 1;    // M39
	inline constexpr size_t MaxGraphNodeValues = 64;
	inline constexpr size_t MaxGraphEnumValues = 64;
	inline constexpr size_t MaxGraphNameBytes = 64;
	inline constexpr size_t MaxGraphKindBytes = 256;
	inline constexpr size_t MaxGraphStringBytes = 4096;
	inline constexpr size_t MaxGraphTooltipBytes = 256;
	inline constexpr uint32_t MaxGraphNodeId = 0x7fffffffu;
	inline constexpr float MaxGraphPosition = 1.0e6f;

	// Registry enum "GraphValueKind": the kind of a field, a variable or a method's parameter or result, one per Luau Field
	// constructor (Architecture §11.2), and Any (M39), which no field has.
	enum class GraphValueKind : uint8_t
	{
		Number,  // Field.Number: a finite f32
		Integer, // Field.Integer: an exact int32
		Bool,    // Field.Bool
		String,  // Field.String
		Vector,  // Field.Vector: a native vector
		Color,   // Field.Color: a linear RGBA Color
		Quat,    // Field.Quat
		Entity,  // Field.Entity: an Entity reference (a field) or an Entity value (a variable), nil by default
		Asset,   // Field.Asset(AssetType): an AssetRef of one type, nil by default
		Enum,    // Field.Enum(EnumValues): one of the choices, a string
		Array,   // Field.Array(Element): an array of Element values, empty by default
		Any      // M39: any value, nil by default; a variable's, a parameter's or a result's, never a field's
	};

	// Registry struct "GraphField": one behaviour field.
	struct GraphField
	{
		std::string Name{};
		GraphValueKind Kind = GraphValueKind::Number;
		VariantValue Default{}; // the kind's JSON spelling (the file comment)
		GraphValueKind Element = GraphValueKind::Number;
		std::string AssetType{};
		std::vector<std::string> EnumValues{};
		std::map<std::string, VariantValue> Options{}; // "Min", "Max", "Step", "Tooltip"

		bool operator==(const GraphField&) const = default;
	};

	// Registry struct "GraphVariable": one variable, a member of every instance that OnCreate initializes to its Default.
	struct GraphVariable
	{
		std::string Name{};
		GraphValueKind Kind = GraphValueKind::Number;
		VariantValue Default{};
		GraphValueKind Element = GraphValueKind::Number;
		std::string AssetType{};
		std::vector<std::string> EnumValues{};

		bool operator==(const GraphVariable&) const = default;
	};

	// Registry struct "GraphParameter": one parameter of a graph method (M39).
	struct GraphParameter
	{
		std::string Name{};
		GraphValueKind Kind = GraphValueKind::Any;
		std::string AssetType{};

		bool operator==(const GraphParameter&) const = default;
	};

	// Registry struct "GraphResult": the result of a graph method (M39).
	struct GraphResult
	{
		GraphValueKind Kind = GraphValueKind::Any;
		std::string AssetType{};

		bool operator==(const GraphResult&) const = default;
	};

	// Registry struct "GraphMethod": a method of the behaviour (M39), which a script calls on its instance and whose
	// Method.<Name> event nodes run.
	struct GraphMethod
	{
		std::string Name{};
		std::vector<GraphParameter> Parameters{};
		std::vector<GraphResult> Results{}; // empty: the method returns nothing; at most MaxGraphMethodResults

		bool operator==(const GraphMethod&) const = default;
	};

	// Registry struct "GraphPinRef": one pin of one node.
	struct GraphPinRef
	{
		uint32_t Node = 0;
		std::string Pin{};

		auto operator<=>(const GraphPinRef&) const = default;
	};

	// Registry struct "GraphNode": one node.
	struct GraphNode
	{
		uint32_t Id = 0;
		std::string Kind{};
		glm::vec2 Position = glm::vec2(0.0f);
		std::map<std::string, VariantValue> Values{}; // input-pin literals by pin name

		bool operator==(const GraphNode&) const = default;
	};

	// Registry struct "GraphLink": a link from an output pin to an input pin.
	struct GraphLink
	{
		GraphPinRef From{};
		GraphPinRef To{};

		bool operator==(const GraphLink&) const = default;
	};

	// A graph: registry struct "Graph". Every field is serialized under its member name, in member order, after "Format":
	// "Graph" and "Version": 2. Plain data.
	struct GraphDocument
	{
		std::string Name{};                // the behaviour's Script.Define name
		bool RunsWhileWorldPaused = false; // M39: the class's world-pause opt-in
		std::vector<GraphField> Fields{};
		std::vector<GraphVariable> Variables{};
		std::vector<GraphMethod> Methods{}; // M39
		std::vector<GraphNode> Nodes{};
		std::vector<GraphLink> Links{};

		bool operator==(const GraphDocument&) const = default;
	};

	// Registers the enum "GraphValueKind" and the structs "GraphPinRef", "GraphField", "GraphVariable", "GraphParameter",
	// "GraphResult", "GraphMethod", "GraphNode", "GraphLink" and "Graph" (GraphDocument). ValidateGraphStructure's rules are
	// the "Graph" struct's Validate hook (all of them but the Name's on an empty Name, the registry's default object, which
	// GraphFromJson still refuses), so a document reports each violation once, in document order; its Generate hook keeps
	// the registry suite's random graphs valid, and the member structs have neither. RegisterAssetPipelineTypes calls it (stream G-A), so graphs read and write wherever the
	// importers' types are registered (the editor, the tests' built-in registry); the Runtime has none.
	void RegisterGraphTypes(TypeRegistry& registry);

	// Whether `name` is a Luau identifier of 1 to MaxGraphNameBytes bytes that is not a Luau reserved word and does not start
	// with "__" (the file comment). Pure.
	[[nodiscard]] bool IsGraphIdentifier(std::string_view name);

	// Whether a method's parameter or result may be of kind `kind` (M39): every GraphValueKind but Enum, whose choices a
	// declaration does not carry, and Array, which an Any carries. Pure.
	[[nodiscard]] bool IsGraphMethodValueKind(GraphValueKind kind);

	// Whether `name` may name a method (M39): an IsGraphIdentifier that is not a reserved script field name and not
	// RunsWhileWorldPaused. Uniqueness is the document's rule. Pure.
	[[nodiscard]] bool IsGraphMethodName(std::string_view name);

	// Checks every rule of the file comment that needs no node library: the names, the ids, the kinds' spelling, the
	// positions, the values' shapes, the links' nodes, the members' defaults, elements, asset types, enum values and options,
	// the methods' declarations, the canonical order and the limits. Pure. Errors: Validation with one issue per violation,
	// each located by its JSON pointer into the .graph document ("/Name", "/Fields/2/Default", "/Nodes/5/Kind",
	// "/Nodes/5/Values/speed", "/Links/3").
	[[nodiscard]] Status ValidateGraphStructure(const GraphDocument& graph);

	// Sorts `graph`'s lists into the canonical order of the file comment and drops a link repeated with the same (From, To).
	// Pure; changes nothing else.
	void CanonicalizeGraph(GraphDocument& graph);

	// The node with id `id`, or nullptr. `graph`'s Nodes are sorted by Id (a binary search). Pure.
	[[nodiscard]] const GraphNode* FindGraphNode(const GraphDocument& graph, uint32_t id);

	// The id graph.addNode gives a new node: one more than the highest id in `graph`, 1 for an empty graph, and 0 when the
	// highest id is MaxGraphNodeId (no further node fits). Pure.
	[[nodiscard]] uint32_t GetNextGraphNodeId(const GraphDocument& graph);

	// What a graph load reports besides its result: the file's version and its warnings (unknown members) in document order.
	struct GraphLoadReport
	{
		uint32_t FileVersion = 0;
		std::vector<ValidationIssue> Diagnostics{};
	};

	// The canonical .graph document ("Format", "Version", then the registry fields in member order, every field written, §6).
	// Errors: Validation for a value that cannot be written (a non-finite position), located. Requires a frozen registry on
	// which RegisterGraphTypes ran (asserted; InvalidState without asserts).
	[[nodiscard]] Result<Json> GraphToJson(const GraphDocument& graph, const TypeRegistry& registry);

	// The canonical text of GraphToJson: Pretty for .graph files.
	[[nodiscard]] Result<std::string> GraphToText(const GraphDocument& graph, const TypeRegistry& registry, JsonStyle style = JsonStyle::Pretty);

	// Reads a .graph document strictly through the registry (StructInfo::FromJson of "Graph": missing keys keep their defaults,
	// unknown keys warn into `report`, or fail with `strictUnknowns`), then ValidateGraphStructure. A document of an older
	// version is migrated to GraphFormatVersion here, so every reader sees the current form and project.upgrade rewrites it
	// (decision 10): version 1 reads as version 2 with no methods and RunsWhileWorldPaused false, its Methods and
	// RunsWhileWorldPaused unknown members (a REFLECTION_UNKNOWN_FIELD warning that drops them, an error with
	// `strictUnknowns`) and a variable of kind Any refused (M39). Errors: Validation (located, every bad field an issue) for a
	// wrong "Format", a version below 1, a wrong JSON type, an out-of-range value or a ValidateGraphStructure violation;
	// UnsupportedVersion naming both versions for a newer file.
	[[nodiscard]] Result<GraphDocument> GraphFromJson(const Json& document, const TypeRegistry& registry, GraphLoadReport& report,
		bool strictUnknowns = false);

	// JsonReader::Parse, then GraphFromJson. Errors: Parse (line and column), and as GraphFromJson.
	[[nodiscard]] Result<GraphDocument> GraphFromText(std::string_view text, const TypeRegistry& registry, GraphLoadReport& report,
		bool strictUnknowns = false);

}
