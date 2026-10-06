#include "EnginePCH.h"
#include "Engine/Reflection/JsonSchema.h"

#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream A (Reflection) implements schema generation and the subset validator.

namespace Engine {

	Json JsonSchema::ForStruct(const StructInfo& /*type*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json::object();
	}

	Json JsonSchema::ForField(const FieldInfo& /*field*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json::object();
	}

	Json JsonSchema::ForComponents(const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json::object();
	}

	Status JsonSchema::Validate(const Json& /*schema*/, const Json& /*instance*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "JsonSchema::Validate is an M3 contract stub");
	}

}
