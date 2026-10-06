#include "EditorPCH.h"
#include "EditorCore/Automation/EntityMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

// M4 contract stub (Roadmap rule 3): stream C (methods) implements the entity domain: its handlers, the
// registration of its structs and enums (JSON keys per the conventions of MethodRegistry.h) and of its methods.

namespace Engine {

	namespace Automation {

		Result<EntityCreateResult> EntityCreate(EditorMethodContext& /*context*/, const EntityCreateParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::EntityCreate is an M4 contract stub");
		}

		Result<EntityGetResult> EntityGet(EditorMethodContext& /*context*/, const EntityGetParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::EntityGet is an M4 contract stub");
		}

		Result<EntityUpdateResult> EntityUpdate(EditorMethodContext& /*context*/, const EntityUpdateParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::EntityUpdate is an M4 contract stub");
		}

		Result<EntityDestroyResult> EntityDestroy(EditorMethodContext& /*context*/, const EntityDestroyParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::EntityDestroy is an M4 contract stub");
		}

		Result<EntityDuplicateResult> EntityDuplicate(EditorMethodContext& /*context*/, const EntityDuplicateParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::EntityDuplicate is an M4 contract stub");
		}

		Result<EntityReparentResult> EntityReparent(EditorMethodContext& /*context*/, const EntityReparentParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "Automation::EntityReparent is an M4 contract stub");
		}

	}

	void RegisterEntityMethodTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void RegisterEntityMethods(MethodRegistry& /*methods*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
