#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The golden scenes (Architecture §15.4): Projects/FeatureTest/Assets/Scenes/Golden/*.scene, kept from the committed
// scaffold Projects/FeatureTest/Scaffold/Golden.jsonl (§15.5), and the scenes of the golden scaffolds that
// Scaffold/Scaffolds.json lists with their GoldenDirectories (Scripts/Scaffold.py; M18 W18.2), rendered headless at 640x360
// with FXAA on and the fixed blue noise, through the same path as viewport.screenshot's game view (ViewportCapture). Frozen
// by the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 14); the golden scaffolds' scenes joined the list in M18.

namespace Engine {

	namespace Test {

		class HeadlessGpuFixture;

		// One golden scene of a project.
		struct GoldenSceneEntry
		{
			std::string Name{};     // the file stem, unique among the project's golden scenes ("ShadowsNear")
			std::string Path{};     // project-relative, '/' separators ("Assets/Scenes/GoldenRendererII/ShadowsNear.scene")
			std::string Scaffold{}; // the scaffold that writes it ("Golden.jsonl" for the kept scenes)
		};

		// The golden tests' scene list of the project at `projectRoot` (Projects/FeatureTest for the golden tests), sorted
		// by name: every .scene directly in Assets/Scenes/Golden (Golden.jsonl's kept scenes) and every .scene below the
		// GoldenDirectories of each golden scaffold its Scaffold/Scaffolds.json lists (a directory that does not exist holds
		// none). Errors: those of reading the manifest (NotFound, Io), Parse or Validation (located) for a malformed one, and
		// Validation for a golden directory that is not "Assets/" followed by plain names (letters, digits, '_', '-', '.', not
		// starting with '.'); Validation naming both paths for two golden scenes with one name; the directory listings'.
		[[nodiscard]] Result<std::vector<GoldenSceneEntry>> ListGoldenScenes(const std::filesystem::path& projectRoot);

		struct GoldenSceneOptions
		{
			uint32_t Width = 640;
			uint32_t Height = 360;
			// Applied to the extracted snapshot before it renders: the DebugDraw golden appends its primitives (scripts emit
			// them from M13), the Tonemappers golden selects each tonemapper.
			std::function<void(RenderSnapshot&)> EditSnapshot{};
			// Synchronously observes the final snapshot after EditSnapshot and real scene annotation assembly, before
			// capture. The const snapshot is borrowed only for this call.
			std::function<void(const RenderSnapshot&)> InspectSnapshot{};
		};

		// Renders the golden scene `name` of Projects/FeatureTest's scene list (ListGoldenScenes; "MaterialGrid" for
		// Golden/MaterialGrid.scene): a copy of Projects/FeatureTest in a temporary directory opened by an
		// EditorAssetManager over the engine resources, with an EnvironmentBaker on the fixture's device (the Studio and Sky
		// environments bake into memory) and the editor's generators; the scene's game view through its primary camera
		// (ExtractRenderSnapshot) captured by a ViewportCapture over SceneRendererPipelines. Every GPU object is released
		// before it returns. Errors: NotFound for a name that is not in the list; those of listing the golden scenes, the
		// project copy, the scene load, the extraction, annotations and the capture. Annotations use the copied project's
		// physics layers. Any scan/load error, Error-level asset diagnostic or scene mutation during capture fails the
		// render.
		[[nodiscard]] Result<Image> RenderGoldenScene(HeadlessGpuFixture& gpu, std::string_view name, const GoldenSceneOptions& options = {});

		// Lays `images` (all of one size and format) out in a grid of `columns` columns, row by row: the Tonemappers golden
		// shows the four tonemappers in one image. Errors: InvalidArgument for no images, mixed sizes or formats, or zero
		// columns.
		[[nodiscard]] Result<Image> ComposeImageGrid(std::span<const Image> images, uint32_t columns);

	}

}
