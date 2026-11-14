#include "TestsPCH.h"

#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"

#include "Engine/AssetPipeline/Graph/GraphTranspiler.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/SceneTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// The graph node library (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decision 6, stream G-B) and ValidateGraph's rules
// against it (decision 5).

namespace Engine {

	namespace {

		GraphPinDescription Pin(GraphPinType type, std::string typeName = {})
		{
			return GraphPinDescription{ .Name = "Pin", .Type = type, .TypeName = std::move(typeName) };
		}

		// A pin whose Luau type is T?: an optional input, or an output that may give nil.
		GraphPinDescription OptionalPin(GraphPinType type, std::string typeName = {})
		{
			GraphPinDescription pin = Pin(type, std::move(typeName));
			pin.Optional = true;
			return pin;
		}

		// A frozen registry with every binding, and the library made from it.
		struct LibrarySetup
		{
			Scope<TypeRegistry> Types = Test::CreateBuiltinRegistry();
			ScriptApiRegistry Api{};
			Scope<GraphNodeLibrary> Library{};

			LibrarySetup()
			{
				const Status registered = RegisterBindings(Api, *Types);
				REQUIRE_MESSAGE(registered.has_value(), registered.error().ToString());
				Result<Scope<GraphNodeLibrary>> library = GraphNodeLibrary::Create(Api);
				REQUIRE_MESSAGE(library.has_value(), library.error().ToString());
				Library = std::move(*library);
			}
		};

		const GraphPinDescription* FindPin(const GraphNodeDescription& node, std::string_view name)
		{
			const auto found = std::ranges::find(node.Pins, name, &GraphPinDescription::Name);
			return found != node.Pins.end() ? &*found : nullptr;
		}

