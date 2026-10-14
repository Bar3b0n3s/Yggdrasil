#include "EditorPCH.h"
#include "EditorCore/Export/TestingExport.h"

namespace Engine {

	Result<TestingExportContent> CollectTestingExportContent(EditorContext& /*editor*/, const ProjectSettings& /*settings*/,
		std::vector<ErrorIssue>& /*issues*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "testing exports arrive with M15 stream B (Docs/Decisions/0024-m15-contract.md decision 6)"));
	}

	bool IsForcedByTestingExport(std::string_view /*projectPath*/, const TestingExportContent& /*content*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}
