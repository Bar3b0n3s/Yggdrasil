#include "TestsPCH.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderingRegistration: Environment and PostProcess are unique per scene" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			CHECK(registry->FindComponent("Environment")->HasFlag(ComponentFlags::UniquePerScene));
			CHECK(registry->FindComponent("PostProcess")->HasFlag(ComponentFlags::UniquePerScene));
			CHECK_FALSE(registry->FindComponent("Camera")->HasFlag(ComponentFlags::UniquePerScene));
		}

		TEST_CASE("RenderingRegistration: colours are Color fields and ranges follow the table" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			CHECK(registry->FindComponent("Camera")->FindField("ClearColor")->GetKind() == FieldType::Color3);
			CHECK(registry->FindComponent("Text")->FindField("Color")->GetKind() == FieldType::Color4);
			CHECK(registry->FindComponent("Environment")->FindField("FallbackColor")->GetKind() == FieldType::Color3);

			const FieldInfo* cascades = registry->FindComponent("DirectionalLight")->FindField("CascadeCount");
			REQUIRE(cascades != nullptr);
			CHECK(cascades->GetMeta().Min == 1.0);
			CHECK(cascades->GetMeta().Max == 4.0);

			const FieldInfo* blur = registry->FindComponent("Environment")->FindField("SkyboxBlur");
			REQUIRE(blur != nullptr);
			CHECK(blur->GetMeta().Min == 0.0);
			CHECK(blur->GetMeta().Max == 1.0);

			CHECK(registry->FindComponent("MeshRenderer")->FindField("Mesh")->GetMeta().AssetFilter == "Mesh");
			CHECK(registry->FindComponent("MeshRenderer")->FindField("Materials")->GetType().GetElement()->GetAssetTypeName() == "Material");
		}

		TEST_CASE("RenderingRegistration: every rendering enum value is registered by name" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const std::vector<std::pair<std::string_view, std::vector<std::string>>> enums = {
				{ "ProjectionType", { "Perspective", "Orthographic" } },
				{ "ClearMode", { "Skybox", "Color" } },
				{ "Tonemapper", { "AgX", "ACES", "PbrNeutral", "Linear" } },
				{ "SsaoQuality", { "Low", "Medium", "High" } },
				{ "TextSpace", { "Screen", "World" } },
				{ "TextAlignment", { "Left", "Center", "Right" } },
			};
			for (const auto& [name, values] : enums)
			{
				INFO(std::string(name));
				const EnumInfo* info = registry->FindEnum(name);
				REQUIRE(info != nullptr);
				std::vector<std::string> registered;
				for (const EnumEntry& entry : info->GetEntries())
					registered.push_back(entry.Name);
				CHECK(registered == values);
			}
		}
	}

}
