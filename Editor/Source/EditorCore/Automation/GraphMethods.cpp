#include "EditorPCH.h"
#include "EditorCore/Automation/GraphMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

// graph.* (Docs/Decisions/0035-m24-contract.md decisions 12 to 14): the wire format and the registration are the contract's;
// stream G-C implements the edit handlers and W24.2's stream B graph.liveValues, each calling its registration functions from
// RegisterMethods.cpp.

namespace Engine {

	namespace Automation {

		Result<GraphCreateResult> GraphCreate(EditorMethodContext& /*context*/, const GraphCreateParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "graph.create (Docs/Decisions/0035-m24-contract.md decision 12, stream G-C)");
		}

		Result<GraphReadResult> GraphRead(EditorMethodContext& /*context*/, const GraphReadParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "graph.read (Docs/Decisions/0035-m24-contract.md decision 13, stream G-C)");
		}

		Result<GraphNodesResult> GraphNodes(EditorMethodContext& /*context*/, const GraphNodesParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "graph.nodes (Docs/Decisions/0035-m24-contract.md decision 13, stream G-C)");
		}

		Result<GraphAddNodeResult> GraphAddNode(EditorMethodContext& /*context*/, const GraphAddNodeParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "graph.addNode (Docs/Decisions/0035-m24-contract.md decision 12, stream G-C)");
		}

		Result<GraphRemoveNodesResult> GraphRemoveNodes(EditorMethodContext& /*context*/, const GraphRemoveNodesParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "graph.removeNodes (Docs/Decisions/0035-m24-contract.md decision 12, stream G-C)");
		}

		Result<GraphEditResult> GraphMoveNodes(EditorMethodContext& /*context*/, const GraphMoveNodesParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "graph.moveNodes (Docs/Decisions/0035-m24-contract.md decision 12, stream G-C)");
		}

		Result<GraphConnectResult> GraphConnect(EditorMethodContext& /*context*/, const GraphConnectParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "graph.connect (Docs/Decisions/0035-m24-contract.md decision 12, stream G-C)");
		}

		Result<GraphEditResult> GraphDisconnect(EditorMethodContext& /*context*/, const GraphDisconnectParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "graph.disconnect (Docs/Decisions/0035-m24-contract.md decision 12, stream G-C)");
		}

		Result<GraphEditResult> GraphSetValues(EditorMethodContext& /*context*/, const GraphSetValuesParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "graph.setValues (Docs/Decisions/0035-m24-contract.md decision 12, stream G-C)");
		}

		Result<GraphSetMembersResult> GraphSetMembers(EditorMethodContext& /*context*/, const GraphSetMembersParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "graph.setMembers (Docs/Decisions/0035-m24-contract.md decision 12, stream G-C)");
		}

		Result<GraphLiveValuesResult> GraphLiveValues(EditorMethodContext& /*context*/, const GraphLiveValuesParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "graph.liveValues (Docs/Decisions/0035-m24-contract.md decision 14, stream B)");
		}

	}

	void RegisterGraphMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<GraphPinType>("GraphPinType", "What a graph pin carries.")
			.Entry(GraphPinType::Exec, "Exec", "Control flow, no value.")
			.Entry(GraphPinType::Any, "Any", "Any value (Luau's any): links to every data pin.")
			.Entry(GraphPinType::Bool, "Bool", "A boolean.")
			.Entry(GraphPinType::Number, "Number", "A number.")
			.Entry(GraphPinType::String, "String", "A string.")
			.Entry(GraphPinType::Vector, "Vector", "A native three-component vector.")
			.Entry(GraphPinType::Color, "Color", "A linear RGBA Color.")
			.Entry(GraphPinType::Quat, "Quat", "A quaternion.")
			.Entry(GraphPinType::Entity, "Entity", "An Entity.")
			.Entry(GraphPinType::Asset, "Asset", "An AssetRef; typeName names its AssetType when the member restricts it.")
			.Entry(GraphPinType::Enum, "Enum", "One choice of a registered enum, a string; typeName names the enum.")
			.Entry(GraphPinType::Object, "Object", "A value of another named type (a component, a TaskHandle, a record such as CollisionContact); typeName names it.");
		registry.Enum<GraphPinDirection>("GraphPinDirection", "Whether a graph pin takes or gives its value.")
			.Entry(GraphPinDirection::Input, "Input", "The pin takes a value (or, for Exec, is where the flow enters).")
			.Entry(GraphPinDirection::Output, "Output", "The pin gives a value (or, for Exec, is where the flow continues).");
		registry.Enum<GraphNodeCategory>("GraphNodeCategory", "Where the graph editor's node menu lists a node kind.")
			.Entry(GraphNodeCategory::Event, "Event", "A behaviour callback: where a flow starts (Event.*).")
			.Entry(GraphNodeCategory::Flow, "Flow", "Branches, sequences, loops and delays (Flow.*).")
			.Entry(GraphNodeCategory::Member, "Member", "The behaviour's own entity, fields and variables (Self.*).")
			.Entry(GraphNodeCategory::Operator, "Operator", "Arithmetic, comparison, logic, vector and text operators.")
			.Entry(GraphNodeCategory::Call, "Call", "A script API function, method or constructor (Call.*).")
			.Entry(GraphNodeCategory::Property, "Property", "A script API property or component field (Get.*, Set.*).")
			.Entry(GraphNodeCategory::Constant, "Constant", "A script API constant (Const.*).");
		registry.Struct<GraphPinDescription>("GraphPinInfo", "One pin of a graph node kind.")
			.Field("name", &GraphPinDescription::Name, "The pin's name, which links and values use.")
			.Field("direction", &GraphPinDescription::Direction, "Input or Output.")
			.Field("type", &GraphPinDescription::Type, "What the pin carries.")
			.Field("typeName", &GraphPinDescription::TypeName, "Enum and Object: the type's name; Asset: its AssetType, or empty; otherwise empty.")
			.Field("optional", &GraphPinDescription::Optional, "An input that may stay unlinked and unset, passing nil; an output that may give nil, which feeds only an optional or Any input (Flow.IfNotNil checks it).")
			.Field("default", &GraphPinDescription::Default, "The literal an unlinked, unset required input takes; null when it must be linked or set.");
		registry.Struct<GraphNodeDescription>("GraphNodeInfo", "One graph node kind of the node library.")
			.Field("kind", &GraphNodeDescription::Kind, "The kind a node names (\"Flow.Branch\", \"Call.Input.GetAxis\").")
			.Field("title", &GraphNodeDescription::Title, "The graph editor's caption.")
			.Field("category", &GraphNodeDescription::Category, "Where the node menu lists it.")
			.Field("description", &GraphNodeDescription::Description, "What the node does: its script API member's description, or the built-in's.")
			.Field("pure", &GraphNodeDescription::Pure, "Whether it has no exec pins and is evaluated where each statement needs its outputs.")
			.Field("pins", &GraphNodeDescription::Pins, "Its pins: the exec input, the data inputs, the exec outputs, the data outputs.");

		registry.Struct<GraphCreateParams>("GraphCreateParams", "The params of graph.create.")
			.Field("path", &GraphCreateParams::Path, "The new graph's \"Assets/....graph\" path.")
			.Field("name", &GraphCreateParams::Name, "The behaviour's name (a Luau identifier); empty takes the file's stem.");
		registry.Struct<GraphCreateResult>("GraphCreateResult", "The graph graph.create made.")
			.Field("graph", &GraphCreateResult::Graph, "The new graph, a Script asset.")
			.Field("name", &GraphCreateResult::Name, "The behaviour's name.")
			.Field("undoIndex", &GraphCreateResult::UndoIndex, "The creation's undo step; 0 in a dry run or a batch.");
		registry.Struct<GraphDiagnostic>("GraphDiagnostic", "One finding of a graph, mapped to its node.")
			.Field("severity", &GraphDiagnostic::Severity, "Error or Warning.")
			.Field("code", &GraphDiagnostic::Code, "GRAPH_INVALID, GRAPH_UNREACHABLE_NODE, SCRIPT_TYPE_ERROR or SCRIPT_COMPILE_ERROR.")
			.Field("message", &GraphDiagnostic::Message, "What is wrong.")
			.Field("node", &GraphDiagnostic::Node, "The node it concerns; 0 for the graph's name or members.")
			.Field("jsonPointer", &GraphDiagnostic::JsonPointer, "Where in the .graph document; empty for a finding of the transpiled text.")
			.Field("line", &GraphDiagnostic::Line, "The transpiled text's line of a script check's finding; 0 otherwise.")
			.Field("incomplete", &GraphDiagnostic::Incomplete, "A finding of an unfinished graph the edits accept (a required input not linked yet).");
		registry.Struct<GraphReadParams>("GraphReadParams", "The params of graph.read.")
			.Field("graph", &GraphReadParams::Graph, "The graph: its id, its \"Assets/...\" path or another asset reference.")
			.Field("luau", &GraphReadParams::Luau, "Also return the transpiled Luau text and its line map.");
		registry.Struct<GraphReadResult>("GraphReadResult", "A graph as its file holds it, with its findings.")
			.Field("graph", &GraphReadResult::Graph, "The graph.")
			.Field("document", &GraphReadResult::Document, "The graph document, in the .graph file's spelling.")
			.Field("diagnostics", &GraphReadResult::Diagnostics, "Its findings, each mapped to its node.")
			.Field("luau", &GraphReadResult::Luau, "With luau: the transpiled text; empty while the graph does not transpile.")
			.Field("lineNodes", &GraphReadResult::LineNodes, "With luau: the node of each line of the text (0 for none).");
		registry.Struct<GraphNodesParams>("GraphNodesParams", "The params of graph.nodes.")
			.Field("graph", &GraphNodesParams::Graph, "A graph whose Self.Get and Self.Set kinds to add; empty for none.")
			.Field("prefix", &GraphNodesParams::Prefix, "Only the kinds that start with it (\"Call.Input.\", \"Flow.\").")
			.Field("kinds", &GraphNodesParams::Kinds, "Only these kinds.");
		registry.Struct<GraphNodesResult>("GraphNodesResult", "Node kinds of the node library.")
			.Field("nodes", &GraphNodesResult::Nodes, "The kinds, sorted by kind.");
		registry.Struct<GraphAddNodeParams>("GraphAddNodeParams", "The params of graph.addNode.")
			.Field("graph", &GraphAddNodeParams::Graph, "The graph.")
			.Field("kind", &GraphAddNodeParams::Kind, "The node's kind (graph.nodes lists them).")
			.Field("position", &GraphAddNodeParams::Position, "The node's place on the canvas; absent: right of the rightmost node.")
			.Field("values", &GraphAddNodeParams::Values, "Literals of the node's inputs, by pin name.");
		registry.Struct<GraphAddNodeResult>("GraphAddNodeResult", "The node graph.addNode added.")
			.Field("graph", &GraphAddNodeResult::Graph, "The graph.")
			.Field("node", &GraphAddNodeResult::Node, "The new node's id.")
			.Field("undoIndex", &GraphAddNodeResult::UndoIndex, "The edit's undo step; 0 in a dry run or a batch.");
		registry.Struct<GraphRemoveNodesParams>("GraphRemoveNodesParams", "The params of graph.removeNodes.")
			.Field("graph", &GraphRemoveNodesParams::Graph, "The graph.")
			.Field("nodes", &GraphRemoveNodesParams::Nodes, "The ids of the nodes to remove.");
		registry.Struct<GraphRemoveNodesResult>("GraphRemoveNodesResult", "The outcome of a graph.removeNodes.")
			.Field("graph", &GraphRemoveNodesResult::Graph, "The graph.")
			.Field("removedLinks", &GraphRemoveNodesResult::RemovedLinks, "The links removed with the nodes.")
			.Field("undoIndex", &GraphRemoveNodesResult::UndoIndex, "The edit's undo step; 0 in a dry run or a batch.");
		registry.Struct<GraphNodePosition>("GraphNodePosition", "A node's new place on the canvas.")
			.Field("node", &GraphNodePosition::Node, "The node's id.")
			.Field("position", &GraphNodePosition::Position, "Its top-left corner in canvas units.");
		registry.Struct<GraphMoveNodesParams>("GraphMoveNodesParams", "The params of graph.moveNodes.")
			.Field("graph", &GraphMoveNodesParams::Graph, "The graph.")
			.Field("positions", &GraphMoveNodesParams::Positions, "The nodes' new places.");
		registry.Struct<GraphEditResult>("GraphEditResult", "The outcome of a graph edit.")
			.Field("graph", &GraphEditResult::Graph, "The graph.")
			.Field("undoIndex", &GraphEditResult::UndoIndex, "The edit's undo step; 0 in a dry run, in a batch or when nothing changed.");
		registry.Struct<GraphConnectParams>("GraphConnectParams", "The params of graph.connect.")
			.Field("graph", &GraphConnectParams::Graph, "The graph.")
			.Field("from", &GraphConnectParams::From, "An output pin: {Node, Pin}.")
			.Field("to", &GraphConnectParams::To, "An input pin: {Node, Pin}.");
		registry.Struct<GraphConnectResult>("GraphConnectResult", "The outcome of a graph.connect.")
			.Field("graph", &GraphConnectResult::Graph, "The graph.")
			.Field("replaced", &GraphConnectResult::Replaced, "The link the new one replaced, if any.")
			.Field("undoIndex", &GraphConnectResult::UndoIndex, "The edit's undo step; 0 in a dry run or a batch.");
		registry.Struct<GraphDisconnectParams>("GraphDisconnectParams", "The params of graph.disconnect.")
			.Field("graph", &GraphDisconnectParams::Graph, "The graph.")
			.Field("from", &GraphDisconnectParams::From, "The link's output pin.")
			.Field("to", &GraphDisconnectParams::To, "The link's input pin.");
		registry.Struct<GraphValueEdit>("GraphValueEdit", "One input literal to set.")
			.Field("node", &GraphValueEdit::Node, "The node's id.")
			.Field("pin", &GraphValueEdit::Pin, "The data input's name.")
			.Field("value", &GraphValueEdit::Value, "The literal in its pin type's JSON spelling; null removes it.");
		registry.Struct<GraphSetValuesParams>("GraphSetValuesParams", "The params of graph.setValues.")
			.Field("graph", &GraphSetValuesParams::Graph, "The graph.")
			.Field("values", &GraphSetValuesParams::Values, "The literals, in order.");
		registry.Struct<GraphMemberRename>("GraphMemberRename", "One field or variable to rename.")
			.Field("from", &GraphMemberRename::From, "Its name.")
			.Field("to", &GraphMemberRename::To, "Its new name.");
		registry.Struct<GraphSetMembersParams>("GraphSetMembersParams", "The params of graph.setMembers.")
			.Field("graph", &GraphSetMembersParams::Graph, "The graph.")
			.Field("name", &GraphSetMembersParams::Name, "The behaviour's new name; empty keeps it.")
			.Field("rename", &GraphSetMembersParams::Rename, "Fields and variables to rename, with their Self nodes.")
			.Field("remove", &GraphSetMembersParams::Remove, "Fields and variables to remove; refused while a node uses one.")
			.Field("fields", &GraphSetMembersParams::Fields, "Fields to add or replace, by name.")
			.Field("variables", &GraphSetMembersParams::Variables, "Variables to add or replace, by name.");
		registry.Struct<GraphSetMembersResult>("GraphSetMembersResult", "The outcome of a graph.setMembers.")
			.Field("graph", &GraphSetMembersResult::Graph, "The graph.")
			.Field("renamedNodes", &GraphSetMembersResult::RenamedNodes, "The Self.Get and Self.Set nodes the renames rewrote.")
			.Field("undoIndex", &GraphSetMembersResult::UndoIndex, "The edit's undo step; 0 in a dry run, in a batch or when nothing changed.");

		registry.Struct<GraphLiveValuesParams>("GraphLiveValuesParams", "The params of graph.liveValues.")
			.Field("entity", &GraphLiveValuesParams::Entity, "An entity of the play scene: its id, a unique id prefix or its path.")
			.Field("script", &GraphLiveValuesParams::Script, "The slot's behaviour name; empty: the entity's first slot holding a graph.");
		registry.Struct<GraphLiveValue>("GraphLiveValue", "A node output's last value.")
			.Field("node", &GraphLiveValue::Node, "The node's id.")
			.Field("pin", &GraphLiveValue::Pin, "The output pin.")
			.Field("type", &GraphLiveValue::Type, "The value's Luau or engine type name.")
			.Field("value", &GraphLiveValue::Value, "The value in reflection's JSON spelling; null for a table, a function or a proxy.");
		registry.Struct<GraphLiveMember>("GraphLiveMember", "A field's or variable's value.")
			.Field("name", &GraphLiveMember::Name, "The member's name.")
			.Field("type", &GraphLiveMember::Type, "The value's Luau or engine type name.")
			.Field("value", &GraphLiveMember::Value, "The value in reflection's JSON spelling.");
		registry.Struct<GraphLiveError>("GraphLiveError", "A retained script error of the instance.")
			.Field("node", &GraphLiveError::Node, "The node whose code raised it; 0 when its line maps to none.")
			.Field("line", &GraphLiveError::Line, "Its line of the transpiled text.")
			.Field("message", &GraphLiveError::Message, "The error's message.")
			.Field("count", &GraphLiveError::Count, "How many times it was raised.");
		registry.Struct<GraphLiveValuesResult>("GraphLiveValuesResult", "A graph behaviour's live values in play.")
			.Field("graph", &GraphLiveValuesResult::Graph, "The graph the slot holds.")
			.Field("entity", &GraphLiveValuesResult::Entity, "The entity.")
			.Field("slot", &GraphLiveValuesResult::Slot, "The slot's index in its Script list.")
			.Field("tick", &GraphLiveValuesResult::Tick, "The play session's tick.")
			.Field("values", &GraphLiveValuesResult::Values, "The nodes' last output values, by node and pin.")
			.Field("members", &GraphLiveValuesResult::Members, "The graph's fields and variables, by name.")
			.Field("errors", &GraphLiveValuesResult::Errors, "The instance's retained script errors, each mapped to its node.")
			.Field("truncated", &GraphLiveValuesResult::Truncated, "Whether a list was cut at its limit.");
	}

	void RegisterGraphMethods(MethodRegistry& methods)
	{
		const Json graph = "Assets/Scripts/Door.graph";
		methods.Add(
			{
				.Name = "graph.create",
				.Description = "Creates an empty visual-scripting graph, a behaviour transpiled to Luau at import, as one undoable edit.",
				.RequiredParams = { "path" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Create the Door behaviour.", .Params = Json{ { "path", graph }, { "name", "Door" } } } },
			},
			&Automation::GraphCreate);
		methods.Add(
			{
				.Name = "graph.read",
				.Description = "Reads a graph's document and its findings mapped to nodes, and optionally its transpiled Luau with each line's node.",
				.RequiredParams = { "graph" },
				.ExposeAsTool = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the Door graph and its Luau.", .Params = Json{ { "graph", graph }, { "luau", true } } } },
			},
			&Automation::GraphRead);
		methods.Add(
			{
				.Name = "graph.nodes",
				.Description = "Lists the node library's kinds with their pins and types, optionally filtered by a kind prefix, with a graph's own "
							   "Self.Get and Self.Set kinds.",
				.ExposeAsTool = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "List the flow nodes.", .Params = Json{ { "prefix", "Flow." } } } },
			},
			&Automation::GraphNodes);
		methods.Add(
			{
				.Name = "graph.addNode",
				.Description = "Adds a node of a kind to a graph, with literals for its inputs, as one undoable edit; returns its id.",
				.RequiredParams = { "graph", "kind" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Add a start event.", .Params = Json{ { "graph", graph }, { "kind", "Event.OnStart" } } } },
			},
			&Automation::GraphAddNode);
		methods.Add(
			{
				.Name = "graph.removeNodes",
				.Description = "Removes nodes and their links from a graph as one undoable edit.",
				.RequiredParams = { "graph", "nodes" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Remove node 3.", .Params = Json{ { "graph", graph }, { "nodes", Json::array({ 3 }) } } } },
			},
			&Automation::GraphRemoveNodes);
		methods.Add(
			{
				.Name = "graph.moveNodes",
				.Description = "Moves nodes on a graph's canvas (layout only) as one undoable edit.",
				.RequiredParams = { "graph", "positions" },
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Move node 3.",
					.Params = Json{ { "graph", graph }, { "positions", Json::array({ { { "node", 3 }, { "position", Json::array({ 240, 0 }) } } }) } } } },
			},
			&Automation::GraphMoveNodes);
		const Json from = { { "Node", 1 }, { "Pin", "Then" } };
		const Json to = { { "Node", 2 }, { "Pin", "In" } };
		methods.Add(
			{
				.Name = "graph.connect",
				.Description = "Links an output pin to a compatible input pin of a graph as one undoable edit; a data input or exec output already "
							   "linked loses its old link.",
				.RequiredParams = { "graph", "from", "to" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Continue the start event at node 2.", .Params = Json{ { "graph", graph }, { "from", from }, { "to", to } } } },
			},
			&Automation::GraphConnect);
		methods.Add(
			{
				.Name = "graph.disconnect",
				.Description = "Removes a link of a graph as one undoable edit.",
				.RequiredParams = { "graph", "from", "to" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Remove the start event's link.", .Params = Json{ { "graph", graph }, { "from", from }, { "to", to } } } },
			},
			&Automation::GraphDisconnect);
		methods.Add(
			{
				.Name = "graph.setValues",
				.Description = "Sets or removes literals of graph nodes' inputs as one undoable edit.",
				.RequiredParams = { "graph", "values" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Log a message.",
					.Params = Json{ { "graph", graph }, { "values", Json::array({ { { "node", 2 }, { "pin", "Values" }, { "value", "Opened" } } }) } } } },
			},
			&Automation::GraphSetValues);
		methods.Add(
			{
				.Name = "graph.setMembers",
				.Description = "Renames the behaviour and renames, removes, adds or replaces its fields and variables as one undoable edit.",
				.RequiredParams = { "graph" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Add a Speed field.",
					.Params = Json{ { "graph", graph },
						{ "fields", Json::array({ { { "Name", "Speed" }, { "Kind", "Number" }, { "Default", 2 }, { "Element", "Number" }, { "AssetType", "" }, { "EnumValues", Json::array() }, { "Options", Json{ { "Min", 0 }, { "Max", 10 } } } } }) } } } },
			},
			&Automation::GraphSetMembers);
	}

	void RegisterGraphLiveValueMethods(MethodRegistry& methods)
	{
		methods.Add(
			{
				.Name = "graph.liveValues",
				.Description = "Reads, during play and without changing the session, a graph behaviour's last node values, its fields and variables and "
							   "its script errors mapped to nodes.",
				.RequiredParams = { "entity" },
				.ExposeAsTool = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the door's live values.", .Params = Json{ { "entity", "/Door" } } } },
			},
			&Automation::GraphLiveValues);
	}

}
