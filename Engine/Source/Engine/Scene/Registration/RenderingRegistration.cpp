#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

#include <utility>

// The Rendering category (Docs/Decisions/0006-m3-decisions.md, decision 9): mesh drawing, cameras, lights, the scene's
// environment and post-processing, and text. Colours are linear and registered as Color fields in [0, 1] (light and
// ambient strength comes from the Intensity fields); angles are degrees, distances metres. Bounds that are not whole
// numbers are float constants, so a value written with the bound's own decimal spelling reads back in range.

namespace Engine {

	namespace Utils {

		// The smallest positive length a camera, light or post effect accepts, in metres.
		constexpr float MinRenderLength = 0.001f;
		// The perspective vertical field of view, in degrees: a projection needs an angle strictly between 0 and 180.
		constexpr double MinVerticalFov = 1.0;
		constexpr double MaxVerticalFov = 179.0;
		// The largest spot cone angle, measured from the light's axis (a half angle), in degrees: below 90, so the cone
		// stays a cone and its shadow frustum (§8.7) a valid perspective projection.
		constexpr double MaxSpotConeAngle = 89.0;
		// The exposure compensation range of PostProcess, in EV (stops): 2^EV stays far inside the float range.
		constexpr double MaxExposureEV = 16.0;
		// The smallest SSAO radius, in metres.
		constexpr float MinSsaoRadius = 0.01f;
		// The text size range, in pixels per em at the 1080p reference height: the largest is about 93 screen heights per em on
		// the screen and 1 km per em in the world (WorldTextPixelsPerMetre, Renderer/RenderSnapshot.h).
		constexpr double MinTextSize = 1.0;
		constexpr double MaxTextSize = 100000.0;
		// The largest environment intensity: the largest finite binary16 value, which the renderer clamps it to like the baked
		// environment it scales (Renderer/Private/LightingInputs.h).
		constexpr double MaxEnvironmentIntensity = 65504.0;

		// A colour channel range: linear colours in [0, 1].
		static FieldMeta ColorRange()
		{
			FieldMeta meta;
			meta.Min = 0.0;
			meta.Max = 1.0;
			return meta;
		}

		// A projection needs a depth range: the near plane strictly before the far plane.
		static void ValidateCamera(const CameraComponent& camera, ValidationContext& context)
		{
			if (!(camera.NearClip < camera.FarClip))
				context.Error("FarClip", "must be greater than NearClip");
		}

		static void MakeCameraValid(CameraComponent& camera, Random& /*random*/)
		{
			if (camera.FarClip < camera.NearClip)
				std::swap(camera.NearClip, camera.FarClip);
			if (camera.FarClip == camera.NearClip)
				camera.FarClip = camera.NearClip + 1.0f;
		}

		// The light fades from full strength at the inner cone to zero at the outer one, so the inner cone is strictly
		// narrower (equal angles would make the falloff a division by zero).
		static void ValidateSpotLight(const SpotLightComponent& light, ValidationContext& context)
		{
			if (!(light.InnerConeAngle < light.OuterConeAngle))
				context.Error("InnerConeAngle", "must be less than OuterConeAngle");
		}

		static void MakeSpotLightValid(SpotLightComponent& light, Random& /*random*/)
		{
			if (light.OuterConeAngle < light.InnerConeAngle)
				std::swap(light.InnerConeAngle, light.OuterConeAngle);
			if (light.InnerConeAngle == light.OuterConeAngle)
			{
				if (light.OuterConeAngle > 0.0f)
					light.InnerConeAngle = light.OuterConeAngle * 0.5f;
				else
					light.OuterConeAngle = static_cast<float>(MaxSpotConeAngle);
			}
		}

	}

