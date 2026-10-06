#include "TestsPCH.h"

#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("BuiltinComponents: every listed type is registered in list order" * doctest::skip(true))
		{
			TypeRegistry registry;
			RegisterBuiltinComponents(registry);
			registry.Freeze();
			REQUIRE(registry.AreComponentsRegistered(BuiltinComponents{}));
			REQUIRE(registry.GetComponents().size() == BuiltinComponents::Size);

			size_t index = 0;
			ForEachType(BuiltinComponents{}, [&]<typename T>()
			{
				const ComponentInfo* info = registry.FindComponent<T>();
				REQUIRE(info != nullptr);
				CHECK(info->GetIndex() == index);
				CHECK(registry.GetComponents()[index] == info);
				++index;
			});
		}

		TEST_CASE("BuiltinComponents: registry names are those of the component table" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const std::vector<std::string> expected = {
				"ID",
				"Name",
				"Tags",
				"Relationship",
				"Transform",
				"Prefab",
				"PrefabLink",
				"MeshRenderer",
				"Camera",
				"DirectionalLight",
				"PointLight",
				"SpotLight",
				"Environment",
				"PostProcess",
				"Text",
				"RigidBody",
				"BoxCollider",
				"SphereCollider",
				"CapsuleCollider",
				"MeshCollider",
				"CharacterController",
				"AudioSource",
				"AudioListener",
				"Script",
			};
			std::vector<std::string> names;
			for (const ComponentInfo* component : registry->GetComponents())
				names.push_back(component->GetName());
			CHECK(names == expected);
		}

		TEST_CASE("BuiltinComponents: every component has a category, a version and descriptions" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const std::vector<std::string> categories = { "Core", "Rendering", "Physics", "Audio", "Scripting" };
			for (const ComponentInfo* component : registry->GetComponents())
			{
				INFO(component->GetName());
				CHECK(std::find(categories.begin(), categories.end(), component->GetCategory()) != categories.end());
				CHECK(component->GetVersion() == 1);
				CHECK_FALSE(component->GetDescription().empty());
				for (const Scope<FieldInfo>& field : component->GetFields())
					CHECK_FALSE(field->GetDescription().empty());
			}
		}

		TEST_CASE("BuiltinComponents: default member initializers are valid for their own metadata" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			for (const ComponentInfo* component : registry->GetComponents())
			{
				INFO(component->GetName());
				const ObjectPtr object = component->CreateDefault();
				REQUIRE(object != nullptr);
				ResolveContext resolve;
				resolve.Registry = registry.get();
				ValidationContext context;
				component->Validate(object.get(), resolve, context);
				CHECK_FALSE(context.HasErrors());
			}
		}
	}

}
