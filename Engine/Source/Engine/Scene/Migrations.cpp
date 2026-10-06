#include "EnginePCH.h"
#include "Engine/Scene/Migrations.h"

#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream C (serialization) implements the file-level and per-component migrations.

namespace Engine {

	Status Migrations::UpgradeDocument(Json& /*document*/, DocumentKind /*kind*/, const TypeRegistry& /*registry*/, LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Migrations::UpgradeDocument is an M3 contract stub");
	}

	Status Migrations::UpgradeVersion0To1(Json& /*document*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Migrations::UpgradeVersion0To1 is an M3 contract stub");
	}

}
