#include "TestsPCH.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("AudioRegistration: AudioSource and AudioListener are registered with their enums" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo* source = registry->FindComponent("AudioSource");
			REQUIRE(source != nullptr);
			CHECK(source->GetCategory() == "Audio");
			CHECK(source->FindField("Clip")->GetMeta().AssetFilter == "AudioClip");
			CHECK(source->FindField("Attenuation")->GetType().GetEnum() == registry->FindEnum("Attenuation"));
			CHECK(source->FindField("Group")->GetType().GetEnum() == registry->FindEnum("AudioGroup"));
			CHECK(registry->FindEnum("Attenuation")->GetEntries().size() == 4);
			CHECK(registry->FindEnum("AudioGroup")->FindByName("Ui") != nullptr);

			const ComponentInfo* listener = registry->FindComponent("AudioListener");
			REQUIRE(listener != nullptr);
			CHECK(listener->FindField("Primary")->GetKind() == FieldType::Bool);
		}
	}

}
