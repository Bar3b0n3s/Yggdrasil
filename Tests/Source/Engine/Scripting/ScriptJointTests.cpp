#include "TestsPCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Scene/Components/FixedJointComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Session/PlaySession.h"
#include "Support/PhysicsTestScene.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <format>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// OnJointBreak (M37, Docs/Decisions/0054-m37-joints.md decision 8; Architecture §9.10, §11.3) in a real PlaySession: a
// broken joint's callback reaches every behaviour of the joint's entity, after the step's collision callbacks, with the
// joint already written Broken and the force and torque that broke it; an instance that has not started holds it until after
// its OnStart, in arrival order with its other held events, and one destroyed before its start drops it.

namespace Engine {

	namespace {

		// Records each callback as a "Trace:<entity>:<what>@<tick>" entity, in the order the callbacks run.
		constexpr std::string_view JointTraceSource = R"(
local T = {}
local function record(self, what)
	Scene.CreateEntity("Trace:" .. self.Entity.Name .. ":" .. what .. "@" .. Time.GetTick())
end
function T:OnCollisionEnter(other)
	record(self, "enter:" .. other.Name)
end
function T:OnJointBreak(force, torque)
	local weld = self.Entity.FixedJoint
	assert(weld ~= nil and weld.Broken, "the joint is written Broken before its callback")
	assert(force > 1, "the force that broke the weld: " .. force)
	assert(torque >= 0)
	record(self, "break")
end
return Script.Define("JointTrace", T)
)";

		std::vector<std::string> JointTrace(Scene& scene)
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

		// Probe.Record(message): a "Trace:<message>" entity, read back in canonical order by JointTrace.
		int RecordJointEvent(ScriptCall& call)
		{
			const std::string message = Lua::Check<std::string>(call, 1);
			static_cast<void>(Lua::GetEngine(call)->GetHost().GetScene().CreateEntity("Trace:" + message));
			return 0;
		}

		std::string DescribeJointErrors(const ScriptErrorStream& errors)
		{
			std::string result;
			for (const ScriptError& error : errors.GetErrors())
				result += std::format("{}:{} {}\n", error.Script, error.Line, error.Message);
			return result;
		}

		// The OnJointBreak session in `mode`: a plank welded to the world that its own weight breaks at the first step, two
		// behaviours on it, and a ball resting on it from the start, so that the first step both begins the contact and breaks
		// the weld. With `snapshots`, its script API coverage is added to them.
		void RunJointBreakSession(RunModes mode, std::vector<ScriptApiCoverage>* snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			const AssetHandle handle(0x2b3701);
			REQUIRE(fixture.AddScript(handle, "Assets/Scripts/JointTrace.luau", std::string(JointTraceSource)));
			ScriptSlot script{};
			script.Script.SetHandle(handle);

			Scene& editScene = fixture.GetScene();
			Entity plank = Test::AddBoxBody(editScene, "Plank", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.5f, 0.05f, 0.25f), BodyType::Dynamic);
			plank.AddComponent<FixedJointComponent>(FixedJointComponent{ .BreakForce = 1.0f });
			plank.AddComponent<ScriptComponent>(ScriptComponent{ { script, script } });
			Entity ball = Test::AddSphereBody(editScene, "Ball", glm::vec3(0.0f, 2.29f, 0.0f), 0.25f, BodyType::Dynamic);
			ball.AddComponent<ScriptComponent>(ScriptComponent{ { script } });

			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.Seed = 7;
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.ScriptRunMode = mode;
			specification.TestMode = true;
			Result<Scope<PlaySession>> started = PlaySession::CreateFromScene(specification, editScene);
			REQUIRE_MESSAGE(started.has_value(), (started ? "" : started.error().ToString()));
			Scope<PlaySession> session = std::move(*started);
			session->SetExtractionEnabled(false);
			Scene& scene = session->GetScene();