		// The pointers of `issues`, with an "!" before the complete (blocking) ones.
		std::vector<std::string> Describe(const std::vector<GraphIssue>& issues)
		{
			std::vector<std::string> pointers;
			for (const GraphIssue& issue : issues)
				pointers.push_back((issue.Incomplete ? "" : "!") + issue.JsonPointer);
			return pointers;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("GraphNodeLibrary: pins link by type, Any links to every data pin, an optional output feeds only an optional input, an Enum feeds a String and exec links only exec")
		{
			CHECK(AreGraphPinsCompatible(Pin(GraphPinType::Exec), Pin(GraphPinType::Exec)));
			CHECK_FALSE(AreGraphPinsCompatible(Pin(GraphPinType::Exec), Pin(GraphPinType::Any)));
			CHECK_FALSE(AreGraphPinsCompatible(Pin(GraphPinType::Any), Pin(GraphPinType::Exec)));
			CHECK(AreGraphPinsCompatible(Pin(GraphPinType::Number), Pin(GraphPinType::Number)));
			CHECK_FALSE(AreGraphPinsCompatible(Pin(GraphPinType::Number), Pin(GraphPinType::Bool)));
			CHECK(AreGraphPinsCompatible(Pin(GraphPinType::Number), Pin(GraphPinType::Any)));
			CHECK(AreGraphPinsCompatible(Pin(GraphPinType::Any), Pin(GraphPinType::Entity)));
			CHECK(AreGraphPinsCompatible(Pin(GraphPinType::Enum, "AudioGroup"), Pin(GraphPinType::String)));
			CHECK_FALSE(AreGraphPinsCompatible(Pin(GraphPinType::String), Pin(GraphPinType::Enum, "AudioGroup")));
			CHECK(AreGraphPinsCompatible(Pin(GraphPinType::Enum, "AudioGroup"), Pin(GraphPinType::Enum, "AudioGroup")));
			CHECK_FALSE(AreGraphPinsCompatible(Pin(GraphPinType::Enum, "AudioGroup"), Pin(GraphPinType::Enum, "Key")));
			CHECK(AreGraphPinsCompatible(Pin(GraphPinType::Object, "Transform"), Pin(GraphPinType::Object, "Transform")));
			CHECK_FALSE(AreGraphPinsCompatible(Pin(GraphPinType::Object, "Transform"), Pin(GraphPinType::Object, "RigidBody")));

			// The Jingle graph's case (GraphImporterTests): Assets.Load's Result (AssetRef?) into Audio.PlayOneShot's clip (AssetRef)
			// would not type-check, so it is refused; it feeds an optional clip (Animator.Play's) and Flow.IfNotNil's Value (Any),
			// whose Result (Any, the value on its True flow) then feeds the required clip.
			const GraphPinDescription loaded = OptionalPin(GraphPinType::Asset);
			CHECK_FALSE(AreGraphPinsCompatible(loaded, Pin(GraphPinType::Asset)));
			CHECK(AreGraphPinsCompatible(loaded, OptionalPin(GraphPinType::Asset)));
			CHECK(AreGraphPinsCompatible(loaded, Pin(GraphPinType::Any)));
			CHECK(AreGraphPinsCompatible(Pin(GraphPinType::Any), Pin(GraphPinType::Asset)));
			CHECK(AreGraphPinsCompatible(Pin(GraphPinType::Asset), OptionalPin(GraphPinType::Asset)));
			// An Entity's component proxy (RigidBody?) and Scene.FindByName's Result (Entity?) likewise.
			CHECK_FALSE(AreGraphPinsCompatible(OptionalPin(GraphPinType::Object, "RigidBody"), Pin(GraphPinType::Object, "RigidBody")));
			CHECK(AreGraphPinsCompatible(OptionalPin(GraphPinType::Object, "RigidBody"), OptionalPin(GraphPinType::Object, "RigidBody")));
			CHECK_FALSE(AreGraphPinsCompatible(OptionalPin(GraphPinType::Entity), Pin(GraphPinType::Entity)));
			CHECK_FALSE(AreGraphPinsCompatible(OptionalPin(GraphPinType::Enum, "AudioGroup"), Pin(GraphPinType::String)));
			CHECK(GraphPinTypeToString(GraphPinType::Object) == "Object");
			CHECK(GraphPinTypeToString(static_cast<GraphPinType>(200)) == "Unknown");
		}

		TEST_CASE("GraphNodeLibrary: every eligible script API member has its node, and no EditorOnly, Test-only or generic one has")
		{
			const LibrarySetup setup;
			const GraphNodeLibrary& library = *setup.Library;
			CHECK(std::ranges::is_sorted(library.GetNodes(), std::less<>(), &GraphNodeDescription::Kind));

			// A pure module function: its parameter by name, its result as Result, no exec pins.
			const GraphNodeDescription* axis = library.Find("Call.Input.GetAxis");
			REQUIRE(axis != nullptr);
			CHECK(axis->Pure);
			CHECK(axis->Category == GraphNodeCategory::Call);
			REQUIRE(FindPin(*axis, "name") != nullptr);
			CHECK(FindPin(*axis, "name")->Type == GraphPinType::String);
			REQUIRE(FindPin(*axis, "Result") != nullptr);
			CHECK(FindPin(*axis, "Result")->Type == GraphPinType::Number);
			CHECK(FindPin(*axis, "In") == nullptr);

			// A statement with optional inputs, an asset and an enum: In and Then exec pins.
			const GraphNodeDescription* oneShot = library.Find("Call.Audio.PlayOneShot");
			REQUIRE(oneShot != nullptr);
			CHECK_FALSE(oneShot->Pure);
			REQUIRE(FindPin(*oneShot, "In") != nullptr);
			REQUIRE(FindPin(*oneShot, "Then") != nullptr);
			CHECK(FindPin(*oneShot, "clip")->Type == GraphPinType::Asset);
			CHECK_FALSE(FindPin(*oneShot, "clip")->Optional);
			CHECK(FindPin(*oneShot, "position")->Optional);
			CHECK(FindPin(*oneShot, "group")->Type == GraphPinType::Enum);
			CHECK(FindPin(*oneShot, "group")->TypeName == "AudioGroup");

			// A variadic function returning nothing: a statement with one optional Values input.
			const GraphNodeDescription* info = library.Find("Call.Log.Info");
			REQUIRE(info != nullptr);
			CHECK_FALSE(info->Pure);
			REQUIRE(FindPin(*info, "Values") != nullptr);
			CHECK(FindPin(*info, "Values")->Optional);

			// A method takes its object as "self"; an event gives its callback's parameters.
			const GraphNodeDescription* lookAt = library.Find("Call.Transform.LookAt");
			REQUIRE(lookAt != nullptr);
			CHECK(FindPin(*lookAt, "self")->Type == GraphPinType::Object);
			CHECK(FindPin(*lookAt, "self")->TypeName == "Transform");
			// A T? result is an optional output (decision 6), and an Entity's component proxy too.
			const GraphNodeDescription* load = library.Find("Call.Assets.Load");
			REQUIRE(load != nullptr);
			CHECK(load->Pure);
			CHECK(FindPin(*load, "Result")->Type == GraphPinType::Asset);
			CHECK(FindPin(*load, "Result")->Optional);
			const GraphNodeDescription* body = library.Find("Get.Entity.RigidBody");
			REQUIRE(body != nullptr);
			CHECK(FindPin(*body, "Value")->TypeName == "RigidBody");
			CHECK(FindPin(*body, "Value")->Optional);
			// A union-typed callback parameter is Any.
			const GraphNodeDescription* changed = library.Find("Event.OnUiValueChanged");
			REQUIRE(changed != nullptr);
			CHECK(FindPin(*changed, "value")->Type == GraphPinType::Any);

			const GraphNodeDescription* collision = library.Find("Event.OnCollisionEnter");
			REQUIRE(collision != nullptr);
			CHECK(collision->Category == GraphNodeCategory::Event);
			CHECK(FindPin(*collision, "other")->Type == GraphPinType::Entity);
			CHECK(FindPin(*collision, "contact")->TypeName == "CollisionContact");
			REQUIRE(library.Find("Get.CollisionContact.Point") != nullptr);
			REQUIRE(library.Find("Get.Transform.Rotation") != nullptr);
			REQUIRE(library.Find("Set.Transform.Rotation") != nullptr);
			CHECK(library.Find("Set.Transform.RenderPosition") == nullptr); // read-only

			// Not modelled or not available everywhere: EditorOnly, Test-only, generic and function-typed members.
			CHECK(library.Find("Event.OnHotReload") == nullptr);
			CHECK(library.Find("Call.Test.Expect") == nullptr);
			CHECK(library.Find("Call.Script.Define") == nullptr);
			CHECK(library.Find("Call.Task.Spawn") == nullptr);

			// The built-ins of decision 6.
			const GraphNodeDescription* branch = library.Find("Flow.Branch");
			REQUIRE(branch != nullptr);
			std::vector<std::string> pins;
			for (const GraphPinDescription& pin : branch->Pins)
				pins.push_back(pin.Name);
			CHECK(pins == std::vector<std::string>{ "In", "Condition", "True", "False" });
			const GraphNodeDescription* ifNotNil = library.Find("Flow.IfNotNil");
			REQUIRE(ifNotNil != nullptr);
			pins.clear();
			for (const GraphPinDescription& pin : ifNotNil->Pins)
				pins.push_back(pin.Name);
			CHECK(pins == std::vector<std::string>{ "In", "Value", "True", "False", "Result" });
			CHECK(FindPin(*ifNotNil, "Value")->Type == GraphPinType::Any);
			CHECK_FALSE(FindPin(*ifNotNil, "Value")->Optional);
			CHECK(FindPin(*ifNotNil, "Result")->Type == GraphPinType::Any);
			CHECK_FALSE(FindPin(*ifNotNil, "Result")->Optional);
			for (const std::string_view kind : { "Flow.Sequence", "Flow.ForRange", "Flow.ForEach", "Flow.Delay", "Self.Entity", "Number.Add", "Compare.Equal",
					 "Logic.Not", "Vector.Make", "Vector.Normalize", "Text.Concat" })
				CHECK_MESSAGE(library.Find(kind) != nullptr, kind);
			CHECK(library.GetFingerprint() != 0);
		}

		TEST_CASE("GraphNodeLibrary: a graph's fields and variables resolve as Self.Get and Self.Set nodes of their kinds")
		{
			const LibrarySetup setup;
			GraphDocument graph;
			graph.Name = "Door";
			graph.Fields = { GraphField{ .Name = "Speed", .Kind = GraphValueKind::Number, .Default = VariantValue(Json(2)) } };
			graph.Variables = { GraphVariable{ .Name = "Opened", .Kind = GraphValueKind::Bool, .Default = VariantValue(Json(false)) } };
			const Result<GraphNodeDescription> speed = setup.Library->Resolve("Self.Get.Speed", graph);
			REQUIRE_MESSAGE(speed.has_value(), speed.error().ToString());
			CHECK(speed->Pure);
			CHECK(speed->Category == GraphNodeCategory::Member);
			CHECK(FindPin(*speed, "Value")->Type == GraphPinType::Number);
			const Result<GraphNodeDescription> opened = setup.Library->Resolve("Self.Set.Opened", graph);
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
			CHECK_FALSE(opened->Pure);
			CHECK(FindPin(*opened, "Value")->Type == GraphPinType::Bool);
			CHECK(setup.Library->Resolve("Self.Get.Missing", graph).error().GetCode() == ErrorCode::NotFound);
			CHECK(setup.Library->GetMemberNodes(graph).size() == 4);
			CHECK(setup.Library->Find("Self.Get.Speed") == nullptr);
		}

		TEST_CASE("GraphNodeLibrary: validation refuses unknown kinds and pins, incompatible links, a second link into a data input and cycles, and reports unfinished inputs as incomplete")
		{
			const LibrarySetup setup;
			GraphDocument graph;
			graph.Name = "Probe";
			graph.Nodes = {
				GraphNode{ .Id = 1, .Kind = "Event.OnStart" },
				GraphNode{ .Id = 2, .Kind = "Flow.Branch" },
				GraphNode{ .Id = 3, .Kind = "Number.Add", .Values = { { "A", VariantValue(Json(1)) }, { "B", VariantValue(Json(2)) } } },
				GraphNode{ .Id = 4, .Kind = "Logic.Not" },
				GraphNode{ .Id = 5, .Kind = "Call.Audio.PlayOneShot" },
			};
			graph.Links = {
				GraphLink{ .From = { 1, "Then" }, .To = { 2, "In" } },
				GraphLink{ .From = { 4, "Result" }, .To = { 2, "Condition" } },
			};
			// Unfinished: the Not's input has its default (false), so only the one-shot's clip is missing, and it is not run.
			CHECK(Describe(ValidateGraph(graph, *setup.Library)) == std::vector<std::string>{ "/Nodes/4/Values/clip" });

			GraphDocument broken = graph;
			broken.Nodes[3].Kind = "Logic.Nope";                                                    // an unknown kind
			broken.Nodes[2].Values["C"] = VariantValue(Json(3));                                    // an unknown pin
			broken.Links.push_back(GraphLink{ .From = { 3, "Result" }, .To = { 2, "Condition" } }); // a Number into a Bool
			CanonicalizeGraph(broken);
			// Canonical, the links are 1.Then to 2.In, 3.Result to 2.Condition (refused) and 4.Result to 2.Condition, whose
			// unknown node 4 is the issue that stands for it; the one-shot's clip is still unfinished.
			const std::vector<GraphIssue> issues = ValidateGraph(broken, *setup.Library);
			CHECK(Describe(issues) == std::vector<std::string>{ "!/Nodes/2/Values/C", "!/Nodes/3/Kind", "/Nodes/4/Values/clip", "!/Links/1" });
			CHECK(std::ranges::any_of(issues, [](const GraphIssue& issue)
			{
				return issue.JsonPointer == "/Nodes/3/Kind" && issue.Node == 4;
			}));
			CHECK(std::ranges::any_of(issues, [](const GraphIssue& issue)
			{
				return issue.JsonPointer == "/Nodes/2/Values/C" && issue.Node == 3;
			}));

			// A data input takes one link, however compatible a second one is: two Number values into one Add's A (canonical:
			// /Links/2 and /Links/3). The transpiler would read only one of them.
			GraphDocument twoInputs;
			twoInputs.Name = "Probe";
			twoInputs.Nodes = {
				GraphNode{ .Id = 1, .Kind = "Event.OnStart" },
				GraphNode{ .Id = 2, .Kind = "Call.Log.Info" },
				GraphNode{ .Id = 3, .Kind = "Number.Add" },
				GraphNode{ .Id = 4, .Kind = "Number.Negate" },
				GraphNode{ .Id = 5, .Kind = "Number.Negate" },
			};
			twoInputs.Links = {
				GraphLink{ .From = { 1, "Then" }, .To = { 2, "In" } },
				GraphLink{ .From = { 3, "Result" }, .To = { 2, "Values" } },
				GraphLink{ .From = { 4, "Result" }, .To = { 3, "A" } },
				GraphLink{ .From = { 5, "Result" }, .To = { 3, "A" } },
			};
			REQUIRE(ValidateGraphStructure(twoInputs).has_value());
			const std::vector<GraphIssue> secondInput = ValidateGraph(twoInputs, *setup.Library);
			CHECK(Describe(secondInput) == std::vector<std::string>{ "!/Links/3" });
			REQUIRE_FALSE(secondInput.empty());
			CHECK(secondInput.front().Node == 3);
			twoInputs.Links.pop_back();
			CHECK(ValidateGraph(twoInputs, *setup.Library).empty());

			// An exec output takes one link: the start's Then into two logs (/Links/1 is the second), which would run one flow
			// and drop the other.
			GraphDocument twoFlows;
			twoFlows.Name = "Probe";
			twoFlows.Nodes = {
				GraphNode{ .Id = 1, .Kind = "Event.OnStart" },
				GraphNode{ .Id = 2, .Kind = "Call.Log.Info" },
				GraphNode{ .Id = 3, .Kind = "Call.Log.Info" },
			};
			twoFlows.Links = {
				GraphLink{ .From = { 1, "Then" }, .To = { 2, "In" } },
				GraphLink{ .From = { 1, "Then" }, .To = { 3, "In" } },
			};
			REQUIRE(ValidateGraphStructure(twoFlows).has_value());
			const std::vector<GraphIssue> secondFlow = ValidateGraph(twoFlows, *setup.Library);
			CHECK(Describe(secondFlow) == std::vector<std::string>{ "!/Links/1" });
			REQUIRE_FALSE(secondFlow.empty());
			CHECK(secondFlow.front().Node == 1);

			// An exec cycle of two statements: the branch runs the one-shot, which runs the branch again. Canonical, its links are
			// /Links/1 (2.True to 5.In) and /Links/3 (5.Then to 2.In); a node never links to itself (ValidateGraphStructure).
			GraphDocument cycle = graph;
			cycle.Links.push_back(GraphLink{ .From = { 2, "True" }, .To = { 5, "In" } });
			cycle.Links.push_back(GraphLink{ .From = { 5, "Then" }, .To = { 2, "In" } });
			CanonicalizeGraph(cycle);
			REQUIRE(ValidateGraphStructure(cycle).has_value());
			CHECK(std::ranges::any_of(ValidateGraph(cycle, *setup.Library), [](const GraphIssue& issue)
			{
				return !issue.Incomplete && (issue.JsonPointer == "/Links/1" || issue.JsonPointer == "/Links/3");
			}));
			CHECK(FindUnreachableGraphNodes(graph, *setup.Library) == std::vector<uint32_t>{ 5 });
		}

		TEST_CASE("GraphNodeLibrary: a statement's output read where it may not have run is incomplete, and an enum literal must be one of its choices")
		{
			const LibrarySetup setup;
			CHECK(std::ranges::equal(setup.Library->GetEnumChoices("AudioGroup"), std::vector<std::string>{ "Music", "Sfx", "Ui" }));
			CHECK(setup.Library->GetEnumChoices("NoSuchEnum").empty());

			// The entity the branch's True flow creates is logged on its False flow, where it was never created; and a group
			// volume names a group the enum lacks.
			GraphDocument graph;
			graph.Name = "Probe";
			graph.Nodes = {
				GraphNode{ .Id = 1, .Kind = "Event.OnStart" },
				GraphNode{ .Id = 2, .Kind = "Flow.Branch" },
				GraphNode{ .Id = 3, .Kind = "Call.Scene.CreateEntity" },
				GraphNode{ .Id = 4, .Kind = "Call.Log.Info" },
				GraphNode{ .Id = 5, .Kind = "Call.Audio.SetGroupVolume", .Values = { { "group", VariantValue(Json("Musik")) }, { "volume", VariantValue(Json(1)) } } },
			};
			graph.Links = {
				GraphLink{ .From = { 1, "Then" }, .To = { 2, "In" } },
				GraphLink{ .From = { 2, "False" }, .To = { 4, "In" } },
				GraphLink{ .From = { 2, "True" }, .To = { 3, "In" } },
				GraphLink{ .From = { 3, "Result" }, .To = { 4, "Values" } },
			};
			CHECK(Describe(ValidateGraph(graph, *setup.Library)) == std::vector<std::string>{ "!/Nodes/4/Values/group", "/Links/3" });

			// Logged after it was created, in the flow that created it, the entity is read where it exists; a choice of the enum fits.
			graph.Links[1] = GraphLink{ .From = { 3, "Then" }, .To = { 4, "In" } };
			graph.Nodes[4].Values["group"] = VariantValue(Json("Music"));
			CHECK(ValidateGraph(graph, *setup.Library).empty());
			CHECK(FindUnreachableGraphNodes(graph, *setup.Library) == std::vector<uint32_t>{ 5 });
		}

		// Decision 6's optional values (W24.1's review): Flow.IfNotNil's Result is its Value, which is not nil only on its True flow,
		// so only nodes that its True flow alone enters may read it: not its False flow, nor a node after a merge that the False
		// flow enters too, where the Result is nil and a required input would get it. Such a read is unfinished (Incomplete), as
		// a statement's output read where it may not have run, so the edits accept it and the import refuses it.
		TEST_CASE("GraphNodeLibrary: Flow.IfNotNil's Result is read only where its True flow alone leads, and elsewhere is incomplete")
		{
			const LibrarySetup setup;
			// The door's name is logged on the True flow, after the node the True flow enters (node 5 reads it through node 6).
			GraphDocument graph;
			graph.Name = "Probe";
			graph.Nodes = {
				GraphNode{ .Id = 1, .Kind = "Event.OnStart" },
				GraphNode{ .Id = 2, .Kind = "Call.Scene.FindByName", .Values = { { "name", VariantValue(Json("Door")) } } },
				GraphNode{ .Id = 3, .Kind = "Flow.IfNotNil" },
				GraphNode{ .Id = 4, .Kind = "Call.Log.Info" },
				GraphNode{ .Id = 5, .Kind = "Call.Log.Info" },
				GraphNode{ .Id = 6, .Kind = "Get.Entity.Name" },
			};
			graph.Links = {
				GraphLink{ .From = { 1, "Then" }, .To = { 3, "In" } },
				GraphLink{ .From = { 2, "Result" }, .To = { 3, "Value" } },
				GraphLink{ .From = { 3, "Result" }, .To = { 6, "self" } },
				GraphLink{ .From = { 3, "True" }, .To = { 4, "In" } },
				GraphLink{ .From = { 4, "Then" }, .To = { 5, "In" } },
				GraphLink{ .From = { 6, "Value" }, .To = { 5, "Values" } },
			};
			REQUIRE(ValidateGraphStructure(graph).has_value());
			CHECK(ValidateGraph(graph, *setup.Library).empty());
			CHECK(TranspileGraph(graph, *setup.Library, "Assets/Scripts/Probe.graph").has_value());

			// The same read on the False flow (node 7), where the Result is nil: the Result's link (/Links/3) is incomplete.
			GraphDocument falseFlow = graph;
			falseFlow.Nodes.push_back(GraphNode{ .Id = 7, .Kind = "Call.Log.Info" });
			falseFlow.Links.push_back(GraphLink{ .From = { 3, "False" }, .To = { 7, "In" } });
			falseFlow.Links.push_back(GraphLink{ .From = { 6, "Value" }, .To = { 7, "Values" } });
			CanonicalizeGraph(falseFlow);
			REQUIRE(ValidateGraphStructure(falseFlow).has_value());
			const std::vector<GraphIssue> onFalse = ValidateGraph(falseFlow, *setup.Library);
			CHECK(Describe(onFalse) == std::vector<std::string>{ "/Links/3" });
			REQUIRE_FALSE(onFalse.empty());
			CHECK(onFalse.front().Node == 6);
			CHECK_FALSE(TranspileGraph(falseFlow, *setup.Library, "Assets/Scripts/Probe.graph").has_value());

			// After a merge that the False flow enters too (node 7 continues at node 5): incomplete.
			GraphDocument merged = graph;
			merged.Nodes.push_back(GraphNode{ .Id = 7, .Kind = "Call.Log.Info" });
			merged.Links.push_back(GraphLink{ .From = { 3, "False" }, .To = { 7, "In" } });
			merged.Links.push_back(GraphLink{ .From = { 7, "Then" }, .To = { 5, "In" } });
			CanonicalizeGraph(merged);
			REQUIRE(ValidateGraphStructure(merged).has_value());
			CHECK(Describe(ValidateGraph(merged, *setup.Library)) == std::vector<std::string>{ "/Links/3" });

			// The node the True flow enters is entered by the False flow too: incomplete.
			GraphDocument bothFlows = graph;
			bothFlows.Links.push_back(GraphLink{ .From = { 3, "False" }, .To = { 4, "In" } });
			CanonicalizeGraph(bothFlows);
			REQUIRE(ValidateGraphStructure(bothFlows).has_value());
			CHECK(Describe(ValidateGraph(bothFlows, *setup.Library)) == std::vector<std::string>{ "/Links/3" });
		}

		// M24's review: a loop sets its Index and Element only in its Body flow, so a loop that runs zero times leaves them nil
		// on its Completed flow and after it; read there into an input that needs a value they are incomplete, as
		// Flow.IfNotNil's Result off its True flow is, while an optional or Any input takes the nil.
		TEST_CASE("GraphNodeLibrary: a loop's Index or Element is read into an input that needs a value only where its Body flow alone leads, and elsewhere is incomplete")
		{
			const LibrarySetup setup;
			// OnStart counts 1 to 3 into Total in the loop's Body (node 5 reads Index through node 4).
			GraphDocument graph;
			graph.Name = "Probe";
			graph.Variables = { GraphVariable{ .Name = "Total", .Kind = GraphValueKind::Number, .Default = VariantValue(Json(0)) } };
			graph.Nodes = {
				GraphNode{ .Id = 1, .Kind = "Event.OnStart" },
				GraphNode{ .Id = 2, .Kind = "Flow.ForRange", .Values = { { "First", VariantValue(Json(1)) }, { "Last", VariantValue(Json(3)) } } },
				GraphNode{ .Id = 3, .Kind = "Self.Get.Total" },
				GraphNode{ .Id = 4, .Kind = "Number.Add" },
				GraphNode{ .Id = 5, .Kind = "Self.Set.Total" },
			};
			graph.Links = {
				GraphLink{ .From = { 1, "Then" }, .To = { 2, "In" } },
				GraphLink{ .From = { 2, "Body" }, .To = { 5, "In" } },
				GraphLink{ .From = { 2, "Index" }, .To = { 4, "B" } },
				GraphLink{ .From = { 3, "Value" }, .To = { 4, "A" } },
				GraphLink{ .From = { 4, "Result" }, .To = { 5, "Value" } },
			};
			REQUIRE(ValidateGraphStructure(graph).has_value());
			CHECK(ValidateGraph(graph, *setup.Library).empty());
			CHECK(TranspileGraph(graph, *setup.Library, "Assets/Scripts/Probe.graph").has_value());

			// The same sum on the Completed flow (node 6 reads Index through node 7): Index's link into node 7 (/Links/4) is
			// incomplete, and the graph has no text.
			GraphDocument completed = graph;
			completed.Nodes.push_back(GraphNode{ .Id = 6, .Kind = "Self.Set.Total" });
			completed.Nodes.push_back(GraphNode{ .Id = 7, .Kind = "Number.Add" });
			completed.Links.push_back(GraphLink{ .From = { 2, "Completed" }, .To = { 6, "In" } });
			completed.Links.push_back(GraphLink{ .From = { 2, "Index" }, .To = { 7, "B" } });
			completed.Links.push_back(GraphLink{ .From = { 3, "Value" }, .To = { 7, "A" } });
			completed.Links.push_back(GraphLink{ .From = { 7, "Result" }, .To = { 6, "Value" } });
			CanonicalizeGraph(completed);
			REQUIRE(ValidateGraphStructure(completed).has_value());
			const std::vector<GraphIssue> onCompleted = ValidateGraph(completed, *setup.Library);
			CHECK(Describe(onCompleted) == std::vector<std::string>{ "/Links/4" });
			REQUIRE_FALSE(onCompleted.empty());
			CHECK(onCompleted.front().Node == 7);
			CHECK_FALSE(TranspileGraph(completed, *setup.Library, "Assets/Scripts/Probe.graph").has_value());

			// On the Completed flow into an input that takes nil (Log.Info's optional Any Values): valid.
			GraphDocument logged = graph;
			logged.Nodes.push_back(GraphNode{ .Id = 6, .Kind = "Call.Log.Info" });
			logged.Links.push_back(GraphLink{ .From = { 2, "Completed" }, .To = { 6, "In" } });
			logged.Links.push_back(GraphLink{ .From = { 2, "Index" }, .To = { 6, "Values" } });
			CanonicalizeGraph(logged);
			REQUIRE(ValidateGraphStructure(logged).has_value());
			CHECK(ValidateGraph(logged, *setup.Library).empty());

			// The node the Body flow enters is entered by the Completed flow too (a merge): incomplete at Index's link (/Links/3).
			GraphDocument merged = graph;
			merged.Links.push_back(GraphLink{ .From = { 2, "Completed" }, .To = { 5, "In" } });
			CanonicalizeGraph(merged);
			REQUIRE(ValidateGraphStructure(merged).has_value());
			CHECK(Describe(ValidateGraph(merged, *setup.Library)) == std::vector<std::string>{ "/Links/3" });

			// Flow.ForEach's Element (Any) read on its Completed flow into Total's Number value: incomplete (/Links/2); in its Body,
			// valid.
			GraphDocument each;
			each.Name = "Probe";
			each.Variables = {
				GraphVariable{ .Name = "Items", .Kind = GraphValueKind::Array, .Default = VariantValue(Json::array()), .Element = GraphValueKind::Number },
				GraphVariable{ .Name = "Total", .Kind = GraphValueKind::Number, .Default = VariantValue(Json(0)) },
			};
			each.Nodes = {
				GraphNode{ .Id = 1, .Kind = "Event.OnStart" },
				GraphNode{ .Id = 2, .Kind = "Flow.ForEach" },
				GraphNode{ .Id = 3, .Kind = "Self.Get.Items" },
				GraphNode{ .Id = 4, .Kind = "Self.Set.Total" },
			};
			each.Links = {
				GraphLink{ .From = { 1, "Then" }, .To = { 2, "In" } },
				GraphLink{ .From = { 2, "Completed" }, .To = { 4, "In" } },
				GraphLink{ .From = { 2, "Element" }, .To = { 4, "Value" } },
				GraphLink{ .From = { 3, "Value" }, .To = { 2, "Array" } },
			};
			REQUIRE(ValidateGraphStructure(each).has_value());
			CHECK(Describe(ValidateGraph(each, *setup.Library)) == std::vector<std::string>{ "/Links/2" });
			each.Links[1] = GraphLink{ .From = { 2, "Body" }, .To = { 4, "In" } };
			REQUIRE(ValidateGraphStructure(each).has_value());
			CHECK(ValidateGraph(each, *setup.Library).empty());
		}

		// Quat.New refuses the zero quaternion at run time, so a Quat input's literal is a nonzero quaternion once rounded to
		// float, as a member's Default is (W24.1's review).
		TEST_CASE("GraphNodeLibrary: a Quat input's literal is a nonzero quaternion, which Quat.New takes")
		{
			const LibrarySetup setup;
			GraphDocument graph;
			graph.Name = "Probe";
			graph.Variables = { GraphVariable{ .Name = "Spin", .Kind = GraphValueKind::Quat, .Default = VariantValue(Json::array({ 0, 0, 0, 1 })) } };
			graph.Nodes = { GraphNode{ .Id = 1, .Kind = "Self.Set.Spin", .Values = { { "Value", VariantValue(Json::array({ 0, 0, 0, 0 })) } } } };
			REQUIRE(ValidateGraphStructure(graph).has_value());
			CHECK(Describe(ValidateGraph(graph, *setup.Library)) == std::vector<std::string>{ "!/Nodes/0/Values/Value" });
			// Any nonzero quaternion fits (Quat.New normalizes it).
			graph.Nodes[0].Values["Value"] = VariantValue(Json::array({ 0, 0, 0, 2 }));
			CHECK(ValidateGraph(graph, *setup.Library).empty());
			graph.Nodes[0].Values["Value"] = VariantValue(Json::array({ 0, 0.5, 0, 0 }));
			CHECK(ValidateGraph(graph, *setup.Library).empty());
		}

		// Docs/Decisions/0035-m24-contract.md risk 1 (W24.1's integration): the transpiled text nests a statement's code at most 64
		// blocks deep, and ValidateGraph refuses a flow that would nest deeper at the statement that first passes the limit, so a
		// graph transpiles exactly when it validates.
		TEST_CASE("GraphNodeLibrary: a flow nested deeper than the transpiled text may nest is invalid at the statement that passes the limit, and a graph transpiles exactly when it validates")
		{
			const LibrarySetup setup;
			// An OnStart event, then `branches` branches, each in the True flow of the one before it; with `pure`, every branch's
			// Condition is node 1000's value, so each branch also opens a block for it.
			const auto nest = [](uint32_t branches, bool pure)
			{
				GraphDocument graph;
				graph.Name = "Nested";
				graph.Nodes.push_back(GraphNode{ .Id = 1, .Kind = "Event.OnStart" });
				for (uint32_t id = 2; id <= branches + 1; ++id)
				{
					graph.Nodes.push_back(GraphNode{ .Id = id, .Kind = "Flow.Branch" });
					graph.Links.push_back(GraphLink{ .From = { id - 1, id == 2 ? "Then" : "True" }, .To = { id, "In" } });
					if (pure)
						graph.Links.push_back(GraphLink{ .From = { 1000, "Result" }, .To = { id, "Condition" } });
				}
				if (pure)
					graph.Nodes.push_back(GraphNode{ .Id = 1000, .Kind = "Logic.Not" });
				CanonicalizeGraph(graph);
				REQUIRE(ValidateGraphStructure(graph).has_value());
				return graph;
			};
			const auto transpiles = [&setup](const GraphDocument& graph)
			{
				return TranspileGraph(graph, *setup.Library, "Assets/Scripts/Nested.graph").has_value();
			};

			// The 64th branch starts 64 blocks deep: valid, and it transpiles.
			const GraphDocument deepest = nest(64, false);
			CHECK(ValidateGraph(deepest, *setup.Library).empty());
			CHECK(transpiles(deepest));

			// The 65th would start 65 blocks deep: one blocking issue at that branch (node 66, the 66th node), and no text.
			const GraphDocument tooDeep = nest(65, false);
			const std::vector<GraphIssue> issues = ValidateGraph(tooDeep, *setup.Library);
			CHECK(Describe(issues) == std::vector<std::string>{ "!/Nodes/65" });
			REQUIRE(issues.size() == 1);
			CHECK(issues.front().Node == 66);
			const Result<GraphTranspileResult> refused = TranspileGraph(tooDeep, *setup.Library, "Assets/Scripts/Nested.graph");
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);
			REQUIRE(refused.error().GetIssues().size() == 1);
			CHECK(refused.error().GetIssues().front().JsonPointer == "/Nodes/65");

			// A statement's block of pure values nests too: with one, the 32nd branch starts 63 blocks deep and the 33rd 65.
			CHECK(ValidateGraph(nest(32, true), *setup.Library).empty());
			CHECK(transpiles(nest(32, true)));
			const GraphDocument blocks = nest(33, true);
			CHECK(Describe(ValidateGraph(blocks, *setup.Library)) == std::vector<std::string>{ "!/Nodes/33" });
			CHECK_FALSE(transpiles(blocks));

			// An unfinished graph is measured too, so the edit that nests a flow too deep is refused while it is built.
			GraphDocument unfinished = tooDeep;
			unfinished.Nodes.push_back(GraphNode{ .Id = 200, .Kind = "Call.Audio.PlayOneShot" });
			CHECK(Describe(ValidateGraph(unfinished, *setup.Library)) == std::vector<std::string>{ "!/Nodes/65", "/Nodes/66/Values/clip" });
			// A graph with another blocking issue (a value for an input its node lacks) has no layout to measure: that issue alone
			// is reported.
			GraphDocument broken = tooDeep;
			broken.Nodes[1].Values["C"] = VariantValue(Json(3));
			CHECK(Describe(ValidateGraph(broken, *setup.Library)) == std::vector<std::string>{ "!/Nodes/1/Values/C" });
		}

