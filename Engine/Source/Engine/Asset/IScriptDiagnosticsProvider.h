#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Reflection/ValidationContext.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Static type diagnostics for scripts (Architecture §3 rule 4, §7.4 ScriptImporter, §11.9). Asset defines the interface;
// EditorCore implements it with the Luau type checker (EditorCore/Scripting/ScriptTypeChecker.cpp, M13) and registers it
// with the EditorAssetManager (EditorAssetManagerSpecification::ScriptDiagnostics). Tools without it import scripts
// without type diagnostics. ScriptImporter and the checker's details arrive with M13, which may add members here through
// its contract.

namespace Engine {

	// One type-check finding, located in a script.
	struct ScriptDiagnostic
	{
		DiagnosticSeverity Severity = DiagnosticSeverity::Error;
		std::string Code{};  // "SCRIPT_TYPE_ERROR" (§13.7)
		std::string File{};  // project-relative path of the script the finding is in (a required module's, too)
		uint32_t Line = 0;   // 1-based
		uint32_t Column = 0; // 1-based
		std::string Message{};
		uint32_t EndLine = 0;   // 1-based exclusive range end; zero only when the checker supplies no range
		uint32_t EndColumn = 0; // UTF-8 byte column, matching Luau's source positions

		bool operator==(const ScriptDiagnostic&) const = default;
	};

	// Owned result of the latest script import attempt. Unchecked is different from a checked script with no findings.
	// SourceHash identifies the exact root bytes; dependency reads/lookups remain in the ordinary import manifest.
	struct ScriptImportCheck
	{
		bool Performed = false;
		uint64_t EnvironmentHash = 0;
		uint64_t SourceHash = 0;
		std::vector<ScriptDiagnostic> Diagnostics{};
	};

	// Reads the modules a script requires. ScriptImporter passes one that reads through ImportContext::ReadDependency, so
	// every module the checker reads is recorded in the cache manifest and a cached script is re-checked when a module it
	// requires changes (§7.5: "changing Board.luau re-extracts and recompiles Game.luau"). Called from the checker's thread
	// (an import job); used by one check at a time.
	class IScriptModuleReader
	{
	public:
		virtual ~IScriptModuleReader() = default;

		// The UTF-8 source of the module at `path` (anywhere under project://Assets). Errors: those of
		// ImportContext::ReadDependency (InvalidArgument outside the Assets folder, NotFound).
		[[nodiscard]] virtual Result<std::string> ReadModule(const VfsPath& path) = 0;
	};

	// What to check: one script's source, with the reader its requires resolve through.
	struct ScriptCheckRequest
	{
		VfsPath Path{};                         // the script, project://Assets/...
		std::string_view Source{};              // its UTF-8 source (the bytes being imported, which may differ from the file)
		IScriptModuleReader* Modules = nullptr; // reads required modules (never through the VFS directly); never null
	};

	// Checks scripts. Implementations are thread-safe: ScriptImporter calls them from import jobs.
	class IScriptDiagnosticsProvider
	{
	public:
		virtual ~IScriptDiagnosticsProvider() = default;
		// Immutable fingerprint of declarations, compiler/solver policy and captured configuration, independent of
		// build configuration. Cached checks are reusable only when this fingerprint and their inputs still match.
		[[nodiscard]] virtual uint64_t GetEnvironmentHash() const = 0;

		// The findings of `request`, sorted by (File, Line, Column, Code). A checker failure that is not a finding of the
		// script (an internal compiler error) is reported as one Error finding located at line 1 (§4.6 item 4).
		[[nodiscard]] virtual std::vector<ScriptDiagnostic> CheckScript(const ScriptCheckRequest& request) = 0;
	};

}
