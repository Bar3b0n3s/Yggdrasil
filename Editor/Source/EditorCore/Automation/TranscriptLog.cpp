#include "EditorPCH.h"
#include "EditorCore/Automation/TranscriptLog.h"

#include "Engine/Core/VirtualFileSystem.h"

// M4 contract stub (Roadmap rule 3): stream C (methods, validator, provenance) implements the editor's transcript lines.

namespace Engine {

	Result<uint64_t> TranscriptLog::AppendRequest(VirtualFileSystem& /*vfs*/, std::string_view /*client*/, const Json& /*id*/,
		std::string_view /*method*/, const Json& /*params*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "TranscriptLog::AppendRequest is an M4 contract stub");
	}

	Status TranscriptLog::AppendResponse(VirtualFileSystem& /*vfs*/, std::string_view /*client*/, const Json& /*id*/,
		uint64_t /*requestLine*/, std::string_view /*summary*/, const Json& /*error*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "TranscriptLog::AppendResponse is an M4 contract stub");
	}

}
