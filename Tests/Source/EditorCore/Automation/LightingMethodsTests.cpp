#include "TestsPCH.h"

#include "EditorCore/Automation/LightingMethods.h"

#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

// lighting.bake (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 8, stream Q-A): the method's in-process
// round trip. BakeLighting's own behaviour, the bakes on the GPU included, is EditorCore/Lighting/LightingBakeTests.cpp.

namespace Engine {

	TEST_SUITE("EditorCore")
	{
		// lighting.bake reaches BakeLighting on the request's scene, whose refusal it answers: the active scene without a
		// "scene" param, an open scene named by its path, and NotFound for a path no open scene has.
		TEST_CASE("LightingMethods: lighting.bake answers BakeLighting's refusal for its scene")
		{
			Test::AutomationFixture setup("LightingBakeMethod");
			const Result<Json> refused = setup.Call("lighting.bake", Json::object());
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
			const Result<Json> named = setup.Call("lighting.bake", Json{ { "scene", "Assets/Scenes/Main.scene" }, { "reflectionProbes", false } });
			REQUIRE_FALSE(named.has_value());
			CHECK(named.error().GetCode() == ErrorCode::InvalidState);
			const Result<Json> elsewhere = setup.Call("lighting.bake", Json{ { "scene", "Assets/Scenes/Other.scene" } });
			REQUIRE_FALSE(elsewhere.has_value());
			CHECK(elsewhere.error().GetCode() == ErrorCode::NotFound);
		}
	}

}
