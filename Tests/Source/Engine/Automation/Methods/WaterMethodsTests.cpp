#include "TestsPCH.h"

#include "Engine/Automation/Methods/WaterMethods.h"

#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <string>
#include <string_view>

// water.sample (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §13.5): the surface under world points by the
// rule for which water holds them, or a named water's own surface, at a given time, 0 in the edit scene and the world's time
// in the play scene; its errors.

namespace Engine {

	namespace {

		Json ParseWaterMethodJson(std::string_view text)
		{
			Result<Json> json = JsonReader::Parse(text);
			REQUIRE(json.has_value());
			return std::move(*json);
		}

		float ReadFloat(const Json& value)
		{
			return JsonReader(value).ReadFloat().value_or(std::numeric_limits<float>::quiet_NaN());
		}

		// A calm sea at y = 0 and a pool at y = 3 over 4 x 4 m around (10, 3, 0), with a 1 m/s current along X; a swell sea
		// whose height changes with time is added with `swell`.
		void BuildWaters(Test::AutomationFixture& setup, bool swell)
		{
			Json batch = ParseWaterMethodJson(R"({"label":"Waters","ops":[
				{"method":"entity.create","params":{"name":"Sea","components":{"Water":{"Waves":[]}}}},
				{"method":"entity.create","params":{"name":"Pool","components":{"Transform":{"Translation":[10,3,0]},
					"Water":{"Shape":"Rectangle","Size":[4,4],"Depth":2,"Headroom":2,"Current":[1,0]}}}},
				{"method":"entity.create","params":{"name":"Rock","components":{"Transform":{"Translation":[0,5,0]}}}}]})");
			if (swell)
				batch["ops"][0]["params"]["components"]["Water"]["Waves"] = ParseWaterMethodJson(R"([{"Wavelength":8,"Amplitude":0.5,"Steepness":0.5}])");
			const Result<Json> built = setup.Call("edit.batch", batch);
			REQUIRE_MESSAGE(built.has_value(), built.error().ToString());
		}

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("WaterMethods: water.sample reads the water that holds each point, or a named water's own surface")
		{
			Test::AutomationFixture setup("WaterSample");
			BuildWaters(setup, false);
			const Result<Json> sampled = setup.Call("water.sample", ParseWaterMethodJson(R"({"points":[[0,-1,0],[10,3.5,0],[10,6,0],[40,2,0]]})"));
			REQUIRE_MESSAGE(sampled.has_value(), sampled.error().ToString());
			CHECK(ReadFloat((*sampled)["time"]) == 0.0f);
			const Json& points = (*sampled)["points"];
			REQUIRE(points.size() == 4);
			// Under the calm sea, 1 m deep.
			CHECK(points[0]["water"]["path"] == "/Sea");
			CHECK(ReadFloat(points[0]["height"]) == 0.0f);
			CHECK(ReadFloat(points[0]["depth"]) == 1.0f);
			CHECK(points[0]["underwater"] == Json(true));
			CHECK(ReadFloat(points[0]["normal"][1]) == 1.0f);
			// In the pool's volume, above its surface: the pool, dry; its current in the velocity.
			CHECK(points[1]["water"]["path"] == "/Pool");
			CHECK(ReadFloat(points[1]["height"]) == 3.0f);
			CHECK(ReadFloat(points[1]["depth"]) == -0.5f);
			CHECK(points[1]["underwater"] == Json(false));
			CHECK(ReadFloat(points[1]["velocity"][0]) == 1.0f);
			// Above the pool's headroom and beyond its footprint: the sea.
			CHECK(points[2]["water"]["path"] == "/Sea");
			CHECK(points[3]["water"]["path"] == "/Sea");
			CHECK(ReadFloat(points[3]["depth"]) == -2.0f);

			// The pool's own surface: none outside its footprint, its own depth over the sea's points.
			const Result<Json> own = setup.Call("water.sample", ParseWaterMethodJson(R"({"entity":"/Pool","points":[[0,-1,0],[11,2.5,1]]})"));
			REQUIRE_MESSAGE(own.has_value(), own.error().ToString());
			CHECK((*own)["points"][0]["water"].is_null());
			CHECK((*own)["points"][0]["underwater"] == Json(false));
			CHECK((*own)["points"][1]["water"]["path"] == "/Pool");
			CHECK(ReadFloat((*own)["points"][1]["depth"]) == 0.5f);
			CHECK((*own)["points"][1]["underwater"] == Json(true));

			// Errors: no point, a non-Water entity, a negative time.
			CHECK_FALSE(setup.Call("water.sample", ParseWaterMethodJson(R"({"points":[]})")).has_value());
			CHECK_FALSE(setup.Call("water.sample", ParseWaterMethodJson(R"({"entity":"/Rock","points":[[0,0,0]]})")).has_value());
			CHECK_FALSE(setup.Call("water.sample", ParseWaterMethodJson(R"({"points":[[0,0,0]],"time":-1})")).has_value());
			CHECK_FALSE(setup.Call("water.sample", ParseWaterMethodJson(R"({"time":1})")).has_value());
			CHECK_FALSE(setup.Call("water.sample", ParseWaterMethodJson(R"({"points":[[0,0,0],[0,2e9,0]]})")).has_value());
		}

		TEST_CASE("WaterMethods: water.sample samples the edit scene at the given time or 0, and the play scene at the world's time")
		{
			Test::AutomationFixture setup("WaterSampleTime");
			BuildWaters(setup, true);
			const Json point = ParseWaterMethodJson(R"({"points":[[1.5,0,2]]})");
			const Result<Json> edit = setup.Call("water.sample", point);
			REQUIRE(edit.has_value());
			Json later = point;
			later["time"] = 1.5;
			const Result<Json> editLater = setup.Call("water.sample", later);
			REQUIRE(editLater.has_value());
			CHECK(ReadFloat((*editLater)["time"]) == 1.5f);
			const float atZero = ReadFloat((*edit)["points"][0]["height"]);
			const float atLater = ReadFloat((*editLater)["points"][0]["height"]);
			CHECK(std::abs(atZero - atLater) > 1.0e-3f);

			// The play scene, 90 ticks in at 60 Hz: the world's time is 1.5 s, the edit scene's given-time sample.
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true }, { "seed", 3 } }).has_value());
			REQUIRE(setup.Call("play.step", Json{ { "ticks", 90 }, { "render", "none" } }).has_value());
			const Result<Json> play = setup.Call("water.sample", point);
			REQUIRE_MESSAGE(play.has_value(), play.error().ToString());
			CHECK(ReadFloat((*play)["time"]) == doctest::Approx(1.5f));
			CHECK(ReadFloat((*play)["points"][0]["height"]) == doctest::Approx(atLater).epsilon(1e-5));
			// A given time overrides the world's; the edit scene is still at 0.
			Json zero = point;
			zero["time"] = 0.0;
			const Result<Json> playAtZero = setup.Call("water.sample", zero);
			REQUIRE(playAtZero.has_value());
			CHECK(ReadFloat((*playAtZero)["points"][0]["height"]) == doctest::Approx(atZero).epsilon(1e-5));
			Json editTarget = point;
			editTarget["target"] = "Edit";
			const Result<Json> stillEdit = setup.Call("water.sample", editTarget);
			REQUIRE(stillEdit.has_value());
			CHECK(ReadFloat((*stillEdit)["points"][0]["height"]) == atZero);
		}
	}

}