		// Docs/Decisions/0035-m24-contract.md decision 7 and risk 1 (W24.1's review): every statement evaluates again the pure
		// values it reads, so the text grows with statements times values, which the document's limits bound only at hundreds of
		// megabytes; the text holds at most 8 MiB (GraphTextLimits.h's MaxGraphTextBytes), and ValidateGraph refuses a graph whose
		// text would pass it at the statement whose code passes it, the layout stopping there, so a graph transpiles exactly when
		// it validates.
		TEST_CASE("GraphNodeLibrary: a graph whose transpiled text would pass its size limit is invalid at the statement whose code passes it, and a graph transpiles exactly when it validates")
		{
			constexpr size_t MaxTextBytes = 8 * 1024 * 1024;
			const LibrarySetup setup;
			const auto transpile = [&setup](const GraphDocument& graph)
			{
				return TranspileGraph(graph, *setup.Library, "Assets/Scripts/Large.graph");
			};

			// An OnStart event, then `statements` logs in a chain, each of a string whose length makes every log's code the same
			// size whatever the digits of its id.
			constexpr size_t LogBytes = 2100;
			const auto chain = [](size_t statements)
			{
				GraphDocument graph;
				graph.Name = "Large";
				graph.Nodes.push_back(GraphNode{ .Id = 1, .Kind = "Event.OnStart" });
				for (uint32_t id = 2; id <= statements + 1; ++id)
				{
					const size_t digits = std::to_string(id).size();
					graph.Nodes.push_back(GraphNode{ .Id = id, .Kind = "Call.Log.Info", .Values = { { "Values", VariantValue(Json(std::string(LogBytes - digits, 'x'))) } } });
					graph.Links.push_back(GraphLink{ .From = { id - 1, "Then" }, .To = { id, "In" } });
				}
				REQUIRE(ValidateGraphStructure(graph).has_value());
				return graph;
			};
			const Result<GraphTranspileResult> one = transpile(chain(1));
			const Result<GraphTranspileResult> two = transpile(chain(2));
			REQUIRE(one.has_value());
			REQUIRE(two.has_value());
			const size_t perStatement = two->Luau.size() - one->Luau.size();
			const size_t layout = one->Luau.size() - perStatement;
			const size_t most = (MaxTextBytes - layout) / perStatement;
			REQUIRE(most + 1 < MaxGraphNodes);

			// The most statements whose text fits: valid, and the text is that size.
			const GraphDocument fits = chain(most);
			CHECK(ValidateGraph(fits, *setup.Library).empty());
			const Result<GraphTranspileResult> largest = transpile(fits);
			REQUIRE(largest.has_value());
			CHECK(largest->Luau.size() == layout + most * perStatement);
			CHECK(largest->Luau.size() <= MaxTextBytes);

			// One more: one blocking issue at that last statement (node index most + 1), and no text.
			const GraphDocument tooLarge = chain(most + 1);
			const std::vector<GraphIssue> issues = ValidateGraph(tooLarge, *setup.Library);
			const std::string pointer = std::format("/Nodes/{}", most + 1);
			CHECK(Describe(issues) == std::vector<std::string>{ "!" + pointer });
			REQUIRE(issues.size() == 1);
			CHECK(issues.front().Node == tooLarge.Nodes.back().Id);
			const Result<GraphTranspileResult> refused = transpile(tooLarge);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);
			REQUIRE(refused.error().GetIssues().size() == 1);
			CHECK(refused.error().GetIssues().front().JsonPointer == pointer);

