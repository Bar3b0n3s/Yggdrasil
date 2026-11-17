#include "TestsPCH.h"

#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	static const ComponentInfo& FindRenderingComponent(const TypeRegistry& registry, std::string_view name)
	{
		const ComponentInfo* info = registry.FindComponent(name);
		REQUIRE(info != nullptr);
		return *info;
	}

	// The JSON pointers of the errors a validation of `object` (of `type`) reports.
	static std::vector<std::string> ValidateRendering(const TypeRegistry& registry, const StructInfo& type, const void* object)
	{
		ResolveContext resolve;
		resolve.Registry = &registry;
		ValidationContext context;
		type.Validate(object, resolve, context);
		std::vector<std::string> pointers;
		for (const ValidationIssue& issue : context.GetIssues())
		{
			if (issue.Severity == DiagnosticSeverity::Error)
				pointers.push_back(issue.JsonPointer);
		}
		return pointers;
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderingRegistration: Environment and PostProcess are unique per scene")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			CHECK(registry->FindComponent("Environment")->HasFlag(ComponentFlags::UniquePerScene));
			CHECK(registry->FindComponent("PostProcess")->HasFlag(ComponentFlags::UniquePerScene));
			CHECK_FALSE(registry->FindComponent("Camera")->HasFlag(ComponentFlags::UniquePerScene));
			for (const std::string_view name : { "MeshRenderer", "Camera", "DirectionalLight", "PointLight", "SpotLight", "Text" })
			{
				INFO(std::string(name));
				const ComponentInfo& component = FindRenderingComponent(*registry, name);
				CHECK(component.GetCategory() == "Rendering");
				CHECK(component.GetFlags() == ComponentFlags::Default);
			}
		}

		TEST_CASE("RenderingRegistration: colours are Color fields and ranges follow the table")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			CHECK(registry->FindComponent("Camera")->FindField("ClearColor")->GetKind() == FieldType::Color3);
			CHECK(registry->FindComponent("Text")->FindField("Color")->GetKind() == FieldType::Color4);
			CHECK(registry->FindComponent("Environment")->FindField("FallbackColor")->GetKind() == FieldType::Color3);
			for (const std::string_view light : { "DirectionalLight", "PointLight", "SpotLight" })
			{
				INFO(std::string(light));
				const FieldInfo* color = FindRenderingComponent(*registry, light).FindField("Color");
				REQUIRE(color != nullptr);
				CHECK(color->GetKind() == FieldType::Color3);
				CHECK(color->GetMeta().Min == 0.0);
				CHECK(color->GetMeta().Max == 1.0);
			}

			const FieldInfo* cascades = registry->FindComponent("DirectionalLight")->FindField("CascadeCount");
			REQUIRE(cascades != nullptr);
			CHECK(cascades->GetMeta().Min == 1.0);
			CHECK(cascades->GetMeta().Max == 4.0);

			const FieldInfo* blur = registry->FindComponent("Environment")->FindField("SkyboxBlur");
			REQUIRE(blur != nullptr);
			CHECK(blur->GetMeta().Min == 0.0);
			CHECK(blur->GetMeta().Max == 1.0);
			// The environment's intensity is bounded by the largest finite binary16 value, as the renderer clamps it
			// (Docs/Decisions/0013-m8-decisions.md decision 27).
			const FieldInfo* intensity = registry->FindComponent("Environment")->FindField("Intensity");
			REQUIRE(intensity != nullptr);
			CHECK(intensity->GetMeta().Min == 0.0);
			CHECK(intensity->GetMeta().Max == 65504.0);

			CHECK(registry->FindComponent("MeshRenderer")->FindField("Mesh")->GetMeta().AssetFilter == "Mesh");
			CHECK(registry->FindComponent("MeshRenderer")->FindField("Materials")->GetType().GetElement()->GetAssetTypeName() == "Material");
			CHECK(registry->FindComponent("Environment")->FindField("Environment")->GetMeta().AssetFilter == "Environment");
			CHECK(registry->FindComponent("Text")->FindField("Font")->GetMeta().AssetFilter == "Font");
		}

		TEST_CASE("RenderingRegistration: angles, distances and sizes carry units and bounds")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& camera = FindRenderingComponent(*registry, "Camera");
			const FieldMeta& fov = camera.FindField("VerticalFov")->GetMeta();
			CHECK(fov.Unit == "deg");
			CHECK(fov.Min == 1.0);
			CHECK(fov.Max == 179.0);
			for (const std::string_view length : { "OrthographicSize", "NearClip", "FarClip" })
			{
				INFO(std::string(length));
				const FieldMeta& meta = camera.FindField(length)->GetMeta();
				CHECK(meta.Unit == "m");
				REQUIRE(meta.Min.has_value());
				CHECK(*meta.Min > 0.0);
			}

			const ComponentInfo& spot = FindRenderingComponent(*registry, "SpotLight");
			for (const std::string_view cone : { "InnerConeAngle", "OuterConeAngle" })
			{
				INFO(std::string(cone));
				const FieldMeta& meta = spot.FindField(cone)->GetMeta();
				CHECK(meta.Unit == "deg");
				CHECK(meta.Min == 0.0);
				CHECK(meta.Max == 89.0);
			}
			CHECK(FindRenderingComponent(*registry, "DirectionalLight").FindField("LightAngle")->GetMeta().Unit == "deg");
			CHECK(FindRenderingComponent(*registry, "Environment").FindField("Rotation")->GetMeta().Unit == "deg");

			const ComponentInfo& post = FindRenderingComponent(*registry, "PostProcess");
			CHECK(post.FindField("ExposureEV")->GetMeta().Min == -16.0);
			CHECK(post.FindField("ExposureEV")->GetMeta().Max == 16.0);
			CHECK(post.FindField("BloomIntensity")->GetMeta().Max == 1.0);

			const ComponentInfo& text = FindRenderingComponent(*registry, "Text");
			CHECK(text.FindField("Size")->GetMeta().Unit == "px");
			CHECK(text.FindField("Size")->GetMeta().Min == 1.0);
			CHECK(text.FindField("Size")->GetMeta().Max == 100000.0);
			CHECK(text.FindField("Anchor")->GetMeta().Max == 1.0);
			CHECK(text.FindField("Offset")->GetMeta().Unit == "px");
			CHECK_FALSE(text.FindField("Offset")->GetMeta().Min.has_value());
		}

		TEST_CASE("RenderingRegistration: a spot light's inner cone is narrower than its outer cone")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& spotType = FindRenderingComponent(*registry, "SpotLight");

			SpotLightComponent light;
			CHECK(ValidateRendering(*registry, spotType, &light).empty());
			light.InnerConeAngle = 30.0f;
			CHECK(ValidateRendering(*registry, spotType, &light) == std::vector<std::string>{ "/InnerConeAngle" });
			light.InnerConeAngle = 45.0f;
			CHECK(ValidateRendering(*registry, spotType, &light) == std::vector<std::string>{ "/InnerConeAngle" });

			Random random(3);
			spotType.Generate(&light, random);
			CHECK(light.InnerConeAngle == 30.0f);
			CHECK(light.OuterConeAngle == 45.0f);

			light.InnerConeAngle = 40.0f;
			light.OuterConeAngle = 40.0f;
			spotType.Generate(&light, random);
			CHECK(light.InnerConeAngle == 20.0f);
			CHECK(light.OuterConeAngle == 40.0f);

			light.InnerConeAngle = 0.0f;
			light.OuterConeAngle = 0.0f;
			spotType.Generate(&light, random);
			CHECK(light.InnerConeAngle == 0.0f);
			CHECK(light.OuterConeAngle == 89.0f);
			CHECK(ValidateRendering(*registry, spotType, &light).empty());
		}

		TEST_CASE("RenderingRegistration: a camera's near clip is closer than its far clip")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& cameraType = FindRenderingComponent(*registry, "Camera");

			CameraComponent camera;
			CHECK(ValidateRendering(*registry, cameraType, &camera).empty());
			camera.NearClip = 2000.0f;
			CHECK(ValidateRendering(*registry, cameraType, &camera) == std::vector<std::string>{ "/FarClip" });

			Random random(5);
			cameraType.Generate(&camera, random);
			CHECK(camera.NearClip == 1000.0f);
			CHECK(camera.FarClip == 2000.0f);

			camera.FarClip = camera.NearClip;
			CHECK(ValidateRendering(*registry, cameraType, &camera) == std::vector<std::string>{ "/FarClip" });
			cameraType.Generate(&camera, random);
			CHECK(camera.FarClip == 1001.0f);
			CHECK(ValidateRendering(*registry, cameraType, &camera).empty());
		}

		TEST_CASE("RenderingRegistration: every rendering enum value is registered by name")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const std::vector<std::pair<std::string_view, std::vector<std::string>>> enums = {
				{ "ProjectionType", { "Perspective", "Orthographic" } },
				{ "ClearMode", { "Skybox", "Color" } },
				{ "Tonemapper", { "AgX", "ACES", "PbrNeutral", "Linear" } },
				{ "SsaoQuality", { "Low", "Medium", "High" } },
				{ "ExposureMode", { "Manual", "Auto" } },
				{ "AntiAliasing", { "None", "Fxaa", "Taa" } },
				{ "TextSpace", { "Screen", "World" } },
				{ "TextAlignment", { "Left", "Center", "Right", "Start", "End" } }, // M38 appends Start and End (ADR 0055 decision 11)
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
			CHECK(registry->FindComponent("PostProcess")->FindField("SsaoQuality")->GetType().GetEnum() == registry->FindEnum("SsaoQuality"));
			CHECK(registry->FindComponent("PostProcess")->FindField("ExposureMode")->GetType().GetEnum() == registry->FindEnum("ExposureMode"));
			CHECK(registry->FindComponent("PostProcess")->FindField("AntiAliasing")->GetType().GetEnum() == registry->FindEnum("AntiAliasing"));
		}

		// M26 (C26.1, R-AEX, R-LUT, R-TAA, R-SSR; Docs/Decisions/0037-m26-contract.md decisions 12 and 27, stream X-A):
		// PostProcess version 2 lists its fields in the format's order with their ranges and units, AntiAliasing in version
		// 1's FxaaEnabled's place, and its validator refuses an inverted automatic exposure range, which Generate swaps.
		TEST_CASE("RenderingRegistration: PostProcess version 2 lists the format's fields and refuses an inverted exposure range")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& post = FindRenderingComponent(*registry, "PostProcess");
			CHECK(post.GetVersion() == 2);
			std::vector<std::string> fields;
			for (const Scope<FieldInfo>& field : post.GetFields())
				fields.push_back(field->GetName());
			const std::vector<std::string> expected = { "ExposureEV", "ExposureMode", "AutoExposureMinEV", "AutoExposureMaxEV",
				"AutoExposureSpeedUp", "AutoExposureSpeedDown", "Tonemap", "Lut", "LutIntensity", "SsaoEnabled", "SsaoRadius",
				"SsaoIntensity", "SsaoQuality", "SsrEnabled", "SsrIntensity", "SsrMaxDistance", "SsrThickness", "SsrMaxRoughness",
				"BloomEnabled", "BloomIntensity", "AntiAliasing" };
			CHECK(fields == expected);
			for (const std::string_view name : { "AutoExposureMinEV", "AutoExposureMaxEV" })
			{
				INFO(std::string(name));
				CHECK(post.FindField(name)->GetMeta().Min == -16.0);
				CHECK(post.FindField(name)->GetMeta().Max == 16.0);
				CHECK(post.FindField(name)->GetMeta().Unit == "EV");
			}
			for (const std::string_view name : { "AutoExposureSpeedUp", "AutoExposureSpeedDown" })
			{
				INFO(std::string(name));
				CHECK(post.FindField(name)->GetMeta().Min == doctest::Approx(0.01));
				CHECK(post.FindField(name)->GetMeta().Max == 100.0);
				CHECK(post.FindField(name)->GetMeta().Unit == "EV/s");
			}
			CHECK(post.FindField("SsrMaxDistance")->GetMeta().Min == doctest::Approx(0.1));
			CHECK(post.FindField("SsrMaxDistance")->GetMeta().Max == 1000.0);
			CHECK(post.FindField("SsrMaxDistance")->GetMeta().Unit == "m");
			CHECK(post.FindField("SsrThickness")->GetMeta().Min == doctest::Approx(0.001));
			CHECK(post.FindField("SsrThickness")->GetMeta().Max == 10.0);
			for (const std::string_view name : { "LutIntensity", "SsrIntensity", "SsrMaxRoughness" })
			{
				INFO(std::string(name));
				CHECK(post.FindField(name)->GetMeta().Min == 0.0);
				CHECK(post.FindField(name)->GetMeta().Max == 1.0);
			}

			PostProcessComponent component;
			CHECK(ValidateRendering(*registry, post, &component).empty());
			component.AutoExposureMinEV = 4.0f;
			component.AutoExposureMaxEV = 4.0f;
			CHECK(ValidateRendering(*registry, post, &component).empty());
			component.AutoExposureMaxEV = 2.0f;
			CHECK(ValidateRendering(*registry, post, &component) == std::vector<std::string>{ "/AutoExposureMaxEV" });
			Random random(7);
			post.Generate(&component, random);
			CHECK(component.AutoExposureMinEV == 2.0f);
			CHECK(component.AutoExposureMaxEV == 4.0f);
			CHECK(ValidateRendering(*registry, post, &component).empty());
		}
	}

}
