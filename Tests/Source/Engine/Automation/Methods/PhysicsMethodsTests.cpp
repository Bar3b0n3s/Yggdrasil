#include "TestsPCH.h"

#include "Engine/Automation/Methods/PhysicsMethods.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <string>

// physics.bodyInfo (Architecture §13.5 "physics.bodyInfo {entity} (velocity, sleeping, contacts, layer)", Runtime subset;
// Docs/Decisions/0014-m11-decisions.md decision 18), in process through the editor's AutomationFixture (§15.2). Skipped
// skeletons of the M11 contract: stream D implements and registers the method and removes the skips; the Runtime's answer
// is tested in RuntimeAutomationServerTests.cpp when the method lands.

namespace Engine {

	namespace {

		Json ParsePhysicsMethodJson(std::string_view text)
		{
			Result<Json> json = JsonReader::Parse(text);
			REQUIRE(json.has_value());
			return std::move(*json);
		}

		// A ground (static box, top at y = 0), a level root with a Static RigidBody and two collider-only pieces, and a ball
		// that lands on the ground; then a lockstep session stepped 90 ticks.
		void StartPhysicsScene(Test::AutomationFixture& setup)
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
			const Result<Json> stepped = setup.Call("play.step", Json{ { "ticks", 90 }, { "render", "none" } });
			REQUIRE_MESSAGE(stepped.has_value(), stepped.error().ToString());
		}

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("PhysicsMethods: physics.bodyInfo reports velocity, sleeping, layer, colliders and contacts" * doctest::skip(true))
		{
			Test::AutomationFixture setup("PhysicsBodyInfo");
			StartPhysicsScene(setup);
			const Result<Json> info = setup.Call("physics.bodyInfo", Json{ { "entity", "/Ball" } });
			REQUIRE_MESSAGE(info.has_value(), info.error().ToString());
			CHECK((*info)["entity"]["name"] == Json("Ball"));
			CHECK((*info)["body"]["id"] == (*info)["entity"]["id"]);
			CHECK((*info)["origin"] == Json("RigidBody"));
			CHECK((*info)["type"] == Json("Dynamic"));
			CHECK((*info)["layer"] == Json("Default"));
			CHECK((*info)["isSensor"] == Json(false));
			REQUIRE((*info)["colliders"].size() == 1);
			CHECK((*info)["linearVelocity"].size() == 3);
			CHECK((*info)["tick"] == Json(90));
			// Resting on the ground: one contact pair, whose partner is the ground.
			REQUIRE((*info)["contacts"].size() == 1);
			CHECK((*info)["contacts"][0]["other"]["name"] == Json("Ground"));
			CHECK((*info)["contacts"][0]["isTrigger"] == Json(false));
			CHECK(JsonReader((*info)["position"][1]).ReadFloat().value_or(0.0f) == doctest::Approx(0.5f).epsilon(0.05));
		}

		TEST_CASE("PhysicsMethods: physics.bodyInfo of a compound child reports the owner's body" * doctest::skip(true))
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

		TEST_CASE("PhysicsMethods: physics.bodyInfo needs a play session and an entity with a body" * doctest::skip(true))
		{
			Test::AutomationFixture setup("PhysicsBodyInfoErrors");
			REQUIRE(setup.Call("entity.create", ParsePhysicsMethodJson(R"({"name":"Empty"})")).has_value());
			// Edit mode: bodies exist only in a session.
			const Result<Json> edit = setup.Call("physics.bodyInfo", Json{ { "entity", "/Empty" } });
			REQUIRE_FALSE(edit.has_value());
			CHECK(edit.error().GetCode() == ErrorCode::InvalidState);
			CHECK(edit.error().GetHint().contains("simulate"));
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true }, { "mode", "simulate" } }).has_value());
			const Result<Json> noBody = setup.Call("physics.bodyInfo", Json{ { "entity", "/Empty" } });
			REQUIRE_FALSE(noBody.has_value());
			CHECK(noBody.error().GetCode() == ErrorCode::NotFound);
			const Json missing = setup.Request("physics.bodyInfo", Json{ { "entity", "/Nobody" } });
			CHECK(missing["error"]["code"] == Json(-32001));
			const Json noEntity = setup.Request("physics.bodyInfo", Json::object());
			CHECK(noEntity["error"]["code"] == Json(-32602));
		}
	}

}
