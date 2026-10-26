#include "TestsPCH.h"

#include "Engine/Renderer/ViewportCapture.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Support/AssetTestFixture.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TrimCounter.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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
				// The top-left corner holds the camera's linear clear colour through the default post chain (§8.9: the AgX Base
				// tonemapper, then the sRGB OETF and the +-1 LSB dither; bloom leaves a uniform region as it is and FXAA does not
				// touch it): (0.2, 0.3, 0.4) -> 134, 152, 165 (TonemapPass.h's AgX formula, computed in double precision).
				const std::array<int, 4> expected = { 134, 152, 165, 255 };
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

		TEST_CASE("ViewportCapture: a capture after a device loss is byte-identical to one before"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, Docs/Decisions/0029-m18-contract.md decisions 32 and 33; Roadmap M18: "a screenshot after
			// recovery is byte-identical to one taken before"): the capture registers itself for its own objects (its read-back
			// command list), and its private renderer, the pipeline set and the cache recover through the placement rule.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				CaptureSetup setup(gpu, assets);
				const RenderSnapshot snapshot = MakeCubeSnapshot(320, 200);
				ViewportCapture& capture = setup.GetCapture();
				Test::DeviceLossTest test;
				test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "ViewportCapture" };
				// Each use captures as an editor's frames do, collecting the garbage after each capture, and as often as the
				// capture's renderer takes to settle, from its creation as from the recovery, so the live GPU objects the loss test
				// compares are those of a settled capture: its RenderTargetPool keeps a free target for DefaultKeepFrames frames
				// (before the loss, the renderer's first 1 x 1 targets, free once the first collection destroyed their
				// framebuffers), and its GPU profiler gets one slot of timer queries per frame in flight. The pixels of every
				// capture are compared.
				GraphicsDevice& device = gpu.GetDevice();
				test.Use = [&device, &capture, &snapshot]() -> Result<std::vector<std::byte>>
				{
					std::vector<std::byte> images;
					for (uint32_t index = 0; index < 2 * RenderTargetPool::DefaultKeepFrames; ++index)
					{
						ENGINE_TRY_ASSIGN(const Image image, capture.Capture({ .Width = 320, .Height = 200 }, snapshot));
						device.RunGarbageCollection();
						images.insert(images.end(), image.Pixels.begin(), image.Pixels.end());
					}
					return images;
				};
				const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
				REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("ViewportCapture: a capture that runs out of memory is retried after a trim and is byte-identical to one without the fault"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, Docs/Decisions/0029-m18-contract.md decisions 42 and 46; Roadmap M18: "an injected
			// out-of-memory recovers with a byte-identical screenshot"). Under oom-rendertarget the capture's first target after the
			// device's first garbage collection fails; the capture trims and captures again, and its image equals the same
			// capture on a device without the fault.
			std::vector<std::vector<std::byte>> images;
			for (const GpuFault fault : { GpuFault::None, GpuFault::OomRenderTarget })
			{
				CAPTURE(GpuFaultToString(fault));
				Test::HeadlessGpuFixture gpu({ .InjectFault = fault });
				ENGINE_REQUIRE_GPU(gpu);
				Test::AssetTestFixture assets;
				{
					CaptureSetup setup(gpu, assets);
					gpu.GetDevice().RunGarbageCollection(); // the application's first frame: the fault is armed from here
					const Result<Image> image = setup.GetCapture().Capture({ .Width = 320, .Height = 200 }, MakeCubeSnapshot(320, 200));
					REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
					images.push_back(image->Pixels);
				}
				gpu.GetDevice().RunGarbageCollection();
			}
			REQUIRE(images.size() == 2);
			CHECK(images[1] == images[0]);
		}

		TEST_CASE("ViewportCapture: a capture that keeps running out of memory fails alone with OutOfMemory, after one trim"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, decisions 42, 43, 44, 45 and 46; Roadmap M18: "a persistent one fails only its request"):
			// the capture's new size runs out of memory again after its one trim, so the capture fails alone with OutOfMemory,
			// never with an image of another size, GetLastOutputMapping stays the last successful capture's, the device stays
			// usable, and the next request is tried again.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomRenderTargetPersistent });
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				CaptureSetup setup(gpu, assets);
				ViewportCapture& capture = setup.GetCapture();
				// Before the first garbage collection (the startup), a capture creates its targets.
				const Result<Image> before = capture.Capture({ .Width = 320, .Height = 200 }, MakeCubeSnapshot(320, 200));
				REQUIRE_MESSAGE(before.has_value(), before.error().ToString());
				const RenderOutputMapping rendered = capture.GetLastOutputMapping();
				gpu.GetDevice().RunGarbageCollection();

				const Test::TrimCounter counter(gpu.GetDevice().GetRecreation());
				for (uint32_t request = 1; request <= 2; ++request)
				{
					CAPTURE(request);
					const Result<Image> failed = capture.Capture({ .Width = 640, .Height = 360 }, MakeCubeSnapshot(640, 360));
					REQUIRE_FALSE(failed.has_value());
					CHECK(failed.error().GetCode() == ErrorCode::OutOfMemory);
					// One trim: the renderer's Resize trimmed and failed alone, and the capture does not trim for its OutOfMemory.
					CHECK(counter.GetTrims() == request);
					CHECK(capture.GetLastOutputMapping() == rendered);
					CHECK_FALSE(gpu.GetDevice().IsLost());
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("ViewportCapture: a trim between captures releases the last capture's targets, and the next capture renders the same image"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, decision 43): a capture keeps no image on display, so its renderer's targets are rebuilt on
			// demand: a trim that another request runs releases them (the capture's TrimUnused hook, SceneRenderer::
			// ReleaseTargets, then its renderer's pool), and the next capture creates them again and reads back the same bytes,
			// at its size and after a capture at another size, which releases the previous size's targets before it resizes.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				CaptureSetup setup(gpu, assets);
				ViewportCapture& capture = setup.GetCapture();
				const GpuResourceTracker& tracker = gpu.GetDevice().GetResourceTracker();
				const Result<Image> first = capture.Capture({ .Width = 320, .Height = 200 }, MakeCubeSnapshot(320, 200));
				REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
				// Sweeps what the capture dropped, so only what it holds stays alive.
				gpu.GetDevice().RunGarbageCollection();
				const uint64_t heldTextures = tracker.GetLiveCount(GpuResourceType::Texture);

				gpu.GetDevice().TrimUnused();
				if (GpuResourceTracker::IsEnabled())
					CHECK(tracker.GetLiveCount(GpuResourceType::Texture) < heldTextures);
				const Result<Image> again = capture.Capture({ .Width = 320, .Height = 200 }, MakeCubeSnapshot(320, 200));
				REQUIRE_MESSAGE(again.has_value(), again.error().ToString());
				CHECK(again->Pixels == first->Pixels);

				const Result<Image> larger = capture.Capture({ .Width = 640, .Height = 360 }, MakeCubeSnapshot(640, 360));
				REQUIRE_MESSAGE(larger.has_value(), larger.error().ToString());
				gpu.GetDevice().TrimUnused();
				const Result<Image> back = capture.Capture({ .Width = 320, .Height = 200 }, MakeCubeSnapshot(320, 200));
				REQUIRE_MESSAGE(back.has_value(), back.error().ToString());
				CHECK(back->Pixels == first->Pixels);
				CHECK_FALSE(gpu.GetDevice().IsLost());
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("ViewportCapture: a debug-view capture whose pipelines keep running out of memory returns OutOfMemory"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, decisions 42, 44 and 46): a capture never returns an image of another debug view than the
			// one asked for; its pipelines run out of memory again after the trim, so the capture fails alone, and a Lit
			// capture, whose pipelines exist, still succeeds.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomPipelinePersistent });
			ENGINE_REQUIRE_GPU(gpu);
			Test::AssetTestFixture assets;
			{
				CaptureSetup setup(gpu, assets);
				gpu.GetDevice().RunGarbageCollection();
				RenderSnapshot albedo = MakeCubeSnapshot(320, 200);
				albedo.DebugView = RenderDebugView::Albedo;
				const Result<Image> failed = setup.GetCapture().Capture({ .Width = 320, .Height = 200 }, albedo);
				REQUIRE_FALSE(failed.has_value());
				CHECK(failed.error().GetCode() == ErrorCode::OutOfMemory);
				const Result<Image> lit = setup.GetCapture().Capture({ .Width = 320, .Height = 200 }, MakeCubeSnapshot(320, 200));
				REQUIRE_MESSAGE(lit.has_value(), lit.error().ToString());
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
