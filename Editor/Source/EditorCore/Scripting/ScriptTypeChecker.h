#pragma once

#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

	class ScriptApiRegistry;
	class VirtualFileSystem;

	// Exact .luaurc bytes captured from project://. Nullopt records an absent file; an empty string is a present file.
	// Lists the project root and every directory under Assets, so a newly created nested config is seen. Only present files
	// enter the environment hash and the editor's refresh hash: an absent file configures nothing, so a new directory without
	// a .luaurc changes no check (Docs/Decisions/0041-script-check-performance.md decision 2).
	struct ScriptTypeCheckerConfigFile
	{
		VfsPath Path{};
		std::optional<std::string> Source{};
	};

	// Owned immutable input once handed to Create. Files are unique and sorted by canonical VFS path. No aliases or
	// config options may widen RequireResolver's relative-only Assets sandbox. Empty Files selects default strict mode.
	struct ScriptTypeCheckerConfiguration
	{
		std::vector<ScriptTypeCheckerConfigFile> Files{};
	};

	// What a checker has done since Create, read at any time from any thread (each count is current when read).
	struct ScriptTypeCheckerStatistics
	{
		// CheckScript calls that analysed their script; a malformed request and a root declaring a type function (refused
		// before analysis) are not counted.
		uint64_t Checks = 0;
		// Frontends prepared, each loading the builtin globals and the definitions once: Create prepares the first, and a check
		// prepares another only while every prepared one is in use by a simultaneous check.
		uint64_t DefinitionLoads = 0;
	};

	// Synchronous, thread-safe diagnostics provider. Only this class's .cpp may use Luau Analysis/Ast/Common APIs.
	// Each call exclusively leases a prepared frontend, whose builtin globals and definitions were loaded and frozen once
	// (Docs/Decisions/0041-script-check-performance.md decision 1), and gives it its own resolvers; a completed check forgets
	// every module and source before the frontend is leased again, and one that throws drops its frontend, so no module,
	// resolver or finding outlives a call and findings never depend on reuse. The idle frontends stay with the checker (as
	// many as checks ever ran at once). Main-thread process flag initialization precedes Create and every import/check
	// worker. No gameplay runs.
	class ScriptTypeChecker final : public IScriptDiagnosticsProvider
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ScriptTypeChecker;
		};

		explicit ScriptTypeChecker(ConstructionKey key);
		~ScriptTypeChecker() override;
		ScriptTypeChecker(const ScriptTypeChecker&) = delete;
		ScriptTypeChecker& operator=(const ScriptTypeChecker&) = delete;
		ScriptTypeChecker(ScriptTypeChecker&&) = delete;
		ScriptTypeChecker& operator=(ScriptTypeChecker&&) = delete;

		// Host call while capturing a project/overlay revision, before scheduling its checks. Reads only through VFS;
		// stores each directory's .luaurc contents or absence. No config reads go through ImportContext::ReadDependency
		// (project root is outside Assets). Errors: VFS errors, Validation for malformed paths/UTF-8; no partial snapshot.
		// The host recaptures to see config changes and replaces its checker only when a present .luaurc was added, changed
		// or removed. Finish all admitted workers before replacing the service-owned checker and its diagnostics-provider
		// reference; never rebind that reference while a worker can still use it.
		[[nodiscard]] static Result<ScriptTypeCheckerConfiguration> CaptureConfiguration(const VirtualFileSystem& vfs);

		// Requires frozen api; consumes its GenerateDefinitions output, never a second hand-maintained declaration list.
		// Copies generated text and owns configuration. Neither api nor its TypeRegistry is retained after this call.
		// Uses SolverMode::New, builtin globals, loadDefinitionFile and freeze, in the first prepared frontend, which validates
		// the definitions. Errors: InvalidState for unfrozen api or missing process initialization; Validation for invalid
		// generated definitions/config input; generator errors.
		// Invalid user config contents become located check findings, not a falsely clean or disabled type checker.
		[[nodiscard]] static Result<Scope<ScriptTypeChecker>> Create(const ScriptApiRegistry& api,
			ScriptTypeCheckerConfiguration configuration = {});

		// Hash of exact definitions, Luau/solver policy and the present configuration files' paths and contents (an absent
		// file configures nothing); immutable for this checker and stable across build configurations. A diagnostics-cache
		// fingerprint, not source/dependency hashes.
		[[nodiscard]] uint64_t GetEnvironmentHash() const override;

		// The checks and definition loads so far; thread-safe.
		[[nodiscard]] ScriptTypeCheckerStatistics GetStatistics() const;

		// Borrows Source and Modules only until return. Source overrides the root's VFS bytes; all other modules use
		// the reader and RequireResolver. Default mode Strict, inherited .luaurc and source directives applied.
		// Findings are 1-based source ranges, sorted by (File, Line, Column, Code) with deterministic ties/deduplication.
		// End positions are exclusive. Required-module errors name that module. No I/O, callbacks or views outlive return.
		// Syntax errors use SCRIPT_COMPILE_ERROR (as import does); every other finding uses SCRIPT_TYPE_ERROR. Nested
		// .luaurc files apply from the project root down, so the nearest directory's configuration wins.
		// InternalCompilerError (including timeout/cancellation) becomes an Error diagnostic; malformed requests/read
		// failures are findings too. A failed check never returns an empty success. No time limit or cancellation token is
		// set, so findings never depend on machine load; a root or required module that declares a user-defined type
		// function (`type function`) is refused before analysis with a SCRIPT_TYPE_ERROR at each declaration's name, so the
		// unbounded type-function VM never runs (§11.9).
		[[nodiscard]] std::vector<ScriptDiagnostic> CheckScript(const ScriptCheckRequest& request) override;
	private:
		struct State;
		Scope<State> m_State{};
	};

}
