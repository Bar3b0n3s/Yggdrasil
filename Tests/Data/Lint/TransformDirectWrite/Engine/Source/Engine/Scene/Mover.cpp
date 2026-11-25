#include "Engine/Scene/Mover.h"

namespace Engine {

	float ReadHeight(Entity& entity)
	{
		// Controls: reads through the mutable access (a const reference, a copy, a comparison, a pointer to const) are not
		// findings.
		const TransformComponent& transform = entity.GetComponent<TransformComponent>();
		const TransformComponent copy = entity.GetComponent<TransformComponent>();
		const auto* pointer = entity.TryGetComponent<TransformComponent>();
		if (entity.GetComponent<TransformComponent>().Translation == copy.Translation)
			return transform.Translation.y + pointer->Translation.y;
		return 0.0f;
	}

	void Lift(Entity& entity, float height)
	{
		// Control: a write through Patch is tracked.
		entity.Patch<TransformComponent>([height](TransformComponent& transform)
		{
			transform.Translation.y = height;
		});
	}

	void Teleport(Entity& entity, const glm::vec3& position)
	{
		// Seeded defects: an assignment through the mutable access, and a mutable reference bound to it, write the
		// component without change tracking.
		entity.GetComponent<TransformComponent>().Translation = position;
		TransformComponent& transform = entity.GetComponent<TransformComponent>();
		transform.Translation.y += 1.0f;
	}

	void Nudge(Entity& entity, Registry& registry)
	{
		// Seeded defects: a compound assignment through the registry, a mutable pointer in an if initializer, an increment,
		// and an address taken into a mutable pointer.
		registry.get<TransformComponent>(0).Translation.x *= 2.0f;
		if (auto* found = entity.TryGetComponent<TransformComponent>(); found != nullptr)
			found->Translation.z = 1.0f;
		++entity.GetComponent<TransformComponent>().Translation.x;
		auto address = &entity.GetComponent<TransformComponent>();
		address->Translation.y = 2.0f;
	}

	// Seeded defect: a view of the mutable component.
	template<typename AnyRegistry>
	void Scan(AnyRegistry& registry)
	{
		static_cast<void>(registry.template view<TransformComponent>());
	}

	// Seeded defects: a get and a try_get of several components that name the mutable component (their tuple binds mutable
	// references, even through a const structured binding), a group that owns it, and a group that reaches it through its
	// get list.
	template<typename AnyRegistry>
	void Settle(AnyRegistry& registry, int entity)
	{
		const auto [settled, body] = registry.template get<TransformComponent, RigidBodyComponent>(entity);
		settled.Translation.y = body.Mass;
		static_cast<void>(registry.template try_get<RigidBodyComponent, TransformComponent>(entity));
		static_cast<void>(registry.template group<TransformComponent>());
		static_cast<void>(registry.template group<RigidBodyComponent>(typename AnyRegistry::template get<TransformComponent>{}));
	}

	// Controls: a get of several components and a group that name the component const read only.
	template<typename AnyRegistry>
	float Weigh(AnyRegistry& registry, int entity)
	{
		const auto [transform, body] = registry.template get<const TransformComponent, const RigidBodyComponent>(entity);
		static_cast<void>(registry.template group<const TransformComponent>());
		return transform.Translation.y + body.Mass;
	}

}
