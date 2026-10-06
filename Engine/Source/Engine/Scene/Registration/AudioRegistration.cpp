#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

// M3 contract stub (Roadmap rule 3): stream D (components) registers, in this order (Docs/Decisions/0006-m3-decisions.md,
// decision 9, has the flags, relations and field metadata):
//   enums Attenuation, AudioGroup; AudioSource, AudioListener.

namespace Engine {

	void RegisterAudioComponents(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
