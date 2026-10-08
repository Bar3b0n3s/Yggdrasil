#include "EditorPCH.h"
#include "EditorCore/Automation/ExportMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Reflection/TypeRegistry.h"

namespace Engine {

	namespace Automation {

		Result<Scope<PendingOperation>> ProjectExport(EditorMethodContext& /*context*/, const ProjectExportParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "project.export is not implemented yet (M7 stream D)");
		}

	}

	void RegisterExportMethodTypes(TypeRegistry& /*registry*/)
	{
		// Registers nothing until stream D lands project.export with the catalogue update (ADR 0012 decision 13).
		ENGINE_CONTRACT_STUB();
	}

	void RegisterExportMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
