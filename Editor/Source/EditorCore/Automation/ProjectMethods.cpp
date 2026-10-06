#include "EditorPCH.h"
#include "EditorCore/Automation/ProjectMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

// M4 contract stub (Roadmap rule 3): stream C (methods) implements the project domain: its handlers, the
// registration of its structs and enums (JSON keys per the conventions of MethodRegistry.h) and of its methods.

namespace Engine {

	namespace Automation {

		Result<ProjectCreateResult> ProjectCreate(EditorMethodContext& /*context*/, const ProjectCreateParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::ProjectCreate is an M4 contract stub");
		}

		Result<ProjectOpenResult> ProjectOpen(EditorMethodContext& /*context*/, const ProjectOpenParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::ProjectOpen is an M4 contract stub");
		}

		Result<ProjectSaveResult> ProjectSave(EditorMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::ProjectSave is an M4 contract stub");
		}

		Result<ProjectInfoResult> ProjectInfo(EditorMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::ProjectInfo is an M4 contract stub");
		}

		Result<ProjectGetSettingsResult> ProjectGetSettings(EditorMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::ProjectGetSettings is an M4 contract stub");
		}

		Result<ProjectSetSettingsResult> ProjectSetSettings(EditorMethodContext& /*context*/, const ProjectSetSettingsParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::ProjectSetSettings is an M4 contract stub");
		}

		Result<ProjectValidateResult> ProjectValidate(EditorMethodContext& /*context*/, const ProjectValidateParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::ProjectValidate is an M4 contract stub");
		}

		Result<ProjectUpgradeResult> ProjectUpgrade(EditorMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::ProjectUpgrade is an M4 contract stub");
		}

	}

	void RegisterProjectMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterProjectMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
