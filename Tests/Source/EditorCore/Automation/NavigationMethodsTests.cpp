#include "TestsPCH.h"
#include "EditorCore/Automation/NavigationMethods.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Scene/Components/NavigationComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AutomationTestClient.h"
#include "Support/NavigationTestScene.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

// navigation.bake (M30, Docs/Decisions/0045-m30-navigation.md decision 8): the method's in-process round trip, and
// project.validate's NAV_MESH_NOT_BAKED and NAV_MESH_STALE around it (decision 9). BakeNavigation's own behaviour is
// EditorCore/Navigation/NavigationBakeTests.cpp.

namespace Engine {

	namespace {

		// The codes project.validate reports on the active scene's Navigation entity, sorted as reported.
		std::vector<std::string> NavigationCodes(Test::AutomationFixture& setup)
		{
			const Result<Json> report = setup.Call("project.validate", Json::object());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			std::vector<std::string> codes;
			for (const Json& diagnostic : (*report)["diagnostics"])
			{
				for (const char* code : { "NAV_MESH_NOT_BAKED", "NAV_MESH_STALE" })
				{
					if (diagnostic["code"] == Json(code))
						codes.emplace_back(code);
				}
			}
			return codes;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("NavigationMethods: navigation.bake bakes the target scene and answers BakeNavigation's refusal")
		{
			Test::AutomationFixture setup("NavigationBakeMethod");
			// The active scene has no Navigation component yet.
			const Result<Json> none = setup.Call("navigation.bake", Json::object());
			REQUIRE_FALSE(none.has_value());
			CHECK(none.error().GetCode() == ErrorCode::NotFound);
			const Result<Json> elsewhere = setup.Call("navigation.bake", Json{ { "scene", "Assets/Scenes/Other.scene" } });
			REQUIRE_FALSE(elsewhere.has_value());
			CHECK(elsewhere.error().GetCode() == ErrorCode::NotFound);
			const Result<Json> unknown = setup.Call("navigation.bake", Json{ { "agents", 1 } });
			REQUIRE_FALSE(unknown.has_value());

			static_cast<void>(Test::AddNavigationArena(setup.GetEditor().GetScene()));
			const Result<Json> baked = setup.Call("navigation.bake", Json{ { "scene", "Assets/Scenes/Main.scene" } });
			REQUIRE_MESSAGE(baked.has_value(), baked.error().ToString());
			CHECK((*baked)["path"] == Json("Assets/Navigation/Scenes/Main.navmesh"));
			CHECK((*baked)["changed"] == Json(true));
			CHECK((*baked)["asset"]["type"] == Json("NavMesh"));
			CHECK((*baked)["entity"]["name"] == Json("Navigation"));
			REQUIRE((*baked)["inputHash"].is_string());
			CHECK((*baked)["inputHash"].dump().size() == 18); // 16 hexadecimal digits and the quotes
			REQUIRE((*baked)["agents"].size() == 2);
			CHECK((*baked)["agents"][1]["name"] == Json("Large"));
			CHECK((*baked)["agents"][1]["polygons"] > Json(0));
			CHECK((*baked)["skippedBodies"].empty());
			const Result<Json> again = setup.Call("navigation.bake", Json::object());
			REQUIRE(again.has_value());
			CHECK((*again)["changed"] == Json(false));
			CHECK((*again)["asset"] == (*baked)["asset"]);
		}

		// World Tree's brief 1 ("project.validate warns when the static geometry or the settings changed after the bake,
		// naming the scene") and Roadmap M30's acceptance "moving a wall makes project.validate report the bake as stale".
		TEST_CASE("NavigationMethods: project.validate reports a scene not baked, then stale after a wall moves, until it is baked again")
		{
			Test::AutomationFixture setup("NavigationValidate");
			Scene& scene = setup.GetEditor().GetScene();
			static_cast<void>(Test::AddNavigationArena(scene));
			CHECK(NavigationCodes(setup) == std::vector<std::string>{ "NAV_MESH_NOT_BAKED" });
			REQUIRE(setup.Call("navigation.bake", Json::object()).has_value());
			CHECK(NavigationCodes(setup).empty());

			scene.FindEntityByPath("/Pillar").Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.x += 1.0f;
			});
			CHECK(NavigationCodes(setup) == std::vector<std::string>{ "NAV_MESH_STALE" });
			const Result<Json> report = setup.Call("project.validate", Json::object());
			REQUIRE(report.has_value());
			bool named = false;
			for (const Json& diagnostic : (*report)["diagnostics"])
			{
				if (diagnostic["code"] == Json("NAV_MESH_STALE"))
					named = diagnostic.dump().contains("Main.scene");
			}
			CHECK(named);

			// A setting changed after the bake is stale too; a bake clears it.
			REQUIRE(setup.Call("navigation.bake", Json::object()).has_value());
			CHECK(NavigationCodes(setup).empty());
			scene.FindEntityByPath("/Navigation").Patch<NavigationComponent>([](NavigationComponent& navigation)
			{
				navigation.CellSize = 0.25f;
			});
			CHECK(NavigationCodes(setup) == std::vector<std::string>{ "NAV_MESH_STALE" });
			// An input that can no longer be read (a layer the project does not define) is stale too.
			scene.FindEntityByPath("/Navigation").Patch<NavigationComponent>([](NavigationComponent& navigation)
			{
				navigation.CellSize = 0.2f;
				navigation.Layers = { "Nowhere" };
			});
			CHECK(NavigationCodes(setup) == std::vector<std::string>{ "NAV_MESH_STALE" });
			// A Navigation whose NavMesh is cleared is not baked.
			scene.FindEntityByPath("/Navigation").Patch<NavigationComponent>([](NavigationComponent& navigation)
			{
				navigation.NavMesh = TypedAssetHandle<AssetType::NavMesh>();
			});
			CHECK(NavigationCodes(setup) == std::vector<std::string>{ "NAV_MESH_NOT_BAKED" });
		}
	}

}
