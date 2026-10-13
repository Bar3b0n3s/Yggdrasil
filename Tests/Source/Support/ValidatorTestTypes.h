#pragma once

#include "Engine/Core/UUID.h"

// The validator's test components (ADR 0022 decision 11), shared by ProjectValidatorTests and the Tests/Data/Validation
// fixtures whose Expected.json sets TestComponents. Every built-in requirement is Transform, which every entity has, and no
// user-facing built-in component has an EntityRef field, so COMPONENT_MISSING_REQUIREMENT and a component's
// ENTITY_DANGLING_REFERENCE need components of the tests' own.

namespace Engine {

	class TypeRegistry;

	namespace Test {

		// Registry component "ValidatorTarget": a user component with an EntityRef field.
		struct ValidatorTargetComponent
		{
			UUID Target;
		};

		// Registry component "ValidatorFollower": requires ValidatorTarget.
		struct ValidatorFollowerComponent
		{
			float Speed = 1.0f;
		};

		// The editor's types (RegisterEditorMethodTypes) plus ValidatorTarget and ValidatorFollower, in that order (an
		// EditorTestFixture registration hook).
		void RegisterValidatorTestTypes(TypeRegistry& registry);

	}

}
