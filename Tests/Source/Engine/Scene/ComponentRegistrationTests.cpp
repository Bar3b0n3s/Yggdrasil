#include "TestsPCH.h"

#include "Engine/Scene/ComponentRegistration.h"

#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("ComponentRegistration: host operations add, read, patch and remove through Entity" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			const Entity entity = fixture.GetScene().CreateEntity("Box");
			const ComponentInfo* info = fixture.GetRegistry().FindComponent<RigidBodyComponent>();
			REQUIRE(info != nullptr);
			const ComponentHostOps* ops = info->GetHostOps();
			REQUIRE(ops != nullptr);
			CHECK(ops == &Detail::ComponentHostOpsFor<RigidBodyComponent>);

			CHECK_FALSE(ops->Has(entity));
			CHECK(ops->Get(entity) == nullptr);
			void* added = ops->Add(entity);
			REQUIRE(added != nullptr);
			CHECK(added == entity.TryGetComponent<RigidBodyComponent>());
			CHECK(ops->Get(entity) == added);

			// The read-only operations work on a const scene's handles.
			const Scene& constScene = fixture.GetScene();
			const ConstEntity readOnly = constScene.FindEntityByID(entity.GetUUID());
			CHECK(ops->Has(readOnly));
			CHECK(ops->GetConst(readOnly) == added);

			const uint64_t revision = fixture.GetScene().GetRevision();
			float mass = 7.0f;
			ops->Patch(entity, [](void* component, void* context)
			{
				static_cast<RigidBodyComponent*>(component)->Mass = *static_cast<float*>(context);
			}, &mass);
			CHECK(entity.GetComponent<RigidBodyComponent>().Mass == 7.0f);
			CHECK(fixture.GetScene().GetRevision() > revision);

			ops->Remove(entity);
			CHECK_FALSE(ops->Has(entity));
		}

		TEST_CASE("ComponentRegistration: every built-in component has host operations" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			for (const ComponentInfo* component : registry->GetComponents())
			{
				INFO(component->GetName());
				CHECK(component->GetHostOps() != nullptr);
			}
			CHECK(registry->FindComponent<TransformComponent>()->GetHostOps() == &Detail::ComponentHostOpsFor<TransformComponent>);
		}
	}

}
