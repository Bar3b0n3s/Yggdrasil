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
#include <optional>
#include <vector>

namespace Engine {

	static void PublishSceneBindingPrefab(Test::ScriptTestFixture& fixture, AssetHandle handle, AssetHandle script = {})
	{
		Test::SceneTestFixture source;
		const Entity root = source.GetScene().CreateEntity("PrefabRoot");
		const Entity child = source.GetScene().CreateEntity("PrefabChild", root);
		if (script.IsValid())
		{
			root.AddComponent<ScriptComponent>().Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(script), .Fields = {}, .ExecutionOrder = 0 });
			child.AddComponent<ScriptComponent>().Scripts.push_back(ScriptSlot{ .Script = TypedAssetHandle<AssetType::Script>(script), .Fields = {}, .ExecutionOrder = 0 });
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
			fixture.CurrentScene = AssetHandle(101);
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
assert(Scene.GetCurrent() == Assets.Load("Assets/Next.scene"))
Scene.Reload({Round = 2})
assert(root:IsValid())
return true
)");
			if (!result)
				INFO(result.error().ToString());
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.RequestedScene == AssetHandle(101));
			CHECK(fixture.RequestedParameters == Json({ { "Nested", { { "Enabled", true } } }, { "Score", 12 }, { "Values", Json::array({ 1, 2, 3 }) } }));
			CHECK(fixture.Parameters["Score"] == 7);
			CHECK(fixture.ReloadRequested);
			CHECK(fixture.RequestedReloadParameters == std::optional<Json>(Json{ { "Round", 2 } }));
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
	function() Scene.Reload(7) end,
	function() Scene.Reload(cycle) end,
	function() Scene.Reload({1, 2}) end,
	function() Scene.Reload({Bad = function() end}) end,
	function() Scene.Reload({Bad = math.huge}) end,
} do assert(not pcall(fn)) end
return Scene.GetEntityCount() == 0
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK_FALSE(fixture.RequestedScene.IsValid());
			CHECK_FALSE(fixture.ReloadRequested);
			CHECK(fixture.ExternalMutations.empty());
		}

		// The native test game's friction NT-05: a game's native components ("Native.<Name>") have no script proxy
		// (Docs/Decisions/0030-m19-contract.md decision 26), so Scene.FindAllWithComponent of one fails with a hint naming
		// native.getComponents instead of suggesting a built-in name; a misspelt built-in name keeps its suggestion.
		TEST_CASE("SceneBindings: FindAllWithComponent of a native component fails with a hint naming native.getComponents")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
