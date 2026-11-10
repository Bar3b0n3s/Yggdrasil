#include "TestsPCH.h"

#include "Engine/Automation/Methods/Physics2DMethods.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

#include <string_view>

// physics2d.raycast and physics2d.overlap (Docs/Decisions/0032-m21-contract.md decision 15, stream P2): their C++ round trip.

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
		TEST_CASE("Physics2DMethods: physics2d.raycast and physics2d.overlap report the play scene's 2D colliders")
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

		// Decision 15 (W21.1's review): the methods' own paths: the play session they need (InvalidState, the hint naming
		// play.start), each param they refuse at its pointer, every hit with `all` sorted by distance, the layer mask, the
		// hit's body, normal and tick, and each collider entity once in an overlap whose entity lies in two bodies.
		TEST_CASE("Physics2DMethods: the queries need a play session, refuse bad params at their pointers, list every hit and each entity once")
		{
			Test::AutomationFixture setup("Physics2DQueryPaths");
			REQUIRE(setup.Call("edit.batch", ParsePhysics2DMethodJson(R"({"label":"Physics2D","ops":[
				{"method":"entity.create","params":{"name":"Wall","components":{"Transform":{"Translation":[3,0,0]},
					"RigidBody2D":{"Type":"Static"},"BoxCollider2D":{"HalfExtents":[0.5,2]}}}},
				{"method":"entity.create","params":{"name":"Post","components":{"Transform":{"Translation":[6,0,0]},
					"BoxCollider2D":{"HalfExtents":[0.5,2]}}}},
				{"method":"entity.create","params":{"name":"Cart","components":{"Transform":{"Translation":[-6,0,0]},
					"RigidBody2D":{"Type":"Static"}}}},
				{"method":"entity.create","params":{"name":"Bumper","parent":"/Cart","components":{
					"BoxCollider2D":{"HalfExtents":[0.5,0.5]},"CircleCollider2D":{"Radius":0.3,"IsTrigger":true}}}}]})"))
					.has_value());

			// Bodies exist only in a play session: InvalidState, and the hint names the play.start that makes one.
			const Json notPlaying = setup.Request("physics2d.raycast", Json{ { "origin", { 0, 1 } }, { "direction", { 1, 0 } }, { "maxDistance", 10 } });
			CHECK(notPlaying["error"]["data"]["errorCode"] == Json("InvalidState"));
			CHECK(notPlaying["error"]["data"]["hint"].dump().contains("play.start"));
			const Result<Json> overlapNotPlaying = setup.Call("physics2d.overlap", Json{ { "shape", "Circle" }, { "center", { 0, 0 } }, { "radius", 1 } });
			REQUIRE_FALSE(overlapNotPlaying.has_value());
			CHECK(overlapNotPlaying.error().GetCode() == ErrorCode::InvalidState);
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true }, { "seed", 3 }, { "mode", "simulate" } }).has_value());

			const auto refusedAt = [&setup](std::string_view method, const Json& params, std::string_view pointer)
			{
				CAPTURE(params.dump());
				const Result<Json> refused = setup.Call(method, params);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				REQUIRE_FALSE(refused.error().GetIssues().empty());
				CHECK(refused.error().GetIssues()[0].JsonPointer == pointer);
			};
			refusedAt("physics2d.raycast", Json{ { "origin", { 0, 1 } }, { "direction", { 0, 0 } }, { "maxDistance", 10 } }, "/direction");
			refusedAt("physics2d.raycast", Json{ { "origin", { 0, 1 } }, { "direction", { 1, 0 } }, { "maxDistance", 0 } }, "/maxDistance");
			refusedAt("physics2d.raycast", Json{ { "origin", { 0, 1 } }, { "direction", { 1, 0 } }, { "maxDistance", -2 } }, "/maxDistance");
			refusedAt("physics2d.overlap", Json{ { "shape", "Circle" }, { "center", { 0, 0 } }, { "radius", 0 } }, "/radius");
			refusedAt("physics2d.overlap", Json{ { "shape", "Box" }, { "center", { 0, 0 } }, { "halfExtents", { 0, 1 } } }, "/halfExtents/0");
			refusedAt("physics2d.overlap", Json{ { "shape", "Box" }, { "center", { 0, 0 } }, { "halfExtents", { 1, 0 } } }, "/halfExtents/1");

			// `all`: every hit, by distance; without it, the closest one.
			const Json ray{ { "origin", { 0, 1 } }, { "direction", { 1, 0 } }, { "maxDistance", 10 }, { "all", true } };
			const Result<Json> every = setup.Call("physics2d.raycast", ray);
			REQUIRE_MESSAGE(every.has_value(), every.error().ToString());
			REQUIRE((*every)["hits"].size() == 2);
			CHECK((*every)["hits"][0]["entity"]["name"] == Json("Wall"));
			CHECK((*every)["hits"][1]["entity"]["name"] == Json("Post"));
			CHECK(JsonReader((*every)["hits"][0]["distance"]).ReadFloat().value_or(0.0f) == doctest::Approx(2.5f));
			CHECK(JsonReader((*every)["hits"][1]["distance"]).ReadFloat().value_or(0.0f) == doctest::Approx(5.5f));
			// The hit's body (the wall's own), its normal in the plane against the ray, and the session's tick.
			CHECK((*every)["hits"][0]["body"]["name"] == Json("Wall"));
			REQUIRE((*every)["hits"][0]["normal"].size() == 2);
			CHECK(JsonReader((*every)["hits"][0]["normal"][0]).ReadFloat().value_or(0.0f) == doctest::Approx(-1.0f));
			CHECK(JsonReader((*every)["hits"][0]["normal"][1]).ReadFloat().value_or(1.0f) == doctest::Approx(0.0f));
			CHECK((*every)["tick"] == Json(0));
			// A mask without the Default layer (bit 0) hits nothing.
			Json masked = ray;
			masked["layerMask"] = 0;
			const Result<Json> none = setup.Call("physics2d.raycast", masked);
			REQUIRE(none.has_value());
			CHECK((*none)["hits"].empty());

			// A gathered collider's hit (the Bumper's box, above its smaller trigger circle) names the body's owner; after a step
			// the result names the new tick.
			REQUIRE(setup.Call("play.step", Json{ { "ticks", 1 } }).has_value());
			const Result<Json> gathered = setup.Call("physics2d.raycast", Json{ { "origin", { -6, 3 } }, { "direction", { 0, -1 } }, { "maxDistance", 10 } });
			REQUIRE(gathered.has_value());
			REQUIRE((*gathered)["hits"].size() == 1);
			CHECK((*gathered)["hits"][0]["entity"]["name"] == Json("Bumper"));
			CHECK((*gathered)["hits"][0]["body"]["name"] == Json("Cart"));
			CHECK((*gathered)["tick"] == Json(1));

			// The Bumper's solid box lies in the Cart's body and its trigger circle in a sensor body of its own: two hits of one
			// collider entity, which the overlap names once.
			const Result<Json> once = setup.Call("physics2d.overlap", Json{ { "shape", "Circle" }, { "center", { -6, 0 } }, { "radius", 0.1 } });
			REQUIRE(once.has_value());
			REQUIRE((*once)["entities"].size() == 1);
			CHECK((*once)["entities"][0]["name"] == Json("Bumper"));
		}

		// C21.2 (Docs/Decisions/0032-m21-contract.md decision 27, stream P3): physics2d.bodyInfo, physics.bodyInfo in the plane;
		// stream P3 registers it.
		TEST_CASE("Physics2DMethods: physics2d.bodyInfo reports a 2D body's pose, colliders and contacts")
		{
			Test::AutomationFixture setup("Physics2DBodyInfo");
			REQUIRE(setup.Call("edit.batch", ParsePhysics2DMethodJson(R"({"label":"Physics2D","ops":[
				{"method":"entity.create","params":{"name":"Floor","components":{"Transform":{"Translation":[0,-0.5,0]},
					"RigidBody2D":{"Type":"Static"},"BoxCollider2D":{"HalfExtents":[5,0.5]}}}},
				{"method":"entity.create","params":{"name":"Crate","components":{"Transform":{"Translation":[1,0.5,0]},
					"RigidBody2D":{},"BoxCollider2D":{}}}},
				{"method":"entity.create","params":{"name":"Lid","parent":"/Crate","components":{"Transform":{"Translation":[0,0.6,0]},
					"BoxCollider2D":{"HalfExtents":[0.5,0.1]}}}},
				{"method":"entity.create","params":{"name":"Wedge","components":{"Transform":{"Translation":[-3,3,0],
					"Rotation":[0,0,0.70710678,0.70710678]},"RigidBody2D":{"Type":"Static"},"BoxCollider2D":{}}}},
				{"method":"entity.create","params":{"name":"Marker","components":{}}}]})"))
					.has_value());
			// Bodies exist only in a play session: InvalidState, the hint naming play.start.
			const Json notPlaying = setup.Request("physics2d.bodyInfo", Json{ { "entity", "/Crate" } });
			CHECK(notPlaying["error"]["data"]["errorCode"] == Json("InvalidState"));
			CHECK(notPlaying["error"]["data"]["hint"].dump().contains("play.start"));
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true }, { "seed", 3 }, { "mode", "simulate" } }).has_value());
			REQUIRE(setup.Call("play.step", Json{ { "ticks", 30 } }).has_value());

			// The gathered child reports its owner's body.
			const Result<Json> lid = setup.Call("physics2d.bodyInfo", Json{ { "entity", "/Crate/Lid" } });
			REQUIRE_MESSAGE(lid.has_value(), lid.error().ToString());
			CHECK((*lid)["entity"]["name"] == Json("Lid"));
			CHECK((*lid)["body"]["name"] == Json("Crate"));
			CHECK((*lid)["origin"] == Json("RigidBody"));
			CHECK((*lid)["type"] == Json("Dynamic"));
			CHECK((*lid)["layer"] == Json("Default"));
			REQUIRE((*lid)["colliders"].size() == 2);
			CHECK((*lid)["colliders"][0]["name"] == Json("Crate"));
			CHECK((*lid)["colliders"][1]["name"] == Json("Lid"));
			REQUIRE((*lid)["position"].size() == 2);
			CHECK(JsonReader((*lid)["position"][0]).ReadFloat().value_or(0.0f) == doctest::Approx(1.0f).epsilon(0.01));
			CHECK((*lid)["angle"].is_number());
			REQUIRE((*lid)["linearVelocity"].size() == 2);
			CHECK((*lid)["angularVelocity"].is_number());
			REQUIRE((*lid)["boundsMin"].size() == 2);
			REQUIRE((*lid)["contacts"].size() == 1);
			CHECK((*lid)["contacts"][0]["other"]["name"] == Json("Floor"));
			CHECK((*lid)["tick"] == Json(30));
			// The angle is reported in degrees about +Z: a quarter turn is 90.
			const Result<Json> wedge = setup.Call("physics2d.bodyInfo", Json{ { "entity", "/Wedge" } });
			REQUIRE_MESSAGE(wedge.has_value(), wedge.error().ToString());
			CHECK((*wedge)["origin"] == Json("RigidBody"));
			CHECK((*wedge)["type"] == Json("Static"));
			CHECK(JsonReader((*wedge)["angle"]).ReadFloat().value_or(0.0f) == doctest::Approx(90.0f).epsilon(1e-4));
			CHECK(JsonReader((*wedge)["position"][1]).ReadFloat().value_or(0.0f) == doctest::Approx(3.0f));
			// An entity without a 2D body: NotFound at /entity.
			const Result<Json> none = setup.Call("physics2d.bodyInfo", Json{ { "entity", "/Marker" } });
			REQUIRE_FALSE(none.has_value());
			CHECK(none.error().GetCode() == ErrorCode::NotFound);
			// The client rebuilds an error from its wire form, whose issues carry the location.
			REQUIRE_FALSE(none.error().GetIssues().empty());
			CHECK(none.error().GetIssues()[0].JsonPointer == "/entity");
		}
	}

}
