#pragma once

#include "EditorCore/Commands/Command.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	class EditorContext;

	// What one entry of an AssetEditCommand changes.
	enum class AssetFileKind : uint8_t
	{
		File,     // Before/After hold the file's bytes (nullopt: the file does not exist)
		Directory // Before/After hold an empty buffer for "exists" (nullopt: it does not exist); asset.create {type: Folder}
	};

	// One file (or directory) of an AssetEditCommand: its state before and after the command.
	struct AssetFileEdit
	{
		VfsPath Path{}; // a project:// path
		AssetFileKind Kind = AssetFileKind::File;
		std::optional<Buffer> Before{};
		std::optional<Buffer> After{};
	};

	// Changes project files by their bytes (Architecture §12.3: "AssetEditCommand (file bytes before/after, for materials,
	// sound effects, scripts written through automation and import settings)"): asset.create (a new source and its .meta),
	// asset.setProperties, asset.setImportSettings (the .meta), prefab.create and prefab.apply (the .prefab), asset.import
	// (the copied files and their metas) and, from M13, script.write. Asset edits write through: Execute writes every After
	// state and Undo every Before state at once, so there are no "dirty native assets" (project.save saves only the scene,
	// ADR 0010) and the files always equal what the editor shows.
	//
	// Files are written, removed and directories created through EditorContext's write path (WriteProjectFile,
	// RemoveProjectFile, CreateProjectDirectory): provenance is recorded, dry runs go to the overlay, the AssetWriter keeps the
	// hot reloader from echoing and the EditorAssetManager updates its registry (a written .meta registers its asset, so the
	// handle is known when Execute returns) and reimports. Execute applies the entries in order and Undo in reverse order;
	// both are atomic (Command.h): when entry k fails, entries before it are restored and the error returned. ChangesScene is
	// false: a file edit never dirties the open scene.
	class AssetEditCommand final : public Command
	{
	public:
		// `edits` (non-empty, asserted; each path once, Before != After) with the label shown in the undo history.
		AssetEditCommand(std::string label, std::vector<AssetFileEdit> edits);

		// A command that writes `bytes` to `path`, reading the current bytes (or absence) as its Before state. Errors:
		// InvalidState without a project; the read errors other than NotFound.
		[[nodiscard]] static Result<Scope<AssetEditCommand>> CreateForWrite(const EditorContext& context, const VfsPath& path,
			std::span<const std::byte> bytes, std::string label);

		// A command that writes several files at once, each with its current state as Before (a source and its .meta).
		// Errors: as CreateForWrite.
		[[nodiscard]] static Result<Scope<AssetEditCommand>> CreateForWrites(const EditorContext& context,
			std::span<const std::pair<VfsPath, Buffer>> files, std::string label);

		// Writes every After state. Errors: the first failing write's (earlier entries restored).
		[[nodiscard]] Status Execute(EditorContext& context) override;
		// Writes every Before state, in reverse order. Errors: the first failing write's (later entries re-applied; the
		// command stays applied).
		[[nodiscard]] Status Undo(EditorContext& context) override;
		[[nodiscard]] std::string_view GetLabel() const override { return m_Label; }
		[[nodiscard]] bool ChangesScene() const override { return false; }
		[[nodiscard]] size_t GetMemorySize() const override;

		[[nodiscard]] std::span<const AssetFileEdit> GetEdits() const { return m_Edits; }
	private:
		std::string m_Label;
		std::vector<AssetFileEdit> m_Edits;
	};

}
