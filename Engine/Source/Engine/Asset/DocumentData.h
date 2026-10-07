#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <span>

// Scene and prefab assets (Architecture §6.8 "scene, prefab ...: minified canonical JSON"). Asset (layer 2) cannot include
// Scene (layer 3), so the loaded asset is the canonical document; Scene code turns it into entities through the one load
// path, SceneSerializer::FromJson and Prefab::FromJson (Scene/PrefabAsset.h for prefabs by handle). The importers
// (SceneImporter, PrefabImporter) cook only documents that passed strict loading and migration (§7.4), at the current
// format version.

namespace Engine {

	// A loaded scene (AssetType::Scene): its canonical document ("Format": "Scene", current Version). Immutable.
	struct SceneData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Scene;
		// The cooked payload layout (CookedHeader::FormatVersion): the minified canonical document.
		static constexpr uint16_t FormatVersion = 1;

		SceneData()
			: Asset(StaticType)
		{
		}

		Ref<const Json> Document{}; // never null in a loaded asset
	};

	// A loaded prefab (AssetType::Prefab): its canonical document ("Format": "Prefab", current Version). Immutable.
	struct PrefabData : Asset
	{
		static constexpr AssetType StaticType = AssetType::Prefab;
		// The cooked payload layout (CookedHeader::FormatVersion): the minified canonical document.
		static constexpr uint16_t FormatVersion = 1;

		PrefabData()
			: Asset(StaticType)
		{
		}

		Ref<const Json> Document{}; // never null in a loaded asset
	};

	// The complete cooked artifact of a JSON document asset (`type` Scene or Prefab, asserted): CookedHeader + the document
	// written by JsonWriter (Minified). Identical documents give identical bytes. Errors: Validation for a value the
	// canonical writer rejects (non-finite, invalid UTF-8), located.
	[[nodiscard]] Result<Buffer> CookDocument(AssetType type, const Json& document, uint32_t importerVersion);

	// The scene of a cooked artifact: ReadCookedArtifact with AssetType::Scene, then JsonReader::Parse of the payload, whose
	// "Format" must be "Scene" (the structure is checked when the scene is loaded from it). Errors: as ReadCookedArtifact;
	// Parse; Validation for another format.
	[[nodiscard]] Result<AssetRef<SceneData>> LoadCookedScene(std::span<const std::byte> cooked);

	// The prefab of a cooked artifact, as LoadCookedScene with AssetType::Prefab and "Format": "Prefab".
	[[nodiscard]] Result<AssetRef<PrefabData>> LoadCookedPrefab(std::span<const std::byte> cooked);

}