	void RegisterRenderingComponents(TypeRegistry& registry)
	{
		registry.Enum<ProjectionType>("ProjectionType", "How a camera projects the scene onto the screen.")
			.Entry(ProjectionType::Perspective, "Perspective", "Perspective projection with the vertical field of view VerticalFov.")
			.Entry(ProjectionType::Orthographic, "Orthographic", "Parallel projection whose half height is OrthographicSize.");

		registry.Enum<ClearMode>("ClearMode", "What a camera draws behind the scene's geometry.")
			.Entry(ClearMode::Skybox, "Skybox", "The scene's Environment skybox (its fallback colour when none is shown).")
			.Entry(ClearMode::Color, "Color", "The camera's ClearColor.");

		registry.Enum<Tonemapper>("Tonemapper", "The curve that maps HDR scene colours to the display.")
			.Entry(Tonemapper::AgX, "AgX", "AgX: filmic, hue-preserving highlight roll-off (the default).")
			.Entry(Tonemapper::ACES, "ACES", "The ACES filmic approximation: higher contrast and saturation.")
			.Entry(Tonemapper::PbrNeutral, "PbrNeutral", "Khronos PBR Neutral: keeps base colours faithful up to the highlights.")
			.Entry(Tonemapper::Linear, "Linear", "No curve: exposure only, then clamped to the display range.");

		registry.Enum<SsaoQuality>("SsaoQuality", "The sample count of screen-space ambient occlusion (GTAO).")
			.Entry(SsaoQuality::Low, "Low", "Fewest samples: fastest, noisiest.")
			.Entry(SsaoQuality::Medium, "Medium", "Balanced sample count (the default).")
			.Entry(SsaoQuality::High, "High", "Most samples: smoothest, slowest.");

		registry.Enum<TextSpace>("TextSpace", "Where a text element is placed.")
			.Entry(TextSpace::Screen, "Screen", "On the screen, positioned by Anchor, Pivot and Offset.")
			.Entry(TextSpace::World, "World", "In the world, at the entity's transform.");

		registry.Enum<TextAlignment>("TextAlignment", "How the lines of a text element are aligned to each other.")
			.Entry(TextAlignment::Left, "Left", "Lines start at the left edge of the text block.")
			.Entry(TextAlignment::Center, "Center", "Lines are centred in the text block.")
			.Entry(TextAlignment::Right, "Right", "Lines end at the right edge of the text block.");

		RegisterComponent<MeshRendererComponent>(registry, "MeshRenderer", "Draws a mesh asset with one material per submesh.")
			.Category("Rendering")
			.Version(1)
			.Field("Mesh", &MeshRendererComponent::Mesh, "The mesh to draw; null draws nothing.")
			.Field("Materials", &MeshRendererComponent::Materials,
				"One material per submesh; an empty list or a null slot uses the mesh's default material for that submesh.")
			.Field("CastShadows", &MeshRendererComponent::CastShadows, "Whether the mesh casts shadows.")
			.Field("ReceiveShadows", &MeshRendererComponent::ReceiveShadows, "Whether shadows fall on the mesh.")
			.Field("Visible", &MeshRendererComponent::Visible, "Whether the mesh is drawn at all.");

		RegisterComponent<CameraComponent>(registry, "Camera", "Renders the scene from the entity, looking down its local -Z axis.")
			.Category("Rendering")
			.Version(1)
			.Field("Projection", &CameraComponent::Projection, "Perspective or orthographic projection.")
			.Field("VerticalFov", &CameraComponent::VerticalFov, "The vertical field of view of the perspective projection, in degrees.",
				{ .Min = Utils::MinVerticalFov, .Max = Utils::MaxVerticalFov, .Unit = "deg" })
			.Field("OrthographicSize", &CameraComponent::OrthographicSize, "Half the visible height of the orthographic projection, in metres.",
				{ .Min = Utils::MinRenderLength, .Unit = "m" })
			.Field("NearClip", &CameraComponent::NearClip, "The near clipping distance, in metres; below FarClip.",
				{ .Min = Utils::MinRenderLength, .Unit = "m" })
			.Field("FarClip", &CameraComponent::FarClip, "The far clipping distance, in metres; beyond it nothing is drawn or culled in.",
				{ .Min = Utils::MinRenderLength, .Unit = "m" })
			.Field("Primary", &CameraComponent::Primary, "Whether this is the scene's primary camera (exactly one active camera should be).")
			.Field("Clear", &CameraComponent::Clear, "What the camera draws behind the scene.")
			.ColorField("ClearColor", &CameraComponent::ClearColor, "The linear background colour when Clear is Color.", Utils::ColorRange())
			.Validate(&Utils::ValidateCamera)
			.Generate(&Utils::MakeCameraValid);

		RegisterComponent<DirectionalLightComponent>(registry, "DirectionalLight", "A sun-like light shining down the entity's local -Z axis.")
			.Category("Rendering")
			.Version(1)
			.ColorField("Color", &DirectionalLightComponent::Color, "The linear light colour.", Utils::ColorRange())
			.Field("Intensity", &DirectionalLightComponent::Intensity, "The unitless brightness multiplied with Color.", { .Min = 0.0 })
			.Field("CastShadows", &DirectionalLightComponent::CastShadows, "Whether the light casts cascaded soft shadows.")
			.Field("ShadowDistance", &DirectionalLightComponent::ShadowDistance, "How far from the camera shadows reach, in metres.",
				{ .Min = Utils::MinRenderLength, .Unit = "m" })
			.Field("CascadeCount", &DirectionalLightComponent::CascadeCount, "The number of shadow cascades, from 1 to 4.",
				{ .Min = 1.0, .Max = 4.0 })
			.Field("CascadeSplitLambda", &DirectionalLightComponent::CascadeSplitLambda,
				"The cascade split blend from uniform (0) to logarithmic (1).", { .Min = 0.0, .Max = 1.0 })
			.Field("LightAngle", &DirectionalLightComponent::LightAngle,
				"The angular diameter of the light source, in degrees; larger values soften shadow penumbrae.",
				{ .Min = 0.0, .Max = 90.0, .Unit = "deg" })
			.Field("DepthBias", &DirectionalLightComponent::DepthBias, "The shadow depth bias, in shadow-map texels.", { .Min = 0.0 })
			.Field("NormalBias", &DirectionalLightComponent::NormalBias, "The shadow normal-offset bias, in shadow-map texels.", { .Min = 0.0 });

		RegisterComponent<PointLightComponent>(registry, "PointLight", "A light shining in every direction from the entity (no shadows).")
			.Category("Rendering")
			.Version(1)
			.ColorField("Color", &PointLightComponent::Color, "The linear light colour.", Utils::ColorRange())
			.Field("Intensity", &PointLightComponent::Intensity, "The unitless brightness multiplied with Color.", { .Min = 0.0 })
			.Field("Range", &PointLightComponent::Range, "The distance at which the light's falloff reaches zero, in metres.",
				{ .Min = Utils::MinRenderLength, .Unit = "m" })
			.Field("SourceRadius", &PointLightComponent::SourceRadius, "The radius of the emitting sphere, in metres; widens highlights.",
				{ .Min = 0.0, .Unit = "m" });

		RegisterComponent<SpotLightComponent>(registry, "SpotLight", "A cone of light shining down the entity's local -Z axis.")
			.Category("Rendering")
			.Version(1)
			.ColorField("Color", &SpotLightComponent::Color, "The linear light colour.", Utils::ColorRange())
			.Field("Intensity", &SpotLightComponent::Intensity, "The unitless brightness multiplied with Color.", { .Min = 0.0 })
			.Field("Range", &SpotLightComponent::Range, "The distance at which the light's falloff reaches zero, in metres.",
				{ .Min = Utils::MinRenderLength, .Unit = "m" })
			.Field("InnerConeAngle", &SpotLightComponent::InnerConeAngle,
				"The angle from the axis inside which the light is at full strength, in degrees; below OuterConeAngle.",
				{ .Min = 0.0, .Max = Utils::MaxSpotConeAngle, .Unit = "deg" })
			.Field("OuterConeAngle", &SpotLightComponent::OuterConeAngle, "The angle from the axis at which the light fades to zero, in degrees.",
				{ .Min = 0.0, .Max = Utils::MaxSpotConeAngle, .Unit = "deg" })
			.Field("CastShadows", &SpotLightComponent::CastShadows, "Whether the light casts soft shadows (shadow atlas budget applies).")
			.Field("SourceRadius", &SpotLightComponent::SourceRadius,
				"The radius of the emitting disc, in metres; widens highlights and shadow penumbrae.", { .Min = 0.0, .Unit = "m" })
			.Validate(&Utils::ValidateSpotLight)
			.Generate(&Utils::MakeSpotLightValid);

		RegisterComponent<EnvironmentComponent>(registry, "Environment", "The scene's image-based lighting and skybox (one per scene).")
			.Category("Rendering")
			.Version(1)
			.Flags(ComponentFlags::UniquePerScene)
			.Field("Environment", &EnvironmentComponent::Environment, "The environment map; null lights the scene with FallbackColor.")
			.Field("Intensity", &EnvironmentComponent::Intensity, "The unitless strength of the image-based lighting.",
				{ .Min = 0.0, .Max = Utils::MaxEnvironmentIntensity })
			.Field("Rotation", &EnvironmentComponent::Rotation, "The rotation of the environment about the +Y axis, in degrees.",
				{ .Unit = "deg" })
			.Field("ShowSkybox", &EnvironmentComponent::ShowSkybox, "Whether cameras that clear to the skybox show the environment map.")
			.Field("SkyboxBlur", &EnvironmentComponent::SkyboxBlur, "How blurred the visible skybox is, from sharp (0) to fully blurred (1).",
				{ .Min = 0.0, .Max = 1.0 })
			.ColorField("FallbackColor", &EnvironmentComponent::FallbackColor,
				"The linear ambient colour used when no environment map is assigned.", Utils::ColorRange());

		RegisterComponent<PostProcessComponent>(registry, "PostProcess",
			"The scene's exposure, tone mapping, ambient occlusion, bloom and anti-aliasing (one per scene).")
			.Category("Rendering")
			.Version(1)
			.Flags(ComponentFlags::UniquePerScene)
			.Field("ExposureEV", &PostProcessComponent::ExposureEV, "The exposure compensation in stops: the image is multiplied by 2^EV.",
				{ .Min = -Utils::MaxExposureEV, .Max = Utils::MaxExposureEV, .Unit = "EV" })
			.Field("Tonemap", &PostProcessComponent::Tonemap, "The tone mapping curve.")
			.Field("SsaoEnabled", &PostProcessComponent::SsaoEnabled, "Whether screen-space ambient occlusion (GTAO) is applied.")
			.Field("SsaoRadius", &PostProcessComponent::SsaoRadius, "The world-space radius within which ambient occlusion is sampled, in metres.",
				{ .Min = Utils::MinSsaoRadius, .Unit = "m" })
			.Field("SsaoIntensity", &PostProcessComponent::SsaoIntensity, "The strength of the ambient occlusion.", { .Min = 0.0 })
			.Field("SsaoQuality", &PostProcessComponent::SsaoQuality, "The ambient occlusion sample count.")
			.Field("BloomEnabled", &PostProcessComponent::BloomEnabled, "Whether bright areas bloom.")
			.Field("BloomIntensity", &PostProcessComponent::BloomIntensity, "How much of the bloom is blended into the image.",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("FxaaEnabled", &PostProcessComponent::FxaaEnabled, "Whether FXAA anti-aliasing is applied.");

		RegisterComponent<TextComponent>(registry, "Text", "Draws a text string on the screen or in the world.")
			.Category("Rendering")
			.Version(1)
			.Field("Text", &TextComponent::Text, "The UTF-8 text; a line feed starts a new line.")
			.Field("Font", &TextComponent::Font, "The font; null uses the default font.")
			.Field("Size", &TextComponent::Size, "The text height in pixels at the 1080p reference resolution.",
				{ .Min = Utils::MinTextSize, .Max = Utils::MaxTextSize, .Unit = "px" })
			.ColorField("Color", &TextComponent::Color, "The linear text colour with opacity in the fourth component.", Utils::ColorRange())
			.Field("Space", &TextComponent::Space, "Whether the text is placed on the screen or in the world.")
			.Field("Anchor", &TextComponent::Anchor, "The screen point the text is placed at, normalized to the viewport (Screen space).",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("Pivot", &TextComponent::Pivot, "The point of the text block that sits on the anchor, normalized to the block.",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("Offset", &TextComponent::Offset, "A further offset from the anchor, in pixels at the 1080p reference resolution.",
				{ .Unit = "px" })
			.Field("Alignment", &TextComponent::Alignment, "How the lines are aligned to each other.")
			.Field("Billboard", &TextComponent::Billboard, "Whether world-space text always faces the camera.");
	}

}
