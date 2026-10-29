#include "TestsPCH.h"

#include "Engine/Automation/Methods/Physics2DMethods.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

#include <string_view>

// physics2d.raycast and physics2d.overlap (Docs/Decisions/0032-m21-contract.md decision 15, stream P2): their C++ round trip,
// skipped until stream P2 registers the methods with their handlers.

namespace Engine {

	namespace {

		Json ParsePhysics2DMethodJson(std::string_view text)
		{
			Result<Json> json = JsonReader::Parse(text);
			REQUIRE(json.has_value());
			return std::move(*json);
		}

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("Physics2DMethods: physics2d.raycast and physics2d.overlap report the play scene's 2D colliders" * doctest::skip(true))
		{
			Test::AutomationFixture setup("Physics2DQueries");
			REQUIRE(setup.Call("edit.batch", ParsePhysics2DMethodJson(R"({"label":"Physics2D","ops":[
				{"method":"entity.create","params":{"name":"Wall","components":{"Transform":{"Translation":[3,0,0]},
					"RigidBody2D":{"Type":"Static"},"BoxCollider2D":{"HalfExtents":[0.5,2]}}}},
				{"method":"entity.create","params":{"name":"Coin","components":{"Transform":{"Translation":[-2,0,0]},
					"CircleCollider2D":{"Radius":0.25,"IsTrigger":true}}}}]})"))
					.has_value());
			// Bodies exist only in a play session.
			CHECK_FALSE(setup.Call("physics2d.raycast", Json{ { "origin", { 0, 1 } }, { "direction", { 1, 0 } }, { "maxDistance", 10 } }).has_value());
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true }, { "seed", 3 }, { "mode", "simulate" } }).has_value());

			const Result<Json> ray = setup.Call("physics2d.raycast", Json{ { "origin", { 0, 1 } }, { "direction", { 1, 0 } }, { "maxDistance", 10 } });
			REQUIRE_MESSAGE(ray.has_value(), ray.error().ToString());
			REQUIRE((*ray)["hits"].size() == 1);
			CHECK((*ray)["hits"][0]["entity"]["name"] == Json("Wall"));
			CHECK(JsonReader((*ray)["hits"][0]["distance"]).ReadFloat().value_or(0.0f) == doctest::Approx(2.5f));
			REQUIRE((*ray)["hits"][0]["point"].size() == 2);

			const Result<Json> overlap = setup.Call("physics2d.overlap", Json{ { "shape", "Circle" }, { "center", { -2, 0 } }, { "radius", 0.1 } });
			REQUIRE_MESSAGE(overlap.has_value(), overlap.error().ToString());
			REQUIRE((*overlap)["entities"].size() == 1);
			CHECK((*overlap)["entities"][0]["name"] == Json("Coin"));
			const Result<Json> box = setup.Call("physics2d.overlap", Json{ { "shape", "Box" }, { "center", { 0, 0 } }, { "halfExtents", { 4, 0.1 } }, { "angle", 0 } });
			REQUIRE(box.has_value());
			CHECK((*box)["entities"].size() == 2);
		}
	}

}
