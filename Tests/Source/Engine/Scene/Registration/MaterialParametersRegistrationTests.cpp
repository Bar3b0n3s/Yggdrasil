#include "TestsPCH.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

#include <string_view>
#include <utility>
#include <vector>

// The MaterialParameters category (M31; Docs/Decisions/0046-m31-material-parameters.md decision 1): a category of its own
// after Terrain (ADR 0042's phase-5 ledger), the component requiring the MeshRenderer whose materials it overrides, its
// fields' ranges refusing what the renderer cannot draw.

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("MaterialParametersRegistration: the component is registered after the TerrainCollider in a category of its own, requiring the MeshRenderer")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo* collider = registry->FindComponent("TerrainCollider");
			const ComponentInfo* info = registry->FindComponent("MaterialParameters");
			REQUIRE(collider != nullptr);
			REQUIRE(info != nullptr);
			CHECK(info->GetIndex() == collider->GetIndex() + 1);
			CHECK(info->GetCategory() == "MaterialParameters");
			CHECK(info->GetVersion() == 1);
			REQUIRE(info->GetRequires().size() == 1);
			CHECK(info->GetRequires()[0] == registry->FindComponent("MeshRenderer"));
			CHECK(info->GetExcludes().empty());
			const std::vector<std::string_view> fields = { "Tint", "Emissive", "EmissiveStrength", "UVOffset", "UVScroll", "DissolveAmount",
				"DissolveEdgeColor", "DissolveEdgeStrength", "DissolveEdgeWidth", "DissolveNoiseScale", "Opacity" };
			REQUIRE(info->GetFields().size() == fields.size());
			for (size_t index = 0; index < fields.size(); ++index)
			{
				CAPTURE(fields[index]);
				CHECK(info->GetFields()[index]->GetName() == fields[index]);
				CHECK(info->GetFields()[index]->GetMeta().Scriptable);
			}
			CHECK(info->FindField("Tint")->GetKind() == FieldType::Color3);
			CHECK(info->FindField("DissolveEdgeColor")->GetKind() == FieldType::Color3);
			CHECK(info->FindField("UVScroll")->GetKind() == FieldType::Vec2);
		}

		// The defaults are valid, and each field refuses a value outside the range the renderer draws.
		TEST_CASE("MaterialParametersRegistration: the defaults are valid and every field refuses a value outside its range")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo* info = registry->FindComponent("MaterialParameters");
			REQUIRE(info != nullptr);
			ResolveContext resolve;
			resolve.Registry = registry.get();
			const auto errors = [&info, &resolve](const MaterialParametersComponent& component)
			{
				ValidationContext context;
				info->Validate(&component, resolve, context);
				return context.GetErrorCount();
			};
			CHECK(errors(MaterialParametersComponent{}) == 0);

			const std::vector<std::pair<std::string_view, MaterialParametersComponent>> invalid = {
				{ "Tint", { .Tint = glm::vec3(1.5f, 1.0f, 1.0f) } },
				{ "Emissive", { .Emissive = glm::vec3(1.0f, 1.0f, -0.1f) } },
				{ "EmissiveStrength", { .EmissiveStrength = -1.0f } },
				{ "UVOffset", { .UVOffset = glm::vec2(2000.0f, 0.0f) } },
				{ "UVScroll", { .UVScroll = glm::vec2(0.0f, -2000.0f) } },
				{ "DissolveAmount", { .DissolveAmount = 1.5f } },
				{ "DissolveEdgeColor", { .DissolveEdgeColor = glm::vec3(1.0f, 2.0f, 0.1f) } },
				{ "DissolveEdgeStrength", { .DissolveEdgeStrength = -0.5f } },
				{ "DissolveEdgeWidth", { .DissolveEdgeWidth = 1.5f } },
				{ "DissolveNoiseScale", { .DissolveNoiseScale = 0.0f } },
				{ "Opacity", { .Opacity = -0.25f } },
			};
			for (const auto& [field, component] : invalid)
			{
				CAPTURE(field);
				CHECK(errors(component) == 1);
			}
		}
	}

}
