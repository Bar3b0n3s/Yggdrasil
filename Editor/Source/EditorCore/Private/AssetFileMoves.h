#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// What AssetMoveCommand and AssetDeleteCommand share: applying a planned list of renames through EditorContext's write path
// atomically, in either direction (Command.h: a failed step restores the steps before it), and naming the assets that
// found an asset by its path (EditorAssetManager::GetPathDependents).

namespace Engine {

	class EditorAssetManager;
	class EditorContext;

	namespace Utils {

		// Renames every move From -> To in order (EditorContext::MoveProjectFile, which creates destination folders) and
		// returns the folders the renames created, deepest first, for RevertAssetFileMoves. When one fails, the renames before
		// it are moved back, newest first, the folders they created are removed, and its error is returned with the context
		// "executing '<label>'".
		[[nodiscard]] Result<std::vector<VfsPath>> ApplyAssetFileMoves(EditorContext& context, std::span<const AssetFileMove> moves,
			std::string_view label);

		// Renames every move To -> From in reverse order, then removes `createdDirectories` (what ApplyAssetFileMoves
		// returned) while they are empty, deepest first, so Execute then Undo leaves the folders as they were (§12.3); a folder
		// something else was put into stays, and one that cannot be removed is logged at Warn. When a rename fails, the
		// renames already reverted are applied again, so everything stays moved, and its error is returned with the context
		// "undoing '<label>'".
		[[nodiscard]] Status RevertAssetFileMoves(EditorContext& context, std::span<const AssetFileMove> moves, std::span<const VfsPath> createdDirectories,
			std::string_view label);

		// "'Assets/Models/Track.gltf', 'Assets/Models/Kart.gltf'": the readable references of `handles`, quoted, in order.
		[[nodiscard]] std::string FormatAssetList(const EditorAssetManager& assets, std::span<const AssetHandle> handles);

		// The project-relative text of the source of the registered asset `handle` ("Assets/Red.material"), or its handle's
		// hex digits when it is not registered.
		[[nodiscard]] std::string GetAssetSourceText(const EditorAssetManager& assets, AssetHandle handle);

	}

}
