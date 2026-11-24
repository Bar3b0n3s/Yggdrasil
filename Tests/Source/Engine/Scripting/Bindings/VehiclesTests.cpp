#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/VehicleWheelComponent.h"
#include "Engine/Scene/Components/WheeledVehicleComponent.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Support/ExpectLog.h"
#include "Support/PhysicsTestScene.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <array>
#include <string_view>
#include <vector>

// The vehicle proxies' methods (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3; Architecture §9.11, §11.5): a
// buggy's speed, revs and gear and each wheel's contact, suspension, spin, steer and slip from a real PhysicsSystem stepped by
// hand, a wheel in the air, the errors (a vehicle not in force, no physics) and the methods' coverage in each run mode.

namespace Engine {

	namespace {

		constexpr std::array<std::string_view, 4> BindingWheelNames = { "FL", "FR", "RL", "RR" };

		// A 1200 kg buggy (a 1.8 x 0.5 x 4 m box) on four wheels of radius 0.4 m, each hanging 0.5 m below the chassis's centre
		// height; the front ones steer 35 degrees, the rear ones are driven. `differential` false leaves it without one, so
		// PHYSICS_VEHICLE_INVALID refuses it.
		Entity AddBindingBuggy(Scene& scene, std::string_view name, const glm::vec3& position, bool differential = true)
		{
			Entity body = Test::AddBoxBody(scene, name, position, glm::vec3(0.9f, 0.25f, 2.0f), BodyType::Dynamic);
			Test::PatchRigidBody(body, [](RigidBodyComponent& rigidBody)
			{
				rigidBody.Mass = 1200.0f;
			});
			std::array<UUID, 4> wheels{};
			for (size_t index = 0; index < BindingWheelNames.size(); ++index)
			{
				const bool front = index < 2;
				Entity wheel = scene.CreateEntity(BindingWheelNames[index], body);
				wheel.Patch<TransformComponent>([index, front](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(index % 2 == 0 ? -0.9f : 0.9f, -0.5f, front ? -1.4f : 1.4f);
				});
				wheel.AddComponent<VehicleWheelComponent>(VehicleWheelComponent{ .Radius = 0.4f, .Width = 0.3f, .MaxSteerAngle = front ? 35.0f : 0.0f });
				wheels[index] = wheel.GetUUID();
			}
			WheeledVehicleComponent vehicle;
			if (differential)
				vehicle.Differentials = { VehicleDifferential{ .LeftWheel = wheels[2], .RightWheel = wheels[3] } };
			body.AddComponent<WheeledVehicleComponent>(vehicle);
			return body;
		}

