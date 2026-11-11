#include "TestsPCH.h"

#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/SceneTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// The graph node library (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decision 6, stream G-B). The pin-type cases are
// live (the contract implements them); the others are skipped until stream G-B implements the library and ValidateGraph.

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

		TEST_CASE("GraphNodeLibrary: every eligible script API member has its node, and no EditorOnly, Test-only or generic one has" * doctest::skip(true))
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

		TEST_CASE("GraphNodeLibrary: a graph's fields and variables resolve as Self.Get and Self.Set nodes of their kinds" * doctest::skip(true))
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

		TEST_CASE("GraphNodeLibrary: validation refuses unknown kinds and pins, incompatible links, a second link into a data input and cycles, and reports unfinished inputs as incomplete"
			* doctest::skip(true))
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
			broken.Links.push_back(GraphLink{ .From = { 3, "Result" }, .To = { 2, "Condition" } }); // a Number into a Bool, and a second link
			CanonicalizeGraph(broken);
			const std::vector<GraphIssue> issues = ValidateGraph(broken, *setup.Library);
			CHECK(std::ranges::count_if(issues, [](const GraphIssue& issue)
			{
				return !issue.Incomplete;
			}) >= 3);
			CHECK(std::ranges::any_of(issues, [](const GraphIssue& issue)
			{
				return issue.JsonPointer == "/Nodes/3/Kind" && issue.Node == 4;
			}));
			CHECK(std::ranges::any_of(issues, [](const GraphIssue& issue)
			{
				return issue.JsonPointer == "/Nodes/2/Values/C" && issue.Node == 3;
			}));

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
	}

}
