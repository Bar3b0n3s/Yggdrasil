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
				AssetType::SpriteSheet,
				AssetType::Tilemap,
				AssetType::StringTable,
				AssetType::Skeleton,
				AssetType::AnimationClip,
				AssetType::IrradianceVolume,
				AssetType::Lut,
				AssetType::Heightfield,
				AssetType::NavMesh,
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
			// M18's sprite sheets, phase 1's only new type (ADR 0028's phase-1 ledger: values follow the landing order).
			static_assert(std::to_underlying(AssetType::SpriteSheet) == 11);
			// M21's tilemaps (C21.2): 12; ADR 0028's phase-2 ledger gives 12 to the first of M21 and M22 to land and 13 to the
			// other, which renumbers its own type at its merge. M21 landed first.
			static_assert(std::to_underlying(AssetType::Tilemap) == 12);
			// M22's string tables: 13, renumbered at M22's landing (Docs/Decisions/0033-m22-contract.md decision 10, "Landing").
			static_assert(std::to_underlying(AssetType::StringTable) == 13);
			// M23's skeletons and animation clips: 14 and 15, ADR 0028's phase-2 ledger's values after M21's and M22's types,
			// renumbered at M23's landing (Docs/Decisions/0034-m23-contract.md decision 23, "Landing").
			static_assert(std::to_underlying(AssetType::Skeleton) == 14);
			static_assert(std::to_underlying(AssetType::AnimationClip) == 15);
			// M26's irradiance volumes, LUTs and heightfields (C26.1, Docs/Decisions/0037-m26-contract.md decision 3): 16 to 18,
			// after phase 2's types in the ledger's order; M24 takes no type (its graphs stay Script assets), so they follow
			// AnimationClip unless a sibling lands a type first, when M26's merge of main renumbers them.
			static_assert(std::to_underlying(AssetType::IrradianceVolume) == 16);
			static_assert(std::to_underlying(AssetType::Lut) == 17);
			static_assert(std::to_underlying(AssetType::Heightfield) == 18);
			// M30's navigation meshes (Docs/Decisions/0045-m30-navigation.md decision 4): 19, phase 5's one new type (ADR 0042's
			// phase-5 reservation ledger).
			static_assert(std::to_underlying(AssetType::NavMesh) == 19);
			CHECK(AssetTypeToString(static_cast<AssetType>(999)) == "None");
		}
	}

}
