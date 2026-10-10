#include "EnginePCH.h"
#include "Engine/Scripting/ScriptProxy.h"

namespace Engine {

	bool ScriptProxy::IsValid(IScriptHost& /*host*/, ScriptEntityIdentity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	Status ScriptProxy::ValidateEntity(IScriptHost& /*host*/, ScriptEntityIdentity /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script entity validation is not implemented");
	}

	Status ScriptProxy::ValidateComponent(IScriptHost& /*host*/, ScriptProxyIdentity /*proxy*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script proxy validation is not implemented");
	}

	Result<std::optional<ScriptProxyIdentity>> ScriptProxy::GetComponent(IScriptHost& /*host*/,
		ScriptEntityIdentity /*entity*/, std::string_view /*component*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script component lookup is not implemented");
	}

	Result<std::optional<ScriptProxyIdentity>> ScriptProxy::GetShortcut(IScriptHost& /*host*/,
		ScriptEntityIdentity /*entity*/, std::string_view /*component*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script shortcut lookup is not implemented");
	}

	Result<Value> ScriptProxy::ReadField(ScriptCall& /*call*/, ScriptProxyIdentity /*proxy*/, std::string_view /*field*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script proxy reads are not implemented");
	}

	Status ScriptProxy::WriteField(ScriptCall& /*call*/, ScriptProxyIdentity /*proxy*/, std::string_view /*field*/,
		const Value& /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Script proxy writes are not implemented");
	}

}
