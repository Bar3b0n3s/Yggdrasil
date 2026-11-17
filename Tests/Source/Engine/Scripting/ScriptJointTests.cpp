#include "TestsPCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Scene/Components/FixedJointComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
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
// joint already written Broken and the force and torque that broke it.

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
	}

}
