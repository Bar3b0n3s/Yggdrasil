#include "TestsPCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Scripting/TaskScheduler.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"
#include "Support/PhysicsTestScene.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	namespace Test {

		static int RecordScriptLifecycle(ScriptCall& call)
		{
			const std::string message = Lua::Check<std::string>(call, 1);
			static_cast<void>(Lua::GetEngine(call)->GetHost().GetScene().CreateEntity("Trace:" + message));
			return 0;
		}

		static ScriptTestFixtureSpecification LifecycleSpecification()
		{
			ScriptTestFixtureSpecification specification{};
			specification.TestMode = true;
			specification.ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				api.Module("Probe", "Records callback ordering into the test scene.").Function("Record", &RecordScriptLifecycle, "(message: string) -> ()", "Append one test observation.", { .Mutates = true });
				return {};
			};
			return specification;
		}

		static std::vector<std::string> ScriptLifecycleTrace(Scene& scene)
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

		static void AttachLifecycleScript(Entity entity, AssetHandle handle, int32_t order = 0)
		{
			ScriptSlot script{};
			script.Script.SetHandle(handle);
			script.ExecutionOrder = order;
			entity.AddComponent<ScriptComponent>(ScriptComponent{ { script } });
		}

		static constexpr std::string_view LifecycleSource = R"(
local T = {}
function T:OnCreate()
	self.Label = self.Entity.Name
	Probe.Record(self.Label .. ":create")
end
function T:OnStart() Probe.Record(self.Label .. ":start") end
function T:OnFixedUpdate(dt) assert(dt > 0); Probe.Record(self.Label .. ":fixed") end
function T:OnUpdate(dt) assert(dt > 0); Probe.Record(self.Label .. ":update") end
function T:OnLateUpdate(dt) assert(dt > 0); Probe.Record(self.Label .. ":late") end
function T:OnDisable() Probe.Record(self.Label .. ":disable") end
function T:OnEnable() Probe.Record(self.Label .. ":enable") end
function T:OnDestroy() Probe.Record(self.Label .. ":destroy") end
return Script.Define("Lifecycle", T)
)";

		static constexpr std::string_view BehaviourCoverageSource = R"(
local T = {}
local function record(self, phase)
	Scene.CreateEntity("Trace:" .. self.Label .. ":" .. phase)
end
function T:OnCreate()
	assert(self.Label == nil)
	self.Label = self.Entity.Name
	self.Phase = "created"
	record(self, "create")
end
function T:OnEnable()
	assert(self.Phase == "created")
	record(self, "enable")
end
function T:OnStart()
	assert(self.Phase == "created")
	self.Phase = "started"
	record(self, "start")
end
function T:OnFixedUpdate(dt)
	assert(self.Phase == "started" or self.Phase == "late")
	assert(dt > 0 and dt == Time.GetFixedDeltaTime())
	self.Phase = "fixed"
	record(self, "fixed")
end
function T:OnUpdate(dt)
	assert(self.Phase == "fixed")
	assert(dt > 0 and dt == Time.GetDeltaTime())
	self.Phase = "update"
	record(self, "update")
end
function T:OnLateUpdate(dt)
	assert(self.Phase == "update")
	assert(dt > 0 and dt == Time.GetDeltaTime())
	self.Phase = "late"
	record(self, "late")
end
function T:OnDisable()
	assert(self.Phase == "late")
	self.Phase = "disabled"
	record(self, "disable")
end
function T:OnDestroy()
	assert(self.Label ~= nil and not self.Destroyed)
	self.Destroyed = true
	record(self, "destroy")
end
function T:OnCollisionEnter(other, contact)
	assert(self.Label == "Solid" and self.Phase == "fixed")
	assert(other:IsValid() and other.Name == "Ground")
	assert(contact.Collider == self.Entity and contact.OtherCollider == other)
	assert(typeof(contact.Point) == "vector" and typeof(contact.Normal) == "vector")
	for _, n in {contact.Point.x, contact.Point.y, contact.Point.z,
		contact.Normal.x, contact.Normal.y, contact.Normal.z, contact.RelativeSpeed} do
		assert(type(n) == "number" and n == n and math.abs(n) < math.huge)
	end
	assert(math.abs(vector.magnitude(contact.Normal) - 1) < 0.001)
	self.Contact = other.ID
	record(self, "collision-enter")
end
function T:OnCollisionExit(other)
	assert(self.Label == "Solid" and self.Phase == "fixed")
	assert(other:IsValid() and other.Name == "Ground" and self.Contact == other.ID)
	self.Contact = nil
	record(self, "collision-exit")
end
function T:OnTriggerEnter(other)
	assert(self.Label == "TriggerActor" and self.Phase == "fixed")
	assert(other:IsValid() and other.Name == "Sensor")
	self.Contact = other.ID
	record(self, "trigger-enter")
end
function T:OnTriggerExit(other)
	assert(self.Label == "TriggerActor" and self.Phase == "fixed")
	assert(other:IsValid() and other.Name == "Sensor" and self.Contact == other.ID)
	self.Contact = nil
	record(self, "trigger-exit")
end
return Script.Define("BehaviourCoverage", T)
)";

		static std::vector<std::string> BehaviourTrace(Scene& scene, std::string_view label)
		{
			std::vector<std::string> result;
			const std::string prefix = std::string(label) + ":";
			for (const auto& entry : ScriptLifecycleTrace(scene))
				if (entry.starts_with(prefix))
					result.push_back(entry.substr(prefix.size()));
			return result;
		}

		// Records through Scene.CreateEntity, so it runs unchanged in a real PlaySession with its own registry.
		static constexpr std::string_view SessionLifecycleSource = R"(
local T = {}
local function record(self, phase)
	Scene.CreateEntity("Trace:" .. self.Label .. ":" .. phase)
end
function T:OnCreate()
	self.Label = self.Entity.Name
	self.Fixed = 0
	record(self, "create")
