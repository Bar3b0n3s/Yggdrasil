#include "TestsPCH.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/NavMeshData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Random.h"
#include "Engine/Navigation/NavigationWorld.h"
#include "Engine/Scene/Components/NavigationComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Support/AssetTestFixture.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"
#include "Support/NavigationTestScene.h"
#include "Support/SceneTestFixture.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Navigation script namespace (Scripting/Bindings/Navigation.cpp; Docs/Decisions/0045-m30-navigation.md decisions 10 and
// 11), end to end: a play session of the baked arena builds its navigation, and scripts evaluated in it query it. Each result
// is compared with the same query of a NavigationWorld built from the same mesh (Navigation/NavigationWorldTests.cpp tests
// the queries themselves).

namespace Engine {

	namespace {

		Json CallOrFail(Test::AutomationFixture& setup, std::string_view method, const Json& params)
		{
			const Result<Json> result = setup.Call(method, params);
			INFO(method);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return *result;
		}

		// Evaluates `code` in the play session and returns its value.
		Json Evaluate(Test::AutomationFixture& setup, std::string_view code)
		{
			return CallOrFail(setup, "script.eval", Json{ { "context", "play" }, { "code", std::string(code) } })["value"];
		}

		Json ToJson(const glm::vec3& value)
		{
			return Json::array({ static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z) });
		}

