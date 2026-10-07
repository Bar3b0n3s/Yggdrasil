#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <cstdint>
#include <string>
#include <string_view>

// The asset reference syntax scripts and automation accept (Architecture §7.1 "AssetRef syntax", §13.4). Responses always
// expand a reference to {id, path, type}; resolving a parsed reference to a handle is the registry's job
// (AssetRegistry::Resolve, AssetManager::Resolve).

namespace Engine {

	enum class AssetReferenceKind : uint8_t
	{
		Handle,      // 16 hex digits: the asset handle itself
		ProjectPath, // a file under the project's Assets/ folder, optionally with a sub-asset key
		EnginePath   // an engine path, "engine://Meshes/Cube" (built-in assets, Resources/EngineAssets.json)
	};

	// One parsed reference. Exactly the members of its kind are set.
	struct AssetReference
	{
		AssetReferenceKind Kind = AssetReferenceKind::Handle;
		// Kind Handle: the handle (never null).
		AssetHandle Handle{};
		// Kind ProjectPath: the project:// path of the file ("project://Assets/Models/Track.glb"); kind EnginePath: the
		// engine:// path ("engine://Meshes/Cube").
		VfsPath Path{};
		// The sub-asset key after '#' ("mesh:0:Straight"); empty for the main asset of the file.
		std::string SubAssetKey{};

		bool operator==(const AssetReference&) const = default;
	};

	// Parses `text` as one of (§7.1):
	//   - a handle: exactly 16 hex digits, either case ("3c9f2e7a11d04b88"); the null handle is rejected;
	//   - a project path: "Assets/..." or "project://Assets/..." naming a file below Assets/ ("Assets/Materials/Red.material");
	//   - a sub-asset path: a project path, '#', and a non-empty sub-asset key ("Assets/Models/Track.glb#mesh:0:Straight");
	//     the text is split at the first '#', so an asset file whose name contains '#' is referenced by handle only;
	//   - an engine path: "engine://<path>" ("engine://Meshes/Cube"), optionally with "#<key>".
	// Paths follow VfsPath's rules and are case-sensitive (§4.10). Nothing is trimmed. Pure and thread-safe. Errors:
	// InvalidArgument for empty text, the null handle, a path outside Assets/ or with another scheme, an empty key, or a path
	// VfsPath rejects (the message names the reason), with a hint listing the accepted forms.
	[[nodiscard]] Result<AssetReference> ParseAssetReference(std::string_view text);

	// The readable spelling of `reference` that responses carry as "path": "Assets/Models/Track.glb#mesh:0:Straight" (the
	// project path without its scheme), "engine://Meshes/Cube", or the 16 lowercase hex digits of a handle. Parsing the
	// result gives `reference` back. Pure and thread-safe.
	[[nodiscard]] std::string FormatAssetReference(const AssetReference& reference);

}
