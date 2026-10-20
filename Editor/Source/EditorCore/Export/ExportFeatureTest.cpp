#include "EditorPCH.h"
#include "EditorCore/Export/ExportFeatureTest.h"

namespace Engine {

	Result<Scope<IExportFeatureTestJob>> StartExportFeatureTest(const ExportFeatureTestRun& run)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "a testing export's --feature-test run of '{}' (Docs/Decisions/0030-m19-contract.md decision 10, stream F)", run.Name);
	}

}