		// A buggy on the ground (its top at y = 0), a buggy held in the air (its GravityFactor 0), a buggy without a differential
		// (refused) and a crate with no vehicle.
		class VehicleBindingFixture
		{
		public:
			explicit VehicleBindingFixture(RunModes mode = RunModes::Editor, bool physics = true)
				: Script({ .Mode = mode, .TestMode = true }), m_Physics(physics)
			{
				Scene& scene = Script.GetScene();
				static_cast<void>(Test::AddGround(scene));
				static_cast<void>(AddBindingBuggy(scene, "Buggy", glm::vec3(0.0f, 1.0f, 0.0f)));
				Entity hover = AddBindingBuggy(scene, "Hover", glm::vec3(20.0f, 10.0f, 0.0f));
				Test::PatchRigidBody(hover, [](RigidBodyComponent& rigidBody)
				{
					rigidBody.GravityFactor = 0.0f;
				});
				static_cast<void>(AddBindingBuggy(scene, "Refused", glm::vec3(-20.0f, 1.0f, 0.0f), false));
				static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(0.0f, 0.5f, 20.0f), glm::vec3(0.5f), BodyType::Dynamic));
			}
			~VehicleBindingFixture()
			{
				Script.Stop();
				Script.Physics = nullptr;
			}
			Status Start()
			{
				if (m_Physics)
				{
					ENGINE_TRY_ASSIGN(System, PhysicsSystem::Create({ .RuntimeScene = &Script.GetScene(), .Gravity = glm::vec3(0.0f, -9.81f, 0.0f), .Assets = Script.GetAssets() }));
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

		void CheckVehicleBinding(Test::ScriptTestFixture& fixture, std::string_view source)
		{
			const auto result = fixture.Evaluate(source);
			if (!result)
				INFO(result.error().ToString());
			REQUIRE(result);
			CHECK(result->Value.Get() == true);
		}

		// The buggy settles for a second, then drives at full throttle with half a right lock for one more second.
		void SettleAndDrive(VehicleBindingFixture& fixture)
		{
			fixture.Step(60);
			CheckVehicleBinding(fixture.Script, R"(
local buggy = Scene.FindByName("Buggy").WheeledVehicle
buggy.Throttle = 1
buggy.Steering = 0.5
return true
)");
			fixture.Step(60);
		}

		// Every vehicle and wheel method read after SettleAndDrive, each checked against the buggy's state.
		constexpr std::string_view VehicleReadsSource = R"(
local buggy = Scene.FindByName("Buggy")
local vehicle = buggy.WheeledVehicle
local ground = Scene.FindByName("Ground")
assert(vehicle:GetSpeed() > 1, "the buggy drives forward: " .. vehicle:GetSpeed())
assert(vehicle:GetEngineRpm() >= 1000 and vehicle:GetEngineRpm() <= 6000, "the revs lie between MinRpm and MaxRpm: " .. vehicle:GetEngineRpm())
assert(vehicle:GetGear() >= 1, "Auto drives in a forward gear: " .. vehicle:GetGear())
assert(type(vehicle:IsShifting()) == "boolean")
for _, name in { "FL", "FR", "RL", "RR" } do
	local wheel = buggy:FindChild(name).VehicleWheel
	assert(wheel:IsTouching(), name .. " touches the ground")
	local point, normal, collider = wheel:GetContact()
	assert(point ~= nil and math.abs(point.y) < 0.05, name .. " touches at the ground's height")
	assert(normal ~= nil and normal.y > 0.99, name .. " stands on a level ground")
	assert(collider == ground, name .. " stands on the ground's collider")
	local length = wheel:GetSuspensionLength()
	assert(length >= wheel.SuspensionMinLength - 1e-4 and length < wheel.SuspensionMaxLength, name .. " compresses its spring: " .. length)
	assert(wheel:GetSuspensionForce() > 1000, name .. " carries part of the weight: " .. wheel:GetSuspensionForce())
	local rotation = wheel:GetRotation()
	assert(rotation >= 0 and rotation < 360, name .. "'s spin is in [0, 360): " .. rotation)
	assert(wheel:GetAngularVelocity() > 0, name .. " rolls forward: " .. wheel:GetAngularVelocity())
	local ratio, angle = wheel:GetSlip()
	assert(ratio >= 0 and angle >= 0 and angle <= 90, name .. "'s slips are magnitudes")
end
-- Half a right lock of 35 degrees steers the front wheels 17.5 degrees right, the rear ones not at all.
local steer = buggy:FindChild("FL").VehicleWheel:GetSteerAngle()
assert(math.abs(steer - 17.5) < 0.01, "the front wheel steers right: " .. steer)
assert(buggy:FindChild("RR").VehicleWheel:GetSteerAngle() == 0, "the rear wheel does not steer")

-- A wheel in the air has no contact, and its spring hangs at full length.
local hover = Scene.FindByName("Hover")
local hanging = hover:FindChild("RL").VehicleWheel
assert(not hanging:IsTouching(), "the hovering buggy's wheel is in the air")
local point, normal, collider = hanging:GetContact()
assert(point == nil and normal == nil and collider == nil, "a wheel in the air has no contact")
assert(math.abs(hanging:GetSuspensionLength() - hanging.SuspensionMaxLength) < 1e-4, "its spring hangs at full length")
assert(hanging:GetSuspensionForce() == 0, "and carries nothing")
local ratio, angle = hanging:GetSlip()
assert(ratio == 0 and angle == 0, "nor slips")
return true
)";

	}

	namespace Test {

