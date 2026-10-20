#include "TestsPCH.h"

#include "EditorCore/EngineAssetGenerators.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Support/AssetTestFixture.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// The output stage (M18, C18.1, R-2D-PIXEL; Docs/Decisions/0029-m18-contract.md decisions 5 to 8): the view-to-scene
// mapping and the CPU ray through it (CPU), and at scale 3 the integer upscale with its letterbox, picking and capture
// through the mapping, the stage order around the upscale, the editor overlays clipped to the scene image and the 2D
// colour path, and a cropped scene (GPU).

namespace Engine {

	namespace {

		// The view and scene sizes of the scale-3 cases: 64 x 36 scene pixels in a 200 x 120 view, Scale 3, the scene image
		// at view pixels [4, 196) x [6, 114).
		constexpr uint32_t ViewWidth = 200;
		constexpr uint32_t ViewHeight = 120;
		constexpr uint32_t SceneWidth = 64;
		constexpr uint32_t SceneHeight = 36;
		constexpr uint32_t Scale = 3;
		constexpr int32_t OffsetX = 4;
		constexpr int32_t OffsetY = 6;

		// The RGBA8 pixel at (x, y).
		glm::u8vec4 GetPixel(const Image& image, uint32_t x, uint32_t y)
		{
			const std::span<const std::byte> texel = image.GetRow(y).subspan(static_cast<size_t>(x) * 4, 4);
			return { std::to_integer<uint8_t>(texel[0]), std::to_integer<uint8_t>(texel[1]), std::to_integer<uint8_t>(texel[2]),
				std::to_integer<uint8_t>(texel[3]) };
		}

		bool IsInSceneImage(uint32_t x, uint32_t y)
		{
			return static_cast<int32_t>(x) >= OffsetX && static_cast<int32_t>(x) < OffsetX + static_cast<int32_t>(Scale * SceneWidth)
				&& static_cast<int32_t>(y) >= OffsetY && static_cast<int32_t>(y) < OffsetY + static_cast<int32_t>(Scale * SceneHeight);
		}

		// A renderer of the view size over the engine resources (the Default font, the built-in meshes) and the generated
		// built-ins (the blue noise of the Lit view's dither).
		class OutputStageSetup
		{
		public:
			explicit OutputStageSetup(Test::HeadlessGpuFixture& gpu)
				: m_Device(&gpu.GetDevice()), m_Assets(Test::AssetTestFixtureOptions{ .EngineResources = true, .EngineAssetGenerators = GetEngineAssetGenerators() })
			{
				m_Assets.OpenProject(false);
				m_Cache = CreateScope<GpuResourceCache>(gpu.GetDevice(), m_Assets.GetManager());
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				m_Pipelines = std::move(*pipelines);
				Result<Scope<SceneRenderer>> renderer =
					SceneRenderer::Create(gpu.GetDevice(), *m_Pipelines, *m_Cache, m_Assets.GetManager(), { ViewWidth, ViewHeight });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				m_Renderer = std::move(*renderer);
			}

			~OutputStageSetup()
			{
				m_Device->WaitForIdle();
				m_Renderer.reset();
				m_Pipelines.reset();
				m_Cache.reset();
				m_Device->RunGarbageCollection();
			}

			OutputStageSetup(const OutputStageSetup&) = delete;
			OutputStageSetup& operator=(const OutputStageSetup&) = delete;

