#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace Engine {

	// The kind of a loadable asset (Architecture §7.1). The underlying values are persisted in the 32-byte cooked header
	// (§6.8, uint16 AssetType), so they never change: append new types at the end. None is "no type": the AssetFilter of a
	// reflected field that accepts any asset, and the type of nothing that can be loaded. Dependency metas (§6.4) have no
	// AssetType of their own.
	enum class AssetType : uint16_t
	{
		None = 0,
		Scene,
		Prefab,
		Mesh,
		Material,
		Texture,
		Environment,
		AudioClip,
		Script,
		Font,
		Replay,
		SpriteSheet, // M18 (ADR 0028's phase-1 ledger; Docs/Decisions/0029-m18-contract.md decision 14)
		// M21 (C21.2; Docs/Decisions/0032-m21-contract.md decision 23): 12. ADR 0028's phase-2 ledger gives 12 to the first of
		// M21 and M22 to land; M21 landed first, so M22 renumbers its own to follow it at its merge (the value lives only in
		// cooked headers, so its checkouts' cooked caches are rebuilt).
		Tilemap,
		// M23 (C23.3; Docs/Decisions/0034-m23-contract.md decision 23): a skin's joints and an animation's channels, the glTF
		// importer's sub-assets. ADR 0028's phase-2 ledger gives M23, the last lander, 14 and 15 after M21's and M22's types;
		// M23's merge of main renumbers them after main's (13 and 14 after M21's Tilemap until M22's StringTable lands; the
		// values live only in cooked headers, so its checkouts' cooked caches are rebuilt).
		Skeleton,
		AnimationClip
	};

	// The PascalCase name of `type` ("AudioClip"), the spelling used in .meta files, reflected AssetFilter metadata, the
	// script API (Field.Asset("AudioClip")) and automation. "None" for AssetType::None and for a value outside the
	// enumeration. Pure and thread-safe.
	[[nodiscard]] constexpr std::string_view AssetTypeToString(AssetType type)
	{
		switch (type)
		{
			case AssetType::None:          return "None";
			case AssetType::Scene:         return "Scene";
			case AssetType::Prefab:        return "Prefab";
			case AssetType::Mesh:          return "Mesh";
			case AssetType::Material:      return "Material";
			case AssetType::Texture:       return "Texture";
			case AssetType::Environment:   return "Environment";
			case AssetType::AudioClip:     return "AudioClip";
			case AssetType::Script:        return "Script";
			case AssetType::Font:          return "Font";
			case AssetType::Replay:        return "Replay";
			case AssetType::SpriteSheet:   return "SpriteSheet";
			case AssetType::Tilemap:       return "Tilemap";
			case AssetType::Skeleton:      return "Skeleton";
			case AssetType::AnimationClip: return "AnimationClip";
		}
		return "None";
	}

	// The type whose name is exactly `name` (case-sensitive, like every enum read from an authored file, §6), or nullopt.
	// "None" parses to AssetType::None. Pure and thread-safe.
	[[nodiscard]] constexpr std::optional<AssetType> AssetTypeFromString(std::string_view name)
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
			AssetType::Skeleton,
			AssetType::AnimationClip,
		};
		for (const AssetType type : Types)
		{
			if (AssetTypeToString(type) == name)
				return type;
		}
		return std::nullopt;
	}

}