		// Bakes the arena into the active scene (Assets/Scenes/Main.scene), saves it, starts a lockstep play session of it and
		// returns a NavigationWorld of the same mesh.
		Scope<NavigationWorld> PlayArena(Test::AutomationFixture& setup)
		{
			static_cast<void>(Test::AddNavigationArena(setup.GetEditor().GetScene()));
			CallOrFail(setup, "navigation.bake", Json::object());
			CallOrFail(setup, "scene.save", Json::object());
			const AssetHandle handle = setup.GetEditor().GetScene().FindEntityByPath("/Navigation").GetComponent<NavigationComponent>().NavMesh.GetHandle();
			const Result<AssetRef<Asset>> loaded = setup.GetEditor().GetAssets().Load(handle);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			const AssetRef<NavMeshData> mesh = AssetCast<NavMeshData>(*loaded);
			REQUIRE(mesh != nullptr);
			Result<Scope<NavigationWorld>> world = NavigationWorld::Create(*mesh);
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			CallOrFail(setup, "play.start", Json{ { "lockstep", true } });
			return std::move(*world);
		}

	}

	namespace {

		// The namespace's work in `mode` on the arena's bake, in a script fixture whose host serves the world, adding its API
		// coverage to `snapshots` ("ScriptApi: every registered function, method, property, operator and constructor has a
		// test").
		void NavigationCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			NavMeshData mesh;
			{
				Test::AssetTestFixture assets;
				assets.OpenProject();
				Test::SceneTestFixture arena;
				static_cast<void>(Test::AddNavigationArena(arena.GetScene()));
				mesh = Test::BakeNavigationScene(arena.GetScene(), assets.GetManager());
			}
			// Declared before the fixture, which stops its VM before the world goes.
			Result<Scope<NavigationWorld>> world = NavigationWorld::Create(mesh);
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			fixture.Navigation = world->get();
			REQUIRE(fixture.Start());
			const auto result = fixture.Evaluate(R"(
local path, complete = Navigation.FindPath(vector.create(-6, 0, -8), vector.create(-6, 0, -2), "Large")
assert(path ~= nil and complete and #path >= 3)
local point = Navigation.ClosestPoint(vector.create(0, 0.9, 0))
assert(point ~= nil and math.abs(point.y) < 0.25)
local hit = Navigation.Raycast(vector.create(-8, 0, 0), vector.create(-1, 0, 0), "Small")
assert(hit ~= nil and not hit.Clear and hit.Normal.x < -0.99)
assert(Navigation.RandomPoint(nil, nil, nil, Random.New(4)) ~= nil)
assert(Navigation.RandomPoint(vector.create(-6, 0, 2), 3) ~= nil)
assert(not pcall(function() return Navigation.FindPath(vector.zero, vector.zero, "Huge") end))
return true
)");
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.Errors.empty());
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

	}

	namespace Test {

		void RunNavigationBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			NavigationCoverage(mode, snapshots);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptNavigation: a host's navigation answers every member of the namespace")
		{
			std::vector<ScriptApiCoverage> snapshots;
			NavigationCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("ScriptNavigation: FindPath, ClosestPoint and Raycast answer as the session's baked mesh does")
		{
			Test::AutomationFixture setup("ScriptNavigationQueries");
			Scope<NavigationWorld> world = PlayArena(setup);

			// Around the lava for the large agent type: the corners and whether they reach the goal.
			const std::optional<NavigationPath> path = world->FindPath(1, glm::vec3(-6.0f, 0.0f, -8.0f), glm::vec3(-6.0f, 0.0f, -2.0f), MaxScriptNavigationPathCorners);
			REQUIRE(path.has_value());
			Json corners = Json::array();
			for (const glm::vec3& corner : path->Corners)
				corners.push_back(ToJson(corner));
			CHECK(Evaluate(setup, "local path, complete = Navigation.FindPath(vector.create(-6, 0, -8), vector.create(-6, 0, -2), 'Large') return {complete, path}")
				== Json::array({ path->Complete, corners }));
			// Without an agent type, the first: Small.
			const std::optional<NavigationPath> small = world->FindPath(0, glm::vec3(-6.0f, 0.0f, 0.0f), glm::vec3(-2.0f, 0.0f, 0.0f), MaxScriptNavigationPathCorners);
			REQUIRE(small.has_value());
			CHECK(Evaluate(setup, "local path = Navigation.FindPath(vector.create(-6, 0, 0), vector.create(-2, 0, 0)) return #path")
				== Json(static_cast<double>(small->Corners.size())));
			// An end off the mesh: nil and false.
			CHECK(Evaluate(setup, "local path, complete = Navigation.FindPath(vector.create(-6, 50, 0), vector.create(-2, 0, 0)) return {path == nil, complete}")
				== Json::array({ true, false }));

			const std::optional<glm::vec3> closest = world->FindClosestPoint(0, glm::vec3(-4.0f, 0.0f, 0.0f), 3.0f);
			REQUIRE(closest.has_value());
			CHECK(Evaluate(setup, "return Navigation.ClosestPoint(vector.create(-4, 0, 0), 3)") == ToJson(*closest));
			CHECK(Evaluate(setup, "return Navigation.ClosestPoint(vector.create(-4, 3.5, 0), 0.4) == nil") == Json(true));

			const std::optional<NavigationRaycast> ray = world->Raycast(0, glm::vec3(-8.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f));
			REQUIRE(ray.has_value());
			CHECK(Evaluate(setup, "local hit = Navigation.Raycast(vector.create(-8, 0, 0), vector.create(-1, 0, 0)) return {hit.Clear, hit.Point, hit.Normal, hit.Fraction}")
				== Json::array({ ray->Clear, ToJson(ray->Point), ToJson(ray->Normal), static_cast<double>(ray->Fraction) }));
			CHECK(Evaluate(setup, "return Navigation.Raycast(vector.create(0, 0, -2), vector.create(0, 0, 2), 'Small').Clear") == Json(true));
			CHECK(Evaluate(setup, "return Navigation.Raycast(vector.create(0, 40, 0), vector.create(0, 0, 2)) == nil") == Json(true));
			CallOrFail(setup, "play.stop", Json::object());
		}

		TEST_CASE("ScriptNavigation: RandomPoint draws from a generator as the mesh does, or from the session's stream")
		{
			Test::AutomationFixture setup("ScriptNavigationRandom");
			Scope<NavigationWorld> world = PlayArena(setup);

			Random anywhere(5);
			const std::optional<glm::vec3> first = world->FindRandomPoint(0, anywhere);
			const std::optional<glm::vec3> second = world->FindRandomPoint(0, anywhere);
			REQUIRE((first.has_value() && second.has_value()));
			CHECK(Evaluate(setup, "local generator = Random.New(5) return {Navigation.RandomPoint(nil, nil, nil, generator), Navigation.RandomPoint(nil, nil, nil, generator)}")
				== Json::array({ ToJson(*first), ToJson(*second) }));
			Random around(6);
			const std::optional<glm::vec3> near = world->FindRandomPointAround(1, glm::vec3(-6.0f, 0.0f, 2.0f), 3.0f, around);
			REQUIRE(near.has_value());
			CHECK(Evaluate(setup, "return Navigation.RandomPoint(vector.create(-6, 0, 2), 3, 'Large', Random.New(6))") == ToJson(*near));

			// The session's stream: each call draws, so two calls give two points of the mesh.
			const Json drawn = Evaluate(setup, "return {Navigation.RandomPoint(), Navigation.RandomPoint()}");
			REQUIRE(drawn.size() == 2);
			CHECK(drawn[0] != drawn[1]);
			CallOrFail(setup, "play.stop", Json::object());
		}

		TEST_CASE("ScriptNavigation: invalid arguments and unknown agent types raise before any query")
		{
			Test::AutomationFixture setup("ScriptNavigationErrors");
			static_cast<void>(PlayArena(setup));
			for (const std::string_view call : {
					 "Navigation.FindPath(vector.create(2e9, 0, 0), vector.zero)",
					 "Navigation.FindPath(vector.zero, vector.zero, 'Huge')",
					 "Navigation.ClosestPoint(vector.zero, 0)",
					 "Navigation.ClosestPoint(vector.zero, 20000)",
					 "Navigation.Raycast(vector.zero, vector.create(0, 0, -2e9))",
					 "Navigation.RandomPoint(vector.zero)",
					 "Navigation.RandomPoint(nil, 3)",
					 "Navigation.RandomPoint(nil, nil, 'Huge', Random.New(1))",
				 })
			{
				CAPTURE(call);
				CHECK(Evaluate(setup, std::string("return (pcall(function() return ") + std::string(call) + " end))") == Json(false));
			}
			CHECK(Evaluate(setup, "return (pcall(function() return Navigation.FindPath(vector.zero, vector.zero, 'Small') end))") == Json(true));
			CallOrFail(setup, "play.stop", Json::object());
		}

		TEST_CASE("ScriptNavigation: a NavMesh that does not load leaves the scene without navigation, with a warning")
		{
			Test::AutomationFixture setup("ScriptNavigationUnloaded");
			Entity navigation = setup.GetEditor().GetScene().CreateEntity("Navigation");
			NavigationComponent component;
			component.NavMesh.SetHandle(AssetHandle{ 0x5eed0001u });
			navigation.AddComponent<NavigationComponent>(component);
			CallOrFail(setup, "scene.save", Json::object());
			{
				const Test::ExpectLog warning(LogLevel::Warn, "does not load");
				CallOrFail(setup, "play.start", Json{ { "lockstep", true } });
			}
			CHECK(Evaluate(setup, "return Navigation.ClosestPoint(vector.zero) == nil") == Json(true));
			CallOrFail(setup, "play.stop", Json::object());
		}

		TEST_CASE("ScriptNavigation: a scene without a baked mesh finds nothing, and Scene.Load builds the next scene's navigation")
		{
			Test::AutomationFixture setup("ScriptNavigationScenes");
			// Assets/Scenes/Other.scene: the arena, baked.
			CallOrFail(setup, "scene.new", Json{ { "path", "Assets/Scenes/Other.scene" } });
			static_cast<void>(Test::AddNavigationArena(setup.GetEditor().GetScene()));
			CallOrFail(setup, "navigation.bake", Json::object());
			CallOrFail(setup, "scene.save", Json::object());
			// Assets/Scenes/Main.scene: a Navigation component never baked.
			CallOrFail(setup, "scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } });
			Entity navigation = setup.GetEditor().GetScene().CreateEntity("Navigation");
			navigation.AddComponent<NavigationComponent>(NavigationComponent{});
			CallOrFail(setup, "scene.save", Json::object());

			CallOrFail(setup, "play.start", Json{ { "lockstep", true } });
			CHECK(Evaluate(setup,
					  "local path, complete = Navigation.FindPath(vector.zero, vector.one, 'Anything') "
					  "return {path == nil, complete, Navigation.ClosestPoint(vector.zero) == nil, Navigation.Raycast(vector.zero, vector.one) == nil, "
					  "Navigation.RandomPoint() == nil}")
				== Json::array({ true, false, true, true, true }));
			CallOrFail(setup, "script.eval", Json{ { "context", "play" }, { "code", "Scene.Load(Assets.Load('Assets/Scenes/Other.scene'), {})" } });
			CallOrFail(setup, "play.step", Json{ { "ticks", 1 } });
			CHECK(Evaluate(setup, "local _, complete = Navigation.FindPath(vector.create(-6, 0, -8), vector.create(-6, 0, -2)) return complete") == Json(true));
			CallOrFail(setup, "play.stop", Json::object());
		}
	}

}
