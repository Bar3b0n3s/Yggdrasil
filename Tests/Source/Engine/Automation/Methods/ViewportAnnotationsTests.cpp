#include "TestsPCH.h"

#include "Engine/Automation/Methods/ScreenshotMethods.h"

#include "Engine/Automation/Methods/Private/RenderQueryTestContext.h"
#include "Engine/Automation/Methods/RaycastMethods.h"
#include "Engine/Scene/Scene.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("ViewportAnnotations: strict options parse with located errors")
		{
			for (const auto& [value, pointer] : std::vector<std::pair<Json, std::string>>{
					 { Json::array(), "/annotate" }, { { { "surprise", true } }, "/annotate/surprise" }, { { { "bounds", 1 } }, "/annotate/bounds" },
					 { { { "labels", "bad" } }, "/annotate/labels" }, { { { "labels", Json::array({ "/A", false }) } }, "/annotate/labels/1" },
					 { { { "labels", Json::array({ "" }) } }, "/annotate/labels/0" }, { { { "axes", "true" } }, "/annotate/axes" }, { { { "colliders", nullptr } }, "/annotate/colliders" } })
			{
				const auto result = ParseViewportAnnotations(VariantValue(value));
				REQUIRE_FALSE(result);
				CHECK(result.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(result.error().GetLocation().JsonPointer == pointer);
			}
			Json many = Json::array();
			for (int i = 0; i < 1001; ++i)
				many.push_back("/A");
			CHECK_FALSE(ParseViewportAnnotations(VariantValue(Json{ { "labels", many } })));
			for (const std::string mode : { "all", "ALL", "selection", "SELECTED", "none" })
			{
				const auto result = ParseViewportAnnotations(VariantValue(Json{ { "labels", mode }, { "bounds", true }, { "axes", true }, { "colliders", true } }));
				REQUIRE(result);
				CHECK(result->Annotations.Bounds);
				CHECK(result->Annotations.Axes);
				CHECK(result->Colliders);
			}
		}
		TEST_CASE("ViewportAnnotations: capture annotations do not persist to the next capture")
		{
			const auto first = ParseViewportAnnotations(VariantValue(Json{ { "labels", "all" }, { "axes", true }, { "bounds", true }, { "colliders", true } }));
			REQUIRE(first);
			const auto clean = ParseViewportAnnotations(VariantValue(Json::object()));
			REQUIRE(clean);
			CHECK(clean->Annotations.Labels == RenderAnnotationLabels::None);
			CHECK_FALSE(clean->Annotations.Bounds);
			CHECK_FALSE(clean->Annotations.Axes);
			CHECK_FALSE(clean->Colliders);
			CHECK(clean->LabelReferences.empty());
		}
		TEST_CASE("ViewportAnnotations: selection aliases and explicit EntityRefs resolve atomically")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			ProjectSettings settings;
			TypeRegistry types;
			RegisterAutomationSharedTypes(types);
			RegisterRaycastMethodTypes(types);
			types.Freeze();
			MethodRegistry methods(types);
			RegisterRaycastMethods(methods);
			methods.Freeze();
			Test::RenderQueryTestContext context(fixture.GetScene(), assets, settings, { .Method = methods.Find("scene.raycast"), .Params = Json::object(), .Registry = &methods });
			context.Runtime = true;
			Entity a = fixture.GetScene().CreateEntityWithID(UUID(0x1234560000000001), "A");
			Entity b = fixture.GetScene().CreateEntityWithID(UUID(0xabcdef0000000002), "B");
			context.Selection = { b.GetUUID() };
			const uint64_t revision = fixture.GetScene().GetRevision();
			const auto options = ParseViewportAnnotations(VariantValue(Json{ { "labels", Json::array({ "/A", "123456", a.GetUUID().ToString() }) } }));
			REQUIRE(options);
			const auto resolved = ResolveViewportAnnotations(context, fixture.GetScene(), *options);
			REQUIRE(resolved);
			CHECK(resolved->LabelEntities == std::vector<UUID>{ a.GetUUID() });
			CHECK(options->Annotations.LabelEntities.empty());
			const auto bad = ParseViewportAnnotations(VariantValue(Json{ { "labels", Json::array({ "/A", "/Missing" }) } }));
			REQUIRE(bad);
			const auto failed = ResolveViewportAnnotations(context, fixture.GetScene(), *bad);
			REQUIRE_FALSE(failed);
			CHECK(failed.error().GetLocation().JsonPointer == "/annotate/labels/1");
			CHECK(fixture.GetScene().GetRevision() == revision);
			CHECK(context.Selection == std::vector<UUID>{ b.GetUUID() });
			const auto empty = ParseViewportAnnotations(VariantValue(Json{ { "labels", Json::array() } }));
			REQUIRE(empty);
			const auto noLabels = ResolveViewportAnnotations(context, fixture.GetScene(), *empty);
			REQUIRE(noLabels);
			CHECK(noLabels->Labels == RenderAnnotationLabels::Explicit);
			CHECK(noLabels->LabelEntities.empty());
			const auto alias = ParseViewportAnnotations(VariantValue(Json{ { "labels", "selected" } }));
			REQUIRE(alias);
			CHECK(alias->Annotations.Labels == RenderAnnotationLabels::Selected);
		}
	}

}