			// An orthographic camera at the origin looking down -Z, 1 m half height, at the scene resolution of `output` (the
			// renderer's size when it has none), with a unit quad facing it at z = -1 and a light; linear post-processing.
			[[nodiscard]] RenderSnapshot Snapshot(const RenderOutputSettings& output) const
			{
				const uint32_t width = output.SceneWidth != 0 ? output.SceneWidth : m_Renderer->GetWidth();
				const uint32_t height = output.SceneHeight != 0 ? output.SceneHeight : m_Renderer->GetHeight();
				RenderSnapshot snapshot;
				snapshot.HasCamera = true;
				snapshot.Camera.ProjectionKind = RenderProjection::Orthographic;
				snapshot.Camera.OrthographicSize = 1.0f;
				snapshot.Camera.NearClip = 0.1f;
				snapshot.Camera.FarClip = 10.0f;
				snapshot.Camera.ViewportWidth = width;
				snapshot.Camera.ViewportHeight = height;
				snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Orthographic, 60.0f, 1.0f, 0.1f, 10.0f, width, height);
				snapshot.Camera.ClearColor = glm::vec3(0.0f, 0.0f, 0.2f);
				snapshot.Camera.ClearToSkybox = false;
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
					.Mesh = BuiltinAssetHandles::QuadMesh,
					.Entity = UUID(900),
					.PickId = 1 });
				snapshot.PickTable = { UUID(900) };
				snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional, .Intensity = 1.0f, .Direction = glm::vec3(0.0f, 0.0f, -1.0f) });
				snapshot.Post = PostProcessSettings{ .ExposureEV = 0.0f, .Tonemap = RenderTonemapper::Linear, .SsaoEnabled = false, .BloomEnabled = false, .FxaaEnabled = false };
				snapshot.Quality.ShadowMapSize = 256;
				snapshot.Output = output;
				snapshot.FrameIndex = ++m_FrameIndex;
				snapshot.SceneRevision = 7;
				return snapshot;
			}

			// Records `snapshot`, executes it and notifies the renderer.
			void Submit(const RenderSnapshot& snapshot)
			{
				Result<nvrhi::CommandListHandle> list = m_Device->CreateCommandList();
				REQUIRE_MESSAGE(list.has_value(), list.error().ToString());
				(*list)->open();
				const Status rendered = m_Renderer->Render(**list, snapshot);
				(*list)->close();
				m_Renderer->OnSubmitted(snapshot.FrameIndex, m_Device->ExecuteCommandList(**list));
				REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
			}

			[[nodiscard]] Image Render(const RenderSnapshot& snapshot)
			{
				Submit(snapshot);
				Readback readback(*m_Device);
				Result<Image> image = readback.ReadTexture(*m_Renderer->GetFinalTexture());
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				return std::move(*image);
			}

			[[nodiscard]] GraphicsDevice& GetDevice() { return *m_Device; }
			[[nodiscard]] SceneRenderer& GetRenderer() { return *m_Renderer; }
			[[nodiscard]] SceneRendererPipelines& GetPipelines() { return *m_Pipelines; }
			[[nodiscard]] GpuResourceCache& GetCache() { return *m_Cache; }
			[[nodiscard]] AssetManager& GetAssets() { return m_Assets.GetManager(); }
		private:
			GraphicsDevice* m_Device = nullptr; // fixture back-reference; the device outlives every owned GPU object
			Test::AssetTestFixture m_Assets;
			Scope<GpuResourceCache> m_Cache{};
			Scope<SceneRendererPipelines> m_Pipelines{};
			Scope<SceneRenderer> m_Renderer{};
			mutable uint64_t m_FrameIndex = 0;
		};

		// The scale-3 output settings: the scene in a red letterbox.
		RenderOutputSettings MakeScale3Output()
		{
			return RenderOutputSettings{ .SceneWidth = SceneWidth, .SceneHeight = SceneHeight, .LetterboxColor = glm::vec3(1.0f, 0.0f, 0.0f) };
		}

		// Picks view pixel (x, y) of the last submitted render and returns the result once it is ready.
		std::optional<PickResult> Pick(OutputStageSetup& setup, uint64_t frameIndex, uint32_t x, uint32_t y)
		{
			const PickRequest request{ .X = x,
				.Y = y,
				.FrameIndex = frameIndex,
				.SceneRevision = 7,
				.Sequence = 1,
				.ViewGeneration = setup.GetRenderer().GetViewGeneration() };
			Result<PickTicket> ticket = setup.GetRenderer().RequestPick(request);
			REQUIRE_MESSAGE(ticket.has_value(), ticket.error().ToString());
			setup.GetDevice().WaitForIdle();
			Result<std::optional<PickResult>> ready = setup.GetRenderer().PollPick(*ticket, frameIndex + 2);
			REQUIRE_MESSAGE(ready.has_value(), ready.error().ToString());
			return *ready;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderOutputMapping: the default settings map the view to itself")
		{
			const RenderOutputMapping mapping = ComputeRenderOutputMapping(640, 360, {});
			CHECK(mapping
				== RenderOutputMapping{ .ViewWidth = 640, .ViewHeight = 360, .SceneWidth = 640, .SceneHeight = 360, .Scale = 1, .OffsetX = 0, .OffsetY = 0 });
			CHECK(mapping.IsIdentity());
			CHECK(MapViewPixelToScenePixel(mapping, 0, 0) == glm::uvec2(0, 0));
			CHECK(MapViewPixelToScenePixel(mapping, 639, 359) == glm::uvec2(639, 359));
			CHECK_FALSE(MapViewPixelToScenePixel(mapping, 640, 0).has_value());
			CHECK(MapViewPointToScenePoint(mapping, glm::vec2(12.5f, 3.5f)) == glm::vec2(12.5f, 3.5f));
			// A 0 component takes the view's: a 320 x 360 scene fits once and is centred horizontally.
			const RenderOutputMapping half = ComputeRenderOutputMapping(640, 360, { .SceneWidth = 320 });
			CHECK(half == RenderOutputMapping{ .ViewWidth = 640, .ViewHeight = 360, .SceneWidth = 320, .SceneHeight = 360, .Scale = 1, .OffsetX = 160, .OffsetY = 0 });
			CHECK_FALSE(half.IsIdentity());
		}

		TEST_CASE("RenderOutputMapping: an integer upscale centres the scene in a letterbox")
		{
			const RenderOutputSettings settings{ .SceneWidth = 160, .SceneHeight = 90 };
			// Exactly 4 times the scene: no letterbox.
			CHECK(ComputeRenderOutputMapping(640, 360, settings)
				== RenderOutputMapping{ .ViewWidth = 640, .ViewHeight = 360, .SceneWidth = 160, .SceneHeight = 90, .Scale = 4, .OffsetX = 0, .OffsetY = 0 });
			// Ten spare pixels each way: five on each side.
			CHECK(ComputeRenderOutputMapping(650, 370, settings)
				== RenderOutputMapping{ .ViewWidth = 650, .ViewHeight = 370, .SceneWidth = 160, .SceneHeight = 90, .Scale = 4, .OffsetX = 5, .OffsetY = 5 });
			// An odd leftover pixel goes to the right and bottom letterbox.
			CHECK(ComputeRenderOutputMapping(651, 371, settings)
				== RenderOutputMapping{ .ViewWidth = 651, .ViewHeight = 371, .SceneWidth = 160, .SceneHeight = 90, .Scale = 4, .OffsetX = 5, .OffsetY = 5 });
			// The smaller ratio decides: 3 across, 11 down.
			CHECK(ComputeRenderOutputMapping(ViewWidth, 400, { .SceneWidth = SceneWidth, .SceneHeight = SceneHeight })
				== RenderOutputMapping{ .ViewWidth = ViewWidth, .ViewHeight = 400, .SceneWidth = SceneWidth, .SceneHeight = SceneHeight, .Scale = 3, .OffsetX = 4, .OffsetY = 146 });
			CHECK(ComputeRenderOutputMapping(ViewWidth, ViewHeight, MakeScale3Output())
				== RenderOutputMapping{ .ViewWidth = ViewWidth, .ViewHeight = ViewHeight, .SceneWidth = SceneWidth, .SceneHeight = SceneHeight, .Scale = Scale, .OffsetX = OffsetX, .OffsetY = OffsetY });
		}

		TEST_CASE("RenderOutputMapping: a view pixel maps to its scene pixel and the letterbox to none")
		{
			const RenderOutputMapping mapping = ComputeRenderOutputMapping(ViewWidth, ViewHeight, MakeScale3Output());
			CHECK(MapViewPixelToScenePixel(mapping, 4, 6) == glm::uvec2(0, 0));
			CHECK(MapViewPixelToScenePixel(mapping, 6, 8) == glm::uvec2(0, 0));
			CHECK(MapViewPixelToScenePixel(mapping, 7, 9) == glm::uvec2(1, 1));
			CHECK(MapViewPixelToScenePixel(mapping, 195, 113) == glm::uvec2(SceneWidth - 1, SceneHeight - 1));
			CHECK_FALSE(MapViewPixelToScenePixel(mapping, 3, 6).has_value());
			CHECK_FALSE(MapViewPixelToScenePixel(mapping, 196, 6).has_value());
			CHECK_FALSE(MapViewPixelToScenePixel(mapping, 4, 5).has_value());
			CHECK_FALSE(MapViewPixelToScenePixel(mapping, 4, 114).has_value());
			CHECK_FALSE(MapViewPixelToScenePixel(mapping, ViewWidth, 0).has_value());
			// The UI's point mapping: exact, not clamped.
			CHECK(MapViewPointToScenePoint(mapping, glm::vec2(4.0f, 6.0f)) == glm::vec2(0.0f, 0.0f));
			CHECK(MapViewPointToScenePoint(mapping, glm::vec2(10.0f, 12.0f)) == glm::vec2(2.0f, 2.0f));
			CHECK(MapViewPointToScenePoint(mapping, glm::vec2(2.5f, 6.0f)) == glm::vec2(-0.5f, 0.0f));
		}

		TEST_CASE("RenderOutputMapping: a scene larger than the view is centred at scale 1 and cropped")
		{
			const RenderOutputMapping cropped = ComputeRenderOutputMapping(100, 50, { .SceneWidth = 160, .SceneHeight = 90 });
			CHECK(cropped
				== RenderOutputMapping{ .ViewWidth = 100, .ViewHeight = 50, .SceneWidth = 160, .SceneHeight = 90, .Scale = 1, .OffsetX = -30, .OffsetY = -20 });
			CHECK(MapViewPixelToScenePixel(cropped, 0, 0) == glm::uvec2(30, 20));
			CHECK(MapViewPixelToScenePixel(cropped, 99, 49) == glm::uvec2(129, 69));
			// Too wide but not too tall: cropped across, letterboxed down.
			const RenderOutputMapping wide = ComputeRenderOutputMapping(100, 100, { .SceneWidth = 160, .SceneHeight = 40 });
			CHECK(wide == RenderOutputMapping{ .ViewWidth = 100, .ViewHeight = 100, .SceneWidth = 160, .SceneHeight = 40, .Scale = 1, .OffsetX = -30, .OffsetY = 30 });
			CHECK_FALSE(MapViewPixelToScenePixel(wide, 0, 29).has_value());
			CHECK(MapViewPixelToScenePixel(wide, 0, 30) == glm::uvec2(30, 0));
			// An odd surplus: floor(-61 / 2) is -31, not the -30 of C++'s truncating division, so the odd pixel is cropped on
			// the left (31 pixels) and 30 on the right.
			const RenderOutputMapping odd = ComputeRenderOutputMapping(100, 101, { .SceneWidth = 161, .SceneHeight = 160 });
			CHECK(odd == RenderOutputMapping{ .ViewWidth = 100, .ViewHeight = 101, .SceneWidth = 161, .SceneHeight = 160, .Scale = 1, .OffsetX = -31, .OffsetY = -30 });
			CHECK(MapViewPixelToScenePixel(odd, 0, 0) == glm::uvec2(31, 30));
			CHECK(MapViewPixelToScenePixel(odd, 99, 100) == glm::uvec2(130, 130));
		}

		TEST_CASE("RenderOutputMapping: at scale 3 a view pixel's ray passes through its scene pixel, and a letterbox pixel has none")
		{
			// viewport.pick's CPU ray (decision 6): the view pixel is mapped to its scene pixel exactly once before the camera,
			// whose viewport is the scene resolution, makes the ray.
			CameraData camera;
			camera.ProjectionKind = RenderProjection::Orthographic;
			camera.OrthographicSize = 1.0f;
			camera.NearClip = 0.1f;
			camera.FarClip = 10.0f;
			camera.ViewportWidth = SceneWidth;
			camera.ViewportHeight = SceneHeight;
			camera.Projection = ComputeReverseZProjection(RenderProjection::Orthographic, 60.0f, 1.0f, 0.1f, 10.0f, SceneWidth, SceneHeight);
			const RenderOutputMapping mapping = ComputeRenderOutputMapping(ViewWidth, ViewHeight, MakeScale3Output());
			// View pixel (36, 22) is in the block of scene pixel (10, 5): (36 - 4) / 3 and (22 - 6) / 3.
			const Result<std::optional<RenderRayInterval>> mapped = ComputeOutputPixelRayInterval(camera, mapping, 36, 22);
			REQUIRE_MESSAGE(mapped.has_value(), mapped.error().ToString());
			REQUIRE(mapped->has_value());
			const Result<RenderRayInterval> scenePixel = ComputeViewPixelRayInterval(camera, 10, 5);
			REQUIRE(scenePixel.has_value());
			CHECK((*mapped)->Ray.Origin == scenePixel->Ray.Origin);
			CHECK((*mapped)->Ray.Direction == scenePixel->Ray.Direction);
			CHECK((*mapped)->MinDistance == scenePixel->MinDistance);
			CHECK((*mapped)->MaxDistance == scenePixel->MaxDistance);
			// Not the ray of the unmapped pixel. (Mapped twice, the pixel would fall in the letterbox, (5 - 6) being above the
			// scene image, which the REQUIRE above refuses.)
			const Result<RenderRayInterval> unmapped = ComputeViewPixelRayInterval(camera, 36, 22);
			REQUIRE(unmapped.has_value());
			CHECK((*mapped)->Ray.Origin != unmapped->Ray.Origin);
			// Every pixel of a scene pixel's block has that pixel's ray.
			const Result<std::optional<RenderRayInterval>> sameBlock = ComputeOutputPixelRayInterval(camera, mapping, 34, 21);
			REQUIRE(sameBlock.has_value());
			REQUIRE(sameBlock->has_value());
			CHECK((*sameBlock)->Ray.Origin == scenePixel->Ray.Origin);
			// A snapshot's camera comes with the request's view size as its viewport (render extraction); the ray still goes
			// through the scene pixel, as the renderer draws it at the scene resolution.
			CameraData viewSized = camera;
			viewSized.ViewportWidth = ViewWidth;
			viewSized.ViewportHeight = ViewHeight;
			const Result<std::optional<RenderRayInterval>> fromViewSized = ComputeOutputPixelRayInterval(viewSized, mapping, 36, 22);
			REQUIRE(fromViewSized.has_value());
			REQUIRE(fromViewSized->has_value());
			CHECK((*fromViewSized)->Ray.Origin == scenePixel->Ray.Origin);
			CHECK((*fromViewSized)->Ray.Direction == scenePixel->Ray.Direction);
			CHECK((*fromViewSized)->MinDistance == scenePixel->MinDistance);
			CHECK((*fromViewSized)->MaxDistance == scenePixel->MaxDistance);
			// The letterbox shows no scene: no ray, so viewport.pick reports no hit.
			const Result<std::optional<RenderRayInterval>> letterbox = ComputeOutputPixelRayInterval(camera, mapping, 1, ViewHeight / 2);
			REQUIRE(letterbox.has_value());
			CHECK_FALSE(letterbox->has_value());
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("OutputStage: at scale 3 each scene pixel is a 3x3 block inside the letterbox")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			OutputStageSetup setup(gpu);
			// The scene alone, rendered at its own resolution, is the reference.
			REQUIRE(setup.GetRenderer().Resize(SceneWidth, SceneHeight).has_value());
			const Image reference = setup.Render(setup.Snapshot({}));
			REQUIRE(reference.Width == SceneWidth);
			REQUIRE(setup.GetRenderer().Resize(ViewWidth, ViewHeight).has_value());
			// Before a Render at the new size, the mapping is the identity at that size.
			CHECK(setup.GetRenderer().GetOutputMapping() == ComputeRenderOutputMapping(ViewWidth, ViewHeight, {}));
			const Image view = setup.Render(setup.Snapshot(MakeScale3Output()));
			REQUIRE(view.Width == ViewWidth);
			REQUIRE(view.Height == ViewHeight);
			CHECK(setup.GetRenderer().GetOutputMapping() == ComputeRenderOutputMapping(ViewWidth, ViewHeight, MakeScale3Output()));
			for (uint32_t y = 0; y < ViewHeight; ++y)
			{
				for (uint32_t x = 0; x < ViewWidth; ++x)
				{
					if (!IsInSceneImage(x, y))
					{
						CHECK(GetPixel(view, x, y) == glm::u8vec4(255, 0, 0, 255));
						continue;
					}
					const uint32_t sceneX = (x - OffsetX) / Scale;
					const uint32_t sceneY = (y - OffsetY) / Scale;
					CHECK(GetPixel(view, x, y) == GetPixel(reference, sceneX, sceneY));
				}
			}
		}

		TEST_CASE("OutputStage: at scale 3 picking maps a view pixel to its scene pixel")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			OutputStageSetup setup(gpu);
			RenderSnapshot snapshot = setup.Snapshot(MakeScale3Output());
			snapshot.Flags = RenderViewFlags::Picking;
			setup.Submit(snapshot);
			const RenderOutputMapping mapping = setup.GetRenderer().GetOutputMapping();
			// The view pixel at the middle of the centre scene pixel's block shows the quad.
			const uint32_t centreX = static_cast<uint32_t>(mapping.OffsetX) + Scale * (SceneWidth / 2) + 1;
			const uint32_t centreY = static_cast<uint32_t>(mapping.OffsetY) + Scale * (SceneHeight / 2) + 1;
			const std::optional<PickResult> hit = Pick(setup, snapshot.FrameIndex, centreX, centreY);
			REQUIRE(hit.has_value());
			CHECK(hit->Entity == UUID(900));
			CHECK(hit->PickId == 1);
			// A scene pixel beside the quad is background, and so is a view pixel of the letterbox.
			snapshot.FrameIndex += 3;
			setup.Submit(snapshot);
			const std::optional<PickResult> corner =
				Pick(setup, snapshot.FrameIndex, static_cast<uint32_t>(mapping.OffsetX) + Scale + 1, static_cast<uint32_t>(mapping.OffsetY) + Scale + 1);
			REQUIRE(corner.has_value());
			CHECK_FALSE(corner->Entity.IsValid());
			snapshot.FrameIndex += 3;
			setup.Submit(snapshot);
			const std::optional<PickResult> letterbox = Pick(setup, snapshot.FrameIndex, 1, ViewHeight / 2);
			REQUIRE(letterbox.has_value());
			CHECK_FALSE(letterbox->Entity.IsValid());
			CHECK(letterbox->PickId == 0);
			// A pixel outside the view is refused, as at the identity (the scene's EntityId is smaller than the view).
			const PickRequest outside{ .X = ViewWidth,
				.Y = 0,
				.FrameIndex = snapshot.FrameIndex,
				.SceneRevision = 7,
				.Sequence = 2,
				.ViewGeneration = setup.GetRenderer().GetViewGeneration() };
			const Result<PickTicket> refused = setup.GetRenderer().RequestPick(outside);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("OutputStage: at scale 3 game Text draws before the upscale and the editor overlays after it")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			OutputStageSetup setup(gpu);
			RenderSnapshot snapshot = setup.Snapshot(MakeScale3Output());
			snapshot.Meshes.clear();
			snapshot.Camera.ClearColor = glm::vec3(0.0f);
			// White game text in the scene's top-left quarter, a green on-top debug line across the scene's middle row, and a
			// yellow debug label in the scene's bottom-right quarter.
			snapshot.Texts.push_back(TextItem{ .Text = "IIII", .Size = 600.0f, .Color = glm::vec4(1.0f), .Anchor = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f) });
			snapshot.DebugDraw.AddLine(glm::vec3(-4.0f, 0.0f, -1.0f), glm::vec3(4.0f, 0.0f, -1.0f), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), 0.0f,
				DebugDepthMode::OnTop);
			snapshot.DebugDraw.AddText(glm::vec3(0.8f, -0.5f, -1.0f), "IIII", 200.0f, glm::vec4(1.0f, 1.0f, 0.0f, 1.0f), 0.0f, DebugDepthMode::OnTop);
			const Image view = setup.Render(snapshot);
			// Before the upscale: every 3x3 block of the text's quarter is one scene pixel, and some are lit.
			uint32_t litBlocks = 0;
			for (uint32_t sceneY = 0; sceneY < SceneHeight / 2 - 2; ++sceneY)
			{
				for (uint32_t sceneX = 0; sceneX < SceneWidth / 2; ++sceneX)
				{
					const uint32_t x = OffsetX + Scale * sceneX;
					const uint32_t y = OffsetY + Scale * sceneY;
					const glm::u8vec4 first = GetPixel(view, x, y);
					for (uint32_t dy = 0; dy < Scale; ++dy)
					{
						for (uint32_t dx = 0; dx < Scale; ++dx)
							CHECK(GetPixel(view, x + dx, y + dy) == first);
					}
					litBlocks += first.r > 128 ? 1U : 0U;
				}
			}
			CHECK(litBlocks > 0);
			// After the upscale: the debug line is a one-pixel line of the view, not a three-pixel band of scene pixels.
			uint32_t greenRows = 0;
			for (uint32_t y = OffsetY; y < OffsetY + Scale * SceneHeight; ++y)
				greenRows += GetPixel(view, ViewWidth / 2, y).g > 128 ? 1U : 0U;
			CHECK(greenRows >= 1);
			CHECK(greenRows < Scale);
			// The label too is drawn at the view's resolution, after the upscale: over the scene's bottom-right quarter, where it
			// draws, some 3x3 block of a scene pixel holds more than one value (an upscaled label would fill whole blocks).
			uint32_t labelPixels = 0;
			uint32_t mixedBlocks = 0;
			for (uint32_t sceneY = SceneHeight / 2 + 2; sceneY < SceneHeight; ++sceneY)
			{
				for (uint32_t sceneX = SceneWidth / 2; sceneX < SceneWidth; ++sceneX)
				{
					const uint32_t x = OffsetX + Scale * sceneX;
					const uint32_t y = OffsetY + Scale * sceneY;
					const glm::u8vec4 first = GetPixel(view, x, y);
					bool mixed = false;
					for (uint32_t dy = 0; dy < Scale; ++dy)
					{
						for (uint32_t dx = 0; dx < Scale; ++dx)
						{
							const glm::u8vec4 pixel = GetPixel(view, x + dx, y + dy);
							mixed = mixed || pixel != first;
							labelPixels += pixel.r > 128 && pixel.g > 128 && pixel.b < 64 ? 1U : 0U;
						}
					}
					mixedBlocks += mixed ? 1U : 0U;
				}
			}
			CHECK(labelPixels > 0);
			CHECK(mixedBlocks > 0);
			// The rows in the declared order: game Text before the upscale, the labels after the other editor overlays, and the
			// letterbox repaint last (the line crosses the scene camera's frustum into the letterbox).
			const RenderStats stats = setup.GetRenderer().GetRenderStats();
			std::vector<std::string_view> names;
			for (const RenderPassStats& pass : stats.Passes)
				names.push_back(pass.Name);
			const auto position = [&names](std::string_view name)
			{
				return std::find(names.begin(), names.end(), name) - names.begin();
			};
			REQUIRE(position("Text") < static_cast<std::ptrdiff_t>(names.size()));
			REQUIRE(position("Labels") < static_cast<std::ptrdiff_t>(names.size()));
			CHECK(position("Text") < position("Upscale"));
			CHECK(position("Upscale") < position("Overlays"));
			CHECK(position("Overlays") < position("Labels"));
			CHECK(names.back() == "Letterbox");
		}

		TEST_CASE("OutputStage: at scale 3 the editor overlays are clipped to the scene image and the letterbox keeps its colour")
		{
			// Decision 6: every editor overlay that crosses the scene camera's frustum (the grid, a debug line, a label, the
			// wireframe and the dilated selection outline of a quad that leaves the frustum on the right) stops at the scaled scene
			// rectangle, as its scissor would stop it: the letterbox repaint ("Letterbox") gives the bars their colour again.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			OutputStageSetup setup(gpu);
			RenderSnapshot snapshot = setup.Snapshot(MakeScale3Output());
			snapshot.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Grid | RenderViewFlags::Selection | RenderViewFlags::Wireframe;
			snapshot.SelectedEntities = { UUID(900) };
			// The unit quad, 1.5 m wide and tall, its right half beyond the frustum's right edge (at 64 / 36 m).
			snapshot.Meshes.front().World = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(1.8f, 0.0f, -1.0f)), glm::vec3(1.5f));
			snapshot.DebugDraw.AddLine(glm::vec3(-4.0f, 0.5f, -1.0f), glm::vec3(4.0f, 0.5f, -1.0f), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), 0.0f,
				DebugDepthMode::OnTop);
			snapshot.DebugDraw.AddLine(glm::vec3(-0.5f, -4.0f, -1.0f), glm::vec3(-0.5f, 4.0f, -1.0f), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), 0.0f,
				DebugDepthMode::OnTop);
			snapshot.DebugDraw.AddText(glm::vec3(-1.75f, -0.5f, -1.0f), "IIIIIIII", 200.0f, glm::vec4(1.0f, 1.0f, 0.0f, 1.0f), 0.0f, DebugDepthMode::OnTop);
			const Image view = setup.Render(snapshot);
			uint32_t letterboxPixels = 0;
			for (uint32_t y = 0; y < ViewHeight; ++y)
			{
				for (uint32_t x = 0; x < ViewWidth; ++x)
				{
					if (IsInSceneImage(x, y))
						continue;
					CAPTURE(x);
					CAPTURE(y);
					CHECK(GetPixel(view, x, y) == glm::u8vec4(255, 0, 0, 255));
					++letterboxPixels;
				}
			}
			CHECK(letterboxPixels == ViewWidth * ViewHeight - Scale * SceneWidth * Scale * SceneHeight);
			// Inside the scene image the overlays draw: the vertical debug line's column, at view x 4 + (1 - 0.5 / (64 / 36)) *
			// 96 = 73 or its neighbour, is green somewhere in the scene image's rows.
			uint32_t green = 0;
			for (uint32_t y = OffsetY; y < OffsetY + Scale * SceneHeight; ++y)
			{
				for (uint32_t x = 70; x < 77; ++x)
				{
					const glm::u8vec4 pixel = GetPixel(view, x, y);
					green += pixel.g > 128 && pixel.r < 64 ? 1U : 0U;
				}
			}
			CHECK(green > 0);
			const RenderStats stats = setup.GetRenderer().GetRenderStats();
			REQUIRE_FALSE(stats.Passes.empty());
			CHECK(stats.Passes.back().Name == "Letterbox");
			CHECK(stats.Passes.back().DrawCalls == 4);
		}

		TEST_CASE("OutputStage: the pixel-art colour path leaves texels unchanged")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			OutputStageSetup setup(gpu);
			RenderSnapshot snapshot = setup.Snapshot(MakeScale3Output());
			snapshot.Meshes.clear();
			// SceneColor holds the linear value of the sRGB texel (200, 100, 50); the post settings would all change it.
			snapshot.Camera.ClearColor = glm::vec3(static_cast<float>(Test::SrgbToLinear(200.0 / 255.0)), static_cast<float>(Test::SrgbToLinear(100.0 / 255.0)),
				static_cast<float>(Test::SrgbToLinear(50.0 / 255.0)));
			snapshot.Post = PostProcessSettings{ .ExposureEV = 2.0f, .Tonemap = RenderTonemapper::AgX, .BloomEnabled = true, .FxaaEnabled = true };
			snapshot.Output.PixelArt = true;
			const Image pixelArt = setup.Render(snapshot);
			for (uint32_t y = OffsetY; y < OffsetY + Scale * SceneHeight; ++y)
			{
				for (uint32_t x = OffsetX; x < OffsetX + Scale * SceneWidth; ++x)
					CHECK(GetPixel(pixelArt, x, y) == glm::u8vec4(200, 100, 50, 255));
			}
			// Without the 2D colour path the same snapshot is tonemapped, so the case can tell the paths apart.
			snapshot.Output.PixelArt = false;
			snapshot.FrameIndex += 1;
			const Image tonemapped = setup.Render(snapshot);
			CHECK(GetPixel(tonemapped, ViewWidth / 2, ViewHeight / 2) != glm::u8vec4(200, 100, 50, 255));
		}

		TEST_CASE("OutputStage: a scene larger than the view is cropped without a validation message")
		{
			// Decision 6: a 161 x 91 scene in the 100 x 50 view is drawn at Scale 1 with negative offsets (-31, -21); the
			// upscale's viewport extends past the view and its scissor is clipped to it, so the fixture's validation (never a
			// negative scissor offset, VUID-vkCmdSetScissor-x-00595) stays clean. The editor overlays draw over the view with the
			// overlay camera, and a scene that covers the view leaves no letterbox to repaint.
			constexpr uint32_t CroppedWidth = 100;
			constexpr uint32_t CroppedHeight = 50;
			const RenderOutputSettings large{ .SceneWidth = 161, .SceneHeight = 91 };
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			OutputStageSetup setup(gpu);
			// The scene alone, rendered at its own resolution, is the reference.
			REQUIRE(setup.GetRenderer().Resize(large.SceneWidth, large.SceneHeight).has_value());
			const Image reference = setup.Render(setup.Snapshot({}));
			REQUIRE(setup.GetRenderer().Resize(CroppedWidth, CroppedHeight).has_value());
			const Image view = setup.Render(setup.Snapshot(large));
			REQUIRE(view.Width == CroppedWidth);
			REQUIRE(view.Height == CroppedHeight);
			const RenderOutputMapping mapping = setup.GetRenderer().GetOutputMapping();
			CHECK(mapping == ComputeRenderOutputMapping(CroppedWidth, CroppedHeight, large));
			CHECK(mapping.OffsetX == -31);
			CHECK(mapping.OffsetY == -21);
			for (uint32_t y = 0; y < CroppedHeight; ++y)
			{
				for (uint32_t x = 0; x < CroppedWidth; ++x)
					CHECK(GetPixel(view, x, y) == GetPixel(reference, x + 31, y + 21));
			}
			// The editor overlays and the selection, over the cropped image, with the depth upscale.
			RenderSnapshot overlays = setup.Snapshot(large);
			overlays.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Selection | RenderViewFlags::Picking;
			overlays.SelectedEntities = { UUID(900) };
			overlays.DebugDraw.AddLine(glm::vec3(-4.0f, 0.0f, -1.0f), glm::vec3(4.0f, 0.0f, -1.0f), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f), 0.0f,
				DebugDepthMode::OnTop);
			const Image overlaid = setup.Render(overlays);
			uint32_t greenRows = 0;
			for (uint32_t y = 0; y < CroppedHeight; ++y)
				greenRows += GetPixel(overlaid, CroppedWidth / 2, y).g > 128 ? 1U : 0U;
			CHECK(greenRows >= 1);
			const RenderStats stats = setup.GetRenderer().GetRenderStats();
			for (const RenderPassStats& pass : stats.Passes)
				CHECK(pass.Name != "Letterbox");
		}

		TEST_CASE("OutputStage: a capture at scale 3 maps its pixels to scene pixels")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			OutputStageSetup setup(gpu);
			Result<Scope<ViewportCapture>> capture =
				ViewportCapture::CreateForScenes(setup.GetDevice(), setup.GetPipelines(), setup.GetCache(), setup.GetAssets());
			REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
			CHECK((*capture)->GetLastOutputMapping() == RenderOutputMapping{});
			const Result<Image> image = (*capture)->Capture({ .Width = ViewWidth, .Height = ViewHeight }, setup.Snapshot(MakeScale3Output()));
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			CHECK(image->Width == ViewWidth);
			CHECK(image->Height == ViewHeight);
			const RenderOutputMapping mapping = (*capture)->GetLastOutputMapping();
			CHECK(mapping == ComputeRenderOutputMapping(ViewWidth, ViewHeight, MakeScale3Output()));
			CHECK(GetPixel(*image, 0, 0) == glm::u8vec4(255, 0, 0, 255));
			const std::optional<glm::uvec2> centre = MapViewPixelToScenePixel(mapping, ViewWidth / 2, ViewHeight / 2);
			REQUIRE(centre.has_value());
			CHECK(*centre == glm::uvec2(32, 18));
			capture->reset();
			setup.GetDevice().RunGarbageCollection();
		}
	}

}
