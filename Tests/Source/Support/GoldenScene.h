#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

// The golden scenes of the M8 goldens (Architecture §15.4; Roadmap M8): Projects/FeatureTest/Assets/Scenes/Golden/*.scene,
// produced by the committed scaffold Projects/FeatureTest/Scaffold/Golden.jsonl (§15.5), rendered headless at 640x360 with
// FXAA on and the fixed blue noise, through the same path as viewport.screenshot's game view (ViewportCapture). Frozen by
// the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 14); stream E implements it with the scenes.

namespace Engine {

	namespace Test {

		class HeadlessGpuFixture;

		struct GoldenSceneOptions
		{
			uint32_t Width = 640;
			uint32_t Height = 360;
			// Applied to the extracted snapshot before it renders: the DebugDraw golden appends its primitives (scripts emit
			// them from M13), the Tonemappers golden selects each tonemapper.
			std::function<void(RenderSnapshot&)> EditSnapshot{};
		};

		// Renders the golden scene `name` ("MaterialGrid" for Golden/MaterialGrid.scene): a copy of Projects/FeatureTest in a
		// temporary directory opened by an EditorAssetManager over the engine resources, with an EnvironmentBaker on the
		// fixture's device (the Studio and Sky environments bake into memory) and the editor's generators; the scene's game
		// view through its primary camera (ExtractRenderSnapshot) captured by a ViewportCapture over SceneRendererPipelines.
		// Every GPU object is released before it returns. Errors: NotFound for an unknown scene; those of the project copy,
		// the scene load, the extraction and the capture.
		[[nodiscard]] Result<Image> RenderGoldenScene(HeadlessGpuFixture& gpu, std::string_view name, const GoldenSceneOptions& options = {});

		// Lays `images` (all of one size and format) out in a grid of `columns` columns, row by row: the Tonemappers golden
		// shows the four tonemappers in one image. Errors: InvalidArgument for no images, mixed sizes or formats, or zero
		// columns.
		[[nodiscard]] Result<Image> ComposeImageGrid(std::span<const Image> images, uint32_t columns);

	}

}
