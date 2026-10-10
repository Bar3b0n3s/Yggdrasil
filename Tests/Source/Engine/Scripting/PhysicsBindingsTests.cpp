#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Support/PhysicsTestScene.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

namespace Engine {

	namespace {

		class PhysicsBindingFixture
		{
		public:
			explicit PhysicsBindingFixture(bool readOnly = false, RunModes mode = RunModes::Editor)
				: Script({ .Mode = mode, .TestMode = true, .ReadOnly = readOnly })
			{
				Test::AddBoxBody(Script.GetScene(), "Near", glm::vec3(0, 0, 0), glm::vec3(0.5f), BodyType::Static);
				Test::AddBoxBody(Script.GetScene(), "Far", glm::vec3(0, 0, -4), glm::vec3(0.5f), BodyType::Static);
				Test::AddBoxBody(Script.GetScene(), "Dynamic", glm::vec3(10, 0, 0), glm::vec3(0.5f), BodyType::Dynamic);
				Test::AddBoxBody(Script.GetScene(), "Kinematic", glm::vec3(20, 0, 0), glm::vec3(0.5f), BodyType::Kinematic);
				const auto character = Script.GetScene().CreateEntity("Character");
				character.Patch<TransformComponent>([](auto& transform)
				{
					transform.Translation = glm::vec3(30, 0, 0);
				});
				character.AddComponent<CharacterControllerComponent>();
			}
			~PhysicsBindingFixture()
			{
				Script.Stop();
				Script.Physics = nullptr;
			}
			Status Start()
			{
				ENGINE_TRY_ASSIGN(System, PhysicsSystem::Create({ .RuntimeScene = &Script.GetScene(), .Gravity = glm::vec3(0), .Assets = Script.GetAssets() }));
				Script.Physics = System.get();
				return Script.Start();
			}
			Test::ScriptTestFixture Script;
			Scope<PhysicsSystem> System{};
		};

	}

	static void CheckPhysicsBinding(Test::ScriptTestFixture& fixture, std::string_view source)
	{
		const auto result = fixture.Evaluate(source);
		if (!result)
			INFO(result.error().ToString());
		REQUIRE(result);
		CHECK(result->Value.Get() == true);
	}