end
function T:OnStart() record(self, "start") end
function T:OnFixedUpdate(dt)
	self.Fixed += 1
	record(self, "fixed")
	if self.Label == "Spawner" and self.Fixed == 1 then
		local spawned = Scene.CreateEntity("Spawned")
		spawned:AddComponent("Script", {Scripts = {{Script = Assets.Load("Assets/Scripts/SessionLifecycle.luau"), Fields = {}, ExecutionOrder = 0}}})
	elseif self.Label == "Survivor" and self.Fixed == 2 then
		Scene.Destroy(Scene.FindByName("Doomed") :: Entity)
	end
end
function T:OnUpdate(dt) record(self, "update") end
function T:OnLateUpdate(dt) record(self, "late") end
function T:OnDestroy() record(self, "destroy") end
function T:OnTriggerEnter(other) record(self, "trigger-enter") end
function T:OnTriggerExit(other) record(self, "trigger-exit") end
return Script.Define("SessionLifecycle", T)
)";

		// A trigger probe: every contact must reach a started instance, and every exit must close a recorded enter.
		static constexpr std::string_view ContactProbeSource = R"(
local T = {}
local function record(self, phase)
	Scene.CreateEntity("Trace:" .. self.Label .. ":" .. phase)
end
function T:OnCreate()
	self.Label = self.Entity.Name
	self.Inside = {}
	self.Fixed = 0
	record(self, "create")
end
function T:OnStart()
	self.Started = true
	record(self, "start")
end
function T:OnFixedUpdate()
	self.Fixed += 1
	record(self, "fixed")
	if self.Label == "Spawner" and self.Fixed == 1 then
		local coin = Scene.CreateEntity("Coin")
		coin.Transform.Translation = vector.create(10, 0, 0)
		coin:AddComponent("BoxCollider", {HalfExtents = vector.create(1, 1, 1), IsTrigger = true})
		coin:AddComponent("RigidBody", {Type = "Static"})
		coin:AddComponent("Script", {Scripts = {{Script = Assets.Load("Assets/Scripts/ContactProbe.luau"), Fields = {}, ExecutionOrder = 0}}})
	end
end
function T:OnTriggerEnter(other)
	assert(self.Started, "a contact reached an instance before its OnStart")
	assert(not self.Inside[other.ID], "a second enter without an exit")
	self.Inside[other.ID] = true
	record(self, "trigger-enter")
end
function T:OnTriggerExit(other)
	assert(self.Inside[other.ID], "an exit without a matching enter")
	self.Inside[other.ID] = nil
	record(self, "trigger-exit")
