#pragma once

namespace Engine {

	class Scene
	{
	public:
		int GetEntityCount() const { return m_EntityCount; }
	private:
		int m_EntityCount = 0;
	};

}