	static void PhysicsQueryCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
	{
		PhysicsBindingFixture fixture(false, mode);
		REQUIRE(fixture.Start());
		CheckPhysicsBinding(fixture.Script, R"(
local near, far = Scene.FindByName("Near"), Scene.FindByName("Far")
local origin, direction = vector.create(0,0,3), vector.create(0,0,-10)
local mask = Physics.LayerMask("Default")
assert(mask ~= 0 and Physics.LayerMask() == 0)
local hit = Physics.Raycast(origin, direction, 10, mask)
assert(hit.Entity == near and hit.Body == near)
assert(math.abs(hit.Distance - 2.5) < 0.001)
assert(hit.Point == vector.create(0,0,0.5) and hit.Normal == vector.create(0,0,1))
local hits = Physics.RaycastAll(origin, direction, 10)
assert(#hits == 2 and hits[1].Entity == near and hits[2].Entity == far)
assert(hits[1].Distance < hits[2].Distance)
assert(Physics.Raycast(origin, direction, 10, 0) == nil)
assert(#Physics.RaycastAll(origin, direction, 10, 0) == 0)
assert(Physics.Raycast(origin, vector.create(0,1,0), 10) == nil)
local sphere = Physics.SphereCast(origin, 0.25, direction, 10)
assert(sphere and sphere.Entity == near and sphere.Body == near and sphere.Distance < hit.Distance)
local overlap = Physics.OverlapSphere(vector.zero, 0.6)
assert(#overlap == 1 and overlap[1] == near)
local box = Physics.OverlapBox(vector.create(0,0,-2), vector.create(1,1,3), Quat.New(0,0,0,2), mask)
assert(#box == 2 and box[1].ID < box[2].ID)
assert(#Physics.OverlapBox(vector.zero, vector.one, nil, 0) == 0)
local lo, hi = Physics.GetBodyBounds(near)
local clo, chi = Physics.GetColliderBounds(near)
assert(lo == vector.create(-0.5,-0.5,-0.5) and hi == vector.create(0.5,0.5,0.5))
assert(clo == lo and chi == hi)
local empty = Scene.CreateEntity("Empty")
assert(Physics.GetBodyBounds(empty) == nil and Physics.GetColliderBounds(empty) == nil)
assert(Physics.GetGravity() == vector.zero)
Physics.SetGravity(vector.create(0,-2,0))
assert(Physics.GetGravity() == vector.create(0,-2,0))
return true
)");
		CHECK(fixture.System->GetGravity() == glm::vec3(0, -2, 0));
		const auto coverage = fixture.Script.GetApi().GetCoverage(mode);
		REQUIRE(coverage);
		for (const auto& member : coverage->Members)
			if (member.Owner == "Physics")
			{
				CAPTURE(member.Member);
				CHECK(member.Calls > 0);
			}
		snapshots.push_back(*coverage);
	}

	static void PhysicsBodyCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
	{
		PhysicsBindingFixture fixture(false, mode);
		REQUIRE(fixture.Start());
		CheckPhysicsBinding(fixture.Script, R"(
local dynamic = Scene.FindByName("Dynamic").RigidBody
dynamic:AddForce(vector.create(1,0,0))
dynamic:AddForceAtPosition(vector.create(0,1,0), vector.create(10,0,0))
dynamic:AddTorque(vector.create(0,0,1))
dynamic:AddImpulse(vector.create(1,0,0))
dynamic:AddAngularImpulse(vector.create(0,1,0))
dynamic:SetLinearVelocity(vector.create(1,2,3))
dynamic:SetAngularVelocity(vector.create(0,1,0))
assert(dynamic:GetLinearVelocity() == vector.create(1,2,3))
assert(dynamic:GetAngularVelocity() == vector.create(0,1,0))
assert(type(dynamic:IsSleeping()) == "boolean")
dynamic:WakeUp()
assert(not dynamic:IsSleeping())
dynamic:Teleport(vector.create(11,0,0), Quat.Identity())
assert(Scene.FindByName("Dynamic").Transform.WorldPosition == vector.create(11,0,0))
Scene.FindByName("Kinematic").RigidBody:MoveKinematic(vector.create(21,0,0), Quat.Identity())
local character = Scene.FindByName("Character").CharacterController
assert(type(character:IsGrounded()) == "boolean")
assert(typeof(character:GetGroundNormal()) == "vector" and typeof(character:GetVelocity()) == "vector")
character:Move(vector.create(1,0,0))
return true
)");
		const auto step = SimStep::FromTick(0, 1.0 / 60.0);
		fixture.System->PreStep(step);
		fixture.System->Step(step);
		fixture.System->PostStep(step);
		CHECK(fixture.System->GetDiagnostics().empty());
		const auto coverage = fixture.Script.GetApi().GetCoverage(mode);
		REQUIRE(coverage);
		for (const auto& member : coverage->Members)
			if ((member.Owner == "RigidBody" || member.Owner == "CharacterController") && member.Kind == ScriptApiMemberKind::Method)
			{
				CAPTURE(member.Owner);
				CAPTURE(member.Member);
				CHECK(member.Calls > 0);
			}
		snapshots.push_back(*coverage);
	}

	namespace Test {

		void RunPhysicsBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			PhysicsQueryCoverage(mode, snapshots);
			PhysicsBodyCoverage(mode, snapshots);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("PhysicsBindings: real queries preserve hit identity ordering masks and bounds")
		{
			std::vector<ScriptApiCoverage> snapshots;
			PhysicsQueryCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("PhysicsBindings: body and character proxies dispatch to the real systems")
		{
			std::vector<ScriptApiCoverage> snapshots;
			PhysicsBodyCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("PhysicsBindings: malformed ranges and wrong motion types fail before mutation")
		{
			PhysicsBindingFixture fixture;
			REQUIRE(fixture.Start());
			CheckPhysicsBinding(fixture.Script, R"(
local zero, up = vector.zero, vector.create(0,1,0)
local dynamic = Scene.FindByName("Dynamic").RigidBody
local fixed = Scene.FindByName("Near").RigidBody
for _, fn in {
	function() Physics.Raycast(zero,zero,10) end,
	function() Physics.Raycast(zero,up,-1) end,
	function() Physics.Raycast(zero,up,10,-1) end,
	function() Physics.Raycast(zero,up,10,1.5) end,
	function() Physics.Raycast(vector.create(1e30,0,0),up,10) end,
	function() Physics.SphereCast(zero,-1,up,10) end,
	function() Physics.OverlapSphere(zero,0) end,
	function() Physics.OverlapBox(zero,vector.create(-1,1,1)) end,
	function() Physics.LayerMask("Missing") end,
	function() Physics.GetBodyBounds(Color.New(1,1,1)) end,
	function() Physics.SetGravity(vector.create(math.huge,0,0)) end,
	function() Physics.SetGravity(vector.create(1e30,0,0)) end,
	function() dynamic:AddForce(vector.create(1e30,0,0)) end,
	function() dynamic:SetLinearVelocity(vector.create(0/0,0,0)) end,
	function() dynamic:MoveKinematic(zero,Quat.Identity()) end,
	function() fixed:AddImpulse(up) end,
	function() fixed:SetAngularVelocity(up) end,
	function() dynamic:Teleport(vector.create(1e30,0,0)) end,
	function() Scene.FindByName("Character").CharacterController:Move(vector.create(1e30,0,0)) end,
} do
	local ok, message = pcall(fn)
	assert(not ok and type(message) == "string")
end
assert(dynamic:GetLinearVelocity() == zero and Physics.GetGravity() == zero)
return true
)");
			CHECK(fixture.Script.ExternalMutations.empty());
			CHECK(fixture.System->GetDiagnostics().empty());
		}

		TEST_CASE("PhysicsBindings: read-only evaluation allows queries and blocks every host write")
		{
			PhysicsBindingFixture fixture(true);
			REQUIRE(fixture.Start());
			CheckPhysicsBinding(fixture.Script, R"(
assert(Physics.Raycast(vector.create(0,0,3),vector.create(0,0,-1),10))
local body = Scene.FindByName("Dynamic").RigidBody
for _, fn in {
	function() Physics.SetGravity(vector.zero) end,
	function() body:AddForce(vector.one) end,
	function() body:AddForceAtPosition(vector.one,vector.zero) end,
	function() body:AddTorque(vector.one) end,
	function() body:AddImpulse(vector.one) end,
	function() body:AddAngularImpulse(vector.one) end,
	function() body:SetLinearVelocity(vector.one) end,
	function() body:SetAngularVelocity(vector.one) end,
	function() body:Teleport(vector.zero) end,
	function() body:WakeUp() end,
	function() Scene.FindByName("Kinematic").RigidBody:MoveKinematic(vector.zero,Quat.Identity()) end,
	function() Scene.FindByName("Character").CharacterController:Move(vector.one) end,
} do assert(not pcall(fn)) end
assert(body:GetLinearVelocity() == vector.zero)
return true
)");
			CHECK(fixture.Script.ExternalMutations.empty());
			CHECK(fixture.System->GetGravity() == glm::vec3(0));
		}
	}

}
