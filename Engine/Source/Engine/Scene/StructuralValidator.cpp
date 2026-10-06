#include "EnginePCH.h"
#include "Engine/Scene/StructuralValidator.h"

#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream C (serialization) implements structural pre-validation and repair.

namespace Engine {

	Result<Json> StructuralValidator::Validate(const Json& /*document*/, DocumentKind /*kind*/, const TypeRegistry& /*registry*/,
		const LoadOptions& /*options*/, LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "StructuralValidator::Validate is an M3 contract stub");
	}

}
