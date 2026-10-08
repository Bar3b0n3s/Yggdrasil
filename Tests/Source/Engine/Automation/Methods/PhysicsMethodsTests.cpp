#include "TestsPCH.h"

#include "Engine/Automation/Methods/PhysicsMethods.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

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
	}

}
