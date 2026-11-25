#pragma once

#include <glm/vec3.hpp>

namespace Engine {

	struct TransformComponent
	{
		glm::vec3 Translation = glm::vec3(0.0f);
	};

	struct RigidBodyComponent
	{
		float Mass = 1.0f;
	};

	// A stand-in for the scene's entity handle: GetComponent and TryGetComponent return the component for a direct write,
	// which bypasses change tracking; Patch writes it through the tracked path.
	class Entity
	{
	public:
		template<typename T>
		[[nodiscard]] T& GetComponent()
		{
			return m_Transform;
		}

		template<typename T>
		[[nodiscard]] T* TryGetComponent()
		{
			return &m_Transform;
		}

		template<typename T, typename Func>
		void Patch(Func&& func)
		{
			func(m_Transform);
		}
	private:
		TransformComponent m_Transform;
	};

	// A stand-in for EnTT's registry: get returns the component.
	class Registry
	{
	public:
		template<typename T>
		[[nodiscard]] T& get(int /*entity*/)
		{
			return m_Transform;
		}
	private:
		TransformComponent m_Transform;
	};

}
