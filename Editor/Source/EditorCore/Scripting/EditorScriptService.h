#pragma once

#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class EditorContext;

	enum class ScriptTemplate : uint8_t
	{
		Behaviour,
		Module,
		Test
	};

	struct EditorScriptWriteResult
	{
		AssetHandle Script{};
		std::string Path{};
		uint64_t UndoIndex = 0;
		// Compile, load-time extraction and static-check findings, including required modules, with full 1-based ranges.
		// Invalid source is saved for iteration; findings are a successful write result, never a fabricated I/O failure.
		std::vector<ScriptDiagnostic> Diagnostics{};
	};

	struct EditorScriptCheckResult
	{
		std::vector<std::string> Paths{};            // Canonical, unique, byte-wise sorted paths actually checked.
		std::vector<ScriptDiagnostic> Diagnostics{}; // Unique, sorted (File, Line, Column, Code, Message).
	};

	// Main-thread service shared by automation and source editing; no UI, automation or Luau Analysis dependency.
	// Editor and diagnostics are borrowed back-references and outlive the service. Reuses AssetEditCommand,
	// EditorContext's write path and EditorAssetManager; it neither owns another watcher nor keeps a schema cache.
	class EditorScriptService
	{
	public:
		EditorScriptService(EditorContext& editor, IScriptDiagnosticsProvider& diagnostics);

		// All paths must name .luau files below project://Assets. Reject escaping paths, embedded NUL and invalid UTF-8
		// with InvalidArgument before any write; InvalidState without a project. Reads and dependency checks see dry-run
		// overlays. Create/Write refuse a read-only project (except an active dry-run overlay) with PermissionDenied.
		// Create uses the shipped template, with a valid class/suite name derived from the filename; AlreadyExists for
		// an existing source. Source and .meta are one AssetEditCommand, so undo removes both and redo keeps the handle.
		[[nodiscard]] Result<EditorScriptWriteResult> Create(const VfsPath& path, ScriptTemplate scriptTemplate);
		[[nodiscard]] Result<std::string> Read(const VfsPath& path) const;
		// Writes exact source bytes, creating a missing file/meta; existing handles survive. No-op bytes record no undo.
		// After command execution, runs the same compiler/extractor/checker as import; no direct native filesystem write.
		// A failed file operation rolls back; script diagnostics do not roll back the user's source. Returns only after
		// diagnostics correspond to these bytes; any ordinary-play hot reload follows the existing asset notification path.
		[[nodiscard]] Result<EditorScriptWriteResult> Write(const VfsPath& path, std::string_view source);
		// Empty paths checks all project scripts; validates every requested path before checking any. Never writes or
		// reloads code. Required modules use the same confined module reader as import. Missing files return NotFound;
		// syntax/type errors are findings. Infrastructure errors retain their ErrorCode and file context.
		[[nodiscard]] Result<EditorScriptCheckResult> Check(std::span<const VfsPath> paths) const;
		// Pins the current immutable cooked schema. NotFound for a missing handle; InvalidArgument for a non-Script;
		// ImportFailed with findings when no valid schema exists. Module/TestSuite legitimately expose zero fields.
		[[nodiscard]] Result<AssetRef<ScriptData>> GetFields(AssetHandle script) const;
	private:
		EditorContext* m_Editor = nullptr;                   // Borrowed; outlives the service.
		IScriptDiagnosticsProvider* m_Diagnostics = nullptr; // Borrowed; outlives each synchronous check.
	};

}
