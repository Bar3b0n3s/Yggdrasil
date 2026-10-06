#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

// M3 contract stub (Roadmap rule 3): stream D (components) registers, in this order (Docs/Decisions/0006-m3-decisions.md,
// decision 9, has the flags, relations and field metadata):
//   enums ProjectionType, ClearMode, Tonemapper, SsaoQuality, TextSpace, TextAlignment;
//   MeshRenderer, Camera, DirectionalLight (CascadeCount 1-4), PointLight, SpotLight (validator: inner cone below outer,
//   with its Generate hook), Environment (UniquePerScene; SkyboxBlur 0-1), PostProcess (UniquePerScene), Text. Colours
//   are ColorFields.

namespace Engine {

	void RegisterRenderingComponents(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
