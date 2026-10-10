#include "EnginePCH.h"
#include "Engine/Automation/Methods/AutomationMethodContext.h"

#include "Engine/Scripting/Sandbox.h"

#include <utility>

namespace Engine {

	AutomationMethodContext::AutomationMethodContext(TypeKey hostKey, MethodRequest request)
		: MethodContext(hostKey, std::move(request))
	{
	}

	AutomationMethodContext::~AutomationMethodContext() = default;

	ScriptErrorStream* AutomationMethodContext::GetScriptErrors() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	Result<ScriptEvaluation> AutomationMethodContext::EvalInEdit(std::string_view /*code*/, std::string_view /*entity*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Edit evaluation is an M13 contract stub");
	}

	Status AutomationMethodContext::StartRecordingSession(const PlayStartOptions& /*options*/, bool /*restart*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Recording session startup is an M13 contract stub");
	}

	Status AutomationMethodContext::RestartForReplay(const ReplayHeader& /*header*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Replay session startup is an M13 contract stub");
	}

	Result<ReplayHeader> AutomationMethodContext::DescribeReplayHeader() const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Replay header access is an M13 contract stub");
	}

	Result<AssetRef<ReplayData>> AutomationMethodContext::LoadReplay(std::string_view /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Replay loading is an M13 contract stub");
	}

	Result<std::string> AutomationMethodContext::ValidateReplayOutput(std::string_view /*path*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Replay output validation is an M13 contract stub");
	}

	Result<std::string> AutomationMethodContext::WriteReplay(std::string_view /*path*/, const ReplayDocument& /*document*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Replay output is an M13 contract stub");
	}

	bool AutomationMethodContext::IsHostType(TypeKey key) const
	{
		return key == TypeKeyOf<AutomationMethodContext>() || MethodContext::IsHostType(key);
	}

}
