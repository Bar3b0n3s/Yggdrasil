#pragma once

#include "Engine/Core/Base.h"

#include <entt/entity/fwd.hpp>

#include <vector>

namespace Engine {

	class ConstEntity;
	class Entity;
	class Scene;

	// The type-erased ECS operations of one registered component type (Architecture §5.4 "type-erased Add/Remove/Has/
	// Patch"). Reflection's ComponentInfo stores a pointer to them without knowing this definition (ADR 0006); Scene's
	// RegisterComponent<T> installs the instance generated for T (Detail::ComponentHostOpsFor<T>). Every operation but
	// CollectCanonical (which takes the scene) takes a valid entity (asserted), and every one runs on the main thread; Add,
	// Remove and Patch go through Entity's templates, so they are recorded by the change tracker and increment the revision.
	// Has and GetConst take a read-only handle, so code holding a const Scene& (the serializer) can use them; an Entity
	// converts implicitly.
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
		// M43: adds a copy of `source` (an object of the component's type) and returns it (asserts that it is absent): the
		// scene sees the component added with its value, so state the scene derives when a component is added (the Static
		// rule's HierarchyStaticTag) follows that value. A load adds its components through it.
		void* (*AddCopy)(Entity entity, const void* source) = nullptr;
		// Removes the component (asserts that it is present).
		void (*Remove)(Entity entity) = nullptr;
		// Calls `mutate(component, context)` inside Entity::Patch<T> (asserts that it is present).
		void (*Patch)(Entity entity, void (*mutate)(void* component, void* context), void* context) = nullptr;
		// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): `out` set to the live entities of `scene` that hold the
		// component, in canonical order (Scene::CollectCanonical<T>), so a by-name query visits only its holders.
		void (*CollectCanonical)(const Scene& scene, std::vector<entt::entity>& out) = nullptr;
	};

}
