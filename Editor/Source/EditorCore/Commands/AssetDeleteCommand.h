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

	// Deletes an asset to the trash (Architecture §12.3 AssetDeleteCommand, §12.2 "delete to Library/Trash/ (undoable)",
	// §13.5 asset.delete): the source, its .meta and every dependency file with its dependency meta (§6.4: trashed together
	// with their owner) are moved into project://Library/Trash/<entry>/, keeping their project-relative paths; never a
	// permanent delete. <entry> is the asset's 16-hex handle, followed by "-<n>" (2, 3, ...) when a previous delete of the
	// same asset still occupies the name. Undo moves everything back and removes the trash folders Execute created while
	// they are empty (§12.3: Execute then Undo leaves the project as it was), and the asset keeps its handle. References to the
	// asset elsewhere stay as they are: their uses report ASSET_MISSING (a reference diagnostic, cleared when the undo
	// registers the handle again, AssetManager.h), and project.validate reports them until they are fixed or the delete is
	// undone. ChangesScene is false.
	//
	// The renames go through EditorContext::MoveProjectFile: moving out of Assets/ removes the files' provenance entries
	// (Library/ is not recorded, §13.12 rule 3) and the registry forgets the asset. Atomic like AssetMoveCommand.
	class AssetDeleteCommand final : public Command
	{
	public:
		// `moves` (non-empty, asserted) as planned by AssetRegistry::PlanTrash, with the label "Delete Asset '<path>'".
		AssetDeleteCommand(std::string label, std::vector<AssetFileMove> moves);

		// The command that trashes the main asset `handle`, choosing a free trash entry. When another asset found `handle` by
		// path (EditorAssetManager::GetPathDependents) it logs a warning naming them (their next import fails until the
		// delete is undone). Errors: InvalidState without a project; those of AssetRegistry::PlanTrash (NotFound,
		// InvalidArgument).
		[[nodiscard]] static Result<Scope<AssetDeleteCommand>> Create(const EditorContext& context, AssetHandle handle);

		// The trash entry directory of `handle` this delete would use: "project://Library/Trash/<handle>" or the first free
		// "-<n>" variant. Errors: InvalidState without a project.
		[[nodiscard]] static Result<VfsPath> ChooseTrashDirectory(const EditorContext& context, AssetHandle handle);

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
		std::vector<VfsPath> m_CreatedDirectories; // the folders the last Execute created, deepest first; removed by Undo
	};

}
