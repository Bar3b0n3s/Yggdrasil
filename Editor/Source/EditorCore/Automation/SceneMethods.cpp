#include "EditorPCH.h"
#include "EditorCore/Automation/SceneMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

// M4 contract stub (Roadmap rule 3): stream C (methods) implements the scene domain: its handlers, the
// registration of its structs and enums (JSON keys per the conventions of MethodRegistry.h) and of its methods.

namespace Engine {

	namespace Automation {

		Result<SceneNewResult> SceneNew(EditorMethodContext& /*context*/, const SceneNewParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::SceneNew is an M4 contract stub");
		}

		Result<SceneOpenResult> SceneOpen(EditorMethodContext& /*context*/, const SceneOpenParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::SceneOpen is an M4 contract stub");
		}

		Result<SceneSaveResult> SceneSave(EditorMethodContext& /*context*/, const SceneSaveParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::SceneSave is an M4 contract stub");
		}

		Result<SceneTreeResult> SceneTree(EditorMethodContext& /*context*/, const SceneTreeParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::SceneTree is an M4 contract stub");
		}

		Result<SceneQueryResult> SceneQuery(EditorMethodContext& /*context*/, const SceneQueryParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::SceneQuery is an M4 contract stub");
		}

		Result<SceneGetResult> SceneGet(EditorMethodContext& /*context*/, const SceneGetParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::SceneGet is an M4 contract stub");
		}

		Result<SceneDiffResult> SceneDiff(EditorMethodContext& /*context*/, const SceneDiffParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::SceneDiff is an M4 contract stub");
		}

	}

	void RegisterSceneMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterSceneMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
