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
	// Includes project root and every directory under Assets, so a newly created nested config invalidates the snapshot.
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

	// Synchronous, thread-safe diagnostics provider. Only this class's .cpp may use Luau Analysis/Ast/Common APIs.
	// A call owns its frontend/resolvers or exclusively leases them; never share mutable Luau analysis state across
	// calls. Main-thread process flag initialization precedes Create and every import/check worker. No gameplay runs.
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
		// The host recaptures on config/directory changes. Finish all admitted workers before replacing the service-owned
		// checker and its diagnostics-provider reference; never rebind that reference while a worker can still use it.
		[[nodiscard]] static Result<ScriptTypeCheckerConfiguration> CaptureConfiguration(const VirtualFileSystem& vfs);

		// Requires frozen api; consumes its GenerateDefinitions output, never a second hand-maintained declaration list.
		// Copies generated text and owns configuration. Neither api nor its TypeRegistry is retained after this call.
		// Uses SolverMode::New, builtin globals, loadDefinitionFile and freeze. Errors: InvalidState for unfrozen api or
		// missing process initialization; Validation for invalid generated definitions/config input; generator errors.
		// Invalid user config contents become located check findings, not a falsely clean or disabled type checker.
		[[nodiscard]] static Result<Scope<ScriptTypeChecker>> Create(const ScriptApiRegistry& api,
			ScriptTypeCheckerConfiguration configuration = {});

		// Hash of exact definitions, Luau/solver policy and configuration paths/contents/absence; immutable for this
		// checker and stable across build configurations. A diagnostics-cache fingerprint, not source/dependency hashes.
		[[nodiscard]] uint64_t GetEnvironmentHash() const override;

		// Borrows Source and Modules only until return. Source overrides the root's VFS bytes; all other modules use
		// the reader and RequireResolver. Default mode Strict, inherited .luaurc and source directives applied.
		// Findings are 1-based source ranges, sorted by (File, Line, Column, Code) with deterministic ties/deduplication.
		// End positions are exclusive. Required-module errors name that module. No I/O, callbacks or views outlive return.
		// InternalCompilerError (including timeout/cancellation) becomes an Error diagnostic; malformed requests/read
		// failures are findings too. A failed check never returns an empty success. The contract stub reports Unsupported.
		[[nodiscard]] std::vector<ScriptDiagnostic> CheckScript(const ScriptCheckRequest& request) override;
	private:
		struct State;
		Scope<State> m_State{};
	};

}
