#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Scene.h"

#include <cstdint>

// Shared setup for Reflection, Scene, serializer and prefab tests: a frozen registry with every built-in component and the
// project settings types, a seeded deterministic UUID generator and an empty scene.

namespace Engine {

	namespace Test {

		// A new registry with RegisterBuiltinComponents, RegisterProjectSettingsTypes, RegisterAssetTypes and
		// RegisterAssetPipelineTypes applied (every reflected component and struct the registry suite covers, §5.4), frozen.
		[[nodiscard]] Scope<TypeRegistry> CreateBuiltinRegistry();

		// The registry, generator and scene of one test. Not copyable or movable (the scene points at the other two).
		class SceneTestFixture
		{
		public:
			// An empty edit scene (or a runtime scene) named "Test" with Seed 0 and a Deterministic generator seeded with
			// `seed`.
			explicit SceneTestFixture(uint64_t seed = 1, bool runtime = false);

			SceneTestFixture(const SceneTestFixture&) = delete;
			SceneTestFixture& operator=(const SceneTestFixture&) = delete;
			SceneTestFixture(SceneTestFixture&&) = delete;
			SceneTestFixture& operator=(SceneTestFixture&&) = delete;

			[[nodiscard]] TypeRegistry& GetRegistry() { return *m_Registry; }
			[[nodiscard]] const TypeRegistry& GetRegistry() const { return *m_Registry; }
			[[nodiscard]] UUIDGenerator& GetGenerator() { return m_Generator; }
			[[nodiscard]] Scene& GetScene() { return *m_Scene; }

			// A second empty scene sharing the registry and generator (serializer copies, prefab targets).
			[[nodiscard]] Scope<Scene> CreateEmptyScene(bool runtime = false);
		private:
			Scope<TypeRegistry> m_Registry;
			UUIDGenerator m_Generator;
			Scope<Scene> m_Scene;
		};

	}

}
