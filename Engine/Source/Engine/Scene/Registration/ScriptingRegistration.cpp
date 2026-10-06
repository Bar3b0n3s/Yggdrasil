#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

// M3 contract stub (Roadmap rule 3): stream D (components) registers (Docs/Decisions/0006-m3-decisions.md, decision 9,
// has the flags, relations and field metadata):
//   Script (NoShortcut; Fields: VariantField with the script field resolver, which checks ResolveContext::OwnerType,
//   reads the Script handle from Owner or, without a C++ object, from OwnerJson["Script"], and resolves each key through
//   ResolveContext::Schemas->FindField(Script handle, key)).

namespace Engine {

	void RegisterScriptingComponents(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
