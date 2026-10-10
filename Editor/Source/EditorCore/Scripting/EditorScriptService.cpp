#include "EditorPCH.h"
#include "EditorCore/Scripting/EditorScriptService.h"

#include "Engine/Core/Base.h"

namespace Engine {

	EditorScriptService::EditorScriptService(EditorContext& /*editor*/, IScriptDiagnosticsProvider& /*diagnostics*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<EditorScriptWriteResult> EditorScriptService::Create(const VfsPath& /*path*/, ScriptTemplate /*scriptTemplate*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script creation contract is not implemented");
	}

	Result<std::string> EditorScriptService::Read(const VfsPath& /*path*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script reading contract is not implemented");
	}

	Result<EditorScriptWriteResult> EditorScriptService::Write(const VfsPath& /*path*/, std::string_view /*source*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script writing contract is not implemented");
	}

	Result<EditorScriptCheckResult> EditorScriptService::Check(std::span<const VfsPath> /*paths*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script checking contract is not implemented");
	}

	Result<AssetRef<ScriptData>> EditorScriptService::GetFields(AssetHandle /*script*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script field inspection contract is not implemented");
	}

}
