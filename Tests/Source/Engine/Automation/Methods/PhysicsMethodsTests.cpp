#include "TestsPCH.h"

#include "Engine/Automation/Methods/PhysicsMethods.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

// physics.bodyInfo (Architecture §13.5 "physics.bodyInfo {entity} (velocity, sleeping, contacts, layer)", Runtime subset;
// Docs/Decisions/0014-m11-decisions.md decision 18), in process through the editor's AutomationFixture (§15.2): a falling
// and then resting ball's velocity, sleeping state, layer, colliders and contacts, a compound child, a trigger, the errors
// and the method's flags. The Runtime's answer is tested in RuntimeAutomationServerTests.cpp.

namespace Engine {

	namespace {

		void StepPhysicsScene(Test::AutomationFixture& setup, uint32_t ticks)
		{
			const Result<Json> stepped = setup.Call("play.step", Json{ { "ticks", ticks }, { "render", "none" } });
			REQUIRE_MESSAGE(stepped.has_value(), stepped.error().ToString());
		}

		// The float at `index` of a JSON array of numbers (NaN when it is missing or not a number).
		float ReadComponent(const Json& array, size_t index)
		{
			return JsonReader(array[index]).ReadFloat().value_or(std::numeric_limits<float>::quiet_NaN());
		}

		Json ParsePhysicsMethodJson(std::string_view text)
		{
			Result<Json> json = JsonReader::Parse(text);
			REQUIRE(json.has_value());
			return std::move(*json);
		}

