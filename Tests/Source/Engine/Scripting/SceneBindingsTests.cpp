#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Prefab.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <format>

#include <vector>

namespace Engine {

	static void PublishSceneBindingPrefab(Test::ScriptTestFixture& fixture, AssetHandle handle, AssetHandle script = {})
	{
		Test::SceneTestFixture source;
		const Entity root = source.GetScene().CreateEntity("PrefabRoot");
		const Entity child = source.GetScene().CreateEntity("PrefabChild", root);
		if (script.IsValid())
		{
			root.AddComponent<ScriptComponent>().Script = TypedAssetHandle<AssetType::Script>(script);
			child.AddComponent<ScriptComponent>().Script = TypedAssetHandle<AssetType::Script>(script);
		}
		const auto prefab = Prefab::CreateFromEntity(root, "ScriptPrefab");
		REQUIRE(prefab.has_value());
		const auto document = prefab->ToJson();
		REQUIRE(document.has_value());
		auto data = CreateRef<PrefabData>();
		data->Document = CreateRef<const Json>(*document);
		fixture.GetAssetManager().Publish(handle, data, "Assets/ScriptPrefab.prefab");
	}

	namespace Test {

		void RunSceneBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			PublishSceneBindingPrefab(fixture, AssetHandle(100));
			auto scene = CreateRef<SceneData>();
			scene->Document = CreateRef<const Json>(Json::object());
			fixture.GetAssetManager().Publish(AssetHandle(101), scene, "Assets/Next.scene");
			fixture.Parameters = { { "Score", 7 }, { "Names", Json::array({ "A", "B" }) } };
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
assert(Scene.GetName() == "Test" and Scene.GetEntityCount() == 0)
local root = Scene.CreateEntity("Root")
local a = Scene.CreateEntity("Duplicate", root)
local b = Scene.CreateEntity("Duplicate", root)
a:AddTag("TestTag")
b:AddTag("TestTag")
assert(Scene.FindByID(a.ID) == a)
assert(Scene.FindByID("0000000000000000") == nil)
assert(Scene.FindByName("Duplicate") == a)
local named, tagged = Scene.FindAllByName("Duplicate"), Scene.FindAllByTag("TestTag")
assert(#named == 2 and named[1] == a and named[2] == b)
assert(#tagged == 2 and tagged[1] == a and tagged[2] == b)
assert(Scene.FindByTag("TestTag") == a)
assert(Scene.FindByName("Missing") == nil and Scene.FindByTag("Missing") == nil)
assert(#Scene.FindAllByName("Missing") == 0 and #Scene.FindAllByTag("Missing") == 0)
assert(Scene.FindByPath("/Root/Duplicate") == nil)
assert(Scene.FindByPath("/Root/Duplicate[1]") == b)
assert(Scene.FindByPath("bad path") == nil)
local components = Scene.FindAllWithComponent("Transform")
assert(#components == 3 and components[1] == root and components[2] == a and components[3] == b)
assert(Scene.GetPrimaryCamera() == nil)
local camera = b:AddComponent("Camera", {Primary = true})
assert(Scene.GetPrimaryCamera() == b)
b:SetActive(false)
assert(Scene.GetPrimaryCamera() == nil)
b:SetActive(true)
local instance = Scene.Instantiate(Assets.Load("Assets/ScriptPrefab.prefab"), vector.create(2, 3, 4), Quat.Identity(), root)
assert(instance:GetParent() == root and #instance:GetChildren() == 1)
assert(instance.Transform.WorldPosition == vector.create(2, 3, 4))
Scene.Destroy(a)
assert(not a:IsValid() and Scene.FindByID(a.ID) == nil)
assert(#Scene.FindAllByName("Duplicate") == 1 and Scene.GetEntityCount() == 4)
local params = Scene.GetLoadParameters()
assert(params.Score == 7 and params.Names[2] == "B")
params.Score = 99
assert(Scene.GetLoadParameters().Score == 7)
Scene.Load(Assets.Load("Assets/Next.scene"), {Score = 12, Nested = {Enabled = true}, Values = {1, 2, 3}})
assert(root:IsValid() and Scene.GetName() == "Test")
return true
)");
			if (!result)
				INFO(result.error().ToString());
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.RequestedScene == AssetHandle(101));
			CHECK(fixture.RequestedParameters == Json({ { "Nested", { { "Enabled", true } } }, { "Score", 12 }, { "Values", Json::array({ 1, 2, 3 }) } }));
			CHECK(fixture.Parameters["Score"] == 7);
			CHECK_FALSE(fixture.GetEngine()->IsStopped());
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			for (const auto& counter : coverage->Members)
				if (counter.Owner == "Scene")
				{
					CAPTURE(counter.Member);
					CHECK(counter.Calls > 0);
				}
			const auto snapshot = fixture.GetApi().GetCoverage(mode);
			REQUIRE(snapshot);
			snapshots.push_back(*snapshot);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("SceneBindings: creation queries destruction and load requests cover every registered member")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunSceneBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("SceneBindings: prefab OnCreate runs for the complete subtree before returning")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle script(102);
			REQUIRE(fixture.AddScript(script, "Assets/Created.luau", R"(
local Created = {}
function Created.OnCreate(self) self.Entity:AddTag("Created") end
return Script.Define("Created", Created)
)")
					.has_value());
			PublishSceneBindingPrefab(fixture, AssetHandle(100), script);
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
local entity = Scene.Instantiate(Assets.Load("Assets/ScriptPrefab.prefab"))
assert(entity:HasTag("Created"))
local child = entity:GetChildren()[1]
assert(child:HasTag("Created") and child:GetScript() ~= nil)
return true
)");
			if (!result)
				INFO(result.error().ToString());
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("SceneBindings: malformed values cannot mutate a scene or queue a load")
		{
			Test::ScriptTestFixture fixture;
			PublishSceneBindingPrefab(fixture, AssetHandle(100));
			auto scene = CreateRef<SceneData>();
			scene->Document = CreateRef<const Json>(Json::object());
			fixture.GetAssetManager().Publish(AssetHandle(101), scene, "Assets/Next.scene");
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
local prefab, scene = Assets.Load("Assets/ScriptPrefab.prefab"), Assets.Load("Assets/Next.scene")
local cycle = {}; cycle.Self = cycle
for _, fn in {
	function() Scene.CreateEntity(1) end,
	function() Scene.CreateEntity("bad\000name") end,
	function() Scene.CreateEntity(string.char(255)) end,
	function() Scene.CreateEntity("Valid", {}) end,
	function() Scene.Instantiate(scene) end,
	function() Scene.Instantiate(prefab, vector.create(math.huge, 0, 0)) end,
	function() Scene.Instantiate(prefab, nil, Color.New(0, 0, 0)) end,
	function() Scene.Destroy({}) end,
	function() Scene.FindByID("123") end,
	function() Scene.FindAllWithComponent("Transfom") end,
	function() Scene.FindAllWithComponent("Relationship") end,
	function() Scene.Load(prefab) end,
	function() Scene.Load(scene, cycle) end,
	function() Scene.Load(scene, {1, 2}) end,
	function() Scene.Load(scene, {Bad = function() end}) end,
	function() Scene.Load(scene, {Bad = math.huge}) end,
} do assert(not pcall(fn)) end
return Scene.GetEntityCount() == 0
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK_FALSE(fixture.RequestedScene.IsValid());
			CHECK(fixture.ExternalMutations.empty());
		}

		TEST_CASE("SceneBindings: read-only mutations are refused before effects")
		{
			Test::ScriptTestFixture fixture({ .ReadOnly = true });
			const Entity target = fixture.GetScene().CreateEntity("Target");
			PublishSceneBindingPrefab(fixture, AssetHandle(100));
			auto scene = CreateRef<SceneData>();
			scene->Document = CreateRef<const Json>(Json::object());
			fixture.GetAssetManager().Publish(AssetHandle(101), scene, "Assets/Next.scene");
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
assert(not pcall(Scene.CreateEntity, "Other"))
assert(not pcall(Scene.Destroy, Scene.FindByName("Target")))
assert(not pcall(Scene.Instantiate, Assets.Load("Assets/ScriptPrefab.prefab")))
assert(not pcall(Scene.Load, Assets.Load("Assets/Next.scene")))
return Scene.GetEntityCount() == 1
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(target.IsValid());
			CHECK_FALSE(fixture.RequestedScene.IsValid());
			CHECK(fixture.ExternalMutations.empty());
		}

		TEST_CASE("SceneBindings: entity limits fail without creating partial prefab subtrees")
		{
			Test::ScriptTestFixture fixture;
			fixture.EntityLimit = 1;
			PublishSceneBindingPrefab(fixture, AssetHandle(100));
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
assert(not pcall(Scene.Instantiate, Assets.Load("Assets/ScriptPrefab.prefab")))
assert(Scene.GetEntityCount() == 0)
local first = Scene.CreateEntity()
assert(first.Name == "Entity")
assert(not pcall(Scene.CreateEntity))
return Scene.GetEntityCount() == 1
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.GetScene().GetEntityCount() == 1);
		}
	}

}
