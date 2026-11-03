#include "TestsPCH.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

#include <cstddef>
#include <span>
#include <string_view>

// The Rendering2D category (Docs/Decisions/0032-m21-contract.md decisions 3, 5, 6, 20 and 24): the contracts register M21's
// sprite, light and tilemap components, so these cases are live; their behaviour is frozen by the skipped cases of the systems
// that read them. SpriteRenderer's NormalMap (C21.2, decision 21) is registered by stream L.

namespace Engine {

	namespace {

		const ComponentInfo& Require2DComponent(const TypeRegistry& registry, std::string_view name)
		{
			const ComponentInfo* info = registry.FindComponent(name);
			REQUIRE(info != nullptr);
			return *info;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Rendering2DRegistration: the sprite components are registered after Scripting with their requirements")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& script = Require2DComponent(*registry, "Script");
			const ComponentInfo& sprite = Require2DComponent(*registry, "SpriteRenderer");
			const ComponentInfo& animator = Require2DComponent(*registry, "SpriteAnimator");
			const ComponentInfo& camera = Require2DComponent(*registry, "PixelPerfectCamera");
			// ADR 0028's phase-2 ledger appends each milestone's categories after Scripting in landing order, so M22's may lie
			// between Script and M21's block: the relative order and M21's own chain only, never an absolute neighbour that a
			// sibling's block can take (Docs/Decisions/0032-m21-contract.md, C21.2's review). Whatever lies between belongs to
			// a sibling's category of its own, neither one before Scripting nor one of M21's.
			CHECK(sprite.GetIndex() > script.GetIndex());
			const std::span<const ComponentInfo* const> components = registry->GetComponents();
			for (size_t index = script.GetIndex() + 1; index < sprite.GetIndex() && index < components.size(); ++index)
			{
				CAPTURE(components[index]->GetName());
				for (const std::string_view category : { "Core", "Rendering", "Physics", "Audio", "Scripting", "Rendering2D", "Physics2D" })
					CHECK(components[index]->GetCategory() != category);
			}
			CHECK(animator.GetIndex() == sprite.GetIndex() + 1);
			CHECK(camera.GetIndex() == animator.GetIndex() + 1);
			for (const ComponentInfo* info : { &sprite, &animator, &camera })
			{
				CAPTURE(info->GetName());
				CHECK(info->GetCategory() == "Rendering2D");
				CHECK(info->GetVersion() == 1);
				REQUIRE(info->GetRequires().size() == 1);
			}
			CHECK(sprite.GetRequires()[0] == registry->FindComponent("Transform"));
			CHECK(animator.GetRequires()[0] == &sprite);
			CHECK(camera.GetRequires()[0] == registry->FindComponent("Camera"));

			CHECK(sprite.FindField("Sheet")->GetKind() == FieldType::AssetRef);
			CHECK(sprite.FindField("Sheet")->GetMeta().AssetFilter == "SpriteSheet");
			CHECK(sprite.FindField("Texture")->GetMeta().AssetFilter == "Texture");
			CHECK(sprite.FindField("Color")->GetKind() == FieldType::Color4);
			CHECK(sprite.FindField("OrderInLayer")->GetKind() == FieldType::Int32);
			CHECK(sprite.FindField("Lit")->GetKind() == FieldType::Bool);
			CHECK(camera.FindField("LetterboxColor")->GetKind() == FieldType::Color3);
			REQUIRE(camera.FindField("ReferenceWidth")->GetMeta().Max.has_value());
			CHECK(*camera.FindField("ReferenceWidth")->GetMeta().Max == 8192.0);
		}

		TEST_CASE("Rendering2DRegistration: Light2D and the tilemap renderer follow the pixel-perfect camera with their requirements")
		{
			// C21.2 (decisions 20 and 24): appended to M21's own category, after C21.1's three components.
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& camera = Require2DComponent(*registry, "PixelPerfectCamera");
			const ComponentInfo& light = Require2DComponent(*registry, "Light2D");
			const ComponentInfo& tilemap = Require2DComponent(*registry, "TilemapRenderer");
			CHECK(light.GetIndex() == camera.GetIndex() + 1);
			CHECK(tilemap.GetIndex() == light.GetIndex() + 1);
			for (const ComponentInfo* info : { &light, &tilemap })
			{
				CAPTURE(info->GetName());
				CHECK(info->GetCategory() == "Rendering2D");
				CHECK(info->GetVersion() == 1);
				REQUIRE(info->GetRequires().size() == 1);
				CHECK(info->GetRequires()[0] == registry->FindComponent("Transform"));
				CHECK(info->GetExcludes().empty());
			}

			CHECK(light.FindField("Type")->GetKind() == FieldType::Enum);
			const EnumInfo* types = registry->FindEnum("Light2DType");
			REQUIRE(types != nullptr);
			REQUIRE(types->GetEntries().size() == 2);
			CHECK(types->GetEntries()[0].Name == "Point");
			CHECK(types->GetEntries()[1].Name == "Global");
			CHECK(light.FindField("Color")->GetKind() == FieldType::Color3);
			for (const std::string_view name : { "Intensity", "Radius", "Falloff", "Height" })
			{
				CAPTURE(name);
				REQUIRE(light.FindField(name) != nullptr);
				CHECK(light.FindField(name)->GetKind() == FieldType::Float);
			}
			REQUIRE(light.FindField("Falloff")->GetMeta().Max.has_value());
			CHECK(*light.FindField("Falloff")->GetMeta().Max == 1.0);

			CHECK(tilemap.FindField("Tilemap")->GetKind() == FieldType::AssetRef);
			CHECK(tilemap.FindField("Tilemap")->GetMeta().AssetFilter == "Tilemap");
			CHECK(tilemap.FindField("Color")->GetKind() == FieldType::Color4);
			CHECK(tilemap.FindField("SortingLayer")->GetKind() == FieldType::String);
			CHECK(tilemap.FindField("OrderInLayer")->GetKind() == FieldType::Int32);
			CHECK(tilemap.FindField("Lit")->GetKind() == FieldType::Bool);
		}

		// Decision 21: NormalMap is frozen in SpriteRendererComponent.h and registered by stream L, whose registration changes
		// FeatureTest's canonical sprite scenes, which W21.2's integration regenerates.
		TEST_CASE("Rendering2DRegistration: SpriteRenderer's NormalMap is a texture field after Lit" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& sprite = Require2DComponent(*registry, "SpriteRenderer");
			const FieldInfo* normalMap = sprite.FindField("NormalMap");
			REQUIRE(normalMap != nullptr);
			CHECK(normalMap->GetKind() == FieldType::AssetRef);
			CHECK(normalMap->GetMeta().AssetFilter == "Texture");
			const auto& fields = sprite.GetFields();
			REQUIRE(fields.size() >= 2);
			CHECK(fields[fields.size() - 2]->GetName() == "Lit");
			CHECK(fields.back()->GetName() == "NormalMap");
		}
	}

}