local ok, message = pcall(Scene.FindAllWithComponent, "Native.Spinner")
assert(not ok and string.find(tostring(message), "native.getComponents", 1, true) ~= nil, tostring(message))
ok, message = pcall(Scene.FindAllWithComponent, "Transfom")
assert(not ok and string.find(tostring(message), "Transform", 1, true) ~= nil, tostring(message))
assert(string.find(tostring(message), "native.getComponents", 1, true) == nil, tostring(message))
return true
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
		}

		TEST_CASE("SceneBindings: an instantiation position that the parent cannot represent fails before anything is created")
		{
			Test::ScriptTestFixture fixture;
			PublishSceneBindingPrefab(fixture, AssetHandle(100));
			REQUIRE(fixture.Start().has_value());
			// Finite inputs whose local translation overflows float: under a tiny parent scale, and against a huge parent
			// translation. TransformSystem::SetWorldPosition would assert (Debug, Release) or store Inf (Dist).
			const auto result = fixture.Evaluate(R"(
local prefab = Assets.Load("Assets/ScriptPrefab.prefab")
local tiny = Scene.CreateEntity("Tiny")
tiny.Transform.Scale = vector.create(0.00011, 0.00011, 0.00011)
local far = Scene.CreateEntity("Far")
far.Transform.Translation = vector.create(3e38, 0, 0)
local before = Scene.GetEntityCount()
local ok, message = pcall(Scene.Instantiate, prefab, vector.create(1e36, 0, 0), nil, tiny)
assert(not ok and string.find(message, "Scene.Instantiate", 1, true) and string.find(message, "cannot be represented relative to parent", 1, true), message)
ok, message = pcall(Scene.Instantiate, prefab, vector.create(-3e38, 0, 0), Quat.Identity(), far)
assert(not ok and string.find(message, "cannot be represented relative to parent", 1, true), message)
assert(Scene.GetEntityCount() == before)
local child = Scene.Instantiate(prefab, vector.create(0.01, 0, 0), nil, tiny)
assert(child:GetParent() == tiny)
local root = Scene.Instantiate(prefab, vector.create(1e36, 0, 0))
assert(root.Transform.Translation == vector.create(1e36, 0, 0))
return true
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.Errors.empty());
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
assert(not pcall(Scene.Reload))
assert(not pcall(Scene.Reload, {Round = 2}))
assert(Scene.GetCurrent() == nil)
return Scene.GetEntityCount() == 1
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(target.IsValid());
			CHECK_FALSE(fixture.RequestedScene.IsValid());
			CHECK_FALSE(fixture.ReloadRequested);
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

		// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): the finds read an index that a structural change, a Name
		// write or a Tags write refreshes, and FindAllWithComponent and GetPrimaryCamera read the component's view, so each
		// still returns what a walk of the scene in canonical order finds: renamed and retagged entities at once, disabled
		// entities included, destroyed ones not, and the order a reparent or a sibling move makes.
		TEST_CASE("SceneBindings: the finds follow renames, tag writes, reparenting and sibling moves in canonical order")
		{
			Test::ScriptTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity first = scene.CreateEntity("First");
			Entity second = scene.CreateEntity("Second");
			Entity third = scene.CreateEntity("Third");
			first.AddTag("Group");
			third.AddTag("Group");
			// Components added against the canonical order, so a view's storage order is not the answer.
			third.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			first.AddComponent<CameraComponent>(CameraComponent{ .Primary = false });
			second.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			REQUIRE(fixture.Start().has_value());
			const auto firstPass = fixture.Evaluate(R"(
local first, second, third = Scene.FindByName("First"), Scene.FindByName("Second"), Scene.FindByName("Third")
assert(first ~= nil and second ~= nil and third ~= nil)
assert(Scene.FindByName("Target") == nil and Scene.FindByTag("Group") == first)
local cameras = Scene.FindAllWithComponent("Camera")
assert(#cameras == 3 and cameras[1] == first and cameras[2] == second and cameras[3] == third)
assert(Scene.GetPrimaryCamera() == second)
-- A rename and tag writes move no structure; the next find sees them.
third.Name = "Target"
assert(Scene.FindByName("Target") == third and Scene.FindByName("Third") == nil)
second.Name = "Target"
local named = Scene.FindAllByName("Target")
assert(#named == 2 and named[1] == second and named[2] == third)
first:RemoveTag("Group")
second:AddTag("Group")
local tagged = Scene.FindAllByTag("Group")
assert(#tagged == 2 and tagged[1] == second and tagged[2] == third)
-- Below First, Third comes before Second in canonical order.
third:SetParent(first)
named = Scene.FindAllByName("Target")
assert(#named == 2 and named[1] == third and named[2] == second)
assert(Scene.FindByTag("Group") == third)
cameras = Scene.FindAllWithComponent("Camera")
assert(#cameras == 3 and cameras[1] == first and cameras[2] == third and cameras[3] == second)
assert(Scene.GetPrimaryCamera() == third)
-- A disabled entity is still found by name and tag, but is no primary camera.
first:SetActive(false)
assert(Scene.FindByName("Target") == third and Scene.FindByTag("Group") == third)
assert(Scene.GetPrimaryCamera() == second)
first:SetActive(true)
return true
)");
			REQUIRE_MESSAGE(firstPass.has_value(), (firstPass ? "" : firstPass.error().ToString()));
			CHECK(firstPass->Value.Get() == Json(true));

			// Host writes reach the next find too: a rename, a tag, a sibling move to the front of the roots and a destruction.
			second.SetName("Moved");
			second.AddTag("Late");
			REQUIRE(scene.SetParent(second, Entity{}, 0u).has_value());
			scene.DestroyEntity(third);
			const auto secondPass = fixture.Evaluate(R"(
local moved = Scene.FindByName("Moved")
assert(moved ~= nil and Scene.FindByName("Target") == nil and Scene.FindByTag("Late") == moved)
assert(#Scene.FindAllByTag("Group") == 1 and Scene.FindByTag("Group") == moved)
local cameras = Scene.FindAllWithComponent("Camera")
assert(#cameras == 2 and cameras[1] == moved and cameras[2] == Scene.FindByName("First"))
assert(Scene.GetPrimaryCamera() == moved)
return true
)");
			REQUIRE_MESSAGE(secondPass.has_value(), (secondPass ? "" : secondPass.error().ToString()));
			CHECK(secondPass->Value.Get() == Json(true));
			CHECK(fixture.Errors.empty());
		}
	}

}