			Test::RunTicks(*session, 1);
			REQUIRE_MESSAGE(session->GetScriptErrors().GetErrors().empty(), DescribeJointErrors(session->GetScriptErrors()));
			const std::vector<std::string> trace = JointTrace(scene);
			// The three collision callbacks (two on the plank, one on the ball) in their order, then each of the plank's two
			// behaviours hears the break, in the same tick; the ball, whose entity has no joint, hears none.
			REQUIRE(trace.size() == 5);
			const std::string tick = trace[0].substr(trace[0].find('@'));
			for (const std::string& entry : trace)
			{
				CAPTURE(entry);
				CHECK(entry.ends_with(tick));
			}
			CHECK(std::multiset<std::string>(trace.begin(), trace.begin() + 3)
				== std::multiset<std::string>{ "Plank:enter:Ball" + tick, "Plank:enter:Ball" + tick, "Ball:enter:Plank" + tick });
			CHECK(trace[3] == "Plank:break" + tick);
			CHECK(trace[4] == "Plank:break" + tick);
			CHECK(scene.FindEntityByID(plank.GetUUID()).GetComponent<FixedJointComponent>().Broken);

			// A broken joint breaks once: nothing more is heard.
			Test::RunTicks(*session, 30);
			REQUIRE_MESSAGE(session->GetScriptErrors().GetErrors().empty(), DescribeJointErrors(session->GetScriptErrors()));
			const std::vector<std::string> later = JointTrace(scene);
			CHECK(std::count_if(later.begin(), later.end(), [](const std::string& entry)
			{
				return entry.starts_with("Plank:break");
			}) == 2);
			if (snapshots != nullptr)
			{
				const auto coverage = session->GetScripts()->GetApi().GetCoverage(mode);
				REQUIRE(coverage);
				snapshots->push_back(*coverage);
			}
		}

	}

	namespace Test {

		// M37 (Docs/Decisions/0054-m37-joints.md decision 8): OnJointBreak in a play session, for the Behaviour scenario
		// (ScriptInstanceLifecycleTests.cpp's RunBehaviourCallbacksCoverage).
		void RunJointBreakCallbackCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			RunJointBreakSession(mode, &snapshots);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptEngine: OnJointBreak reaches the joint entity's behaviours after the step's pair events")
		{
			RunJointBreakSession(RunModes::Editor, nullptr);
		}

		// An instance that has not started yet (added by another behaviour in the tick its weld breaks, before the physics)
		// holds OnJointBreak, as it holds physics callbacks and animation events: it arrives after its OnStart, at the next
		// fixed step's start flush, once (the W37.2 review's finding 1, after M23's case for OnAnimationEvent).
		TEST_CASE("ScriptEngine: an instance that has not started holds OnJointBreak until after its OnStart")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle held(0x2b3702);
			REQUIRE(fixture.AddScript(held, "Assets/Scripts/HeldBreak.luau", R"(
local T = {}
function T:OnStart() Scene.CreateEntity("Trace:start") end
function T:OnJointBreak(force, torque) Scene.CreateEntity("Trace:break") end
return Script.Define("HeldBreak", T)
)"));
			const AssetHandle spawner(0x2b3703);
			REQUIRE(fixture.AddScript(spawner, "Assets/Scripts/Spawner.luau", R"(
local S = {}
function S:OnFixedUpdate()
	if Time.GetTick() == 0 then
		Scene.FindByName("Plank"):AddComponent("Script", {Scripts = {{Script = Assets.Load("Assets/Scripts/HeldBreak.luau"), Fields = {}, ExecutionOrder = 0}}})
	end
end
return Script.Define("Spawner", S)
)"));
			// A plank welded to the world that its own weight breaks at tick 0's PostStep.
			Scene& editScene = fixture.GetScene();
			Entity plank = Test::AddBoxBody(editScene, "Plank", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.5f, 0.05f, 0.25f), BodyType::Dynamic);
			plank.AddComponent<FixedJointComponent>(FixedJointComponent{ .BreakForce = 1.0f });
			ScriptSlot slot{};
			slot.Script.SetHandle(spawner);
			editScene.CreateEntity("Spawner").AddComponent<ScriptComponent>(ScriptComponent{ { slot } });

			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.Seed = 7;
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.ScriptRunMode = RunModes::Editor;
			Result<Scope<PlaySession>> started = PlaySession::CreateFromScene(specification, editScene);
			REQUIRE_MESSAGE(started.has_value(), (started ? "" : started.error().ToString()));
			Scope<PlaySession> session = std::move(*started);
			session->SetExtractionEnabled(false);
			Test::RunTicks(*session, 3);
			REQUIRE_MESSAGE(session->GetScriptErrors().GetErrors().empty(), DescribeJointErrors(session->GetScriptErrors()));
			CHECK(session->GetScene().FindEntityByPath("/Plank").GetComponent<FixedJointComponent>().Broken);
			// The break of tick 0 waited for the instance's OnStart, at tick 1's start flush, which then delivered it once.
			std::vector<std::string> trace = JointTrace(session->GetScene());
			CHECK(trace == std::vector<std::string>{ "start", "break" });
		}

		// The held queue keeps arrival order across kinds, and an instance destroyed before its start flush discards what it
		// holds: its OnJointBreak never arrives (the W37.2 review's finding 1, after M23's case for animation events).
		TEST_CASE("ScriptEngine: held contacts and joint breaks arrive in arrival order, and an instance destroyed before its start drops them")
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.TestMode = true;
			specification.ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				api.Module("Probe", "Records callback ordering into the test scene.").Function("Record", &RecordJointEvent, "(message: string) -> ()", "Append one test observation.", { .Mutates = true });
				return {};
			};
			Test::ScriptTestFixture fixture(specification);
			const AssetHandle held(0x2b3704);
			REQUIRE(fixture.AddScript(held, "Assets/Scripts/HeldOrder.luau", R"(
local T = {}
function T:OnStart() Probe.Record(self.Entity.Name .. ":start") end
function T:OnTriggerEnter(other) Probe.Record(self.Entity.Name .. ":trigger") end
function T:OnJointBreak(force, torque) Probe.Record(self.Entity.Name .. ":break:" .. force .. ":" .. torque) end
return Script.Define("HeldOrder", T)
)"));
			REQUIRE(fixture.Start());
			Scene& scene = fixture.GetScene();
			ScriptEngine& engine = *fixture.GetEngine();
			const Entity kept = scene.CreateEntityWithID(UUID(170), "Kept");
			const Entity dropped = scene.CreateEntityWithID(UUID(171), "Dropped");
			for (Entity entity : { kept, dropped })
			{
				ScriptSlot slot{};
				slot.Script.SetHandle(held);
				entity.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });
			}
			REQUIRE(engine.SynchronizeInstances());

			engine.OnPhysicsEvent(PhysicsEvent{ .Type = PhysicsEventType::TriggerEnter, .Self = UUID(170), .Other = UUID(172) });
			engine.OnPhysicsJointBreak(PhysicsJointBreak{ .Joint = UUID(170), .Force = 5.0f, .Torque = 2.0f, .Tick = 0 });
			engine.OnPhysicsJointBreak(PhysicsJointBreak{ .Joint = UUID(171), .Force = 7.0f, .Torque = 1.0f, .Tick = 0 });
			CHECK(JointTrace(scene).empty());

			scene.DestroyEntity(dropped);
			CHECK(engine.PrepareDestroyFlush() == 1);
			engine.FinishDestroyFlush();
			scene.FlushPendingDestroys();
			fixture.Frame.Phase = InputPhase::Step;
			engine.StartPending();
			CHECK(JointTrace(scene) == std::vector<std::string>{ "Kept:start", "Kept:trigger", "Kept:break:5:2" });
			CHECK(fixture.Errors.empty());
		}
	}

}
