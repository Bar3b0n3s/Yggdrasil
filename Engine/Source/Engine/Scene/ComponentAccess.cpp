#include "EnginePCH.h"
#include "Engine/Scene/ComponentAccess.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Entity.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream B (Scene core) implements by-name component access on top of the registry
// and ComponentHostOps.

namespace Engine {

	Result<Json> ComponentAccess::GetComponentJson(ConstEntity /*entity*/, std::string_view /*component*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ComponentAccess::GetComponentJson is an M3 contract stub");
	}

	Status ComponentAccess::AddComponent(Entity /*entity*/, std::string_view /*component*/, const Json* /*initial*/,
		const IFieldSchemaSource* /*schemas*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ComponentAccess::AddComponent is an M3 contract stub");
	}

	Status ComponentAccess::RemoveComponent(Entity /*entity*/, std::string_view /*component*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ComponentAccess::RemoveComponent is an M3 contract stub");
	}

	Status ComponentAccess::SetComponentJson(Entity /*entity*/, std::string_view /*component*/, const Json& /*value*/,
		const IFieldSchemaSource* /*schemas*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ComponentAccess::SetComponentJson is an M3 contract stub");
	}

	Status ComponentAccess::PatchComponentJson(Entity /*entity*/, std::string_view /*component*/, const Json& /*patch*/,
		const IFieldSchemaSource* /*schemas*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ComponentAccess::PatchComponentJson is an M3 contract stub");
	}

	Result<Value> ComponentAccess::GetFieldValue(Entity /*entity*/, std::string_view /*component*/, std::string_view /*field*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ComponentAccess::GetFieldValue is an M3 contract stub");
	}

	Status ComponentAccess::SetFieldValue(Entity /*entity*/, std::string_view /*component*/, std::string_view /*field*/,
		const Value& /*value*/, const IFieldSchemaSource* /*schemas*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ComponentAccess::SetFieldValue is an M3 contract stub");
	}

}
