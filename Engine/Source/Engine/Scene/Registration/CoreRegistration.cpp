#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"
#include "Engine/Scene/TransformSystem.h"

// M3 contract stub (Roadmap rule 3): stream D (components) registers, in this order (Docs/Decisions/0006-m3-decisions.md,
// decision 9, has the flags, relations and field metadata):
//   enum PrefabOverrideKind; struct PrefabEntityKeys; struct PrefabOverride (Value: VariantField with the prefab
//   override resolver, which checks ResolveContext::OwnerType and resolves Field, AddComponent and EntityKey values);
//   ID (Required, Hidden, EntityLevel, not Removable; ID read-only), Name (Required, EntityLevel, not Removable),
//   Tags (EntityLevel, not Removable), Relationship (Required, Hidden, EntityLevel, not Removable; fields read-only),
//   Transform (Required, not Removable; Scale MinMagnitude 1e-4; virtual EulerAngles, WorldPosition, WorldRotation,
//   WorldScale, RenderPosition, RenderRotation through TransformSystem), Prefab (Hidden, not EditorVisible, not
//   Removable), PrefabLink (Hidden, not EditorVisible, not Removable).

namespace Engine {

	void RegisterCoreComponents(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
