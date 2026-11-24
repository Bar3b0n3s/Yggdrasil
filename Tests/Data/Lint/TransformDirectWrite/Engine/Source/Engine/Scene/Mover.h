#pragma once

#include "Engine/Scene/Entity.h"

namespace Engine {

	[[nodiscard]] float ReadHeight(Entity& entity);

	void Lift(Entity& entity, float height);

	void Teleport(Entity& entity, const glm::vec3& position);

	void Nudge(Entity& entity, Registry& registry);

}