			// Within the document's limits, a chain of 300 values read by each of 3,000 statements would lay out about 100 MB;
			// the layout stops at the limit, at the statement whose code passes it.
			GraphDocument repeated;
			repeated.Name = "Repeated";
			repeated.Fields = { GraphField{ .Name = "Base", .Kind = GraphValueKind::Number, .Default = VariantValue(Json(0)) } };
			repeated.Nodes.push_back(GraphNode{ .Id = 1, .Kind = "Event.OnStart" });
			repeated.Nodes.push_back(GraphNode{ .Id = 2, .Kind = "Self.Get.Base" });
			constexpr uint32_t Values = 300;
			for (uint32_t id = 3; id < 3 + Values; ++id)
			{
				repeated.Nodes.push_back(GraphNode{ .Id = id, .Kind = "Number.Add", .Values = { { "B", VariantValue(Json(1)) } } });
				repeated.Links.push_back(GraphLink{ .From = { id - 1, id == 3 ? "Value" : "Result" }, .To = { id, "A" } });
			}
			constexpr uint32_t FirstLog = 1000;
			constexpr uint32_t Logs = 3000;
			for (uint32_t id = FirstLog; id < FirstLog + Logs; ++id)
			{
				repeated.Nodes.push_back(GraphNode{ .Id = id, .Kind = "Call.Log.Info" });
				repeated.Links.push_back(GraphLink{ .From = { id == FirstLog ? 1 : id - 1, "Then" }, .To = { id, "In" } });
				repeated.Links.push_back(GraphLink{ .From = { 2 + Values, "Result" }, .To = { id, "Values" } });
			}
			CanonicalizeGraph(repeated);
			REQUIRE(ValidateGraphStructure(repeated).has_value());
			const std::vector<GraphIssue> repeats = ValidateGraph(repeated, *setup.Library);
			REQUIRE(repeats.size() == 1);
			CHECK_FALSE(repeats.front().Incomplete);
			CHECK(repeats.front().Node >= FirstLog);
			CHECK(repeats.front().Node < FirstLog + Logs);
			CHECK_FALSE(transpile(repeated).has_value());
		}
	}

}
