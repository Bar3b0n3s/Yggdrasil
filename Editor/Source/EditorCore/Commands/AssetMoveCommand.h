#pragma once

#include "EditorCore/Commands/Command.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class EditorContext;

	// Moves an asset (Architecture §12.3 AssetMoveCommand, §13.5 asset.move): the source, its .meta and, for a glTF, every
	// dependency file with its dependency meta (§6.4: they move together with their owner), keeping their paths relative to
	// the source so its URIs stay valid. Handles never change (§7.3: a moved pair keeps its handle), so every reference by
	// handle in scenes, prefabs and materials stays valid without rewriting anything; a reference by path (a glTF's URI to a
	// standalone texture) would not, so Create refuses that move. The open scene is unaffected (ChangesScene is false).
	//
	// The renames go through EditorContext::MoveProjectFile (provenance follows each file, the AssetWriter keeps the hot
	// reloader from echoing, the registry is updated). Execute applies them in order, creating destination directories; Undo
	// moves back in reverse order (directories it created stay, empty). Both are atomic (Command.h).
	class AssetMoveCommand final : public Command
	{
	public:
		// `moves` (non-empty, asserted) as planned by AssetRegistry::PlanMove, with the label "Move Asset '<old path>'".
		AssetMoveCommand(std::string label, std::vector<AssetFileMove> moves);

		// The command that moves the main asset `handle` to `newSourcePath` (AssetRegistry::PlanMove on the editor's registry).
		// Errors: InvalidState without a project; InvalidState when another asset's last import found `handle` by its path
		// (EditorAssetManager::GetPathDependents: a glTF that reuses this standalone texture through its URI, which a move
		// would break), naming those assets, with the hint "move the glTF together with it, or re-export it"; those of
		// PlanMove (NotFound, InvalidArgument, AlreadyExists).
		[[nodiscard]] static Result<Scope<AssetMoveCommand>> Create(const EditorContext& context, AssetHandle handle, const VfsPath& newSourcePath);

		// Errors: the first failing rename's (earlier renames undone).
		[[nodiscard]] Status Execute(EditorContext& context) override;
		// Errors: the first failing rename's (later renames re-applied; the command stays applied).
		[[nodiscard]] Status Undo(EditorContext& context) override;
		[[nodiscard]] std::string_view GetLabel() const override { return m_Label; }
		[[nodiscard]] bool ChangesScene() const override { return false; }
		[[nodiscard]] size_t GetMemorySize() const override;

		[[nodiscard]] std::span<const AssetFileMove> GetMoves() const { return m_Moves; }
	private:
		std::string m_Label;
		std::vector<AssetFileMove> m_Moves;
	};

}
