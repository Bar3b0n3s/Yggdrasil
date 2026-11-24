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

}
