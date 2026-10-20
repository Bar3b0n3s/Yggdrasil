#include "EditorPCH.h"
#include "EditorCore/Automation/Private/ProjectReplaySource.h"

#include <format>

namespace Engine {

	namespace Utils {

		Result<ReplayDocument> ReadProjectReplay(const EditorContext& /*editor*/, std::string_view path, ReplayLoadReport& /*report*/,
			std::string& /*canonicalPath*/)
		{
			ENGINE_CONTRACT_STUB();
			return std::unexpected(Error(ErrorCode::Unsupported, std::format("reading the source replay '{}' (Docs/Decisions/0030-m19-contract.md decision 14, stream B)", path))
					.WithLocation({ .File = {}, .JsonPointer = "/path", .Entity = {} }));
		}

	}

}
