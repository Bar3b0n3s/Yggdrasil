#include "TestsPCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"
#include "Support/PhysicsTestScene.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <algorithm>
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
			ScriptComponent script{};
			script.Script.SetHandle(handle);
			script.ExecutionOrder = order;
			entity.AddComponent<ScriptComponent>(script);
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
			Test::ScriptTestFixture fixture(Test::LifecycleSpecification());
			const AssetHandle script(0x200001);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Lifecycle.luau", std::string(Test::LifecycleSource)));
			Scene& scene = fixture.GetScene();
			const Entity root = scene.CreateEntityWithID(UUID(90), "Root");
			const Entity child = scene.CreateEntityWithID(UUID(20), "Child", root);
			const Entity early = scene.CreateEntityWithID(UUID(70), "Early");
			Test::AttachLifecycleScript(root, script);
			Test::AttachLifecycleScript(child, script);
			Test::AttachLifecycleScript(early, script, -5);
			REQUIRE(fixture.Start());
			CHECK(Test::ScriptLifecycleTrace(scene) == std::vector<std::string>{ "Early:create", "Root:create", "Child:create", "Early:start", "Root:start", "Child:start" });
			fixture.Frame.DeltaTime = 0.02;
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->FixedUpdate();
			fixture.GetEngine()->Update();
			fixture.GetEngine()->LateUpdate();
			const auto updated = Test::ScriptLifecycleTrace(scene);
			REQUIRE(updated.size() == 15);
			CHECK(std::vector<std::string>(updated.begin() + 6, updated.end()) == std::vector<std::string>{ "Early:fixed", "Root:fixed", "Child:fixed", "Early:update", "Root:update", "Child:update", "Early:late", "Root:late", "Child:late" });
			scene.DestroyEntity(root);
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 2);
			CHECK(fixture.GetEngine()->PrepareDestroyFlush() == 0);
			const auto destroyed = Test::ScriptLifecycleTrace(scene);
			REQUIRE(destroyed.size() == 17);
			CHECK(destroyed[15] == "Child:destroy");
			CHECK(destroyed[16] == "Root:destroy");
			fixture.GetEngine()->FinishDestroyFlush();
			scene.FlushPendingDestroys();
			fixture.Stop();
			CHECK(Test::ScriptLifecycleTrace(scene).back() == "Early:destroy");
			CHECK(fixture.Errors.empty());
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
			Test::ScriptTestFixture fixture(Test::LifecycleSpecification());
			const AssetHandle badScript(0x200003);
			const AssetHandle goodScript(0x200004);
			REQUIRE(fixture.AddScript(badScript, "Assets/Scripts/Fail.luau", R"(local T = {}
local function broken() error("callback failure") end
function T:OnFixedUpdate() broken() end
return Script.Define("Fail", T))"));
			REQUIRE(fixture.AddScript(goodScript, "Assets/Scripts/Lifecycle.luau", std::string(Test::LifecycleSource)));
			const Entity bad = fixture.GetScene().CreateEntity("Bad");
			const Entity good = fixture.GetScene().CreateEntity("Good");
			Test::AttachLifecycleScript(bad, badScript);
			Test::AttachLifecycleScript(good, goodScript);
			REQUIRE(fixture.Start());
			fixture.Frame.Tick = 17;
			fixture.GetEngine()->FixedUpdate();
			fixture.GetEngine()->FixedUpdate();
			REQUIRE(fixture.Errors.size() == 1);
			const ScriptError& error = fixture.Errors.front();
			CHECK(error.Entity == bad.GetUUID());
			CHECK(error.EntityName == "Bad");
			CHECK(error.Callback == "OnFixedUpdate");
			CHECK(error.Script == "Assets/Scripts/Fail.luau");
			CHECK(error.Line == 2);
			CHECK(error.Tick == 17);
			CHECK(error.Message.find("callback failure") != std::string::npos);
			CHECK_FALSE(error.Traceback.empty());
			CHECK(fixture.PauseRequested);
			CHECK_FALSE(fixture.FatalError);
			const auto instances = fixture.GetEngine()->GetInstances();
			REQUIRE(instances.size() == 2);
			CHECK(instances[0].Disabled);
			CHECK_FALSE(instances[1].Disabled);
			CHECK(Test::ScriptLifecycleTrace(fixture.GetScene()) == std::vector<std::string>{ "Good:create", "Good:start", "Good:fixed", "Good:fixed" });
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
