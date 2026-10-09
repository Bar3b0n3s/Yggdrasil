#include "TestsPCH.h"

#include "Engine/Renderer/RenderStats.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Core/Log.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"

#include <array>
#include <limits>
#include <string_view>

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderStats: screenshot sampling cannot replace displayed view timings")
		{
			RenderStatsHistory scene;
			RenderStatsHistory game;
			RenderStatsHistory capture;
			scene.PublishCpu({ .Available = true, .FrameIndex = 10, .Passes = { { .Name = "ForwardOpaque", .DrawCalls = 2 } } });
			game.PublishCpu({ .Available = true, .FrameIndex = 11, .Passes = { { .Name = "ForwardOpaque", .DrawCalls = 3 } } });
			scene.PublishGpu({ .Available = true, .FrameIndex = 7, .Samples = { { .Name = "ForwardOpaque", .Milliseconds = 1.0 } } });
			game.PublishGpu({ .Available = true, .FrameIndex = 8, .Samples = { { .Name = "ForwardOpaque", .Milliseconds = 2.0 } } });
			capture.PublishCpu({ .Available = true, .FrameIndex = 500, .Passes = { { .Name = "ForwardOpaque", .DrawCalls = 8 } } });
			capture.PublishGpu({ .Available = true, .FrameIndex = 499, .Samples = { { .Name = "ForwardOpaque", .Milliseconds = 9.0 } } });
			capture.Reset();
			CHECK_FALSE(capture.GetLatest().Available);
			CHECK(capture.GetLatest().Passes.empty());
			REQUIRE(scene.GetLatest().Passes.size() == 1);
			REQUIRE(game.GetLatest().Passes.size() == 1);
			CHECK(scene.GetLatest().FrameIndex == 10);
			CHECK(scene.GetLatest().GpuFrameIndex == 7);
			CHECK(scene.GetLatest().Passes[0].DrawCalls == 2);
			CHECK(scene.GetLatest().Passes[0].GpuMilliseconds == 1.0);
			CHECK(game.GetLatest().FrameIndex == 11);
			CHECK(game.GetLatest().GpuFrameIndex == 8);
			CHECK(game.GetLatest().Passes[0].DrawCalls == 3);
			CHECK(game.GetLatest().Passes[0].GpuMilliseconds == 2.0);
		}

		TEST_CASE("RenderStats: unavailable and delayed samples preserve source frame identity")
		{
			RenderStatsHistory history;
			history.PublishCpu({ .Available = true, .FrameIndex = 9, .CpuMilliseconds = 12.0, .Passes = { { .Name = "Prepare", .CpuMilliseconds = 1.0 }, { .Name = "ForwardOpaque", .DrawCalls = 4 } } });
			history.PublishGpu({ .Available = true, .FrameIndex = 6, .Samples = { { .Name = "ForwardOpaque", .Milliseconds = 2.0 }, { .Name = "ForwardOpaque", .Milliseconds = 3.0 } } });
			const RenderStats& stats = history.GetLatest();
			CHECK(stats.FrameIndex == 9);
			CHECK(stats.GpuFrameIndex == 6);
			CHECK(stats.CpuMilliseconds == 12.0);
			REQUIRE(stats.Passes.size() == 2);
			CHECK_FALSE(stats.Passes[0].GpuAvailable);
			CHECK(stats.Passes[0].CpuMilliseconds == 1.0);
			CHECK(stats.Passes[1].GpuAvailable);
			CHECK(stats.Passes[1].GpuMilliseconds == 5.0);
			CHECK(stats.Passes[1].DrawCalls == 4);
			history.PublishGpu({});
			CHECK_FALSE(stats.GpuAvailable);
			CHECK_FALSE(stats.Passes[1].GpuAvailable);
			CHECK(stats.Passes[1].GpuMilliseconds == 0.0);
			CHECK(stats.Passes[1].DrawCalls == 4);
		}

		TEST_CASE("RenderStats: every enabled pass is reported in fixed pass order")
		{
			RenderStats stats{ .Available = true, .FrameIndex = 3 };
			double nowSeconds = 0.0;
			RenderRecordingContext recording(stats, [&nowSeconds]()
			{
				return nowSeconds;
			});
			constexpr std::array<std::string_view, 4> EnabledPasses = { "Prepare", "DepthPyramid", "ForwardOpaque", "Tonemap" };
			for (std::string_view name : EnabledPasses)
			{
				recording.BeginPass(name);
				nowSeconds += 0.001;
				recording.EndPass({ .DrawCalls = 1 });
			}
			RenderStatsHistory history;
			history.PublishCpu(stats);
			REQUIRE(history.GetLatest().Passes.size() == EnabledPasses.size());
			for (size_t index = 0; index < EnabledPasses.size(); ++index)
			{
				CHECK(history.GetLatest().Passes[index].Name == EnabledPasses[index]);
				CHECK(history.GetLatest().Passes[index].DrawCalls == 1);
			}
			history.PublishCpu({ .Available = true, .FrameIndex = 4, .Passes = { { .Name = "Prepare" } } });
			REQUIRE(history.GetLatest().Passes.size() == 1);
			CHECK(history.GetLatest().Passes[0].Name == "Prepare");
			CHECK(history.GetLatest().Passes[0].DrawCalls == 0);
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("RenderRecordingContext: GPU scopes preserve pass names and accumulate repeated work")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			GpuProfiler profiler(device, 1);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			RenderStats stats{ .Available = true, .FrameIndex = 31 };
			double nowSeconds = 0.0;
			RenderRecordingContext recording(stats, [&nowSeconds]()
			{
				return nowSeconds;
			}, &profiler);
			profiler.BeginFrame(0, 31);
			(*commandList)->open();
			recording.BeginPass("Prepare");
			nowSeconds += 0.001;
			recording.EndPass({});
			for (uint32_t cascade = 0; cascade < 2; ++cascade)
			{
				recording.BeginPass("DirectionalShadows", *commandList);
				nowSeconds += 0.002;
				recording.EndPass({ .DrawCalls = 1, .Triangles = 6 });
			}
			// A busy profiling slot can still record paired markers and CPU observations without query allocation.
			RenderStats untimedStats{};
			RenderRecordingContext untimed(untimedStats, [&nowSeconds]()
			{
				return nowSeconds;
			});
			untimed.BeginPass("Overlays", *commandList);
			untimed.EndPass({});
			(*commandList)->close();
			device.ExecuteCommandList(**commandList);
			device.WaitForIdle();
			profiler.BeginFrame(0, 90);
			const GpuTimingFrame result = profiler.GetLastFrameResult();
			REQUIRE(result.Available);
			CHECK(result.FrameIndex == 31);
			REQUIRE(result.Samples.size() == 2);
			CHECK(result.Samples[0].Name == "DirectionalShadows");
			CHECK(result.Samples[1].Name == "DirectionalShadows");
			CHECK(result.Samples[0].Depth == 0);
			CHECK(result.Samples[1].Depth == 0);
			RenderStatsHistory history;
			history.PublishCpu(stats);
			history.PublishGpu(result);
			REQUIRE(history.GetLatest().Passes.size() == 2);
			CHECK_FALSE(history.GetLatest().Passes[0].GpuAvailable);
			CHECK(history.GetLatest().Passes[1].GpuAvailable);
			CHECK(history.GetLatest().Passes[1].GpuMilliseconds == doctest::Approx(result.Samples[0].Milliseconds + result.Samples[1].Milliseconds));
			CHECK(history.GetLatest().Passes[1].CpuMilliseconds == doctest::Approx(4.0));
			CHECK(history.GetLatest().Passes[1].DrawCalls == 2);
			CHECK(history.GetLatest().Passes[1].Triangles == 12);
			REQUIRE(untimedStats.Passes.size() == 1);
			CHECK_FALSE(untimedStats.Passes[0].GpuAvailable);
		}

		TEST_CASE("RenderStats: enabled GPU passes have completed timings under both API caps")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			auto snapshot = fixture.Snapshot();
			snapshot.Flags = RenderViewFlags::Picking | RenderViewFlags::EditorOverlays | RenderViewFlags::Selection;
			snapshot.SelectedEntities = { UUID(900) };
			snapshot.Post.SsaoEnabled = true;
			snapshot.Post.BloomEnabled = true;
			snapshot.Post.FxaaEnabled = true;
			LightData directional;
			directional.CastShadows = true;
			directional.ShadowDistance = 10;
			directional.CascadeCount = 2;
			LightData spot = directional;
			spot.Type = RenderLightType::Spot;
			spot.Range = 10;
			spot.Entity = UUID(20);
			snapshot.Lights = { directional, spot };
			snapshot.DebugDraw.AddLine({ -1, 0, -2 }, { 1, 0, -2 }, glm::vec4(1));
			const uint32_t slots = gpu.GetDevice().GetFramesInFlight();
			for (uint32_t index = 0; index <= slots; ++index)
			{
				snapshot.FrameIndex = 100 + index * 3;
				fixture.Submit(snapshot);
				gpu.GetDevice().WaitForIdle(); // Test establishes retirement; production only polls completed submission IDs.
			}
			const auto stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.FrameIndex == snapshot.FrameIndex);
			CHECK(stats.GpuFrameIndex == 100);
			CHECK(stats.Available);
			REQUIRE(stats.GpuAvailable);
			CHECK(stats.Width == 64);
			CHECK(stats.Height == 64);
			CHECK(stats.VisibleMeshes == 1);
			// The quad at depth 3 is culled from the first practical-split cascade; one directional and one spot draw remain.
			CHECK(stats.ShadowDraws == 2);
			CHECK(stats.ShadowedSpotLights == 1);
			const std::array<std::string_view, 15> enabled = { "DirectionalShadows", "SpotShadows", "DepthNormal", "DepthPyramid", "GTAO", "GTAODenoiseHorizontal", "GTAODenoiseVertical", "ForwardOpaque", "Bloom", "Tonemap", "FXAA", "SelectionMask", "SelectionDilateHorizontal", "SelectionDilateVertical", "SelectionComposite" };
			for (const auto name : enabled)
			{
				CAPTURE(name);
				const auto* pass = Test::SceneRendererIntegrationFixture::FindPass(stats, name);
				REQUIRE(pass != nullptr);
				CHECK(pass->GpuAvailable);
				CHECK(pass->GpuMilliseconds >= 0);
				CHECK(pass->DrawCalls + pass->Dispatches > 0);
			}
			const auto* depth = Test::SceneRendererIntegrationFixture::FindPass(stats, "DepthNormal");
			REQUIRE(depth != nullptr);
			CHECK(depth->DrawCalls == 1);
			CHECK(depth->Triangles == 2);
			CHECK_FALSE(Test::SceneRendererIntegrationFixture::FindPass(stats, "Prepare")->GpuAvailable);
			// A capture uses its own history, frame sequence and targets while sharing immutable pipelines.
			auto capture = ViewportCapture::CreateForScenes(gpu.GetDevice(), fixture.Pipelines(), fixture.Cache(), fixture.Assets());
			REQUIRE(capture);
			auto captured = (*capture)->Capture({ .Width = 32, .Height = 24 }, snapshot);
			REQUIRE_MESSAGE(captured.has_value(), captured.error().ToString());
			const auto after = fixture.Renderer().GetRenderStats();
			CHECK(after.FrameIndex == stats.FrameIndex);
			CHECK(after.GpuFrameIndex == stats.GpuFrameIndex);
			CHECK(after.Width == 64);
			CHECK(after.Passes.size() == stats.Passes.size());
		}

		TEST_CASE("Renderer: logs the 1080p frame time as a warning-only measurement")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 1920, 1080);
			auto snapshot = fixture.Snapshot();
			snapshot.Post.SsaoEnabled = true;
			for (uint32_t index = 0; index <= gpu.GetDevice().GetFramesInFlight(); ++index)
			{
				snapshot.FrameIndex = 200 + index;
				fixture.Submit(snapshot);
				gpu.GetDevice().WaitForIdle();
			}
			const auto stats = fixture.Renderer().GetRenderStats();
			REQUIRE(stats.GpuAvailable);
			double gpuMilliseconds = 0;
			for (const auto& pass : stats.Passes)
				if (pass.GpuAvailable)
					gpuMilliseconds += pass.GpuMilliseconds;
			Test::ExpectLog measured(LogLevel::Warn, "1080p renderer measurement");
			ENGINE_WARN("1080p renderer measurement: CPU {:.3f} ms at frame {}, GPU {:.3f} ms at frame {}; diagnostic only",
				stats.CpuMilliseconds, stats.FrameIndex, gpuMilliseconds, stats.GpuFrameIndex);
		}
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderRecordingContext: subpasses report independent CPU times and recorded counters")
		{
			RenderStats stats{};
			double nowSeconds = 1.0;
			RenderRecordingContext recording(stats, [&nowSeconds]()
			{
				return nowSeconds;
			});
			recording.BeginPass("GTAO");
			nowSeconds += 0.002;
			recording.EndPass({ .Dispatches = 1 });
			recording.BeginPass("GTAODenoiseHorizontal");
			nowSeconds += 0.003;
			recording.EndPass({ .Dispatches = 1 });
			recording.BeginPass("GTAODenoiseVertical");
			nowSeconds += 0.004;
			recording.EndPass({ .Dispatches = 1 });
			recording.BeginPass("ForwardOpaque");
			nowSeconds += 0.005;
			recording.EndPass({ .DrawCalls = std::numeric_limits<uint32_t>::max(), .Triangles = std::numeric_limits<uint32_t>::max() });
			recording.BeginPass("ForwardOpaque");
			nowSeconds += 0.006;
			recording.EndPass({ .DrawCalls = 1, .Triangles = 2 });
			// An early failure before recording work still closes its CPU scope; a subsequent pass can start.
			recording.BeginPass("SelectionMask");
			recording.EndPass({});
			recording.BeginPass("Tonemap");
			recording.EndPass({ .Dispatches = 1 });
			REQUIRE(stats.Passes.size() == 6);
			CHECK(stats.Passes[0].CpuMilliseconds == doctest::Approx(2.0));
			CHECK(stats.Passes[1].CpuMilliseconds == doctest::Approx(3.0));
			CHECK(stats.Passes[2].CpuMilliseconds == doctest::Approx(4.0));
			CHECK(stats.Passes[3].CpuMilliseconds == doctest::Approx(11.0));
			CHECK(stats.Passes[0].Dispatches == 1);
			CHECK(stats.Passes[1].Dispatches == 1);
			CHECK(stats.Passes[2].Dispatches == 1);
			CHECK(stats.Passes[3].DrawCalls == std::numeric_limits<uint32_t>::max());
			CHECK(stats.Passes[3].Triangles == std::numeric_limits<uint32_t>::max());
			CHECK(stats.Passes[4].DrawCalls == 0);
			CHECK(stats.Passes[4].Dispatches == 0);
			CHECK(stats.Passes[4].CpuMilliseconds == 0.0);
			CHECK(stats.Passes[5].Dispatches == 1);
		}
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderStats: an unavailable collection cannot relabel an earlier successful frame")
		{
			RenderStatsHistory history;
			history.PublishCpu({ .Available = true, .FrameIndex = 20, .Passes = { { .Name = "ForwardOpaque" } } });
			history.PublishGpu({ .Available = true, .FrameIndex = 10, .Samples = { { .Name = "ForwardOpaque", .Milliseconds = 2.0 } } });
			history.PublishGpu({});
			history.PublishGpu({ .Available = true, .FrameIndex = 9, .Samples = { { .Name = "ForwardOpaque", .Milliseconds = 9.0 } } });
			CHECK_FALSE(history.GetLatest().GpuAvailable);
			CHECK_FALSE(history.GetLatest().Passes[0].GpuAvailable);
			history.PublishGpu({ .Available = true, .FrameIndex = 12, .Samples = { { .Name = "ForwardOpaque", .Milliseconds = 3.0 } } });
			CHECK(history.GetLatest().FrameIndex == 20);
			CHECK(history.GetLatest().GpuFrameIndex == 12);
			CHECK(history.GetLatest().Passes[0].GpuMilliseconds == 3.0);
			history.PublishGpu({ .Available = true, .FrameIndex = 11, .Samples = { { .Name = "ForwardOpaque", .Milliseconds = 8.0 } } });
			CHECK(history.GetLatest().GpuFrameIndex == 12);
			CHECK(history.GetLatest().Passes[0].GpuMilliseconds == 3.0);
			history.Reset();
			history.PublishCpu({ .Available = true, .FrameIndex = 2, .Passes = { { .Name = "ForwardOpaque" } } });
			history.PublishGpu({ .Available = true, .FrameIndex = 1, .Samples = { { .Name = "ForwardOpaque", .Milliseconds = 4.0 } } });
			CHECK(history.GetLatest().GpuFrameIndex == 1);
		}
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderStats: toggling passes preserves CPU and GPU source-frame separation")
		{
			RenderStatsHistory history;
			history.PublishCpu({ .Available = true, .FrameIndex = 10, .Passes = { { .Name = "GTAO", .Dispatches = 1 }, { .Name = "ForwardOpaque", .DrawCalls = 2 } } });
			history.PublishGpu({ .Available = true, .FrameIndex = 8, .Samples = { { .Name = "GTAO", .Milliseconds = 1.0 }, { .Name = "ForwardOpaque", .Milliseconds = 2.0 } } });
			history.PublishCpu({ .Available = true, .FrameIndex = 11, .Passes = { { .Name = "ForwardOpaque", .DrawCalls = 7 }, { .Name = "SelectionMask", .DrawCalls = 1 } } });
			const RenderStats& stats = history.GetLatest();
			REQUIRE(stats.Passes.size() == 2);
			CHECK(stats.FrameIndex == 11);
			CHECK(stats.GpuFrameIndex == 8);
			CHECK(stats.Passes[0].DrawCalls == 7);
			CHECK(stats.Passes[0].GpuAvailable);
			CHECK(stats.Passes[0].GpuMilliseconds == 2.0);
			CHECK_FALSE(stats.Passes[1].GpuAvailable);
			CHECK(stats.Passes[1].GpuMilliseconds == 0.0);
			CHECK(stats.Passes[1].DrawCalls == 1);
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("DebugViews: overdraw counts accepted fragments before depth rejection")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			MaterialData rejectedMask;
			rejectedMask.AlphaMode = AlphaMode::Mask;
			rejectedMask.BaseColor.a = 0.25f;
			fixture.Assets().Publish(UUID(8920), CreateRef<MaterialData>(rejectedMask));
			MaterialData blend;
			blend.AlphaMode = AlphaMode::Blend;
			blend.BaseColor.a = 0.25f;
			fixture.Assets().Publish(UUID(8921), CreateRef<MaterialData>(blend));
			blend.BaseColor.a = 0;
			fixture.Assets().Publish(UUID(8922), CreateRef<MaterialData>(blend));
			const std::array<glm::ivec3, 7> palette = { glm::ivec3(0), { 0, 0, 255 }, { 0, 255, 255 }, { 0, 255, 0 }, { 255, 255, 0 }, { 255, 0, 0 }, { 255, 0, 0 } };
			for (const auto projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
			{
				for (uint32_t count = 0; count <= 6; ++count)
				{
					CAPTURE(count);
					auto snapshot = fixture.Snapshot(projection);
					const auto prototype = snapshot.Meshes[0];
					snapshot.Meshes.clear();
					snapshot.DebugView = RenderDebugView::Overdraw;
					snapshot.Post.ExposureEV = 10;
					snapshot.Post.SsaoEnabled = snapshot.Post.BloomEnabled = snapshot.Post.FxaaEnabled = true;
					snapshot.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Grid | RenderViewFlags::Icons | RenderViewFlags::Selection | RenderViewFlags::Wireframe;
					snapshot.SelectedEntities = { UUID(900) };
					snapshot.DebugDraw.AddLine({ -1, 0, -1 }, { 1, 0, -1 }, glm::vec4(1), 0, DebugDepthMode::OnTop);
					for (uint32_t index = 0; index < count + 2; ++index)
					{
						auto draw = prototype;
						draw.World[3].z -= static_cast<float>(index) * 0.1f;
						if (index < count && index % 2 == 1)
						{
							draw.Materials = { UUID(8921) };
							draw.World[0][0] = -1; // Mirrored winding still contributes.
						}
						if (index >= count)
							draw.Materials = { UUID(index == count ? 8920 : 8922) };
						snapshot.Meshes.push_back(draw);
					}
					const auto image = fixture.Render(snapshot);
					for (size_t channel = 0; channel < 3; ++channel)
						CHECK(Test::SceneRendererIntegrationFixture::Channel(image, 32, 32, channel) == palette[count][static_cast<glm::length_t>(channel)]);
					const auto stats = fixture.Renderer().GetRenderStats();
					const auto* pass = Test::SceneRendererIntegrationFixture::FindPass(stats, "Overdraw");
					REQUIRE(pass != nullptr);
					CHECK(pass->DrawCalls == count + 2);
					CHECK(pass->Triangles == 2 * (count + 2));
					for (const auto disabled : { "DirectionalShadows", "GTAO", "ForwardOpaque", "Bloom", "FXAA", "SelectionMask", "Wireframe", "Overlays", "Text" })
						CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, disabled) == nullptr);
				}
			}
		}
	}

}
