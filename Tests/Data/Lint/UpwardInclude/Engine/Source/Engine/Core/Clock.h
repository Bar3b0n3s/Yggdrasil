#pragma once

#include "Engine/Scene/Scene.h"

namespace Engine {

	// Seeded defect: Core (layer 0) includes a Scene header (layer 3).
	class Clock
	{
	public:
		explicit Clock(const Scene& scene)
			: m_Scene(&scene)
		{
		}

		const Scene& GetScene() const { return *m_Scene; }
	private:
		const Scene* m_Scene = nullptr;
	};

}
