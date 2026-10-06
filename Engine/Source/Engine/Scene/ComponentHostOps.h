#pragma once

#include "Engine/Core/Base.h"

namespace Engine {

	class ConstEntity;
	class Entity;

	// The type-erased ECS operations of one registered component type (Architecture §5.4 "type-erased Add/Remove/Has/
	// Patch"). Reflection's ComponentInfo stores a pointer to them without knowing this definition (ADR 0006); Scene's
	// RegisterComponent<T> installs the instance generated for T (Detail::ComponentHostOpsFor<T>). Every operation takes a
	// valid entity (asserted) and runs on the main thread; Add, Remove and Patch go through Entity's templates, so they are
	// recorded by the change tracker and increment the revision. Has and GetConst take a read-only handle, so code holding
	// a const Scene& (the serializer) can use them; an Entity converts implicitly.
	struct ComponentHostOps
	{
		// True when the entity has the component.
		bool (*Has)(ConstEntity entity) = nullptr;
		// The component object, or nullptr when absent: read-only.
		const void* (*GetConst)(ConstEntity entity) = nullptr;
		// The component object, or nullptr when absent. For writes only by runtime systems (bypasses the change tracker).
		void* (*Get)(Entity entity) = nullptr;
		// Adds a default-constructed component and returns it (asserts that it is absent).
		void* (*Add)(Entity entity) = nullptr;
		// Removes the component (asserts that it is present).
		void (*Remove)(Entity entity) = nullptr;
		// Calls `mutate(component, context)` inside Entity::Patch<T> (asserts that it is present).
		void (*Patch)(Entity entity, void (*mutate)(void* component, void* context), void* context) = nullptr;
	};

}
