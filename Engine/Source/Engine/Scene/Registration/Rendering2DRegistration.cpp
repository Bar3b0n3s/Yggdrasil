#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

// The Rendering2D category (M21, Docs/Decisions/0032-m21-contract.md decisions 3, 5, 6, 20 and 24; ADR 0028's phase-2 ledger:
// a milestone's new components go in categories of their own after Scripting): sprites, their tick-driven animation and the
// pixel-perfect camera (C21.1), then the 2D lights and the tilemap renderer (C21.2). Colours are linear and registered as
// Color fields in [0, 1]. SpriteRenderer's NormalMap (C21.2, decision 21) follows Lit; W21.2's integration regenerates
// FeatureTest's canonical sprite scenes with it.

namespace Engine {

	namespace Utils {

		// The pixels per world unit of a sprite or a pixel-perfect camera.
		constexpr double MinPixelsPerUnit = 0.001;
		constexpr double MaxPixelsPerUnit = 65536.0;

		// A colour channel range: linear colours in [0, 1].
		static FieldMeta Sprite2DColorRange()
		{
			FieldMeta meta;
			meta.Min = 0.0;
			meta.Max = 1.0;
			return meta;
		}

		// A 2D light's intensity, a multiplier of its linear colour (HDR above 1).
		constexpr double MaxLight2DIntensity = 1000.0;
		// A 2D light's radius and height, in world units (the 3D collider bound, so a light never spans more than a world).
		constexpr double MinLight2DDistance = 0.001;
		constexpr double MaxLight2DDistance = 100000.0;

	}

