#include "EnginePCH.h"
#include "Engine/Automation/Methods/AutomationMethodContext.h"

#include "Engine/Scripting/Sandbox.h"
#include "Engine/Session/PlaySession.h"

#include <utility>

namespace Engine {

	AutomationMethodContext::AutomationMethodContext(TypeKey hostKey, MethodRequest request)
		: MethodContext(hostKey, std::move(request))
	{
	}

	AutomationMethodContext::~AutomationMethodContext() = default;

	void AutomationMethodContext::ReleaseReplayInput(uint64_t /*sessionSerial*/)
	{
	}

	ScriptErrorStream* AutomationMethodContext::GetScriptErrors() const
	{
		PlaySession* session = GetPlaySession();
		return session ? &session->GetScriptErrors() : nullptr;
	}

	Result<ScriptEvaluation> AutomationMethodContext::EvalInEdit(std::string_view /*code*/, std::string_view /*entity*/, std::string_view /*behaviour*/)
	{
		return std::unexpected(Error(ErrorCode::Unsupported, "Edit evaluation is unavailable in this host").WithLocation({ .File = {}, .JsonPointer = "/context", .Entity = {} }));
	}

	Status AutomationMethodContext::StartRecordingSession(const PlayStartOptions& /*options*/, bool /*restart*/)
	{
		return MakeError(ErrorCode::Unsupported, "recording is unavailable in this host");
	}

	Status AutomationMethodContext::RestartForReplay(const ReplayHeader& /*header*/)
	{
		return MakeError(ErrorCode::Unsupported, "replay is unavailable in this host");
	}

	Result<ReplayHeader> AutomationMethodContext::DescribeReplayHeader() const
	{
		return MakeError(ErrorCode::Unsupported, "replay identity is unavailable in this host");
	}

	Result<AssetRef<ReplayData>> AutomationMethodContext::LoadReplay(std::string_view /*path*/)
	{
		return MakeError(ErrorCode::Unsupported, "replay loading is unavailable in this host");
	}

	Result<std::string> AutomationMethodContext::ValidateReplayOutput(std::string_view /*path*/) const
	{
		return MakeError(ErrorCode::Unsupported, "replay output is unavailable in this host");
	}

	Result<std::string> AutomationMethodContext::WriteReplay(std::string_view /*path*/, const ReplayDocument& /*document*/)
	{
		return MakeError(ErrorCode::Unsupported, "replay output is unavailable in this host");
	}

	Result<ReplayDocument> AutomationMethodContext::ReadReplayDocument(std::string_view /*path*/, ReplayLoadReport& /*report*/, std::string& /*canonicalPath*/) const
	{
		return std::unexpected(Error(ErrorCode::Unsupported, "the Runtime holds no replay sources: read the replay in the editor").WithLocation({ .File = {}, .JsonPointer = "/path", .Entity = {} }));
	}

	Result<std::string> AutomationMethodContext::WriteKeptFile(std::string_view extension, std::span<const std::byte> /*bytes*/)
	{
		// The editor and the Runtime keep files (their overrides); a host without a kept-files directory keeps none.
		return MakeError(ErrorCode::Unsupported, "kept output files (.{}) are unavailable in this host", extension);
	}

	bool AutomationMethodContext::IsHostType(TypeKey key) const
	{
		return key == TypeKeyOf<AutomationMethodContext>() || MethodContext::IsHostType(key);
	}

}
