#pragma once

#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <span>
#include <string>
#include <string_view>

namespace Engine {

	class IScriptModuleReader;
	namespace Detail {

		struct SandboxAccess;

	}

	// Shared runtime/load-time path policy (§11.1-11.2). Own-thread, per VM/import, no native filesystem or VFS reads.
	// Sandbox owns cached module return values; this class owns resolution, dependency edges and the active stack.
	class RequireResolver
	{
	public:
		RequireResolver();
		~RequireResolver();
		RequireResolver(const RequireResolver&) = delete;
		RequireResolver& operator=(const RequireResolver&) = delete;

		// importer must be a .luau file strictly below project://Assets. request begins ./ or ../; normalize dot
		// segments without ever escaping Assets and append .luau when extensionless. Explicit .luau is accepted.
		// Reject schemes, absolute paths, backslashes, aliases, other extensions and invalid VfsPath spellings with
		// Validation, located at importer. No directory/init or case-insensitive fallback. Does not read the module.
		// Records the edge even on a module-cache hit; Requires are unique/sorted (From, Request, Path, Handle).
		// From/Path are Assets/...; Request keeps the supplied spelling. Handle is unset until the importer resolves it.
		[[nodiscard]] Result<VfsPath> Resolve(const VfsPath& importer, std::string_view request);

		// Reads only through the Asset interface (the importer records content dependencies there). Checks confinement
		// before calling the reader. Propagates reader errors with module location/context; Dist returns Unsupported
		// without calling it. Runtime cooked modules use Resolve and their trusted asset provider instead.
		[[nodiscard]] Result<std::string> ReadSource(IScriptModuleReader& reader, const VfsPath& path) const;

		// Enter before executing an uncached module (root included); an active path returns Script with the complete
		// cycle chain and does not alter the stack. Cache hits record Resolve's edge but never re-enter or execute.
		[[nodiscard]] Status EnterModule(const VfsPath& path);
		// Pops the matching active module on success or failure; unmatched/out-of-order calls are programmer errors.
		void LeaveModule(const VfsPath& path);
		// View invalidated by the next Resolve or destruction. Canonical order, independent of discovery order.
		[[nodiscard]] std::span<const ScriptRequire> GetRequires() const;
	private:
		friend struct Detail::SandboxAccess;
		[[nodiscard]] bool HasActiveModules() const noexcept;
		struct State;
		Scope<State> m_State{};
	};

}
