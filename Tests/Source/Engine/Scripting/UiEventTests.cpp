#include "TestsPCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiInteraction.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <format>
#include <string>
#include <string_view>
#include <vector>

// The UI events (M22, C22.2, R-UI-EVENTS; Docs/Decisions/0033-m22-contract.md decision 27): the UiEvents phase's delivery
// (ScriptEngine::DispatchUiEvents), bubbling per behaviour on M20's script slots (ADR 0031 decision 18). The Ui module's case
// is Bindings/UiTests.cpp's. Skipped by the contract until stream B lands the dispatch and the callbacks' registration.

namespace Engine {

	namespace {

		int RecordUiEvent(ScriptCall& call)
		{
			const std::string message = Lua::Check<std::string>(call, 1);
			static_cast<void>(Lua::GetEngine(call)->GetHost().GetScene().CreateEntity("Trace:" + message));
			return 0;
		}

		Test::ScriptTestFixtureSpecification UiEventSpecification()
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.TestMode = true;
			specification.ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				api.Module("Probe", "Records UI callbacks into the test scene.").Function("Record", &RecordUiEvent, "(message: string) -> ()", "Append one test observation.", { .Mutates = true });
				return {};
			};
			return specification;
		}

		std::vector<std::string> UiEventTrace(Scene& scene)
		{
			std::vector<std::string> result;
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const Entity entity = scene.FindEntityByID(id);
				if (entity && entity.GetName().starts_with("Trace:"))
					result.push_back(entity.GetName().substr(6));
			}
			return result;
		}

		// A behaviour named `name` that records every UI callback as "<entity>:<name>:<callback>:<element>[:<value>]" and
		// returns `stop` from each.
		std::string UiEventBehaviourSource(std::string_view name, bool stop)
		{
			return std::format(R"(local T = {{}}
local function record(self, callback, element, value)
	Probe.Record(self.Entity.Name .. ":{0}:" .. callback .. ":" .. element.Name .. (value ~= nil and (":" .. tostring(value)) or ""))
	return {1}
end
function T:OnUiClick(element) return record(self, "click", element) end
function T:OnUiValueChanged(element, value) return record(self, "value", element, value) end
function T:OnUiFocus(element, focused) return record(self, "focus", element, focused) end
function T:OnUiHover(element, hovered) return record(self, "hover", element, hovered) end
function T:OnUiCancel(element) return record(self, "cancel", element) end
function T:OnUiScroll(element, position) return record(self, "scroll", element, position.y) end
return Script.Define("{0}", T)
)",
				name, stop ? "true" : "nil");
		}

		void AddUiEventSlots(Test::ScriptTestFixture& fixture, Entity entity, std::string_view scripts)
		{
			const Result<Json> slots = JsonReader::Parse(std::string(R"({ "Scripts": )") + std::string(scripts) + "}");
			REQUIRE(slots.has_value());
			const Status added = ComponentAccess::AddComponent(entity, "Script", &*slots, fixture.GetSchemaSnapshot().get());
			REQUIRE_MESSAGE(added.has_value(), (added ? std::string() : added.error().ToString()));
		}

	}

	TEST_SUITE("Scripting")
	{
		// Decision 27 (ADR 0031 decision 18): the target first, then each ancestor; every behaviour of an entity that defines
		// the callback receives it in slot order; a true return stops the event after that entity, its other slots included.
		TEST_CASE("UiEvents: an event bubbles from its element per behaviour and stops after an entity where a callback returns true" * doctest::skip(true))
		{
			Test::ScriptTestFixture fixture(UiEventSpecification());
			const AssetHandle listen(0x220001);
			const AssetHandle stop(0x220002);
			REQUIRE(fixture.AddScript(listen, "Assets/Scripts/Listen.luau", UiEventBehaviourSource("Listen", false)));
			REQUIRE(fixture.AddScript(stop, "Assets/Scripts/Stop.luau", UiEventBehaviourSource("Stop", true)));
			Scene& scene = fixture.GetScene();
			const Entity root = scene.CreateEntityWithID(UUID(10), "Root");
			const Entity menu = scene.CreateEntityWithID(UUID(20), "Menu", root);
			const Entity group = scene.CreateEntityWithID(UUID(30), "Group", menu);
			const Entity button = scene.CreateEntityWithID(UUID(40), "Button", group);
			AddUiEventSlots(fixture, root, std::format(R"([ {{ "Script": "{}" }} ])", listen.ToString()));
			AddUiEventSlots(fixture, menu, std::format(R"([ {{ "Script": "{}" }}, {{ "Script": "{}" }} ])", stop.ToString(), listen.ToString()));
			AddUiEventSlots(fixture, button, std::format(R"([ {{ "Script": "{}" }} ])", listen.ToString()));
			REQUIRE(fixture.Start());
			REQUIRE(fixture.Errors.empty());

			// Group has no behaviour and is passed over; Menu's Stop returns true, so its Listen still hears it and Root does not.
			fixture.GetEngine()->DispatchUiEvents(std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Click, .Target = button.GetUUID() } });
			CHECK(UiEventTrace(scene) == std::vector<std::string>{ "Button:Listen:click:Button", "Menu:Stop:click:Button", "Menu:Listen:click:Button" });

			// An event at Root reaches only Root.
			fixture.GetEngine()->DispatchUiEvents(std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Cancel, .Target = root.GetUUID() } });
			const std::vector<std::string> trace = UiEventTrace(scene);
			REQUIRE(trace.size() == 4);
			CHECK(trace.back() == "Root:Listen:cancel:Root");
			CHECK(fixture.Errors.empty());
		}

		// Decision 27: each callback's value and events delivered in order; an event whose element the scene no longer holds,
		// or holds pending destruction, is dropped; and during an event's walk an ancestor that one of its callbacks
		// destroyed is passed over, the walk going on to its parent.
		TEST_CASE("UiEvents: each callback receives its element and value in order, and an event whose element is gone is dropped" * doctest::skip(true))
		{
			Test::ScriptTestFixture fixture(UiEventSpecification());
			const AssetHandle listen(0x220003);
			const AssetHandle destroy(0x220004);
			REQUIRE(fixture.AddScript(listen, "Assets/Scripts/Listen.luau", UiEventBehaviourSource("Listen", false)));
			// Records its clicks and hovers, and destroys its entity's parent (with the entity itself) on a click.
			REQUIRE(fixture.AddScript(destroy, "Assets/Scripts/Destroy.luau", R"(local T = {}
function T:OnUiClick(element)
	Probe.Record(self.Entity.Name .. ":Destroy:click:" .. element.Name)
	Scene.Destroy(self.Entity:GetParent())
end
function T:OnUiHover(element, hovered)
	Probe.Record(self.Entity.Name .. ":Destroy:hover:" .. element.Name)
end
return Script.Define("Destroy", T)
)"));
			Scene& scene = fixture.GetScene();
			const Entity widget = scene.CreateEntityWithID(UUID(10), "Widget");
			const Entity gone = scene.CreateEntityWithID(UUID(11), "Gone");
			const Entity root = scene.CreateEntityWithID(UUID(20), "Root");
			const Entity menu = scene.CreateEntityWithID(UUID(30), "Menu", root);
			const Entity button = scene.CreateEntityWithID(UUID(40), "Button", menu);
			const std::string listenSlot = std::format(R"([ {{ "Script": "{}" }} ])", listen.ToString());
			for (const Entity listener : { widget, gone, root, menu })
				AddUiEventSlots(fixture, listener, listenSlot);
			AddUiEventSlots(fixture, button, std::format(R"([ {{ "Script": "{}" }} ])", destroy.ToString()));
			REQUIRE(fixture.Start());
			// Gone is pending destruction (the fixture's scene is a runtime scene, which destroys at its flush).
			scene.DestroyEntity(gone);

			fixture.GetEngine()->DispatchUiEvents(std::vector<UiEvent>{
				UiEvent{ .Kind = UiEventKind::Focus, .Target = widget.GetUUID(), .Flag = true },
				UiEvent{ .Kind = UiEventKind::Hover, .Target = widget.GetUUID(), .Flag = false },
				UiEvent{ .Kind = UiEventKind::ValueChanged, .Target = widget.GetUUID(), .Flag = true },
				UiEvent{ .Kind = UiEventKind::ValueChanged, .Target = widget.GetUUID(), .HasNumber = true, .Number = 2.5f },
				UiEvent{ .Kind = UiEventKind::Scroll, .Target = widget.GetUUID(), .Vector = glm::vec2(0.0f, 40.0f) },
				UiEvent{ .Kind = UiEventKind::Click, .Target = UUID(99) },
				UiEvent{ .Kind = UiEventKind::Click, .Target = gone.GetUUID() },
			});
			CHECK(UiEventTrace(scene) == std::vector<std::string>{ "Widget:Listen:focus:Widget:true", "Widget:Listen:hover:Widget:false", "Widget:Listen:value:Widget:true", "Widget:Listen:value:Widget:2.5", "Widget:Listen:scroll:Widget:40" });

			// The button's click destroys Menu and so the button: the walk passes Menu over and reaches Root; the hover
			// queued behind the click finds its element pending destruction and is dropped.
			fixture.GetEngine()->DispatchUiEvents(std::vector<UiEvent>{
				UiEvent{ .Kind = UiEventKind::Click, .Target = button.GetUUID() },
				UiEvent{ .Kind = UiEventKind::Hover, .Target = button.GetUUID(), .Flag = false },
			});
			const std::vector<std::string> trace = UiEventTrace(scene);
			REQUIRE(trace.size() == 7);
			CHECK(trace[5] == "Button:Destroy:click:Button");
			CHECK(trace[6] == "Root:Listen:click:Button");
			CHECK(fixture.Errors.empty());
		}
	}

}
