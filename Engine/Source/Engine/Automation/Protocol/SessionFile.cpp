#include "EnginePCH.h"
#include "Engine/Automation/Protocol/SessionFile.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements session files.

namespace Engine {

	std::filesystem::path SessionFile::GetPath(const std::filesystem::path& /*directory*/, uint32_t /*pid*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::string SessionFile::ToText(const SessionFileContent& /*content*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<SessionFileContent> SessionFile::FromText(std::string_view /*text*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SessionFile::FromText is an M4 contract stub");
	}

	Status SessionFile::Write(const std::filesystem::path& /*directory*/, const SessionFileContent& /*content*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SessionFile::Write is an M4 contract stub");
	}

	Status SessionFile::Remove(const std::filesystem::path& /*directory*/, uint32_t /*pid*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "SessionFile::Remove is an M4 contract stub");
	}

	std::string SessionFile::FormatUtcTimestamp(std::chrono::system_clock::time_point /*time*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
