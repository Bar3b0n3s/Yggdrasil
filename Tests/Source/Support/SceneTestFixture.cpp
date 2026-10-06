#include "TestsPCH.h"
#include "Support/SceneTestFixture.h"

#include "Engine/Project/ProjectSettings.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

namespace Engine {

	namespace Test {

		Scope<TypeRegistry> CreateBuiltinRegistry()
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			RegisterBuiltinComponents(*registry);
			RegisterProjectSettingsTypes(*registry);
			registry->Freeze();
			return registry;
		}

		SceneTestFixture::SceneTestFixture(uint64_t seed, bool runtime)
			: m_Registry(CreateBuiltinRegistry()), m_Generator(UUIDGenerator::CreateDeterministic(seed))
		{
			m_Scene = CreateEmptyScene(runtime);
		}

		Scope<Scene> SceneTestFixture::CreateEmptyScene(bool runtime)
		{
			SceneSpecification specification;
			specification.Name = "Test";
			specification.Registry = m_Registry.get();
			specification.IdGenerator = &m_Generator;
			specification.Runtime = runtime;
			return Scene::Create(specification);
		}

	}

}
