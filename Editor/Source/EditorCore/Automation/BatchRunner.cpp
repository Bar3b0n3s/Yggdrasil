#include "EditorPCH.h"
#include "EditorCore/Automation/BatchRunner.h"

#include "EditorCore/Automation/AutomationServer.h"

// M4 contract stub (Roadmap rule 3): stream B (protocol) implements batch and CLI runs.

namespace Engine {

	Result<std::vector<BatchRequest>> BatchRunner::LoadFile(const std::filesystem::path& /*file*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "BatchRunner::LoadFile is an M4 contract stub");
	}

	struct BatchRunner::State
	{
		ClientId Client = NoClient;
		size_t NextRequest = 0;
		bool InFlight = false;
		std::optional<uint64_t> TranscriptLine{};
	};

	BatchRunner::BatchRunner(std::vector<BatchRequest> requests, BatchRunOptions options)
		: m_Requests(std::move(requests)), m_Options(std::move(options)), m_State(CreateScope<State>())
	{
	}

	BatchRunner::~BatchRunner() = default;

	void BatchRunner::Advance(AutomationServer& /*server*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	bool BatchRunner::IsFinished() const
	{
		ENGINE_CONTRACT_STUB();
		return true;
	}

}
