#include "TestsPCH.h"
#include "Support/ValidatorTestTypes.h"

#include "EditorCore/Automation/RegisterMethods.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

namespace Engine {

	namespace Test {

		void RegisterValidatorTestTypes(TypeRegistry& registry)
		{
			RegisterEditorMethodTypes(registry);
			RegisterComponent<ValidatorTargetComponent>(registry, "ValidatorTarget", "Refers to another entity of the scene (validator tests).")
				.Category("Tests")
				.Version(1)
				.Field("Target", &ValidatorTargetComponent::Target, "The entity it refers to.");
			RegisterComponent<ValidatorFollowerComponent>(registry, "ValidatorFollower", "Follows its ValidatorTarget (validator tests).")
				.Category("Tests")
				.Version(1)
				.Requires<ValidatorTargetComponent>()
				.Field("Speed", &ValidatorFollowerComponent::Speed, "How fast it follows, in metres per second.", { .Min = 0.0, .Unit = "m/s" });
		}

	}

}
