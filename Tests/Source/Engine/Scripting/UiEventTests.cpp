#include "TestsPCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiInteraction.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// The UI events (M22, C22.2, R-UI-EVENTS; Docs/Decisions/0033-m22-contract.md decision 27): the UiEvents phase's delivery
// (ScriptEngine::DispatchUiEvents), bubbling per behaviour on M20's script slots (ADR 0031 decision 18), and the callbacks'
// registration. The Ui module's cases are Bindings/UiTests.cpp's.

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
		TEST_CASE("UiEvents: an event bubbles from its element per behaviour and stops after an entity where a callback returns true")
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
		TEST_CASE("UiEvents: each callback receives its element and value in order, and an event whose element is gone is dropped")
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

		// Decision 27 (§11.7): a callback's fault disables only its own instance, and the event goes on to the next receiver;
		// only the boolean true stops an event, so a callback returning another truthy value lets it bubble on; a stopped
		// engine delivers nothing.
		TEST_CASE("UiEvents: a faulting callback disables only its instance, and only the boolean true stops an event")
		{
			Test::ExpectLog expected(LogLevel::Error, "ui callback fault");
			Test::ScriptTestFixture fixture(UiEventSpecification());
			const AssetHandle listen(0x220005);
			const AssetHandle fault(0x220006);
			REQUIRE(fixture.AddScript(listen, "Assets/Scripts/Listen.luau", UiEventBehaviourSource("Listen", false)));
			REQUIRE(fixture.AddScript(fault, "Assets/Scripts/Fault.luau", R"(local T = {}
function T:OnUiCancel(element)
	Probe.Record(self.Entity.Name .. ":Fault:cancel:" .. element.Name)
	return 1
end
function T:OnUiClick(element)
	error("ui callback fault")
end
return Script.Define("Fault", T)
)"));
			Scene& scene = fixture.GetScene();
			const Entity root = scene.CreateEntityWithID(UUID(10), "Root");
			const Entity panel = scene.CreateEntityWithID(UUID(20), "Panel", root);
			const Entity button = scene.CreateEntityWithID(UUID(30), "Button", panel);
			AddUiEventSlots(fixture, root, std::format(R"([ {{ "Script": "{}" }} ])", listen.ToString()));
			AddUiEventSlots(fixture, panel, std::format(R"([ {{ "Script": "{}" }}, {{ "Script": "{}" }} ])", fault.ToString(), listen.ToString()));
			REQUIRE(fixture.Start());

			fixture.GetEngine()->DispatchUiEvents(std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Cancel, .Target = button.GetUUID() } });
			CHECK(UiEventTrace(scene)
				== std::vector<std::string>{ "Panel:Fault:cancel:Button", "Panel:Listen:cancel:Button", "Root:Listen:cancel:Button" });

			// The first click faults Panel's Fault slot: its Listen slot and Root still hear both clicks.
			fixture.GetEngine()->DispatchUiEvents(std::vector<UiEvent>{
				UiEvent{ .Kind = UiEventKind::Click, .Target = button.GetUUID() },
				UiEvent{ .Kind = UiEventKind::Click, .Target = button.GetUUID() },
			});
			const std::vector<std::string> trace = UiEventTrace(scene);
			CHECK(trace
				== std::vector<std::string>{ "Panel:Fault:cancel:Button", "Panel:Listen:cancel:Button", "Root:Listen:cancel:Button",
					"Panel:Listen:click:Button", "Root:Listen:click:Button", "Panel:Listen:click:Button", "Root:Listen:click:Button" });
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors[0].Entity == panel.GetUUID());
			CHECK(fixture.Errors[0].Slot == 0);
			CHECK(fixture.Errors[0].Callback == "OnUiClick");

			fixture.GetEngine()->Stop();
			fixture.GetEngine()->DispatchUiEvents(std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Click, .Target = button.GetUUID() } });
			CHECK(UiEventTrace(scene) == trace);
		}

		// Decision 27: an event's walk is its element and the ancestors the element had when the walk started, so a callback
		// that moves an ancestor elsewhere neither adds the new ancestors to that walk nor removes the old ones; the next
		// event walks the new hierarchy.
		TEST_CASE("UiEvents: an event walks the ancestors its element had when the walk started, whatever a callback reparents")
		{
			Test::ScriptTestFixture fixture(UiEventSpecification());
			const AssetHandle listen(0x220007);
			const AssetHandle mover(0x220008);
			REQUIRE(fixture.AddScript(listen, "Assets/Scripts/Listen.luau", UiEventBehaviourSource("Listen", false)));
			REQUIRE(fixture.AddScript(mover, "Assets/Scripts/Mover.luau", R"(local T = {}
function T:OnUiClick(element)
	Probe.Record(self.Entity.Name .. ":Mover:click:" .. element.Name)
	self.Entity:GetParent():SetParent(Scene.FindByName("Other"))
end
return Script.Define("Mover", T)
)"));
			Scene& scene = fixture.GetScene();
			const Entity root = scene.CreateEntityWithID(UUID(10), "Root");
			const Entity menu = scene.CreateEntityWithID(UUID(20), "Menu", root);
			const Entity button = scene.CreateEntityWithID(UUID(30), "Button", menu);
			const Entity other = scene.CreateEntityWithID(UUID(40), "Other");
			const std::string listenSlot = std::format(R"([ {{ "Script": "{}" }} ])", listen.ToString());
			AddUiEventSlots(fixture, root, listenSlot);
			AddUiEventSlots(fixture, other, listenSlot);
			AddUiEventSlots(fixture, button, std::format(R"([ {{ "Script": "{}" }} ])", mover.ToString()));
			REQUIRE(fixture.Start());

			fixture.GetEngine()->DispatchUiEvents(std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Click, .Target = button.GetUUID() } });
			CHECK(UiEventTrace(scene) == std::vector<std::string>{ "Button:Mover:click:Button", "Root:Listen:click:Button" });
			CHECK(menu.GetParent().GetUUID() == other.GetUUID());
			fixture.GetEngine()->DispatchUiEvents(std::vector<UiEvent>{ UiEvent{ .Kind = UiEventKind::Click, .Target = button.GetUUID() } });
			CHECK(UiEventTrace(scene)
				== std::vector<std::string>{ "Button:Mover:click:Button", "Root:Listen:click:Button", "Button:Mover:click:Button",
					"Other:Listen:click:Button" });
			CHECK(fixture.Errors.empty());
		}

		// Decision 27: the six callbacks' names join the reserved names a behaviour's fields cannot take, as the earlier
		// callbacks' did.
		TEST_CASE("UiEvents: the UI callbacks' names are reserved, so no behaviour field takes one")
		{
			Test::ScriptTestFixture fixture;
			uint32_t index = 0;
			for (const UiEventKind kind :
				{ UiEventKind::Click, UiEventKind::ValueChanged, UiEventKind::Focus, UiEventKind::Hover, UiEventKind::Cancel, UiEventKind::Scroll })
			{
				const std::string_view callback = GetUiEventCallbackName(kind);
				CAPTURE(callback);
				++index;
				const auto compiled = fixture.AddScript(AssetHandle(0x220010 + index), std::format("Assets/Scripts/Reserved{}.luau", index),
					std::format("local T = {{Fields = {{{} = Field.Number()}}}}\nreturn Script.Define(\"Reserved\", T)\n", callback));
				CHECK_FALSE(compiled.has_value());
			}
			CHECK(fixture.AddScript(AssetHandle(0x220020), "Assets/Scripts/Allowed.luau",
				"local T = {Fields = {OnUiClicked = Field.Number()}}\nreturn Script.Define(\"Allowed\", T)\n"));
		}
	}

}
