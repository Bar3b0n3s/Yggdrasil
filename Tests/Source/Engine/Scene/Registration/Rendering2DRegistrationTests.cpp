#include "TestsPCH.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

#include <string_view>

// The Rendering2D category (Docs/Decisions/0032-m21-contract.md decisions 3, 5 and 6): the contract registers M21's sprite
// components, so these cases are live; their behaviour is frozen by the skipped cases of the sprite systems.

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
			CHECK(sprite.GetIndex() == script.GetIndex() + 1);
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
	}

}