		// A ground (static box, top at y = 0), a level root with a Static RigidBody and two collider-only pieces, and a ball
		// that lands on the ground; then a lockstep session stepped `ticks` ticks.
		void StartPhysicsScene(Test::AutomationFixture& setup, uint32_t ticks = 90)
		{
			REQUIRE(setup.Call("edit.batch", ParsePhysicsMethodJson(R"({"label":"Physics","ops":[
				{"method":"entity.create","params":{"name":"Ground","components":{"Transform":{"Translation":[0,-0.5,0]},
					"RigidBody":{"Type":"Static"},"BoxCollider":{"HalfExtents":[50,0.5,50]}}}},
				{"method":"entity.create","params":{"name":"Level","components":{"Transform":{"Translation":[20,0,0]},"RigidBody":{"Type":"Static"}}}},
				{"method":"entity.create","params":{"name":"Piece","parent":"/Level","components":{"BoxCollider":{}}}},
				{"method":"entity.create","params":{"name":"Piece","parent":"/Level","components":{"Transform":{"Translation":[1,0,0]},"BoxCollider":{}}}},
				{"method":"entity.create","params":{"name":"Ball","components":{"Transform":{"Translation":[0,3,0]},
					"RigidBody":{"Type":"Dynamic"},"SphereCollider":{"Radius":0.5}}}}]})"))
					.has_value());
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true }, { "seed", 3 } }).has_value());
			StepPhysicsScene(setup, ticks);
		}

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("PhysicsMethods: physics.bodyInfo reports velocity, sleeping, layer, colliders and contacts")
		{
			Test::AutomationFixture setup("PhysicsBodyInfo");
			// Tick 20: the ball falls (about 3.3 m/s after a third of a second), awake and touching nothing.
			StartPhysicsScene(setup, 20);
			const Result<Json> falling = setup.Call("physics.bodyInfo", Json{ { "entity", "/Ball" } });
			REQUIRE_MESSAGE(falling.has_value(), falling.error().ToString());
			CHECK(ReadComponent((*falling)["linearVelocity"], 1) < -1.0f);
			CHECK((*falling)["sleeping"] == Json(false));
			CHECK((*falling)["contacts"].empty());

			StepPhysicsScene(setup, 70);
			const Result<Json> info = setup.Call("physics.bodyInfo", Json{ { "entity", "/Ball" } });
			REQUIRE_MESSAGE(info.has_value(), info.error().ToString());
			CHECK((*info)["entity"]["name"] == Json("Ball"));
			CHECK((*info)["body"]["id"] == (*info)["entity"]["id"]);
			CHECK((*info)["origin"] == Json("RigidBody"));
			CHECK((*info)["type"] == Json("Dynamic"));
			CHECK((*info)["layer"] == Json("Default"));
			CHECK((*info)["isSensor"] == Json(false));
			REQUIRE((*info)["colliders"].size() == 1);
			REQUIRE((*info)["linearVelocity"].size() == 3);
			REQUIRE((*info)["angularVelocity"].size() == 3);
			CHECK((*info)["tick"] == Json(90));
			// At rest on the ground.
			for (size_t axis = 0; axis < 3; ++axis)
			{
				CHECK(std::abs(ReadComponent((*info)["linearVelocity"], axis)) < 0.05f);
				CHECK(std::abs(ReadComponent((*info)["angularVelocity"], axis)) < 0.05f);
			}
			// Resting on the ground: one contact pair, whose partner is the ground.
			REQUIRE((*info)["contacts"].size() == 1);
			CHECK((*info)["contacts"][0]["other"]["name"] == Json("Ground"));
			CHECK((*info)["contacts"][0]["isTrigger"] == Json(false));
			CHECK(JsonReader((*info)["position"][1]).ReadFloat().value_or(0.0f) == doctest::Approx(0.5f).epsilon(0.05));

			// Resting long enough, it falls asleep, and keeps its contact (sleeping never ends a pair).
			StepPhysicsScene(setup, 150);
			const Result<Json> asleep = setup.Call("physics.bodyInfo", Json{ { "entity", "/Ball" } });
			REQUIRE_MESSAGE(asleep.has_value(), asleep.error().ToString());
			CHECK((*asleep)["sleeping"] == Json(true));
			CHECK((*asleep)["contacts"].size() == 1);
		}

		TEST_CASE("PhysicsMethods: physics.bodyInfo of a compound child reports the owner's body")
		{
			Test::AutomationFixture setup("PhysicsBodyInfoCompound");
			StartPhysicsScene(setup);
			const Result<Json> info = setup.Call("physics.bodyInfo", Json{ { "entity", "/Level/Piece[1]" } });
			REQUIRE_MESSAGE(info.has_value(), info.error().ToString());
			CHECK((*info)["entity"]["path"] == Json("/Level/Piece[1]"));
			CHECK((*info)["body"]["name"] == Json("Level"));
			CHECK((*info)["type"] == Json("Static"));
			CHECK((*info)["colliders"].size() == 2);
			CHECK((*info)["sleeping"] == Json(true));
			CHECK(JsonReader((*info)["boundsMax"][0]).ReadFloat().value_or(0.0f) == doctest::Approx(21.5f).epsilon(1.0e-3));
		}

		// M36 (Docs/Decisions/0053-m36-contact-surfaces.md decision 7): groundCollider, after groundNormal.
		TEST_CASE("PhysicsMethods: physics.bodyInfo of a character reports the collider it stands on")
		{
			Test::AutomationFixture setup("PhysicsBodyInfoGround");
			REQUIRE(setup.Call("entity.create", ParsePhysicsMethodJson(R"({"name":"Walker","components":{"Transform":{"Translation":[21,0.6,0]},
				"CharacterController":{}}})"))
					.has_value());
			StartPhysicsScene(setup, 60);
			const Result<Json> info = setup.Call("physics.bodyInfo", Json{ { "entity", "/Walker" } });
			REQUIRE_MESSAGE(info.has_value(), info.error().ToString());
			CHECK((*info)["origin"] == Json("Character"));
			CHECK((*info)["grounded"] == Json(true));
			CHECK((*info)["groundCollider"]["path"] == Json("/Level/Piece[1]"));
			// No ground collider for a body that is no character.
			const Result<Json> ball = setup.Call("physics.bodyInfo", Json{ { "entity", "/Ball" } });
			REQUIRE_MESSAGE(ball.has_value(), ball.error().ToString());
			CHECK((*ball)["groundCollider"]["id"] == Json(""));
		}

		// M36 W36.2 (Docs/Decisions/0053-m36-contact-surfaces.md decision 14): squeezed and squeezedBy, after groundCollider.
		TEST_CASE("PhysicsMethods: physics.bodyInfo of a squeezed character reports it and the press")
		{
			Test::AutomationFixture setup("PhysicsBodyInfoSqueeze");
			REQUIRE(setup.Call("edit.batch", ParsePhysicsMethodJson(R"({"label":"Squeeze","ops":[
				{"method":"entity.create","params":{"name":"Floor","components":{"Transform":{"Translation":[0,-0.5,0]},
					"RigidBody":{"Type":"Static"},"BoxCollider":{"HalfExtents":[10,0.5,10]}}}},
				{"method":"entity.create","params":{"name":"Pinned","components":{"Transform":{"Translation":[0,0.02,0]},"CharacterController":{}}}},
				{"method":"entity.create","params":{"name":"Press","components":{"Transform":{"Translation":[0,2.35,0]},
					"RigidBody":{"Type":"Kinematic"},"BoxCollider":{"HalfExtents":[1,0.25,1]}}}}]})"))
					.has_value());
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true }, { "seed", 3 } }).has_value());
			StepPhysicsScene(setup, 10);
			const Result<Json> standing = setup.Call("physics.bodyInfo", Json{ { "entity", "/Pinned" } });
			REQUIRE_MESSAGE(standing.has_value(), standing.error().ToString());
			CHECK((*standing)["squeezed"] == Json(false));
			CHECK((*standing)["squeezedBy"]["id"] == Json(""));
			// The press comes down at 1 m/s through entity.update of the play scene's Transform, one tick at a time.
			bool squeezed = false;
			for (uint32_t tick = 0; tick < 40 && !squeezed; ++tick)
			{
				const double y = 2.35 - static_cast<double>(tick + 1) / 60.0;
				REQUIRE(setup.Call("entity.update", Json{ { "entity", "/Press" }, { "target", "play" }, { "components", Json{ { "Transform", Json{ { "Translation", Json::array({ 0.0, y, 0.0 }) } } } } } })
						.has_value());
				StepPhysicsScene(setup, 1);
				const Result<Json> info = setup.Call("physics.bodyInfo", Json{ { "entity", "/Pinned" } });
				REQUIRE_MESSAGE(info.has_value(), info.error().ToString());
				squeezed = (*info)["squeezed"] == Json(true);
				if (squeezed)
					CHECK((*info)["squeezedBy"]["path"] == Json("/Press"));
			}
			CHECK(squeezed);
		}

		// M48 (Docs/Decisions/0066-m48-characters-off-the-ground-and-on-light-bodies.md decision 3): lastMove, after
		// squeezedBy.
		TEST_CASE("PhysicsMethods: physics.bodyInfo reports a character's last move")
		{
			Test::AutomationFixture setup("PhysicsBodyInfoLastMove");
			REQUIRE(setup.Call("entity.create", ParsePhysicsMethodJson(R"({"name":"Swimmer","components":{"Transform":{"Translation":[0,10,0]},
				"CharacterController":{}}})"))
					.has_value());
			StartPhysicsScene(setup, 1);
			const auto lastMove = [&setup](std::string_view entity)
			{
				const Result<Json> info = setup.Call("physics.bodyInfo", Json{ { "entity", std::string(entity) } });
				REQUIRE_MESSAGE(info.has_value(), info.error().ToString());
				return (*info)["lastMove"];
			};
			CHECK(lastMove("/Swimmer") == Json("Walk"));
			// A free move in the air, which the next step consumes.
			const Result<Json> evaluated = setup.Call("script.eval",
				Json{ { "context", "play" }, { "code", "Scene.FindByName('Swimmer').CharacterController:MoveFree(vector.create(0, 1, 0))" } });
			REQUIRE_MESSAGE(evaluated.has_value(), evaluated.error().ToString());
			StepPhysicsScene(setup, 1);
			CHECK(lastMove("/Swimmer") == Json("Free"));
			// Without another call the next update walks.
			StepPhysicsScene(setup, 1);
			CHECK(lastMove("/Swimmer") == Json("Walk"));
			// Empty for a body that is no character.
			CHECK(lastMove("/Ball") == Json(""));
		}

		TEST_CASE("PhysicsMethods: physics.bodyInfo needs a play session and an entity with a body")
		{
			Test::AutomationFixture setup("PhysicsBodyInfoErrors");
			REQUIRE(setup.Call("entity.create", ParsePhysicsMethodJson(R"({"name":"Empty"})")).has_value());
			// Edit mode: bodies exist only in a session. The whole response, for the hint (Call keeps code, detail and issues).
			const Json edit = setup.Request("physics.bodyInfo", Json{ { "entity", "/Empty" } });
			CHECK(edit["error"]["code"] == Json(-32002));
			CHECK(edit["error"]["data"]["errorCode"] == Json("InvalidState"));
			const Result<std::string> hint = JsonReader(edit["error"]["data"]).ReadMember<std::string>("hint");
			REQUIRE(hint.has_value());
			CHECK(hint->contains("simulate"));
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true }, { "mode", "simulate" } }).has_value());
			const Result<Json> noBody = setup.Call("physics.bodyInfo", Json{ { "entity", "/Empty" } });
			REQUIRE_FALSE(noBody.has_value());
			CHECK(noBody.error().GetCode() == ErrorCode::NotFound);
			const Json missing = setup.Request("physics.bodyInfo", Json{ { "entity", "/Nobody" } });
			CHECK(missing["error"]["code"] == Json(-32001));
			const Json noEntity = setup.Request("physics.bodyInfo", Json::object());
			CHECK(noEntity["error"]["code"] == Json(-32602));
		}

		TEST_CASE("PhysicsMethods: physics.bodyInfo is a read-only batchable method of both hosts and no tool")
		{
			Test::EditorTestFixture fixture("PhysicsBodyInfoFlags");
			MethodRegistry methods(fixture.GetEngine().GetTypeRegistry());
			RegisterEditorMethods(methods, {});
			methods.Freeze();
			const MethodDescriptor* method = methods.Find("physics.bodyInfo");
			REQUIRE(method != nullptr);
			const MethodSpecification& specification = method->Specification;
			CHECK_FALSE(specification.Mutates);
			CHECK_FALSE(specification.SupportsDryRun);
			CHECK_FALSE(specification.ExposeAsTool);
			CHECK_FALSE(specification.AvailableInLauncher);
			CHECK(specification.AvailableInRuntime);
			CHECK(specification.AllowedInBatch);
			CHECK(specification.RequiredParams == std::vector<std::string>{ "entity" });
			CHECK_FALSE(method->Pending);
		}

		TEST_CASE("PhysicsMethods: physics.bodyInfo of a trigger reports a Kinematic sensor")
		{
			// A goal authored as a Static RigidBody with a trigger collider is planned Kinematic (§9.2: triggers are kinematic
			// and kept active, so they see sleeping bodies).
			Test::AutomationFixture setup("PhysicsBodyInfoTrigger");
			REQUIRE(setup.Call("edit.batch", ParsePhysicsMethodJson(R"({"label":"Goal","ops":[
				{"method":"entity.create","params":{"name":"Goal","components":{"RigidBody":{"Type":"Static"},"BoxCollider":{"IsTrigger":true}}}},
				{"method":"entity.create","params":{"name":"Checkpoint","components":{"SphereCollider":{"IsTrigger":true}}}}]})"))
					.has_value());
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true }, { "mode", "simulate" } }).has_value());
			const Result<Json> goal = setup.Call("physics.bodyInfo", Json{ { "entity", "/Goal" } });
			REQUIRE_MESSAGE(goal.has_value(), goal.error().ToString());
			CHECK((*goal)["origin"] == Json("RigidBody"));
			CHECK((*goal)["type"] == Json("Kinematic"));
			CHECK((*goal)["isSensor"] == Json(true));
			CHECK((*goal)["tick"] == Json(0));
			const Result<Json> checkpoint = setup.Call("physics.bodyInfo", Json{ { "entity", "/Checkpoint" } });
			REQUIRE_MESSAGE(checkpoint.has_value(), checkpoint.error().ToString());
			CHECK((*checkpoint)["origin"] == Json("ImplicitSensor"));
			CHECK((*checkpoint)["type"] == Json("Kinematic"));
			CHECK((*checkpoint)["isSensor"] == Json(true));
			REQUIRE((*checkpoint)["colliders"].size() == 1);
			CHECK((*checkpoint)["colliders"][0]["name"] == Json("Checkpoint"));
		}

		TEST_CASE("PhysicsMethods: physics.jointInfo reports a joint's bodies, reading, force and motor in play")
		{
			Test::AutomationFixture setup("PhysicsJointInfo");
			REQUIRE(setup.Call("edit.batch", ParsePhysicsMethodJson(R"({"label":"Joints","ops":[
				{"method":"entity.create","params":{"name":"Post","components":{"Transform":{"Translation":[0,5,0]},
					"RigidBody":{"Type":"Static"},"BoxCollider":{"HalfExtents":[0.1,0.1,0.1]}}}},
				{"method":"entity.create","params":{"name":"Bob","components":{"Transform":{"Translation":[0,3,0]},
					"RigidBody":{"Type":"Dynamic"},"SphereCollider":{"Radius":0.1},
					"DistanceJoint":{"ConnectedBody":"/Post","MinDistance":0,"MaxDistance":2}}}},
				{"method":"entity.create","params":{"name":"Door","components":{"Transform":{"Translation":[5,2,0]},
					"RigidBody":{"Type":"Dynamic"},"BoxCollider":{"HalfExtents":[0.5,1,0.05]},
					"HingeJoint":{"Anchor":[-0.5,0,0],"Axis":[0,1,0],"Motor":"Velocity","MotorTargetVelocity":90}}}},
				{"method":"entity.create","params":{"name":"Loose","components":{"Transform":{"Translation":[10,2,0]},
					"RigidBody":{"Type":"Dynamic"},"BoxCollider":{},"FixedJoint":{"Broken":true}}}},
				{"method":"entity.create","params":{"name":"Floating","components":{"Transform":{"Translation":[15,2,0]},"HingeJoint":{}}}}]})"))
					.has_value());
			// The hinge on an entity without a RigidBody at or above it has no body: PHYSICS_JOINT_INVALID "body".
			const Test::ExpectLog invalid(LogLevel::Error, "PHYSICS_JOINT_INVALID");
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true }, { "seed", 3 } }).has_value());
			StepPhysicsScene(setup, 30);

			// The bob hangs at the end of its rope from the post, which carries its weight.
			const Result<Json> bob = setup.Call("physics.jointInfo", Json{ { "entity", "/Bob" } });
			REQUIRE_MESSAGE(bob.has_value(), bob.error().ToString());
			CHECK((*bob)["entity"]["name"] == Json("Bob"));
			CHECK((*bob)["type"] == Json("Distance"));
			CHECK((*bob)["body"]["name"] == Json("Bob"));
			CHECK((*bob)["connectedBody"]["name"] == Json("Post"));
			CHECK((*bob)["created"] == Json(true));
			CHECK((*bob)["broken"] == Json(false));
			CHECK(JsonReader((*bob)["distance"]).ReadFloat().value_or(0.0f) == doctest::Approx(2.0f).epsilon(0.02));
			CHECK(JsonReader((*bob)["force"]).ReadFloat().value_or(0.0f) > 1.0f);
			CHECK((*bob)["angle"] == Json(0.0));
			CHECK((*bob)["position"] == Json(0.0));
			CHECK((*bob)["motor"] == Json("Off"));
			CHECK((*bob)["tick"] == Json(30));

			// The door turns on its motor, about a vertical hinge to the world.
			const Result<Json> door = setup.Call("physics.jointInfo", Json{ { "entity", "/Door" } });
			REQUIRE_MESSAGE(door.has_value(), door.error().ToString());
			CHECK((*door)["type"] == Json("Hinge"));
			CHECK((*door)["connectedBody"].is_null());
			CHECK((*door)["created"] == Json(true));
			CHECK(JsonReader((*door)["angle"]).ReadFloat().value_or(0.0f) > 10.0f);
			CHECK((*door)["distance"] == Json(0.0));
			CHECK((*door)["motor"] == Json("Velocity"));

			// A joint authored broken is not in force and carries nothing.
			const Result<Json> loose = setup.Call("physics.jointInfo", Json{ { "entity", "/Loose" } });
			REQUIRE_MESSAGE(loose.has_value(), loose.error().ToString());
			CHECK((*loose)["type"] == Json("Fixed"));
			CHECK((*loose)["created"] == Json(false));
			CHECK((*loose)["broken"] == Json(true));
			CHECK((*loose)["force"] == Json(0.0));
			CHECK((*loose)["torque"] == Json(0.0));

			// A joint without a body: body is null, and it is not in force.
			const Result<Json> floating = setup.Call("physics.jointInfo", Json{ { "entity", "/Floating" } });
			REQUIRE_MESSAGE(floating.has_value(), floating.error().ToString());
			CHECK((*floating)["type"] == Json("Hinge"));
			CHECK((*floating)["body"].is_null());
			CHECK((*floating)["connectedBody"].is_null());
			CHECK((*floating)["created"] == Json(false));
			CHECK(invalid.GetMatchCount() >= 1);
		}

		TEST_CASE("PhysicsMethods: physics.jointInfo needs a play session and an entity with a joint")
		{
			Test::AutomationFixture setup("PhysicsJointInfoErrors");
			REQUIRE(setup.Call("entity.create", ParsePhysicsMethodJson(R"({"name":"Empty"})")).has_value());
			const Json edit = setup.Request("physics.jointInfo", Json{ { "entity", "/Empty" } });
			CHECK(edit["error"]["data"]["errorCode"] == Json("InvalidState"));
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true }, { "mode", "simulate" } }).has_value());
			const Result<Json> noJoint = setup.Call("physics.jointInfo", Json{ { "entity", "/Empty" } });
			REQUIRE_FALSE(noJoint.has_value());
			CHECK(noJoint.error().GetCode() == ErrorCode::NotFound);
			const Json missing = setup.Request("physics.jointInfo", Json{ { "entity", "/Nobody" } });
			CHECK(missing["error"]["code"] == Json(-32001));
			const Json noEntity = setup.Request("physics.jointInfo", Json::object());
			CHECK(noEntity["error"]["code"] == Json(-32602));
		}

		TEST_CASE("PhysicsMethods: physics.jointInfo is a read-only batchable method of both hosts and no tool")
		{
			Test::EditorTestFixture fixture("PhysicsJointInfoFlags");
			MethodRegistry methods(fixture.GetEngine().GetTypeRegistry());
			RegisterEditorMethods(methods, {});
			methods.Freeze();
			const MethodDescriptor* method = methods.Find("physics.jointInfo");
			REQUIRE(method != nullptr);
			const MethodSpecification& specification = method->Specification;
			CHECK_FALSE(specification.Mutates);
			CHECK_FALSE(specification.SupportsDryRun);
			CHECK_FALSE(specification.ExposeAsTool);
			CHECK_FALSE(specification.AvailableInLauncher);
			CHECK(specification.AvailableInRuntime);
			CHECK(specification.AllowedInBatch);
			CHECK(specification.RequiredParams == std::vector<std::string>{ "entity" });
			CHECK_FALSE(method->Pending);
		}
	}

}
