#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <span>

// Loaders turn cooked bytes into loaded assets (Architecture §3 rule 4, §7.2 "One load path"). The same loader reads a
// Library/Cache entry in the editor (EditorAssetManager) and a pak entry in an exported game (RuntimeAssetManager), so every
// editor session exercises the shipping loaders.

namespace Engine {

	class TypeRegistry;

	// What a loader may use besides the bytes.
	struct AssetLoadContext
	{
		// The context's frozen type registry (MaterialData is a reflected struct, §5.4); never null.
		const TypeRegistry* Registry = nullptr;
		// The asset being loaded, for error messages.
		AssetHandle Handle{};
	};

	// The loader of one AssetType. Loaders are stateless: Load is a pure function of the bytes and the context, safe to call
	// from any number of jobs at once.
	class IAssetLoader
	{
	public:
		virtual ~IAssetLoader() = default;

		// The type this loader reads (never None).
		[[nodiscard]] virtual AssetType GetType() const = 0;

		// Decodes one complete cooked artifact (CookedHeader + payload) of GetType() into a loaded asset, validating every
		// byte (cooked data is untrusted: a corrupted cache entry or pak entry, §6.8). Never asserts on data. Errors: those
		// of ReadCookedArtifact and of the type's reader (Parse, Validation, UnsupportedVersion), with the handle as context.
		[[nodiscard]] virtual Result<AssetRef<Asset>> Load(std::span<const std::byte> cooked, const AssetLoadContext& context) const = 0;
	};

}
