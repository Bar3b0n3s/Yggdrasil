#include "TestsPCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptError.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// Multiple scripts per entity (M20, S-MULTI; Docs/Decisions/0031-m20-contract.md decisions 17 and 18): the Script component
// is an ordered list, and every slot is its own instance.

namespace Engine {

	namespace {

		int RecordScriptSlot(ScriptCall& call)
		{
			const std::string message = Lua::Check<std::string>(call, 1);
			static_cast<void>(Lua::GetEngine(call)->GetHost().GetScene().CreateEntity("Trace:" + message));
			return 0;
		}

		Test::ScriptTestFixtureSpecification ScriptSlotSpecification()
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.TestMode = true;
			specification.ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				api.Module("Probe", "Records callback ordering into the test scene.").Function("Record", &RecordScriptSlot, "(message: string) -> ()", "Append one test observation.", { .Mutates = true });
				return {};
			};
			return specification;
		}

		std::vector<std::string> ScriptSlotTrace(Scene& scene)
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

		// A behaviour named `name` that records its callbacks as "<entity>:<name>:<callback>" and keeps Kind = `name`.
		std::string SlotBehaviourSource(std::string_view name)
		{
			return std::string(R"(local T = {}
function T:OnCreate()
	self.Kind = ")")
				+ std::string(name) + R"("
	Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":create")
