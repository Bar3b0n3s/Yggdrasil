#include "TestsPCH.h"

#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("AudioRegistration: AudioSource and AudioListener are registered with their enums")
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
			CHECK(listener->GetCategory() == "Audio");
			CHECK(listener->FindField("Primary")->GetKind() == FieldType::Bool);
		}

		TEST_CASE("AudioRegistration: attenuation distances and pitch are strictly positive")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo* source = registry->FindComponent("AudioSource");
			REQUIRE(source != nullptr);
			for (const std::string_view field : { "MinDistance", "MaxDistance", "Pitch" })
			{
				INFO(std::string(field));
				const FieldMeta& meta = source->FindField(field)->GetMeta();
				REQUIRE(meta.Min.has_value());
				CHECK(*meta.Min > 0.0);
			}
			CHECK(source->FindField("MinDistance")->GetMeta().Unit == "m");
			for (const std::string_view field : { "Volume", "Rolloff", "DopplerFactor" })
			{
				INFO(std::string(field));
				CHECK(source->FindField(field)->GetMeta().Min == 0.0);
			}
		}

		TEST_CASE("AudioRegistration: MinDistance does not exceed MaxDistance")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo* sourceType = registry->FindComponent("AudioSource");
			REQUIRE(sourceType != nullptr);
			ResolveContext resolve;
			resolve.Registry = registry.get();

			AudioSourceComponent source;
			source.MinDistance = 50.0f; // equal to MaxDistance: no attenuation, but valid
			ValidationContext equal;
			sourceType->Validate(&source, resolve, equal);
			CHECK_FALSE(equal.HasErrors());

			source.MinDistance = 60.0f;
			ValidationContext inverted;
			sourceType->Validate(&source, resolve, inverted);
			REQUIRE(inverted.GetErrorCount() == 1);
			CHECK(inverted.GetIssues()[0].JsonPointer == "/MinDistance");

			Random random(9);
			sourceType->Generate(&source, random);
			CHECK(source.MinDistance == 50.0f);
			CHECK(source.MaxDistance == 60.0f);
			ValidationContext repaired;
			sourceType->Validate(&source, resolve, repaired);
			CHECK_FALSE(repaired.HasErrors());
		}
	}

}
