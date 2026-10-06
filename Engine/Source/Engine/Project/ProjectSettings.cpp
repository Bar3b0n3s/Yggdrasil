#include "EnginePCH.h"
#include "Engine/Project/ProjectSettings.h"

#include "Engine/Reflection/TypeRegistry.h"

// M3 contract stub (Roadmap rule 3): stream C (serialization and project) registers the settings types with their field
// metadata, validators and the Generate hooks that make random values satisfy those validators.

namespace Engine {

	void RegisterProjectSettingsTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
