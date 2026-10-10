#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Asset/DocumentData.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <vector>

namespace Engine {

	namespace Test {

		void RunAssetsBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true, .ReadOnly = true });
			auto asset = CreateRef<SceneData>();
			asset->Document = CreateRef<const Json>(Json::object());
			fixture.GetAssetManager().Publish(AssetHandle(101), asset, "Assets/Scenes/Test.scene");
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
local asset = Assets.Load("Assets/Scenes/Test.scene")
assert(asset ~= nil and Assets.IsValid(asset))
assert(Assets.GetPath(asset) == "Assets/Scenes/Test.scene")
assert(Assets.GetType(asset) == "Scene")
assert(Assets.IsValid(Assets.Load("0000000000000065")))
assert(Assets.Load("Assets/Missing.scene") == nil)
assert(not Assets.IsValid(nil))
return true
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.ExternalMutations.empty());
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			for (const auto& counter : coverage->Members)
				if (counter.Owner == "Assets")
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
		TEST_CASE("AssetsBindings: real asset references resolve paths and types in read-only evaluation")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunAssetsBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("AssetsBindings: invalid values never coerce to asset handles")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
assert(not pcall(Assets.Load, 101))
assert(not pcall(Assets.Load, "Assets/\000Test.scene"))
assert(not pcall(Assets.Load, string.char(255)))
for _, value in {101, "0000000000000065", {}, Color.New(1, 1, 1), Quat.Identity()} do
	assert(not pcall(Assets.IsValid, value))
	assert(not pcall(Assets.GetPath, value))
	assert(not pcall(Assets.GetType, value))
end
assert(not pcall(Assets.GetPath, nil) and not pcall(Assets.GetType, nil))
return true
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.ExternalMutations.empty());
		}
	}

}