end
return Script.Define("ContactProbe", T)
)";

		static Scope<PlaySession> StartScriptSession(ScriptTestFixture& fixture, bool testMode = false, const ScriptEnvironment& environment = {})
		{
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.Seed = 719;
			specification.Project.Physics.Gravity = glm::vec3(0.0f);
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.ScriptRunMode = RunModes::Editor;
			specification.TestMode = testMode;
			specification.Environment = environment;
			auto started = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(started, (started ? "" : started.error().ToString()));
			(*started)->SetExtractionEnabled(false);
			return std::move(*started);
		}

		static std::string DescribeScriptErrors(const ScriptErrorStream& errors)
		{
			std::string result;
			for (const ScriptError& error : errors.GetErrors())
				result += std::format("{}:{} {}\n", error.Script, error.Line, error.Message);
			return result;
		}

		static ptrdiff_t TracePosition(const std::vector<std::string>& trace, std::string_view entry)
		{
			const auto found = std::ranges::find(trace, entry);
			REQUIRE_MESSAGE(found != trace.end(), entry);
			return found - trace.begin();
		}

		static void RunBehaviourSessionCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			const AssetHandle handle(0x20000b);
			REQUIRE(fixture.AddScript(handle, "Assets/Scripts/BehaviourCoverage.luau", std::string(BehaviourCoverageSource)));
			Scene& editScene = fixture.GetScene();
			static_cast<void>(AddBoxBody(editScene, "Ground", glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(2.0f, 0.5f, 2.0f), BodyType::Static));
			const Entity solid = AddSphereBody(editScene, "Solid", glm::vec3(0.0f, 0.4f, 0.0f), 0.5f, BodyType::Dynamic);
			const Entity sensor = AddBoxBody(editScene, "Sensor", glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(1.0f), BodyType::Static);
			sensor.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			const Entity triggerActor = AddSphereBody(editScene, "TriggerActor", glm::vec3(10.0f, 0.0f, 0.0f), 0.5f, BodyType::Dynamic);
			const Entity lifecycle = editScene.CreateEntity("Lifecycle");
			for (const Entity entity : { solid, triggerActor })
				PatchRigidBody(entity, [](RigidBodyComponent& body)
				{
					body.AllowSleeping = false;
				});
			for (const Entity entity : { solid, triggerActor, lifecycle })
				AttachLifecycleScript(entity, handle);

			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.Seed = 719;
			specification.Project.Physics.Gravity = glm::vec3(0.0f);
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.ScriptRunMode = mode;
			specification.TestMode = true;
			auto started = PlaySession::CreateFromScene(specification, editScene);
			REQUIRE_MESSAGE(started, (started ? "" : started.error().ToString()));
			auto session = std::move(*started);
			session->SetExtractionEnabled(false);
			Scene& scene = session->GetScene();
			REQUIRE(session->GetScripts() != nullptr);
			for (const char* label : { "Solid", "TriggerActor", "Lifecycle" })
				CHECK(BehaviourTrace(scene, label) == std::vector<std::string>{ "create", "start" });

			// Jolt produces both pairs; no callback or counter is invoked directly by the test.
			session->Tick();
			REQUIRE(session->GetScriptErrors().GetErrors().empty());
			CHECK(BehaviourTrace(scene, "Solid") == std::vector<std::string>{ "create", "start", "fixed", "collision-enter", "update", "late" });
			CHECK(BehaviourTrace(scene, "TriggerActor") == std::vector<std::string>{ "create", "start", "fixed", "trigger-enter", "update", "late" });
			CHECK(BehaviourTrace(scene, "Lifecycle") == std::vector<std::string>{ "create", "start", "fixed", "update", "late" });
			REQUIRE(session->GetPhysics().Teleport(solid.GetUUID(), glm::vec3(100.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
			REQUIRE(session->GetPhysics().Teleport(triggerActor.GetUUID(), glm::vec3(110.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
			session->Tick();
			REQUIRE(session->GetScriptErrors().GetErrors().empty());
			CHECK(BehaviourTrace(scene, "Solid") == std::vector<std::string>{ "create", "start", "fixed", "collision-enter", "update", "late", "fixed", "collision-exit", "update", "late" });
			CHECK(BehaviourTrace(scene, "TriggerActor") == std::vector<std::string>{ "create", "start", "fixed", "trigger-enter", "update", "late", "fixed", "trigger-exit", "update", "late" });

			const Entity runtimeLifecycle = scene.FindEntityByID(lifecycle.GetUUID());
			REQUIRE(runtimeLifecycle);
			runtimeLifecycle.SetActive(false);
			session->Tick();
			const auto disabled = BehaviourTrace(scene, "Lifecycle");
			CHECK(disabled == std::vector<std::string>{ "create", "start", "fixed", "update", "late", "fixed", "update", "late", "disable", "destroy" });
			runtimeLifecycle.SetActive(true);
			session->Tick();
			CHECK(BehaviourTrace(scene, "Lifecycle") == std::vector<std::string>{ "create", "start", "fixed", "update", "late", "fixed", "update", "late", "disable", "destroy", "create", "enable", "start", "fixed", "update", "late" });

			session->GetScripts()->Stop();
			const auto stopped = ScriptLifecycleTrace(scene);
			session->GetScripts()->Stop();
			CHECK(ScriptLifecycleTrace(scene) == stopped);
			for (const char* label : { "Solid", "TriggerActor", "Lifecycle" })
			{
				const auto trace = BehaviourTrace(scene, label);
				REQUIRE_FALSE(trace.empty());
				CHECK(trace.back() == "destroy");
				CHECK(std::ranges::count(trace, "destroy") == (std::string_view(label) == "Lifecycle" ? 2 : 1));
			}
			CHECK(session->GetScriptErrors().GetErrors().empty());
			CHECK(session->GetPhysics().GetDiagnostics().empty());
			const auto coverage = session->GetScripts()->GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

		static void RunBehaviourReloadCoverage(std::vector<ScriptApiCoverage>& snapshots)
		{
			ScriptTestFixture fixture({ .Mode = RunModes::Editor, .TestMode = true });
			const AssetHandle handle(0x20000c);
			REQUIRE(fixture.AddScript(handle, "Assets/Scripts/ReloadCoverage.luau", R"(
local T = {Fields = {Count = Field.Integer(3)}}
function T:OnCreate() self.Created = (self.Created or 0) + 1 end
function T:Value() return 10 end
return Script.Define("ReloadCoverage", T)
)"));
			const Entity entity = fixture.GetScene().CreateEntity("Reload");
			AttachLifecycleScript(entity, handle);
			REQUIRE(fixture.Start());
			REQUIRE(fixture.Evaluate("self.Count = 41; self.Class = getmetatable(self); self.Saved = self.Value; return nil", entity.GetUUID()));
			REQUIRE(fixture.AddScript(handle, "Assets/Scripts/ReloadCoverage.luau", R"(
local T = {Fields = {Count = Field.Integer(99)}}
function T:OnCreate() error("reload must preserve the instance") end
function T:Value() return 20 end
function T:OnHotReload()
	assert(self.Created == 1 and self.Count == 41 and self.Class == getmetatable(self))
	assert(self:Value() == 20 and self:Saved() == 10)
	self.Reloaded = (self.Reloaded or 0) + 1
	Scene.CreateEntity("Trace:Reload:reload")
end
function T:OnDestroy()
	assert(self.Reloaded == 1 and self.Count == 41)
	Scene.CreateEntity("Trace:Reload:destroy")
end
return Script.Define("ReloadCoverage", T)
)"));
			const auto reloaded = fixture.GetEngine()->Reload(handle);
			REQUIRE_MESSAGE(reloaded, (reloaded ? "" : reloaded.error().ToString()));
			CHECK_FALSE(reloaded->Deferred);
			CHECK(reloaded->Scripts == std::vector<AssetHandle>{ handle });
			const auto result = fixture.Evaluate("return {self.Created, self.Count, self.Reloaded, self:Value(), self:Saved()}", entity.GetUUID());
			REQUIRE_MESSAGE(result, (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json::array({ 1, 41, 1, 20, 10 }));
			CHECK(BehaviourTrace(fixture.GetScene(), "Reload") == std::vector<std::string>{ "reload" });
			fixture.GetEngine()->Stop();
			fixture.GetEngine()->Stop();
			CHECK(BehaviourTrace(fixture.GetScene(), "Reload") == std::vector<std::string>{ "reload", "destroy" });
			CHECK(fixture.Errors.empty());
			const auto coverage = fixture.GetApi().GetCoverage(RunModes::Editor);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

		void RunBehaviourCallbacksCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			const size_t firstSnapshot = snapshots.size();
			RunBehaviourSessionCoverage(mode, snapshots);
			if (mode == RunModes::Editor)
				RunBehaviourReloadCoverage(snapshots);
			REQUIRE(snapshots.size() > firstSnapshot);
			for (const auto& member : snapshots[firstSnapshot].Members)
			{
				if (member.Owner != "Behaviour" || !HasFlag(member.Modes, mode))
					continue;
				CAPTURE(member.Member);
				uint64_t calls = 0;
				for (size_t index = firstSnapshot; index < snapshots.size(); ++index)
					for (const auto& counter : snapshots[index].Members)
						if (counter.Owner == member.Owner && counter.Member == member.Member)
							calls += counter.Calls;
				CHECK(calls > 0);
			}
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("Behaviour: real lifecycle physics and reload callbacks cover every allowed mode after destruction")
		{
			for (const auto mode : { RunModes::Editor, RunModes::Release, RunModes::Dist })
			{
				CAPTURE(static_cast<uint8_t>(mode));
				std::vector<ScriptApiCoverage> snapshots;
				Test::RunBehaviourCallbacksCoverage(mode, snapshots);
			}
		}

		TEST_CASE("Callbacks: order and phases match §5.7")
		{
			// Driven only through PlaySession::Tick, so the session's phase order and flushes are what is observed.
			Test::ScriptTestFixture fixture({ .TestMode = true });
			const AssetHandle script(0x200001);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/SessionLifecycle.luau", std::string(Test::SessionLifecycleSource)));
			Scene& editScene = fixture.GetScene();
			const Entity root = editScene.CreateEntityWithID(UUID(90), "Root");
			const Entity child = editScene.CreateEntityWithID(UUID(20), "Child", root);
			const Entity early = editScene.CreateEntityWithID(UUID(70), "Early");
			const Entity spawner = editScene.CreateEntity("Spawner");
			const Entity doomed = Test::AddBoxBody(editScene, "Doomed", glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(1.0f), BodyType::Static);
			doomed.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			const Entity survivor = Test::AddSphereBody(editScene, "Survivor", glm::vec3(10.0f, 0.0f, 0.0f), 0.5f, BodyType::Dynamic);
			Test::PatchRigidBody(survivor, [](RigidBodyComponent& body)
			{
				body.AllowSleeping = false;
			});
			for (const Entity entity : { root, child, spawner, doomed, survivor })
				Test::AttachLifecycleScript(entity, script);
			Test::AttachLifecycleScript(early, script, -5);
			auto session = Test::StartScriptSession(fixture, true);
			Scene& scene = session->GetScene();
			// Every instance is created before any starts, both in (ExecutionOrder, canonical) order.
			CHECK(Test::ScriptLifecycleTrace(scene) == std::vector<std::string>{ "Early:create", "Root:create", "Child:create", "Spawner:create", "Doomed:create", "Survivor:create", "Early:start", "Root:start", "Child:start", "Spawner:start", "Doomed:start", "Survivor:start" });
			session->Tick();
			session->Tick();
			REQUIRE_MESSAGE(session->GetScriptErrors().GetErrors().empty(), Test::DescribeScriptErrors(session->GetScriptErrors()));
			for (const char* label : { "Early", "Root", "Child", "Spawner" })
			{
				CAPTURE(label);
				CHECK(Test::BehaviourTrace(scene, label) == std::vector<std::string>{ "create", "start", "fixed", "update", "late", "fixed", "update", "late" });
			}
			// Created during tick 1's fixed phase: not visited by it, started by the next start flush before its first update.
			CHECK(Test::BehaviourTrace(scene, "Spawned") == std::vector<std::string>{ "create", "start", "update", "late", "fixed", "update", "late" });
			// Destroyed during tick 2's fixed phase: OnDestroy in the destroy flush and no exit of its own.
			CHECK(Test::BehaviourTrace(scene, "Doomed") == std::vector<std::string>{ "create", "start", "fixed", "trigger-enter", "update", "late", "fixed", "destroy" });
			CHECK(Test::BehaviourTrace(scene, "Survivor") == std::vector<std::string>{ "create", "start", "fixed", "trigger-enter", "update", "late", "fixed", "trigger-exit", "update", "late" });
			const auto trace = Test::ScriptLifecycleTrace(scene);
			const auto at = [&trace](std::string_view entry)
			{
				return Test::TracePosition(trace, entry);
			};
			// The fixed phase follows its snapshot; creation inside it is immediate (OnCreate before the call returns).
			CHECK(at("Early:fixed") < at("Root:fixed"));
			CHECK(at("Root:fixed") < at("Child:fixed"));
			CHECK(at("Child:fixed") < at("Spawner:fixed"));
			CHECK(at("Spawner:fixed") < at("Spawned:create"));
			CHECK(at("Spawned:create") < at("Doomed:fixed"));
			// The fixed step that created it does not start it (its physics already ran); the frame's start flush does,
			// before the Update phase.
			CHECK(at("Survivor:trigger-enter") < at("Spawned:start"));
			CHECK(at("Doomed:trigger-enter") < at("Spawned:start"));
			CHECK(at("Spawned:start") < at("Early:update"));
			// §5.7 step 8: OnDestroy runs before the synthesized exit to the surviving contact partner.
			CHECK(at("Doomed:destroy") < at("Survivor:trigger-exit"));
			// Stop tears down every remaining instance in reverse canonical order: children before parents.
			session->GetScripts()->Stop();
			const auto stopped = Test::ScriptLifecycleTrace(scene);
			CHECK(std::vector<std::string>(stopped.begin() + static_cast<ptrdiff_t>(trace.size()), stopped.end())
				== std::vector<std::string>{ "Spawned:destroy", "Survivor:destroy", "Spawner:destroy", "Early:destroy", "Child:destroy", "Root:destroy" });
			CHECK(session->GetScriptErrors().GetErrors().empty());
		}

		TEST_CASE("ScriptEngine: deactivation tears down children first and reactivation recreates instances")
		{
			Test::ScriptTestFixture fixture(Test::LifecycleSpecification());
			const AssetHandle script(0x200002);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Lifecycle.luau", std::string(Test::LifecycleSource)));
			const Entity root = fixture.GetScene().CreateEntity("Root");
			const Entity child = fixture.GetScene().CreateEntity("Child", root);
			Test::AttachLifecycleScript(root, script);
			Test::AttachLifecycleScript(child, script);
			REQUIRE(fixture.Start());
			root.SetActive(false);
			REQUIRE(fixture.GetEngine()->SynchronizeInstances());
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 2);
			fixture.GetEngine()->FinishDestroyFlush();
			root.SetActive(true);
			REQUIRE(fixture.GetEngine()->SynchronizeInstances());
			fixture.GetEngine()->StartPending();
			CHECK(Test::ScriptLifecycleTrace(fixture.GetScene()) == std::vector<std::string>{ "Root:create", "Child:create", "Root:start", "Child:start", "Root:disable", "Child:disable", "Child:destroy", "Root:destroy", "Root:create", "Child:create", "Root:enable", "Child:enable", "Root:start", "Child:start" });
		}

		TEST_CASE("ScriptInstanceLifecycle: the destruction flush observes newly disabled descendants without an earlier phase")
		{
			Test::ScriptTestFixture fixture(Test::LifecycleSpecification());
			const AssetHandle script(0x200006);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Lifecycle.luau", std::string(Test::LifecycleSource)));
			const Entity root = fixture.GetScene().CreateEntity("Root");
			const Entity child = fixture.GetScene().CreateEntity("Child", root);
			Test::AttachLifecycleScript(root, script);
			Test::AttachLifecycleScript(child, script);
			REQUIRE(fixture.Start());
			root.SetActive(false);
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 2);
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 0);
			fixture.GetEngine()->FinishDestroyFlush();
			CHECK(fixture.GetEngine()->GetInstances().empty());
			CHECK(Test::ScriptLifecycleTrace(fixture.GetScene()) == std::vector<std::string>{ "Root:create", "Child:create", "Root:start", "Child:start", "Root:disable", "Child:disable", "Child:destroy", "Root:destroy" });
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("ScriptInstanceLifecycle: disable then enable before flushing still replaces the old instance")
		{
			Test::ScriptTestFixture fixture(Test::LifecycleSpecification());
			const AssetHandle script(0x200007);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Lifecycle.luau", std::string(Test::LifecycleSource)));
			const Entity entity = fixture.GetScene().CreateEntity("Owner");
			Test::AttachLifecycleScript(entity, script);
			REQUIRE(fixture.Start());
			REQUIRE(fixture.Evaluate("self.Marker = 91; self.Entity:SetActive(false); self.Entity:SetActive(true)", entity.GetUUID()));
			fixture.GetEngine()->FixedUpdate();
			CHECK(Test::ScriptLifecycleTrace(fixture.GetScene()) == std::vector<std::string>{ "Owner:create", "Owner:start", "Owner:disable" });
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 1);
			fixture.GetEngine()->FinishDestroyFlush();
			fixture.GetEngine()->StartPending();
			const auto value = fixture.Evaluate("return self.Marker == nil", entity.GetUUID());
			REQUIRE(value);
			CHECK(value->Value.Get() == Json(true));
			CHECK(Test::ScriptLifecycleTrace(fixture.GetScene()) == std::vector<std::string>{ "Owner:create", "Owner:start", "Owner:disable", "Owner:destroy", "Owner:create", "Owner:enable", "Owner:start" });
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("ScriptInstanceLifecycle: re-enabling a subtree in OnDisable still tears down every observed instance")
		{
			Test::ScriptTestFixture fixture(Test::LifecycleSpecification());
			const AssetHandle script(0x20000a);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Reenable.luau", R"(
local T = {}
function T:OnCreate() self.Label = self.Entity.Name end
function T:OnDisable()
	Probe.Record(self.Label .. ":disable")
	if self.Label == "Root" then self.Entity:SetActive(true) end
end
function T:OnDestroy() Probe.Record(self.Label .. ":destroy") end
return Script.Define("Reenable", T)
)"));
			const Entity root = fixture.GetScene().CreateEntity("Root");
			const Entity child = fixture.GetScene().CreateEntity("Child", root);
			Test::AttachLifecycleScript(root, script);
			Test::AttachLifecycleScript(child, script);
			REQUIRE(fixture.Start());
			root.SetActive(false);
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 2);
			CHECK(root.IsActive());
			CHECK(child.IsActive());
			CHECK(Test::ScriptLifecycleTrace(fixture.GetScene()) == std::vector<std::string>{ "Root:disable", "Child:disable", "Child:destroy", "Root:destroy" });
			fixture.GetEngine()->FinishDestroyFlush();
			CHECK(fixture.GetEngine()->GetInstances().empty());
			fixture.GetEngine()->StartPending();
			CHECK(fixture.GetEngine()->GetInstances().size() == 2);
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("ScriptInstanceLifecycle: active state toggles cannot revive a fault-disabled behaviour")
		{
			Test::ExpectLog expected(LogLevel::Error, "still disabled");
			Test::ScriptTestFixture fixture;
			const AssetHandle script(0x200008);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Fault.luau", R"(
local T = {}
function T:OnFixedUpdate() error("still disabled") end
return Script.Define("Fault", T)
)"));
			const Entity entity = fixture.GetScene().CreateEntity("Owner");
			Test::AttachLifecycleScript(entity, script);
			REQUIRE(fixture.Start());
			fixture.GetEngine()->FixedUpdate();
			REQUIRE(fixture.Errors.size() == 1);
			entity.SetActive(false);
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 1);
			fixture.GetEngine()->FinishDestroyFlush();
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 0);
			entity.SetActive(true);
			fixture.GetEngine()->StartPending();
			fixture.GetEngine()->FixedUpdate();
			CHECK(fixture.Errors.size() == 1);
			const auto instances = fixture.GetEngine()->GetInstances();
			REQUIRE(instances.size() == 1);
			CHECK(instances[0].Disabled);
		}

		TEST_CASE("ScriptInstanceLifecycle: stopping includes pending-destroy instances in child-first order")
		{
			Test::ScriptTestFixture fixture(Test::LifecycleSpecification());
			const AssetHandle script(0x200009);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Lifecycle.luau", std::string(Test::LifecycleSource)));
			const Entity root = fixture.GetScene().CreateEntity("Root");
			const Entity child = fixture.GetScene().CreateEntity("Child", root);
			Test::AttachLifecycleScript(root, script);
			Test::AttachLifecycleScript(child, script);
			REQUIRE(fixture.Start());
			fixture.GetScene().DestroyEntity(root);
			fixture.GetEngine()->Stop();
			fixture.GetEngine()->Stop();
			CHECK(Test::ScriptLifecycleTrace(fixture.GetScene()) == std::vector<std::string>{ "Root:create", "Child:create", "Root:start", "Child:start", "Child:destroy", "Root:destroy" });
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("Errors: location, traceback and instance disabling")
		{
			Test::ExpectLog expected(LogLevel::Error, "callback failure");
			Test::ExpectLog expectedTask(LogLevel::Error, "task failure");
			// Ordinary editor play (test runs never pause, see the PauseOnError case below).
			auto specification = Test::LifecycleSpecification();
			specification.TestMode = false;
			Test::ScriptTestFixture fixture(specification);
			const AssetHandle badScript(0x200003);
			const AssetHandle goodScript(0x200004);
			const AssetHandle taskScript(0x20000f);
			REQUIRE(fixture.AddScript(badScript, "Assets/Scripts/Fail.luau", R"(local T = {}
local function broken() error("callback failure") end
function T:OnFixedUpdate() broken() end
return Script.Define("Fail", T))"));
			REQUIRE(fixture.AddScript(goodScript, "Assets/Scripts/Lifecycle.luau", std::string(Test::LifecycleSource)));
			// Resumed threads: a task faults after a wait, while a sibling task of the same instance keeps waiting.
			REQUIRE(fixture.AddScript(taskScript, "Assets/Scripts/TaskFail.luau", R"(local T = {}
local function fail() error("task failure") end
function T:OnStart()
	Task.Spawn(function()
		while true do
			Task.WaitTicks(1)
			Probe.Record("TaskBad:sibling")
		end
	end)
	Task.Spawn(function()
		Task.WaitTicks(1)
		fail()
	end)
end
function T:OnFixedUpdate() Probe.Record("TaskBad:fixed") end
return Script.Define("TaskFail", T))"));
			const Entity bad = fixture.GetScene().CreateEntity("Bad");
			const Entity good = fixture.GetScene().CreateEntity("Good");
			const Entity taskBad = fixture.GetScene().CreateEntity("TaskBad");
			Test::AttachLifecycleScript(bad, badScript);
			Test::AttachLifecycleScript(good, goodScript);
			Test::AttachLifecycleScript(taskBad, taskScript);
			REQUIRE(fixture.Start());
			REQUIRE(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 2);
			fixture.Frame.Tick = 17;
			fixture.GetEngine()->FixedUpdate();
			fixture.GetEngine()->ResumeTasks();
			fixture.Frame.Tick = 18;
			fixture.GetEngine()->FixedUpdate();
			fixture.GetEngine()->ResumeTasks();
			REQUIRE(fixture.Errors.size() == 2);
			const ScriptError& error = fixture.Errors.front();
			CHECK(error.Entity == bad.GetUUID());
			CHECK(error.EntityName == "Bad");
			CHECK(error.Callback == "OnFixedUpdate");
			CHECK(error.Script == "Assets/Scripts/Fail.luau");
			CHECK(error.Line == 2);
			CHECK(error.Tick == 17);
			CHECK(error.Message.find("callback failure") != std::string::npos);
			CHECK_FALSE(error.Traceback.empty());
			const ScriptError& taskError = fixture.Errors.back();
			CHECK(taskError.Entity == taskBad.GetUUID());
			CHECK(taskError.EntityName == "TaskBad");
			CHECK(taskError.Callback == "Task");
			CHECK(taskError.Script == "Assets/Scripts/TaskFail.luau");
			CHECK(taskError.Line == 2);
			CHECK(taskError.Tick == 17);
			CHECK(taskError.Message.find("task failure") != std::string::npos);
			REQUIRE(taskError.Traceback.size() >= 2);
			CHECK(taskError.Traceback[0].Script == "Assets/Scripts/TaskFail.luau");
			CHECK(taskError.Traceback[0].Line == 2);
			CHECK(taskError.Traceback[1].Line == 12);
			CHECK(fixture.PauseRequested);
			CHECK_FALSE(fixture.FatalError);
			// Both faulting instances are disabled; the task fault also cancelled its owner's waiting sibling task.
			const auto instances = fixture.GetEngine()->GetInstances();
			REQUIRE(instances.size() == 3);
			CHECK(instances[0].Disabled);
			CHECK_FALSE(instances[1].Disabled);
			CHECK(instances[2].Disabled);
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			CHECK(Test::ScriptLifecycleTrace(fixture.GetScene()) == std::vector<std::string>{ "Good:create", "Good:start", "Good:fixed", "TaskBad:fixed", "TaskBad:sibling", "Good:fixed" });
		}

		TEST_CASE("ScriptEngine: test runs record script errors without pausing the session")
		{
			Test::ExpectLog expected(LogLevel::Error, "suite fault");
			for (const bool testMode : { false, true })
			{
				CAPTURE(testMode);
				Test::ScriptTestFixture fixture({ .TestMode = testMode });
				const AssetHandle handle(0x200010);
				REQUIRE(fixture.AddScript(handle, "Assets/Scripts/SuiteFault.luau", R"(
local T = {}
function T:OnFixedUpdate() error("suite fault") end
return Script.Define("SuiteFault", T)
)"));
				Test::AttachLifecycleScript(fixture.GetScene().CreateEntity("Faulty"), handle);
				REQUIRE(fixture.Start());
				REQUIRE(fixture.GetEngine()->GetSettings().PauseOnError);
				REQUIRE(fixture.Environment.IsEditor);
				fixture.GetEngine()->FixedUpdate();
				REQUIRE(fixture.Errors.size() == 1);
				// The runner owns a test run's time: Editor, Release and Dist runs must observe the same audio and alpha.
				CHECK(fixture.PauseRequested == !testMode);
			}
			// The same holds for a real editor-environment test session, whose pause would silence non-lockstep audio.
			Test::ScriptTestFixture fixture({ .TestMode = true });
			const AssetHandle handle(0x200011);
			REQUIRE(fixture.AddScript(handle, "Assets/Scripts/SuiteFault.luau", R"(
local T = {}
function T:OnFixedUpdate() error("suite fault") end
return Script.Define("SuiteFault", T)
)"));
			Test::AttachLifecycleScript(fixture.GetScene().CreateEntity("Faulty"), handle);
			auto session = Test::StartScriptSession(fixture, true, ScriptEnvironment{ .IsEditor = true });
			session->Tick();
			REQUIRE(session->GetScriptErrors().GetErrors().size() == 1);
			CHECK_FALSE(session->IsPaused());
		}

		TEST_CASE("ScriptEngine: a failing evaluation never disables the addressed behaviour or cancels its tasks")
		{
			Test::ExpectLog compileFailure(LogLevel::Error, "compile:");
			Test::ExpectLog runtimeFailure(LogLevel::Error, "attempt to index nil");
			Test::ScriptTestFixture fixture;
			const AssetHandle handle(0x200012);
			REQUIRE(fixture.AddScript(handle, "Assets/Scripts/Ball.luau", R"(
local T = { Fields = { Speed = Field.Number(3) } }
function T:OnStart()
	self.Fixed = 0
	self.Waits = 0
	Task.Spawn(function()
		while true do
			Task.WaitTicks(1)
			self.Waits += 1
		end
	end)
end
function T:OnFixedUpdate() self.Fixed += 1 end
return Script.Define("Ball", T)
)"));
			const Entity ball = fixture.GetScene().CreateEntity("Ball");
			Test::AttachLifecycleScript(ball, handle);
			REQUIRE(fixture.Start());
			const auto typo = fixture.Evaluate("retrun self.Speed", ball.GetUUID());
			REQUIRE_FALSE(typo);
			const auto missing = fixture.Evaluate("return self.Missing.x", ball.GetUUID());
			REQUIRE_FALSE(missing);
			REQUIRE(fixture.Errors.size() == 2);
			// Reported to the caller and the error stream, never against the behaviour's file or as its fault.
			CHECK(fixture.Errors[0].Kind == ScriptErrorKind::Compile);
			CHECK(fixture.Errors[0].Script != "Assets/Scripts/Ball.luau");
			CHECK(fixture.Errors[0].Entity == ball.GetUUID());
			CHECK(fixture.Errors[1].Message.find("attempt to index nil") != std::string::npos);
			CHECK_FALSE(fixture.PauseRequested);
			const auto instances = fixture.GetEngine()->GetInstances();
			REQUIRE(instances.size() == 1);
			CHECK_FALSE(instances[0].Disabled);
			REQUIRE(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 1);
			for (uint64_t tick = 1; tick <= 3; ++tick)
			{
				fixture.Frame.Tick = tick;
				fixture.GetEngine()->FixedUpdate();
				fixture.GetEngine()->ResumeTasks();
			}
			const auto state = fixture.Evaluate("return {self.Fixed, self.Waits}", ball.GetUUID());
			REQUIRE(state);
			CHECK(state->Value.Get() == Json::array({ 3, 3 }));
			CHECK(fixture.Errors.size() == 2);
		}

		TEST_CASE("ScriptInstanceLifecycle: a module top that removes an instance never leaves a callback on it")
		{
			Test::ScriptTestFixture fixture(Test::LifecycleSpecification());
			const AssetHandle bee(0x200013), ay(0x200014), selfRemoving(0x200015);
			REQUIRE(fixture.AddScript(bee, "Assets/Scripts/Bee.luau", R"(
local T = {}
function T:OnCreate() Probe.Record("B:create") end
function T:OnStart() Probe.Record("B:start") end
function T:OnDisable() Probe.Record("B:disable") end
function T:OnDestroy() Probe.Record("B:destroy") end
return Script.Define("Bee", T)
)"));
			// The load-time VM rejects Scene inside the pcall, so import succeeds; the runtime top then removes B.
			REQUIRE(fixture.AddScript(ay, "Assets/Scripts/Ay.luau", R"(
pcall(function()
	local b = Scene.FindByName("B") :: Entity
	b:RemoveComponent("Script")
	b:SetActive(not b:IsActiveSelf())
end)
local T = {}
function T:OnCreate() Probe.Record("A:create") end
function T:OnStart() Probe.Record("A:start") end
return Script.Define("Ay", T)
)"));
			REQUIRE(fixture.AddScript(selfRemoving, "Assets/Scripts/SelfRemoving.luau", R"(
pcall(function()
	local owner = Scene.FindByName("Self") :: Entity
	owner:RemoveComponent("Script")
	owner:SetActive(false)
end)
local T = {}
function T:OnCreate() Probe.Record("Self:create") end
function T:OnDestroy() Probe.Record("Self:destroy") end
return Script.Define("SelfRemoving", T)
)"));
			// Canonical order B, A, Self: B's table exists (its OnCreate is pending) when A's module top removes it.
			const Entity b = fixture.GetScene().CreateEntity("B");
			const Entity a = fixture.GetScene().CreateEntity("A");
			const Entity removed = fixture.GetScene().CreateEntity("Self");
			Test::AttachLifecycleScript(b, bee);
			Test::AttachLifecycleScript(a, ay);
			Test::AttachLifecycleScript(removed, selfRemoving);
			REQUIRE(fixture.Start());
			CHECK(Test::ScriptLifecycleTrace(fixture.GetScene()) == std::vector<std::string>{ "A:create", "A:start" });
			CHECK(fixture.Errors.empty());
			const auto instances = fixture.GetEngine()->GetInstances();
			REQUIRE(instances.size() == 1);
			CHECK(instances[0].Entity == a.GetUUID());
			CHECK_FALSE(b.HasComponent<ScriptComponent>());
			CHECK_FALSE(removed.HasComponent<ScriptComponent>());
		}

		TEST_CASE("ScriptInstanceLifecycle: an instance destroyed before the next lifecycle refresh still receives OnDestroy")
		{
			Test::ScriptTestFixture fixture(Test::LifecycleSpecification());
			const AssetHandle handle(0x200016);
			REQUIRE(fixture.AddScript(handle, "Assets/Scripts/Ephemeral.luau", R"(
local T = {}
function T:OnCreate()
	self.Label = self.Entity.Name
	Probe.Record(self.Label .. ":create")
	if self.Label == "Ephemeral" then Scene.Destroy(self.Entity) end
end
function T:OnFixedUpdate()
	if self.Label == "Spawner" and not self.Spawned then
		self.Spawned = true
		local spawned = Scene.CreateEntity("Ephemeral")
		-- The add succeeds; the new instance's OnCreate destroys its entity before the call returns.
		pcall(function()
			spawned:AddComponent("Script", {Scripts = {{Script = Assets.Load("Assets/Scripts/Ephemeral.luau"), Fields = {}, ExecutionOrder = 0}}})
		end)
	end
end
function T:OnDestroy() Probe.Record(self.Label .. ":destroy") end
return Script.Define("Ephemeral", T)
)"));
			Test::AttachLifecycleScript(fixture.GetScene().CreateEntity("Spawner"), handle);
			REQUIRE(fixture.Start());
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->FixedUpdate();
			// Created and destroyed inside one phase, between two refreshes of the lifecycle order.
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 1);
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 0);
			fixture.GetEngine()->FinishDestroyFlush();
			fixture.GetScene().FlushPendingDestroys();
			CHECK(Test::ScriptLifecycleTrace(fixture.GetScene()) == std::vector<std::string>{ "Spawner:create", "Ephemeral:create", "Ephemeral:destroy" });
			fixture.Stop();
			CHECK(Test::ScriptLifecycleTrace(fixture.GetScene()) == std::vector<std::string>{ "Spawner:create", "Ephemeral:create", "Ephemeral:destroy", "Spawner:destroy" });
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("ScriptEngine: contacts that begin before OnStart reach the script after it, in enter and exit pairs")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle handle(0x200017);
			REQUIRE(fixture.AddScript(handle, "Assets/Scripts/ContactProbe.luau", std::string(Test::ContactProbeSource)));
			Scene& editScene = fixture.GetScene();
			const Entity player = Test::AddSphereBody(editScene, "Player", glm::vec3(10.0f, 0.0f, 0.0f), 0.5f, BodyType::Dynamic);
			Test::PatchRigidBody(player, [](RigidBodyComponent& body)
			{
				body.AllowSleeping = false;
			});
			Test::AttachLifecycleScript(player, handle);
			Test::AttachLifecycleScript(editScene.CreateEntity("Spawner"), handle);
			auto session = Test::StartScriptSession(fixture);
			Scene& scene = session->GetScene();
			// Tick 1: Spawner creates the scripted trigger Coin in OnFixedUpdate; its new body overlaps Player at PostStep,
			// before Coin's OnStart. Tick 2: the held enter reaches Coin after OnStart, before its first OnFixedUpdate.
			session->Tick();
			REQUIRE(scene.FindEntityByPath("/Coin"));
			session->Tick();
			REQUIRE(session->GetPhysics().Teleport(player.GetUUID(), glm::vec3(100.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
			session->Tick();
			REQUIRE_MESSAGE(session->GetScriptErrors().GetErrors().empty(), Test::DescribeScriptErrors(session->GetScriptErrors()));
			CHECK(Test::BehaviourTrace(scene, "Coin") == std::vector<std::string>{ "create", "start", "trigger-enter", "fixed", "fixed", "trigger-exit" });
			CHECK(Test::BehaviourTrace(scene, "Player") == std::vector<std::string>{ "create", "start", "fixed", "trigger-enter", "fixed", "fixed", "trigger-exit" });
			CHECK(session->GetPhysics().GetDiagnostics().empty());
		}

		TEST_CASE("ScriptEngine: read-only evaluation resolves fields without running lifecycle or mutating the host")
		{
			auto specification = Test::LifecycleSpecification();
			specification.ReadOnly = true;
			Test::ScriptTestFixture fixture(specification);
			const AssetHandle script(0x200005);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/ReadOnly.luau", R"(local T = {Fields = {Speed = Field.Number(7)}}
function T:OnCreate() error("OnCreate must not run") end
function T:OnStart() error("OnStart must not run") end
return Script.Define("ReadOnly", T))"));
			const Entity entity = fixture.GetScene().CreateEntity("ReadOnly");
			Test::AttachLifecycleScript(entity, script);
			REQUIRE(fixture.Start());
			const auto result = fixture.Evaluate("return self.Speed", entity.GetUUID());
			REQUIRE(result);
			CHECK(result->Value.Get() == Json(7));
			CHECK(fixture.Errors.empty());
			Test::ExpectLog expected(LogLevel::Error, "script engine is read-only");
			const auto refused = fixture.Evaluate("self.Entity.Name = 'changed'", entity.GetUUID());
			CHECK_FALSE(refused);
			CHECK(expected.GetMatchCount() == 1);
			CHECK(entity.GetName() == "ReadOnly");
			CHECK(fixture.ExternalMutations.empty());
		}
	}

}
