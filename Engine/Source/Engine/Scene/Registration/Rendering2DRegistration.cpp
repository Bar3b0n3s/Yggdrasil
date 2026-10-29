#include "EnginePCH.h"
#include "Engine/Scene/Components/BuiltinComponents.h"

#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentRegistration.h"

// The Rendering2D category (M21, Docs/Decisions/0032-m21-contract.md decisions 3, 5 and 6; ADR 0028's phase-2 ledger: a
// milestone's new components go in categories of their own after Scripting): sprites, their tick-driven animation and the
// pixel-perfect camera. Colours are linear and registered as Color fields in [0, 1]. C21.2 appends Light2D and the
// tilemap renderer to this category.

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
			.Field("Lit", &SpriteRendererComponent::Lit, "Whether 2D lights light the sprite; an unlit sprite keeps its texels unchanged.");

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
	}

}
