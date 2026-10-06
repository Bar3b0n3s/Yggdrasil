#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Entity.h"

#include <string_view>
#include <type_traits>

namespace Engine {

	namespace Detail {

		template<typename T>
		bool HasComponentOp(ConstEntity entity)
		{
			return entity.HasComponent<T>();
		}

		template<typename T>
		const void* GetConstComponentOp(ConstEntity entity)
		{
			return entity.TryGetComponent<T>();
		}

		template<typename T>
		void* GetComponentOp(Entity entity)
		{
			return entity.TryGetComponent<T>();
		}

		template<typename T>
		void* AddComponentOp(Entity entity)
		{
			return &entity.AddComponent<T>();
		}

		template<typename T>
		void RemoveComponentOp(Entity entity)
		{
			entity.RemoveComponent<T>();
		}

		template<typename T>
		void PatchComponentOp(Entity entity, void (*mutate)(void* component, void* context), void* context)
		{
			entity.Patch<T>([mutate, context](T& component)
			{
				mutate(&component, context);
			});
		}

		// The ECS operations of component type T, built on Entity's templates. A constant, not mutable state.
		template<typename T>
		inline constexpr ComponentHostOps ComponentHostOpsFor = {
			.Has = &HasComponentOp<T>,
			.GetConst = &GetConstComponentOp<T>,
			.Get = &GetComponentOp<T>,
			.Add = &AddComponentOp<T>,
			.Remove = &RemoveComponentOp<T>,
			.Patch = &PatchComponentOp<T>,
		};

	}

	// Registers component type T with the registry and installs its ECS operations, so the serializer, ComponentAccess,
	// automation and scripts can add, remove, read and patch it by name (Architecture §5.4). Every built-in component is
	// registered through this function by the Scene/Registration/*Registration.cpp files:
	//     RegisterComponent<RigidBodyComponent>(registry, "RigidBody", "Simulates the entity with Jolt physics.")
	//         .Category("Physics").Version(1).Requires<TransformComponent>().Excludes<CharacterControllerComponent>()
	//         .Field(...);
	template<typename T>
	[[nodiscard]] ComponentBuilder<T> RegisterComponent(TypeRegistry& registry, std::string_view name, std::string_view description)
	{
		static_assert(!std::is_empty_v<T>, "Tag components are not reflected (DisabledTag is the entity key \"Active\")");
		return registry.Component<T>(name, description).HostOps(&Detail::ComponentHostOpsFor<T>);
	}

}
