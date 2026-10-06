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
		Replay
	};

	// The PascalCase name of `type` ("AudioClip"), the spelling used in .meta files, reflected AssetFilter metadata, the
	// script API (Field.Asset("AudioClip")) and automation. "None" for AssetType::None and for a value outside the
	// enumeration. Pure and thread-safe.
	[[nodiscard]] constexpr std::string_view AssetTypeToString(AssetType type)
	{
		switch (type)
		{
			case AssetType::None:        return "None";
			case AssetType::Scene:       return "Scene";
			case AssetType::Prefab:      return "Prefab";
			case AssetType::Mesh:        return "Mesh";
			case AssetType::Material:    return "Material";
			case AssetType::Texture:     return "Texture";
			case AssetType::Environment: return "Environment";
			case AssetType::AudioClip:   return "AudioClip";
			case AssetType::Script:      return "Script";
			case AssetType::Font:        return "Font";
			case AssetType::Replay:      return "Replay";
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
		};
		for (const AssetType type : Types)
		{
			if (AssetTypeToString(type) == name)
				return type;
		}
		return std::nullopt;
	}

}