	void RegisterRendering2DComponents(TypeRegistry& registry)
	{
		RegisterComponent<SpriteRendererComponent>(registry, "SpriteRenderer", "Draws a sprite: a sprite sheet frame or a texture in the entity's XY plane.")
			.Category("Rendering2D")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Sheet", &SpriteRendererComponent::Sheet, "The sprite sheet whose frame Frame is drawn; null draws Texture whole.")
			.Field("Frame", &SpriteRendererComponent::Frame,
				"The index of the sheet's frame to draw; an index the sheet lacks draws nothing (SPRITE_FRAME_OUT_OF_RANGE).",
				{ .Min = 0.0, .Max = static_cast<double>(MaxSpriteSheetFrames - 1) })
			.Field("Texture", &SpriteRendererComponent::Texture,
				"The texture drawn whole, pivoted at its centre, when Sheet is null; null draws the built-in white square.")
			.ColorField("Color", &SpriteRendererComponent::Color, "The linear colour every texel is multiplied with, with opacity in the fourth component.",
				Utils::Sprite2DColorRange())
			.Field("FlipX", &SpriteRendererComponent::FlipX, "Whether the image is mirrored left to right about its pivot.")
			.Field("FlipY", &SpriteRendererComponent::FlipY, "Whether the image is mirrored top to bottom about its pivot.")
			.Field("PixelsPerUnit", &SpriteRendererComponent::PixelsPerUnit,
				"How many texels of the image make one unit of the entity's local space; the sprite's size is its texels divided by this.",
				{ .Min = Utils::MinPixelsPerUnit, .Max = Utils::MaxPixelsPerUnit, .Unit = "px" })
			.Field("SortingLayer", &SpriteRendererComponent::SortingLayer,
				"The sorting layer name from project settings; later layers draw over earlier ones, and an undeclared name draws in the first.")
			.Field("OrderInLayer", &SpriteRendererComponent::OrderInLayer,
				"The draw order within the sorting layer: higher values draw over lower ones, equal values in canonical entity order.")
			.Field("Lit", &SpriteRendererComponent::Lit, "Whether 2D lights light the sprite; an unlit sprite keeps its texels unchanged.")
			.Field("NormalMap", &SpriteRendererComponent::NormalMap,
				"A lit sprite's tangent-space normal map, laid like its image; point lights then light it by the angle to the light. Null: flat.");

		RegisterComponent<SpriteAnimatorComponent>(registry, "SpriteAnimator",
			"Plays a clip of the SpriteRenderer's sprite sheet, one step per clip frame for its ticks, driven by the fixed step.")
			.Category("Rendering2D")
			.Version(1)
			.Requires<SpriteRendererComponent>()
			.Field("Clip", &SpriteAnimatorComponent::Clip, "The name of the sheet's clip to play; empty plays nothing.")
			.Field("Playing", &SpriteAnimatorComponent::Playing,
				"Whether the clip advances each fixed step; a clip without Loop clears it after its last step and holds its last frame.")
			.Field("Step", &SpriteAnimatorComponent::Step, "The index of the clip step shown.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxSpriteClipFrames - 1) })
			.Field("StepTicks", &SpriteAnimatorComponent::StepTicks, "How many fixed steps the current clip step has been shown.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxSpriteFrameTicks - 1) });

		RegisterComponent<PixelPerfectCameraComponent>(registry, "PixelPerfectCamera",
			"Renders the entity's camera at a reference resolution, upscaled by the largest whole factor that fits the view in a letterbox.")
			.Category("Rendering2D")
			.Version(1)
			.Requires<CameraComponent>()
			.Field("ReferenceWidth", &PixelPerfectCameraComponent::ReferenceWidth, "The width the scene renders at, in pixels.",
				{ .Min = 1.0, .Max = static_cast<double>(MaxPixelPerfectReferenceSide), .Unit = "px" })
			.Field("ReferenceHeight", &PixelPerfectCameraComponent::ReferenceHeight,
				"The height the scene renders at, in pixels; the orthographic view is this many pixels of PixelsPerUnit high.",
				{ .Min = 1.0, .Max = static_cast<double>(MaxPixelPerfectReferenceSide), .Unit = "px" })
			.Field("PixelsPerUnit", &PixelPerfectCameraComponent::PixelsPerUnit,
				"How many scene pixels make one world unit; sprites of the same PixelsPerUnit map each texel to one pixel.",
				{ .Min = Utils::MinPixelsPerUnit, .Max = Utils::MaxPixelsPerUnit, .Unit = "px" })
			.ColorField("LetterboxColor", &PixelPerfectCameraComponent::LetterboxColor, "The linear colour of the bars around the upscaled image.",
				Utils::Sprite2DColorRange());

		registry.Enum<Light2DType>("Light2DType", "What a 2D light lights.")
			.Entry(Light2DType::Point, "Point", "A circle of light around the entity, fading to nothing at its Radius.")
			.Entry(Light2DType::Global, "Global", "The same light at every lit pixel of the view, wherever the entity is.");

		RegisterComponent<Light2DComponent>(registry, "Light2D",
			"A 2D light: lights the lit sprites and tilemaps of the view, never unlit ones; no shadows.")
			.Category("Rendering2D")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Type", &Light2DComponent::Type, "Whether the light is a point light around the entity or a global light of the whole view.")
			.ColorField("Color", &Light2DComponent::Color, "The light's linear colour.", Utils::Sprite2DColorRange())
			.Field("Intensity", &Light2DComponent::Intensity, "How strong the light is: its colour is multiplied by this, so values above 1 overbright.",
				{ .Min = 0.0, .Max = Utils::MaxLight2DIntensity })
			.Field("Radius", &Light2DComponent::Radius, "Point: how far the light reaches from the entity, in world units.",
				{ .Min = Utils::MinLight2DDistance, .Max = Utils::MaxLight2DDistance, .Unit = "m" })
			.Field("Falloff", &Light2DComponent::Falloff,
				"Point: the fraction of the radius, from its edge inward, over which the light fades smoothly to nothing; 0 is a hard edge.",
				{ .Min = 0.0, .Max = 1.0 })
			.Field("Height", &Light2DComponent::Height,
				"Point: how far above the lit plane the light sits, in world units; only normal-mapped sprites read it.",
				{ .Min = Utils::MinLight2DDistance, .Max = Utils::MaxLight2DDistance, .Unit = "m" });

		RegisterComponent<TilemapRendererComponent>(registry, "TilemapRenderer",
			"Draws a tilemap's tiles in the entity's XY plane, each tile as a sprite of its palette entry's sheet frame.")
			.Category("Rendering2D")
			.Version(1)
			.Requires<TransformComponent>()
			.Field("Tilemap", &TilemapRendererComponent::Tilemap, "The tilemap whose tiles are drawn; null draws nothing.")
			.ColorField("Color", &TilemapRendererComponent::Color,
				"The linear colour every tile's texels are multiplied with, with opacity in the fourth component.", Utils::Sprite2DColorRange())
			.Field("PixelsPerUnit", &TilemapRendererComponent::PixelsPerUnit,
				"How many texels of a tile's frame make one unit of the entity's local space, as a sprite's.",
				{ .Min = Utils::MinPixelsPerUnit, .Max = Utils::MaxPixelsPerUnit, .Unit = "px" })
			.Field("SortingLayer", &TilemapRendererComponent::SortingLayer,
				"The sorting layer name from project settings; the tilemap sorts with the sprites, and an undeclared name draws in the first.")
			.Field("OrderInLayer", &TilemapRendererComponent::OrderInLayer,
				"The draw order within the sorting layer, among the sprites: higher values draw over lower ones, equal values in canonical entity order.")
			.Field("Lit", &TilemapRendererComponent::Lit, "Whether 2D lights light the tiles; unlit tiles keep their texels unchanged.");
	}

}
