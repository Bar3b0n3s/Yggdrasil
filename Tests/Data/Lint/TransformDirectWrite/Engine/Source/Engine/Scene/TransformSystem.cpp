#include "Engine/Scene/TransformSystem.h"

namespace Engine {

	void SnapToGround(Entity& entity)
	{
		// Control: a file of Banned.TransformWriteFiles may write the component directly (the engine's files then call
		// Scene::MarkTransformWritten).
		entity.GetComponent<TransformComponent>().Translation.y = 0.0f;
	}

}