		// The vehicle proxies' methods in `mode` (ScriptApiRegistryTests.cpp's coverage scenario): every read of the buggy and its
		// wheels a second into a drive.
		void RunVehiclesBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			const Test::ExpectLog refused(LogLevel::Error, "PHYSICS_VEHICLE_INVALID");
			VehicleBindingFixture fixture(mode);
			REQUIRE(fixture.Start());
			SettleAndDrive(fixture);
			CheckVehicleBinding(fixture.Script, VehicleReadsSource);
			const auto coverage = fixture.Script.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("VehicleBindings: a vehicle reads its speed, revs and gear, and each wheel its contact, suspension, spin, steer and slip after the last step")
		{
			for (const RunModes mode : { RunModes::Editor, RunModes::Release, RunModes::Dist })
			{
				CAPTURE(static_cast<int>(mode));
				std::vector<ScriptApiCoverage> snapshots;
				Test::RunVehiclesBindingsCoverage(mode, snapshots);
				REQUIRE(snapshots.size() == 1);
				for (const auto& member : snapshots.front().Members)
				{
					if ((member.Owner == "WheeledVehicle" || member.Owner == "VehicleWheel") && member.Kind == ScriptApiMemberKind::Method)
					{
						CAPTURE(member.Owner);
						CAPTURE(member.Member);
						CHECK(member.Calls > 0);
					}
				}
			}
		}

		TEST_CASE("VehicleBindings: a vehicle not in force and a session without physics raise instead of reading")
		{
			const Test::ExpectLog refused(LogLevel::Error, "PHYSICS_VEHICLE_INVALID");
			VehicleBindingFixture fixture;
			REQUIRE(fixture.Start());
			fixture.Step(1);
			CHECK(refused.GetMatchCount() >= 1);
			// The W44.3 review's finding 2: a wheel its vehicle in force does not hold (the front left, disabled; no differential or
			// bar names it, so the buggy is made again on its three other wheels) raises, while the vehicle and its other wheels read.
			CheckVehicleBinding(fixture.Script, R"(
Scene.FindByName("Buggy"):FindChild("FL"):SetActive(false)
return true
)");
			fixture.Step(1);
			fixture.System->FlushDestroyed(1);
			fixture.Step(1);
			CheckVehicleBinding(fixture.Script, R"(
local buggy = Scene.FindByName("Buggy")
assert(buggy.WheeledVehicle:GetEngineRpm() >= 1000, "the buggy is in force")
assert(buggy:FindChild("RL").VehicleWheel:IsTouching() ~= nil, "its other wheels read")
local ok, message = pcall(function() return buggy:FindChild("FL").VehicleWheel:IsTouching() end)
assert(not ok and string.find(tostring(message), "entity has no vehicle in force", 1, true), tostring(message))
ok, message = pcall(function() return buggy:FindChild("FL").VehicleWheel:GetContact() end)
assert(not ok and string.find(tostring(message), "entity has no vehicle in force", 1, true), tostring(message))
return true
)");
			CheckVehicleBinding(fixture.Script, R"(
local refused = Scene.FindByName("Refused")
for _, read in {
	function() return refused.WheeledVehicle:GetSpeed() end,
	function() return refused.WheeledVehicle:GetGear() end,
	function() return refused:FindChild("FL").VehicleWheel:IsTouching() end,
	function() return refused:FindChild("RR").VehicleWheel:GetContact() end,
} do
	local ok, message = pcall(read)
	assert(not ok and string.find(message, "entity has no vehicle in force", 1, true), message)
end
-- A vehicle in force reads; once a write leaves it without a differential, it is no longer in force from the next physics
-- pre-step on.
local hover = Scene.FindByName("Hover")
assert(hover.WheeledVehicle:GetEngineRpm() >= 1000)
hover.WheeledVehicle.Differentials = {}
return true
)");
			fixture.Step(1);
			CheckVehicleBinding(fixture.Script, R"(
-- A vehicle whose fields a write made invalid is not in force either, nor are its wheels.
local hover = Scene.FindByName("Hover")
local ok, message = pcall(function() return hover.WheeledVehicle:GetEngineRpm() end)
assert(not ok and string.find(message, "entity has no vehicle in force", 1, true), message)
ok, message = pcall(function() return hover:FindChild("FR").VehicleWheel:GetSteerAngle() end)
assert(not ok and string.find(message, "entity has no vehicle in force", 1, true), message)
return true
)");

			VehicleBindingFixture edit(RunModes::Editor, false);
			REQUIRE(edit.Start());
			CheckVehicleBinding(edit.Script, R"(
local buggy = Scene.FindByName("Buggy")
local ok, message = pcall(function() return buggy.WheeledVehicle:GetSpeed() end)
assert(not ok and string.find(message, "physics is unavailable", 1, true), message)
ok, message = pcall(function() return buggy:FindChild("FL").VehicleWheel:GetRotation() end)
assert(not ok and string.find(message, "physics is unavailable", 1, true), message)
return true
)");
		}
	}

}
