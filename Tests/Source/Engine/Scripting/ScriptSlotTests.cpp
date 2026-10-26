#include "TestsPCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/ScriptComponent.h"
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
function T:OnTriggerEnter(other) Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":trigger") end
function T:OnEnable() Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":enable") end
function T:OnDisable() Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":disable") end
function T:OnDestroy() Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":destroy") end
return Script.Define(")"
				+ std::string(name) + R"(", T)
)";
		}

		// The observations recorded after the first `from`.
		std::vector<std::string> ScriptSlotTraceSince(Scene& scene, size_t from)
		{
			const std::vector<std::string> trace = ScriptSlotTrace(scene);
			return { trace.begin() + static_cast<ptrdiff_t>(std::min(from, trace.size())), trace.end() };
		}

		// Attaches `script` as the entity's one script slot.
		void AttachScript(Entity entity, AssetHandle script)
		{
			ScriptComponent component;
			component.Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(script), .Fields = {}, .ExecutionOrder = 0 });
			entity.AddComponent<ScriptComponent>(component);
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
		TEST_CASE("ScriptSlots: each slot is its own instance, called in (ExecutionOrder, canonical entity, slot) order")
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
		TEST_CASE("ScriptSlots: a fault disables only its own slot's instance")
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
		TEST_CASE("ScriptSlots: Entity:GetScript(name) and Entity:GetScripts() return the slots' instances")
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
			// Evaluate binds self to the first slot of the named behaviour.
			const Result<ScriptEvaluation> bound = fixture.GetEngine()->Evaluate("return self == self.Entity:GetScripts()[2]", VfsPath{}, UUID(50), "Fire");
			REQUIRE_MESSAGE(bound.has_value(), bound.error().ToString());
			CHECK(bound->Value.Get() == Json(true));
		}

		// Decision 17's entity callbacks: a physics callback addressed to the entity reaches every started slot in slot order,
		// whatever the slots' ExecutionOrder, and OnDestroy runs for an entity's slots in reverse slot order, children first.
		TEST_CASE("ScriptSlots: entity callbacks reach the slots in slot order and OnDestroy in reverse slot order")
		{
			Test::ScriptTestFixture fixture(ScriptSlotSpecification());
			const AssetHandle aim(0x210001);
			const AssetHandle fire(0x210002);
			REQUIRE(fixture.AddScript(aim, "Assets/Scripts/Aim.luau", SlotBehaviourSource("Aim")));
			REQUIRE(fixture.AddScript(fire, "Assets/Scripts/Fire.luau", SlotBehaviourSource("Fire")));
			Scene& scene = fixture.GetScene();
			const Entity parent = scene.CreateEntityWithID(UUID(80), "Parent");
			const Entity child = scene.CreateEntityWithID(UUID(81), "Child", parent);
			AddScriptSlots(fixture, parent, std::format(R"([ {{ "Script": "{}", "ExecutionOrder": 5 }}, {{ "Script": "{}" }} ])", aim.ToString(), fire.ToString()));
			AddScriptSlots(fixture, child, std::format(R"([ {{ "Script": "{}" }} ])", aim.ToString()));
			REQUIRE(fixture.Start());

			size_t from = ScriptSlotTrace(scene).size();
			fixture.GetEngine()->OnPhysicsEvent(PhysicsEvent{ .Type = PhysicsEventType::TriggerEnter, .Self = UUID(80), .Other = UUID(81) });
			CHECK(ScriptSlotTraceSince(scene, from) == std::vector<std::string>{ "Parent:Aim:trigger", "Parent:Fire:trigger" });

			from = ScriptSlotTrace(scene).size();
			scene.DestroyEntity(parent);
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 3);
			fixture.GetEngine()->FinishDestroyFlush();
			CHECK(ScriptSlotTraceSince(scene, from) == std::vector<std::string>{ "Child:Aim:destroy", "Parent:Fire:destroy", "Parent:Aim:destroy" });
			CHECK(fixture.GetEngine()->GetInstances().empty());
			CHECK(fixture.Errors.empty());
		}

		// Decision 17's entity callbacks: OnEnable and OnDisable reach an entity's slots together, in slot order, at the turn of
		// its slot that comes first in (ExecutionOrder, canonical entity) order; the teardown of the disable runs in reverse
		// slot order, and the instances recreated on enable are created in (ExecutionOrder, canonical entity, slot) order.
		TEST_CASE("ScriptSlots: OnEnable and OnDisable reach an entity's slots together in slot order")
		{
			Test::ScriptTestFixture fixture(ScriptSlotSpecification());
			const AssetHandle aim(0x210001);
			const AssetHandle fire(0x210002);
			REQUIRE(fixture.AddScript(aim, "Assets/Scripts/Aim.luau", SlotBehaviourSource("Aim")));
			REQUIRE(fixture.AddScript(fire, "Assets/Scripts/Fire.luau", SlotBehaviourSource("Fire")));
			Scene& scene = fixture.GetScene();
			const Entity first = scene.CreateEntityWithID(UUID(120), "First");
			const Entity second = scene.CreateEntityWithID(UUID(121), "Second");
			AddScriptSlots(fixture, first, std::format(R"([ {{ "Script": "{}", "ExecutionOrder": 2 }}, {{ "Script": "{}" }} ])", aim.ToString(), fire.ToString()));
			AddScriptSlots(fixture, second, std::format(R"([ {{ "Script": "{}", "ExecutionOrder": 1 }} ])", aim.ToString()));
			REQUIRE(fixture.Start());
			CHECK(ScriptSlotTrace(scene) == std::vector<std::string>{ "First:Fire:create", "Second:Aim:create", "First:Aim:create", "First:Fire:start", "Second:Aim:start", "First:Aim:start" });

			size_t from = ScriptSlotTrace(scene).size();
			first.SetActive(false);
			second.SetActive(false);
			REQUIRE(fixture.GetEngine()->SynchronizeInstances());
			CHECK(ScriptSlotTraceSince(scene, from) == std::vector<std::string>{ "First:Aim:disable", "First:Fire:disable", "Second:Aim:disable" });
			from = ScriptSlotTrace(scene).size();
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 3);
			fixture.GetEngine()->FinishDestroyFlush();
			CHECK(ScriptSlotTraceSince(scene, from) == std::vector<std::string>{ "Second:Aim:destroy", "First:Fire:destroy", "First:Aim:destroy" });

			from = ScriptSlotTrace(scene).size();
			first.SetActive(true);
			REQUIRE(fixture.GetEngine()->SynchronizeInstances());
			fixture.GetEngine()->StartPending();
			CHECK(ScriptSlotTraceSince(scene, from) == std::vector<std::string>{ "First:Fire:create", "First:Aim:create", "First:Aim:enable", "First:Fire:enable", "First:Fire:start", "First:Aim:start" });
			CHECK(fixture.Errors.empty());
		}

		// Decision 17's faults: a fault-disabled slot keeps its disabled record through an entity's disable and enable, while
		// the entity's other slots are torn down and recreated as usual.
		TEST_CASE("ScriptSlots: a fault-disabled slot stays disabled across a toggle while its siblings are recreated")
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
			Scene& scene = fixture.GetScene();
			const Entity entity = scene.CreateEntityWithID(UUID(130), "Toggled");
			AddScriptSlots(fixture, entity, std::format(R"([ {{ "Script": "{}" }}, {{ "Script": "{}" }} ])", fail.ToString(), aim.ToString()));
			REQUIRE(fixture.Start());
			fixture.GetEngine()->FixedUpdate();
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors.front().Slot == 0);

			entity.SetActive(false);
			REQUIRE(fixture.GetEngine()->SynchronizeInstances());
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 2);
			fixture.GetEngine()->FinishDestroyFlush();
			// While the entity is inactive, its torn-down slot is not recreated; the faulted one keeps its record.
			REQUIRE(fixture.GetEngine()->SynchronizeInstances());
			std::vector<ScriptInstanceInfo> instances = fixture.GetEngine()->GetInstances();
			REQUIRE(instances.size() == 1);
			CHECK(instances[0].Slot == 0);
			CHECK(instances[0].Disabled);

			const size_t from = ScriptSlotTrace(scene).size();
			entity.SetActive(true);
			REQUIRE(fixture.GetEngine()->SynchronizeInstances());
			fixture.GetEngine()->StartPending();
			fixture.GetEngine()->FixedUpdate();
			instances = fixture.GetEngine()->GetInstances();
			REQUIRE(instances.size() == 2);
			CHECK(instances[0].Disabled);
			CHECK_FALSE(instances[1].Disabled);
			CHECK(fixture.Errors.size() == 1);
			CHECK(ScriptSlotTraceSince(scene, from) == std::vector<std::string>{ "Toggled:Aim:create", "Toggled:Aim:enable", "Toggled:Aim:start", "Toggled:Aim:fixed" });
		}

		// Decision 17's held events: each slot holds or discards its own physics events, so a slot added after its entity
		// started receives the event at its own start flush, while its started siblings receive it at once.
		TEST_CASE("ScriptSlots: a slot that has not started holds its physics event until its start flush")
		{
			Test::ScriptTestFixture fixture(ScriptSlotSpecification());
			const AssetHandle aim(0x210001);
			const AssetHandle fire(0x210002);
			REQUIRE(fixture.AddScript(aim, "Assets/Scripts/Aim.luau", SlotBehaviourSource("Aim")));
			REQUIRE(fixture.AddScript(fire, "Assets/Scripts/Fire.luau", SlotBehaviourSource("Fire")));
			Scene& scene = fixture.GetScene();
			const Entity entity = scene.CreateEntityWithID(UUID(140), "Late");
			AddScriptSlots(fixture, entity, std::format(R"([ {{ "Script": "{}" }} ])", aim.ToString()));
			REQUIRE(fixture.Start());
			const Result<Json> grown = JsonReader::Parse(std::format(R"({{ "Scripts": [ {{ "Script": "{}" }}, {{ "Script": "{}" }} ] }})", aim.ToString(), fire.ToString()));
			REQUIRE(grown.has_value());
			REQUIRE(ComponentAccess::SetComponentJson(entity, "Script", *grown, fixture.GetSchemaSnapshot().get()));
			REQUIRE(fixture.GetEngine()->SynchronizeInstances());

			const size_t from = ScriptSlotTrace(scene).size();
			fixture.GetEngine()->OnPhysicsEvent(PhysicsEvent{ .Type = PhysicsEventType::TriggerEnter, .Self = UUID(140), .Other = UUID(141) });
			CHECK(ScriptSlotTraceSince(scene, from) == std::vector<std::string>{ "Late:Aim:trigger" });
			fixture.Frame.Phase = InputPhase::Step;
			fixture.GetEngine()->StartPending();
			CHECK(ScriptSlotTraceSince(scene, from) == std::vector<std::string>{ "Late:Aim:trigger", "Late:Fire:start", "Late:Fire:trigger" });
			CHECK(fixture.Errors.empty());
		}

		// Decision 17's list writes: a write of the Script list keeps an instance whose slot index still holds the same
		// script, and destroys and creates the others, as removing and adding the component would.
		TEST_CASE("ScriptSlots: a write of the Script list keeps the instances whose slot keeps its script")
		{
			Test::ScriptTestFixture fixture(ScriptSlotSpecification());
			const AssetHandle aim(0x210001);
			const AssetHandle fire(0x210002);
			REQUIRE(fixture.AddScript(aim, "Assets/Scripts/Aim.luau", SlotBehaviourSource("Aim")));
			REQUIRE(fixture.AddScript(fire, "Assets/Scripts/Fire.luau", SlotBehaviourSource("Fire")));
			Scene& scene = fixture.GetScene();
			const Entity entity = scene.CreateEntityWithID(UUID(90), "Rig");
			AddScriptSlots(fixture, entity, std::format(R"([ {{ "Script": "{}" }}, {{ "Script": "{}" }} ])", aim.ToString(), fire.ToString()));
			REQUIRE(fixture.Start());
			REQUIRE(fixture.GetEngine()->Evaluate("self.Marker = 1", VfsPath{}, UUID(90), "Aim"));
			REQUIRE(fixture.GetEngine()->Evaluate("self.Marker = 2", VfsPath{}, UUID(90), "Fire"));
			const auto writeSlots = [&fixture, entity](const std::string& scripts)
			{
				const Result<Json> value = JsonReader::Parse(R"({ "Scripts": )" + scripts + "}");
				REQUIRE(value.has_value());
				const Status written = ComponentAccess::SetComponentJson(entity, "Script", *value, fixture.GetSchemaSnapshot().get());
				REQUIRE_MESSAGE(written.has_value(), (written ? std::string() : written.error().ToString()));
				REQUIRE(fixture.GetEngine()->SynchronizeInstances());
			};
			const auto markers = [&fixture]()
			{
				const Result<ScriptEvaluation> read = fixture.Evaluate(R"(
local markers = {}
for index, instance in Scene.FindByName("Rig"):GetScripts() do markers[index] = instance.Kind .. ":" .. tostring(instance.Marker) end
return markers
)");
				REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
				return read->Value.Get();
			};

			// Slot 1 changes its script: slot 0 keeps its instance and its state, slot 1 is replaced.
			size_t from = ScriptSlotTrace(scene).size();
			writeSlots(std::format(R"([ {{ "Script": "{}", "ExecutionOrder": 3 }}, {{ "Script": "{}" }} ])", aim.ToString(), aim.ToString()));
			CHECK(ScriptSlotTraceSince(scene, from) == std::vector<std::string>{ "Rig:Fire:destroy", "Rig:Aim:create" });
			CHECK(markers() == Json::array({ "Aim:1", "Aim:nil" }));
			CHECK(fixture.GetEngine()->GetInstances().front().ExecutionOrder == 3);

			// The list shrinks, then grows: the removed slot's instance goes, and an appended slot gets a new one.
			from = ScriptSlotTrace(scene).size();
			writeSlots(std::format(R"([ {{ "Script": "{}" }} ])", aim.ToString()));
			writeSlots(std::format(R"([ {{ "Script": "{}" }}, {{ "Script": "{}" }} ])", aim.ToString(), fire.ToString()));
			CHECK(ScriptSlotTraceSince(scene, from) == std::vector<std::string>{ "Rig:Aim:destroy", "Rig:Fire:create" });
			CHECK(markers() == Json::array({ "Aim:1", "Fire:nil" }));
			CHECK(fixture.Errors.empty());
		}

		// Decision 17's faults and tasks: a task belongs to the slot whose callback spawned it, so a fault of one slot
		// cancels only that slot's tasks, and the entity's other slots keep theirs.
		TEST_CASE("ScriptSlots: a fault cancels only its own slot's tasks")
		{
			Test::ExpectLog expected(LogLevel::Error, "breaker fault");
			auto specification = ScriptSlotSpecification();
			specification.TestMode = false;
			Test::ScriptTestFixture fixture(specification);
			const auto tickerSource = [](std::string_view name, bool faults)
			{
				return std::format(R"(local T = {{}}
function T:OnStart()
	local name = self.Entity.Name .. ":{0}"
	Task.Spawn(function()
		while true do
			Task.WaitTicks(1)
			Probe.Record(name .. ":task")
		end
	end)
end
{1}
return Script.Define("{0}", T))",
					name, faults ? "function T:OnFixedUpdate() error(\"breaker fault\") end" : "");
			};
			const AssetHandle ticker(0x210005);
			const AssetHandle breaker(0x210006);
			REQUIRE(fixture.AddScript(ticker, "Assets/Scripts/Ticker.luau", tickerSource("Ticker", false)));
			REQUIRE(fixture.AddScript(breaker, "Assets/Scripts/Breaker.luau", tickerSource("Breaker", true)));
			const Entity entity = fixture.GetScene().CreateEntityWithID(UUID(100), "Pair");
			AddScriptSlots(fixture, entity, std::format(R"([ {{ "Script": "{}" }}, {{ "Script": "{}" }} ])", breaker.ToString(), ticker.ToString()));
			REQUIRE(fixture.Start());
			for (uint64_t tick = 1; tick <= 2; ++tick)
			{
				fixture.Frame.Tick = tick;
				fixture.GetEngine()->FixedUpdate();
				fixture.GetEngine()->ResumeTasks();
			}
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors.front().Slot == 0);
			CHECK(ScriptSlotTrace(fixture.GetScene()) == std::vector<std::string>{ "Pair:Ticker:task", "Pair:Ticker:task" });
		}

		// Decision 17's hot reload: a reload patches the instance of every slot that holds the reloaded behaviour, in
		// (ExecutionOrder, canonical entity, slot) order, and leaves the other slots alone.
		TEST_CASE("ScriptSlots: a hot reload patches every slot holding the reloaded behaviour")
		{
			Test::ScriptTestFixture fixture(ScriptSlotSpecification());
			const AssetHandle aim(0x210001);
			const AssetHandle fire(0x210002);
			REQUIRE(fixture.AddScript(aim, "Assets/Scripts/Aim.luau", SlotBehaviourSource("Aim")));
			REQUIRE(fixture.AddScript(fire, "Assets/Scripts/Fire.luau", SlotBehaviourSource("Fire")));
			const Entity entity = fixture.GetScene().CreateEntityWithID(UUID(110), "Twin");
			AddScriptSlots(fixture, entity, std::format(R"([ {{ "Script": "{}" }}, {{ "Script": "{}" }}, {{ "Script": "{}" }} ])", aim.ToString(), fire.ToString(), aim.ToString()));
			REQUIRE(fixture.Start());
			REQUIRE(fixture.GetEngine()->Evaluate("self.Marker = 3", VfsPath{}, UUID(110), "Aim"));
			const size_t from = ScriptSlotTrace(fixture.GetScene()).size();
			REQUIRE(fixture.AddScript(aim, "Assets/Scripts/Aim.luau", R"(local T = {}
function T:OnHotReload() Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":reload:" .. tostring(self.Marker)) end
return Script.Define("Aim", T))"));
			const Result<ScriptReloadResult> reloaded = fixture.GetEngine()->Reload(aim);
			REQUIRE_MESSAGE(reloaded.has_value(), reloaded.error().ToString());
			CHECK(ScriptSlotTraceSince(fixture.GetScene(), from) == std::vector<std::string>{ "Twin:Aim:reload:3", "Twin:Aim:reload:nil" });
			CHECK(fixture.Errors.empty());
		}

		// Decision 18 (script.eval's `script`): Evaluate binds self to the instance of the slot whose behaviour has the name,
		// or of the first slot without one, and refuses a name no slot of the entity has before running anything; the
		// refusal is not an error of the entity's behaviour.
		TEST_CASE("ScriptSlots: Evaluate binds self to the named behaviour's slot and refuses a name no slot has")
		{
			Test::ScriptTestFixture fixture(ScriptSlotSpecification());
			const AssetHandle aim(0x210001);
			REQUIRE(fixture.AddScript(aim, "Assets/Scripts/Aim.luau", SlotBehaviourSource("Aim")));
			AttachScript(fixture.GetScene().CreateEntityWithID(UUID(60), "Gun"), aim);
			REQUIRE(fixture.Start());
			ScriptEngine& engine = *fixture.GetEngine();

			const Result<ScriptEvaluation> named = engine.Evaluate("return self.Kind", VfsPath{}, UUID(60), "Aim");
			REQUIRE_MESSAGE(named.has_value(), named.error().ToString());
			CHECK(named->Value.Get() == Json("Aim"));
			const Result<ScriptEvaluation> first = engine.Evaluate("return self.Kind", VfsPath{}, UUID(60));
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			CHECK(first->Value.Get() == Json("Aim"));

			const size_t traced = ScriptSlotTrace(fixture.GetScene()).size();
			const Result<ScriptEvaluation> missing = engine.Evaluate("Probe.Record(\"ran\") return 1", VfsPath{}, UUID(60), "Fire");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(ScriptSlotTrace(fixture.GetScene()).size() == traced);
			CHECK(fixture.Errors.empty());
			for (const ScriptInstanceInfo& instance : engine.GetInstances())
				CHECK_FALSE(instance.Disabled);
		}

		// Decision 18: GetScript and GetScripts return only instances a script can still address, so an instance being torn
		// down (its OnDestroy running) is left out, while its entity's reads keep working until the flush.
		TEST_CASE("ScriptSlots: GetScript and GetScripts leave out an instance being torn down")
		{
			Test::ScriptTestFixture fixture(ScriptSlotSpecification());
			const AssetHandle keeper(0x210004);
			REQUIRE(fixture.AddScript(keeper, "Assets/Scripts/Keeper.luau", R"(local T = {}
function T:OnDestroy()
	local entity = self.Entity
	Probe.Record(string.format("%d:%s:%s", #entity:GetScripts(), tostring(entity:GetScript() == nil), tostring(entity:GetScript("Keeper") == nil)))
end
return Script.Define("Keeper", T))"));
			const Entity holder = fixture.GetScene().CreateEntityWithID(UUID(70), "Holder");
			AttachScript(holder, keeper);
			REQUIRE(fixture.Start());
			const Result<ScriptEvaluation> live = fixture.Evaluate(R"(
local holder = Scene.FindByName("Holder")
local all = holder:GetScripts()
return #all == 1 and all[1] == holder:GetScript() and holder:GetScript("Keeper") == all[1]
)");
			REQUIRE_MESSAGE(live.has_value(), live.error().ToString());
			CHECK(live->Value.Get() == Json(true));

			fixture.GetScene().DestroyEntity(holder);
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 1);
			fixture.GetEngine()->FinishDestroyFlush();
			CHECK(ScriptSlotTrace(fixture.GetScene()) == std::vector<std::string>{ "0:true:true" });
			CHECK(fixture.Errors.empty());
		}
	}

}
