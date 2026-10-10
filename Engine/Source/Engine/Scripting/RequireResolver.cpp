#include "EnginePCH.h"
#include "Engine/Scripting/RequireResolver.h"

namespace Engine {

	struct RequireResolver::State
	{
	};

	RequireResolver::RequireResolver()
	{
		ENGINE_CONTRACT_STUB();
	}

	RequireResolver::~RequireResolver()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<VfsPath> RequireResolver::Resolve(const VfsPath& importer, std::string_view request)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(importer);
		static_cast<void>(request);
		return MakeError(ErrorCode::Unsupported, "RequireResolver is an M13 contract stub");
	}

	Result<std::string> RequireResolver::ReadSource(IScriptModuleReader& reader, const VfsPath& path) const
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(reader);
		static_cast<void>(path);
		return MakeError(ErrorCode::Unsupported, "RequireResolver is an M13 contract stub");
	}

	Status RequireResolver::EnterModule(const VfsPath& path)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(path);
		return MakeError(ErrorCode::Unsupported, "RequireResolver is an M13 contract stub");
	}

	void RequireResolver::LeaveModule(const VfsPath& path)
	{
		ENGINE_CONTRACT_STUB();
		static_cast<void>(path);
	}

	std::span<const ScriptRequire> RequireResolver::GetRequires() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
