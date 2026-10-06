#include "TestsPCH.h"

#include "Engine/Asset/AssetType.h"

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("AssetType: names round-trip and parsing is case-sensitive")
		{
			constexpr AssetType Types[] = {
				AssetType::None,
				AssetType::Scene,
				AssetType::Prefab,
				AssetType::Mesh,
				AssetType::Material,
				AssetType::Texture,
				AssetType::Environment,
				AssetType::AudioClip,
				AssetType::Script,
				AssetType::Font,
				AssetType::Replay,
			};
			for (const AssetType type : Types)
			{
				const std::optional<AssetType> parsed = AssetTypeFromString(AssetTypeToString(type));
				REQUIRE(parsed.has_value());
				CHECK(*parsed == type);
			}

			static_assert(AssetTypeToString(AssetType::AudioClip) == "AudioClip");
			static_assert(AssetTypeFromString("Mesh") == AssetType::Mesh);
			CHECK_FALSE(AssetTypeFromString("mesh").has_value());
			CHECK_FALSE(AssetTypeFromString("Dependency").has_value());
			CHECK_FALSE(AssetTypeFromString("").has_value());
		}

		TEST_CASE("AssetType: persisted values never change")
		{
			// The cooked header stores the underlying value (§6.8).
			static_assert(std::to_underlying(AssetType::None) == 0);
			static_assert(std::to_underlying(AssetType::Scene) == 1);
			static_assert(std::to_underlying(AssetType::Replay) == 10);
			CHECK(AssetTypeToString(static_cast<AssetType>(999)) == "None");
		}
	}

}