end
function T:OnStart() Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":start") end
function T:OnFixedUpdate() Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":fixed") end
return Script.Define(")"
				+ std::string(name) + R"(", T)
)";
		}

		// Adds a Script component holding `scripts` (Script version 2's list of slot objects) through the registry, the way
		// a scene file or automation sets it.
		void AddScriptSlots(Test::ScriptTestFixture& fixture, Entity entity, std::string_view scripts)
		{
			const Result<Json> slots = JsonReader::Parse(std::string(R"({ "Scripts": )") + std::string(scripts) + "}");
			REQUIRE(slots.has_value());
			const Status added = ComponentAccess::AddComponent(entity, "Script", &*slots, fixture.GetSchemaSnapshot().get());
			REQUIRE_MESSAGE(added.has_value(), (added ? std::string() : added.error().ToString()));
		}

	}

	TEST_SUITE("Scripting")
	{
		// Decision 17 (Roadmap M20 acceptance "script callback order, slot included, is identical in all four runs"): every
		// slot is an instance of its own, keyed (entity, slot), and callbacks run in (ExecutionOrder, canonical entity, slot)
		// order; the same script may fill two slots of one entity.
		TEST_CASE("ScriptSlots: each slot is its own instance, called in (ExecutionOrder, canonical entity, slot) order" * doctest::skip(true))
		{
			Test::ScriptTestFixture fixture(ScriptSlotSpecification());
			const AssetHandle aim(0x210001);
			const AssetHandle fire(0x210002);
			REQUIRE(fixture.AddScript(aim, "Assets/Scripts/Aim.luau", SlotBehaviourSource("Aim")));
			REQUIRE(fixture.AddScript(fire, "Assets/Scripts/Fire.luau", SlotBehaviourSource("Fire")));
			Scene& scene = fixture.GetScene();
			const Entity first = scene.CreateEntityWithID(UUID(10), "E1");
			const Entity second = scene.CreateEntityWithID(UUID(20), "E2");
			const Entity third = scene.CreateEntityWithID(UUID(30), "E3");
			AddScriptSlots(fixture, first, std::format(R"([ {{ "Script": "{}" }}, {{ "Script": "{}" }} ])", aim.ToString(), fire.ToString()));
			AddScriptSlots(fixture, second, std::format(R"([ {{ "Script": "{}", "ExecutionOrder": -1 }} ])", fire.ToString()));
			AddScriptSlots(fixture, third, std::format(R"([ {{ "Script": "{}" }}, {{ "Script": "{}" }} ])", aim.ToString(), aim.ToString()));
			REQUIRE(fixture.Start());
			REQUIRE(fixture.Errors.empty());

			CHECK(ScriptSlotTrace(scene) == std::vector<std::string>{ "E2:Fire:create", "E1:Aim:create", "E1:Fire:create", "E3:Aim:create", "E3:Aim:create", "E2:Fire:start", "E1:Aim:start", "E1:Fire:start", "E3:Aim:start", "E3:Aim:start" });
			const size_t started = ScriptSlotTrace(scene).size();
			fixture.GetEngine()->FixedUpdate();
			const std::vector<std::string> trace = ScriptSlotTrace(scene);
			CHECK(std::vector<std::string>(trace.begin() + static_cast<ptrdiff_t>(started), trace.end())
				== std::vector<std::string>{ "E2:Fire:fixed", "E1:Aim:fixed", "E1:Fire:fixed", "E3:Aim:fixed", "E3:Aim:fixed" });

			std::vector<ScriptInstanceInfo> instances = fixture.GetEngine()->GetInstances();
			std::sort(instances.begin(), instances.end(), [](const ScriptInstanceInfo& left, const ScriptInstanceInfo& right)
			{
				return left.Entity != right.Entity ? left.Entity < right.Entity : left.Slot < right.Slot;
			});
			REQUIRE(instances.size() == 5);
			CHECK(instances[0].Entity == UUID(10));
			CHECK(instances[0].Slot == 0);
			CHECK(instances[0].Script == aim);
			CHECK(instances[1].Slot == 1);
			CHECK(instances[1].Script == fire);
			CHECK(instances[2].Entity == UUID(20));
			CHECK(instances[2].ExecutionOrder == -1);
			CHECK(instances[3].Entity == UUID(30));
			CHECK(instances[4].Slot == 1);
			CHECK(instances[4].Script == aim);
		}

		// Decision 17 (Roadmap M20 acceptance "a fault stays in its instance"): a fault disables its own slot; the entity's
		// other slots keep running, and the error names the slot. The faulting behaviour fills slot 1, so an error whose slot
		// is left at its default (0) fails.
		TEST_CASE("ScriptSlots: a fault disables only its own slot's instance" * doctest::skip(true))
		{
			Test::ExpectLog expected(LogLevel::Error, "slot failure");
			auto specification = ScriptSlotSpecification();
			specification.TestMode = false;
			Test::ScriptTestFixture fixture(specification);
			const AssetHandle fail(0x210003);
			const AssetHandle aim(0x210001);
			REQUIRE(fixture.AddScript(fail, "Assets/Scripts/SlotFail.luau", R"(local T = {}
function T:OnFixedUpdate() error("slot failure") end
return Script.Define("SlotFail", T))"));
			REQUIRE(fixture.AddScript(aim, "Assets/Scripts/Aim.luau", SlotBehaviourSource("Aim")));
			const Entity entity = fixture.GetScene().CreateEntityWithID(UUID(40), "Both");
			AddScriptSlots(fixture, entity, std::format(R"([ {{ "Script": "{}" }}, {{ "Script": "{}" }} ])", aim.ToString(), fail.ToString()));
			REQUIRE(fixture.Start());
			fixture.GetEngine()->FixedUpdate();
			fixture.GetEngine()->FixedUpdate();

			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors.front().Entity == UUID(40));
			CHECK(fixture.Errors.front().Slot == 1);
			CHECK(fixture.Errors.front().Callback == "OnFixedUpdate");
			const std::vector<std::string> trace = ScriptSlotTrace(fixture.GetScene());
			CHECK(std::count(trace.begin(), trace.end(), "Both:Aim:fixed") == 2);
			for (const ScriptInstanceInfo& instance : fixture.GetEngine()->GetInstances())
			{
				CAPTURE(instance.Slot);
				CHECK(instance.Disabled == (instance.Slot == 1));
			}
		}

		// Decision 18 (the S-MULTI bindings): Entity:GetScript() keeps returning the first slot's instance, GetScript(name)
		// the first slot whose behaviour has that Script.Define name, and GetScripts() every slot's instance in slot order.
		TEST_CASE("ScriptSlots: Entity:GetScript(name) and Entity:GetScripts() return the slots' instances" * doctest::skip(true))
		{
			Test::ScriptTestFixture fixture(ScriptSlotSpecification());
			const AssetHandle aim(0x210001);
			const AssetHandle fire(0x210002);
			REQUIRE(fixture.AddScript(aim, "Assets/Scripts/Aim.luau", SlotBehaviourSource("Aim")));
			REQUIRE(fixture.AddScript(fire, "Assets/Scripts/Fire.luau", SlotBehaviourSource("Fire")));
			const Entity entity = fixture.GetScene().CreateEntityWithID(UUID(50), "Turret");
			AddScriptSlots(fixture, entity, std::format(R"([ {{ "Script": "{}" }}, {{ "Script": "{}" }}, {{ "Script": "{}" }} ])", aim.ToString(), fire.ToString(), fire.ToString()));
			REQUIRE(fixture.Start());

			const Result<ScriptEvaluation> evaluated = fixture.Evaluate(R"(
local turret = Scene.FindByName("Turret")
local all = turret:GetScripts()
return #all == 3 and all[1].Kind == "Aim" and all[2].Kind == "Fire" and all[3].Kind == "Fire"
	and turret:GetScript() == all[1] and turret:GetScript("Fire") == all[2] and turret:GetScript("Missing") == nil
)");
			REQUIRE_MESSAGE(evaluated.has_value(), evaluated.error().ToString());
			CHECK(evaluated->Value.Get() == Json(true));
		}
	}

}
