#include "TestsPCH.h"

#include "Engine/Renderer/ViewportCapture.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Support/AssetTestFixture.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <span>
#include <utility>

// The viewport capture (Architecture §8.13): snapshots rendered through the capture's own SceneRenderer over shared
// pipelines and read back at the requested size (Docs/Decisions/0012-m7-decisions.md decision 9).

namespace Engine {

	namespace {

		// A cube in front of a perspective camera at (0, 0, 5), lit along -Z, for a `width` x `height` view.
		RenderSnapshot MakeCubeSnapshot(uint32_t width, uint32_t height)
		{
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera.Position = glm::vec3(0.0f, 0.0f, 5.0f);
			snapshot.Camera.View = glm::translate(glm::mat4(1.0f), -snapshot.Camera.Position);
			snapshot.Camera.ViewportWidth = width;
			snapshot.Camera.ViewportHeight = height;
			snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.1f, 1000.0f, width, height);
			snapshot.Camera.ClearColor = glm::vec3(0.2f, 0.3f, 0.4f);
			snapshot.Meshes.push_back(MeshDrawItem{ .Mesh = BuiltinAssetHandles::CubeMesh });
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional, .Direction = glm::vec3(0.0f, 0.0f, -1.0f) });
			return snapshot;
		}

		// The pipelines, the cache and a capture over the fixture's device and asset manager.
		class CaptureSetup
		{
		public:
			CaptureSetup(Test::HeadlessGpuFixture& gpu, Test::AssetTestFixture& assets)
				: m_Cache(gpu.GetDevice(), assets.GetManager())
			{
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				m_Pipelines = std::move(*pipelines);
				Result<Scope<ViewportCapture>> capture = ViewportCapture::CreateForScenes(gpu.GetDevice(), *m_Pipelines, m_Cache, assets.GetManager());
				REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
				m_Capture = std::move(*capture);
			}

			[[nodiscard]] ViewportCapture& GetCapture() { return *m_Capture; }
		private:
			// Destroyed in reverse: the capture (its renderer), the pipelines, then the cache (§8.14 item 4).
			GpuResourceCache m_Cache;
			Scope<SceneRendererPipelines> m_Pipelines;
			Scope<ViewportCapture> m_Capture;
		};

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("ViewportCapture: captures a snapshot at the requested size" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				CaptureSetup setup(gpu, assets);
				const RenderSnapshot snapshot = MakeCubeSnapshot(DefaultViewportScreenshotWidth, DefaultViewportScreenshotHeight);

				// The default request is the golden-image size; two captures of one request are identical (§8.3: no temporal
				// effects), and the cube covers the centre while the corners show the clear colour.
				const Result<Image> first = setup.GetCapture().Capture({}, snapshot);
				REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
				CHECK(first->Width == DefaultViewportScreenshotWidth);
				CHECK(first->Height == DefaultViewportScreenshotHeight);
				CHECK(first->Format == nvrhi::Format::RGBA8_UNORM);
				REQUIRE(first->IsValid());
				const Result<Image> second = setup.GetCapture().Capture({}, snapshot);
				REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
				CHECK(first->Pixels == second->Pixels);
				const std::span<const std::byte> centreRow = first->GetRow(first->Height / 2);
				const std::span<const std::byte> topRow = first->GetRow(0);
				const auto centre = static_cast<std::ptrdiff_t>(static_cast<size_t>(first->Width / 2) * 4);
				CHECK_FALSE(std::equal(centreRow.begin() + centre, centreRow.begin() + centre + 3, topRow.begin()));
				// The top-left corner holds the camera's linear clear colour, sRGB-encoded: (0.2, 0.3, 0.4) -> 124, 149, 170.
				const std::array<int, 4> expected = { 124, 149, 170, 255 };
				for (size_t channel = 0; channel < expected.size(); ++channel)
				{
					CAPTURE(channel);
					CHECK(std::abs(std::to_integer<int>(topRow[channel]) - expected[channel]) <= 1);
				}

				// Another size needs no new pipeline: the capture renders any size over the shared pipelines.
				const Result<Image> small = setup.GetCapture().Capture({ .Width = 320, .Height = 200 }, MakeCubeSnapshot(320, 200));
				REQUIRE_MESSAGE(small.has_value(), small.error().ToString());
				CHECK(small->Width == 320);
				CHECK(small->Height == 200);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("ViewportCapture: MaxDimension scales the image down and keeps the aspect ratio" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				CaptureSetup setup(gpu, assets);
				const Result<Image> image = setup.GetCapture().Capture({ .MaxDimension = 160 }, MakeCubeSnapshot(640, 360));
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				CHECK(image->Width == 160);
				CHECK(image->Height == 90);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("ViewportCapture: a zero or oversized request is InvalidArgument" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				CaptureSetup setup(gpu, assets);
				const std::array<ViewportScreenshotRequest, 4> invalid = { {
					{ .Width = 0, .Height = 360 },
					{ .Width = 640, .Height = 0 },
					{ .Width = MaxViewportScreenshotDimension + 1, .Height = 360 },
					{ .Width = 640, .Height = MaxViewportScreenshotDimension + 1 },
				} };
				for (const ViewportScreenshotRequest& request : invalid)
				{
					CAPTURE(request.Width);
					CAPTURE(request.Height);
					const Result<Image> image = setup.GetCapture().Capture(request, RenderSnapshot{});
					REQUIRE_FALSE(image.has_value());
					CHECK(image.error().GetCode() == ErrorCode::InvalidArgument);
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("ViewportCapture: renders a snapshot at the requested size over shared pipelines" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets.GetManager());
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<ViewportCapture>> capture = ViewportCapture::CreateForScenes(gpu.GetDevice(), **pipelines, cache, assets.GetManager());
				REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());

				RenderSnapshot snapshot;
				snapshot.HasCamera = true;
				snapshot.Camera.Position = glm::vec3(0.0f, 0.0f, 5.0f);
				snapshot.Camera.View = glm::translate(glm::mat4(1.0f), -snapshot.Camera.Position);
				snapshot.Camera.ViewportWidth = 96;
				snapshot.Camera.ViewportHeight = 48;
				snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.1f, 1000.0f, 96, 48);
				snapshot.Meshes.push_back(MeshDrawItem{ .Mesh = BuiltinAssetHandles::CubeMesh });
				Result<Image> image = (*capture)->Capture({ .Width = 96, .Height = 48, .MaxDimension = 0 }, snapshot);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				CHECK(image->Width == 96);
				CHECK(image->Height == 48);
				// MaxDimension downscales the rendered image.
				Result<Image> small = (*capture)->Capture({ .Width = 96, .Height = 48, .MaxDimension = 48 }, snapshot);
				REQUIRE_MESSAGE(small.has_value(), small.error().ToString());
				CHECK(small->Width == 48);
				CHECK(small->Height == 24);
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
