#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Scene/Components/DistanceJointComponent.h"
#include "Engine/Scene/Components/FixedJointComponent.h"
#include "Engine/Scene/Components/HingeJointComponent.h"
#include "Engine/Scene/Components/SliderJointComponent.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Support/PhysicsTestScene.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <string_view>
#include <vector>

// The joint proxies' methods (M37, Docs/Decisions/0054-m37-joints.md decision 8; Architecture §11.5): each type's reading,
// force and torque from a real PhysicsSystem stepped by hand, their errors (no joint in force, no physics) and their coverage
// in each run mode.

namespace Engine {

	namespace {

		// A door on a hinge to the world, a carriage on a slider, a bob on a rope from a static post, a plank welded to the
		// world, and a weld authored broken.
		class JointBindingFixture
		{
		public:
			explicit JointBindingFixture(RunModes mode = RunModes::Editor, bool physics = true)
				: Script({ .Mode = mode, .TestMode = true }), m_Physics(physics)
			{
				Scene& scene = Script.GetScene();
				Entity door = Test::AddBoxBody(scene, "Door", glm::vec3(0.5f, 2.0f, 0.0f), glm::vec3(0.5f, 1.0f, 0.05f), BodyType::Dynamic);
				door.AddComponent<HingeJointComponent>(HingeJointComponent{ .Anchor = glm::vec3(-0.5f, 0.0f, 0.0f),
					.Axis = glm::vec3(0.0f, 1.0f, 0.0f),
					.Motor = JointMotorMode::Velocity,
					.MotorTargetVelocity = 90.0f });
				Entity carriage = Test::AddBoxBody(scene, "Carriage", glm::vec3(5.0f, 2.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
				carriage.AddComponent<SliderJointComponent>(SliderJointComponent{ .Motor = JointMotorMode::Velocity, .MotorTargetVelocity = 1.0f });
				Entity post = Test::AddBoxBody(scene, "Post", glm::vec3(10.0f, 5.0f, 0.0f), glm::vec3(0.1f), BodyType::Static);
				Entity bob = Test::AddSphereBody(scene, "Bob", glm::vec3(10.0f, 3.0f, 0.0f), 0.1f, BodyType::Dynamic);
				bob.AddComponent<DistanceJointComponent>(DistanceJointComponent{ .ConnectedBody = post.GetUUID(), .MinDistance = 0.0f, .MaxDistance = 2.0f });
				Entity plank = Test::AddBoxBody(scene, "Plank", glm::vec3(15.0f, 2.0f, 0.0f), glm::vec3(0.5f, 0.05f, 0.25f), BodyType::Dynamic);
				plank.AddComponent<FixedJointComponent>(FixedJointComponent{ .Anchor = glm::vec3(-0.5f, 0.0f, 0.0f) });
				Entity loose = Test::AddBoxBody(scene, "Loose", glm::vec3(20.0f, 2.0f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic);
				loose.AddComponent<FixedJointComponent>(FixedJointComponent{ .Broken = true });
			}
			~JointBindingFixture()
			{
				Script.Stop();
				Script.Physics = nullptr;
			}
			Status Start()
			{
				if (m_Physics)
				{
					ENGINE_TRY_ASSIGN(System, PhysicsSystem::Create({ .RuntimeScene = &Script.GetScene(), .Gravity = glm::vec3(0.0f, -10.0f, 0.0f), .Assets = Script.GetAssets() }));
					Script.Physics = System.get();
				}
				return Script.Start();
			}
			// Steps the physics `ticks` times, as the play session's phases would.
			void Step(uint64_t ticks)
			{
				for (uint64_t tick = 0; tick < ticks; ++tick)
				{
					const SimStep step = SimStep::FromTick(m_Tick++, 1.0 / 60.0);
					System->PreStep(step);
					System->Step(step);
					System->PostStep(step);
				}
			}
			Test::ScriptTestFixture Script;
			Scope<PhysicsSystem> System{};
		private:
			bool m_Physics = true;
			uint64_t m_Tick = 0;
		};

		void CheckJointBinding(Test::ScriptTestFixture& fixture, std::string_view source)
		{
			const auto result = fixture.Evaluate(source);
			if (!result)
				INFO(result.error().ToString());
			REQUIRE(result);
			CHECK(result->Value.Get() == true);
		}

	}

	namespace Test {

		// The joint proxies' methods in `mode` (ScriptApiRegistryTests.cpp's coverage scenario): each type's reading, force and
		// torque half a second into the motions of JointBindingFixture's joints.
		void RunJointsBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			JointBindingFixture fixture(mode);
			REQUIRE(fixture.Start());
			fixture.Step(30);
			CheckJointBinding(fixture.Script, R"(
local door = Scene.FindByName("Door").HingeJoint
local carriage = Scene.FindByName("Carriage").SliderJoint
local bob = Scene.FindByName("Bob").DistanceJoint
local plank = Scene.FindByName("Plank").FixedJoint
-- Half a second at 90 deg/s and 1 m/s.
assert(math.abs(door:GetAngle() - 45) < 2, "the door turned about 45 degrees: " .. door:GetAngle())
assert(door:GetForce() > 0 and door:GetTorque() > 0, "the hinge carries the door")
assert(math.abs(carriage:GetPosition() - 0.5) < 0.05, "the carriage slid about half a metre: " .. carriage:GetPosition())
assert(carriage:GetForce() > 0 and carriage:GetTorque() >= 0)
assert(math.abs(bob:GetDistance() - 2) < 0.05, "the rope is taut: " .. bob:GetDistance())
assert(bob:GetForce() > 0)
assert(plank:GetForce() > 0 and plank:GetTorque() > 0, "the weld carries the plank")
return true
)");
			const auto coverage = fixture.Script.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("JointBindings: each joint type reads its angle, position or distance, its force and its torque after the last step")
		{
			for (const RunModes mode : { RunModes::Editor, RunModes::Release, RunModes::Dist })
			{
				CAPTURE(static_cast<int>(mode));
				std::vector<ScriptApiCoverage> snapshots;
				Test::RunJointsBindingsCoverage(mode, snapshots);
				REQUIRE(snapshots.size() == 1);
				for (const auto& member : snapshots.front().Members)
				{
					const bool joint = member.Owner == "HingeJoint" || member.Owner == "SliderJoint" || member.Owner == "DistanceJoint" || member.Owner == "FixedJoint";
					if (joint && member.Kind == ScriptApiMemberKind::Method)
					{
						CAPTURE(member.Owner);
						CAPTURE(member.Member);
						CHECK(member.Calls > 0);
					}
				}
			}
		}

		TEST_CASE("JointBindings: a joint not in force and a session without physics raise instead of reading")
		{
			JointBindingFixture fixture;
			REQUIRE(fixture.Start());
			fixture.Step(1);
			CheckJointBinding(fixture.Script, R"(
local loose = Scene.FindByName("Loose").FixedJoint
local ok, message = pcall(function() return loose:GetForce() end)
assert(not ok and string.find(message, "entity has no joint in force", 1, true), message)
ok, message = pcall(function() return loose:GetTorque() end)
assert(not ok and string.find(message, "entity has no joint in force", 1, true), message)
-- A joint written broken is not in force from the next physics pre-step on.
Scene.FindByName("Door").HingeJoint.Broken = true
return true
)");
			fixture.Step(1);
			CheckJointBinding(fixture.Script, R"(
local ok, message = pcall(function() return Scene.FindByName("Door").HingeJoint:GetAngle() end)
assert(not ok and string.find(message, "entity has no joint in force", 1, true), message)
return true
)");

			JointBindingFixture edit(RunModes::Editor, false);
			REQUIRE(edit.Start());
			CheckJointBinding(edit.Script, R"(
local ok, message = pcall(function() return Scene.FindByName("Door").HingeJoint:GetAngle() end)
assert(not ok and string.find(message, "physics is unavailable", 1, true), message)
return true
)");
		}
	}

}
