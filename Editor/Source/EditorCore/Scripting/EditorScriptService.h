#pragma once

#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class EditorContext;
	struct AssetMetadata;

	enum class ScriptTemplate : uint8_t
	{
		Behaviour,
		Module,
		Test
	};

	// A range of a script source's lines (Docs/Decisions/0038-m27-rolling-ball.md friction F24): from the one-based Line up
	// to the exclusive EndLine, as a diagnostic's range (ScriptDiagnostic Line, EndLine). Empty when Line == EndLine: it then
	// holds no line and sits before line Line.
	struct ScriptLineRange
	{
		uint32_t Line = 1;
		uint32_t EndLine = 1;

		[[nodiscard]] bool operator==(const ScriptLineRange& other) const = default;
	};

	// The single span a write changed: the lines from the first to the last that differ once the lines the two sources
	// share at their start and at their end are set aside, as the source it replaced held them (Before) and as the written
	// one holds them (After). One span, not a diff: unchanged lines between two edits are inside it.
	struct ScriptChangedLines
	{
		ScriptLineRange Before{};
		ScriptLineRange After{};

		[[nodiscard]] bool operator==(const ScriptChangedLines& other) const = default;
	};

	// The number of lines of `source`: each '\n' ends one, and a last line without one counts too, so "" has 0 lines and
	// "a\nb" and "a\nb\n" have 2. A '\r' before a '\n' belongs to its line, which "\r\n" ends as '\n' does: the lines are
	// those Luau numbers, so a diagnostic's line n is the n-th. Saturates at the largest uint32_t. Pure.
	[[nodiscard]] uint32_t CountScriptLines(std::string_view source);

	// The span between `before` and `after` (ScriptChangedLines): their lines are compared with their line ends, so a
	// changed line end ("\n" for "\r\n", a newline added after the last line) changes its line, and the span is empty in
	// both only when the bytes are equal, for which it is nullopt. Removed lines alone give an empty After at the line they
	// were removed before, added lines alone an empty Before. Pure.
	[[nodiscard]] std::optional<ScriptChangedLines> FindChangedScriptLines(std::string_view before, std::string_view after);

	struct EditorScriptWriteResult
	{
		AssetHandle Script{};
		std::string Path{};
		uint64_t UndoIndex = 0;
		// Compile, load-time extraction and static-check findings, including required modules, with full 1-based ranges.
		// Invalid source is saved for iteration; findings are a successful write result, never a fabricated I/O failure.
		std::vector<ScriptDiagnostic> Diagnostics{};
		// HashCookedArtifact of the artifact ScriptImporter cooks from the written bytes (as GetCookedHash reports); nullopt
		// when they do not cook (a compile or load-time failure, an unresolved require).
		std::optional<uint64_t> CookedHash{};
		// The lines the write changed against the source it replaced (FindChangedScriptLines); for a new file, every line
		// of it after an empty Before at line 1. Nullopt when the bytes are unchanged.
		std::optional<ScriptChangedLines> ChangedLines{};
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
		// Create uses the shipped template: Behaviour/Module get a "Script_"-prefixed identifier derived from the filename;
		// Test names its suite after the file stem without ".test" ("Tetris.test.luau" -> "Tetris"). AlreadyExists for
		// an existing source. Source and .meta are one AssetEditCommand, so undo removes both and redo keeps the handle.
		[[nodiscard]] Result<EditorScriptWriteResult> Create(const VfsPath& path, ScriptTemplate scriptTemplate);
		[[nodiscard]] Result<std::string> Read(const VfsPath& path) const;
		// Writes exact source bytes, creating a missing file/meta; existing handles survive. No-op bytes record no undo.
		// After command execution, runs the same compiler/extractor/checker as import; no direct native filesystem write.
		// A failed file operation rolls back; script diagnostics do not roll back the user's source. Returns only after
		// diagnostics correspond to these bytes; any ordinary-play hot reload follows the existing asset notification path.
		[[nodiscard]] Result<EditorScriptWriteResult> Write(const VfsPath& path, std::string_view source);
		// The cooked hash of `source` as the script `metadata` describes at `path` (Docs/Decisions/0038-m27-rolling-ball.md
		// friction F24): HashCookedArtifact of the artifact ScriptImporter cooks from these bytes, the same detached import
		// Write checks with but without the type check, which cooking never reads. It equals what
		// EditorAssetManager::GetCookedHash returns once the editor has published the import of these bytes, which is what a
		// replay's start fingerprint records for the script; any changed byte changes it. Nullopt when `source` does not cook
		// (a compile or load-time failure, an unresolved require). Never writes, publishes or reloads anything. Errors: the
		// path rules above; the import's infrastructure failures (Io, PermissionDenied, InvalidState, Unsupported).
		[[nodiscard]] Result<std::optional<uint64_t>> GetCookedHash(const VfsPath& path, std::string_view source, const AssetMetadata& metadata) const;
		// Empty paths checks all project scripts; validates every requested path before checking any. Never writes or
		// reloads code. Required modules use the same confined module reader as import. A missing requested path returns
		// NotFound; a missing or invalid required module, syntax and type errors are located findings. Infrastructure
		// errors (Io, PermissionDenied) retain their ErrorCode and file context.
		[[nodiscard]] Result<EditorScriptCheckResult> Check(std::span<const VfsPath> paths) const;
		// Pins the current immutable cooked schema. NotFound for a missing handle; InvalidArgument for a non-Script;
		// ImportFailed with findings when no valid schema exists. Module/TestSuite legitimately expose zero fields.
		[[nodiscard]] Result<AssetRef<ScriptData>> GetFields(AssetHandle script) const;
	private:
		EditorContext* m_Editor = nullptr;                   // Borrowed; outlives the service.
		IScriptDiagnosticsProvider* m_Diagnostics = nullptr; // Borrowed; outlives each synchronous check.
	};

}
