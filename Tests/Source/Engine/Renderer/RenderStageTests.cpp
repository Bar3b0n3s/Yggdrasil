#include "TestsPCH.h"

#include "Engine/Renderer/RenderStage.h"

#include "Engine/Core/Profiler.h"
#include "Engine/Graphics/GpuProfiler.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderStats.h"
#include "Support/DeathTest.h"
#include "Support/HeadlessGpuFixture.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The GPU-zone hook at each stage boundary and the profiler's GPU merge (M18, C18.1, R-PASS and R-PROF;
// Docs/Decisions/0029-m18-contract.md decisions 4 and 11).

namespace Engine {

	ENGINE_DEATH_TEST("Renderer/StageInsideStage")
	{
		RenderStats stats;
		RenderRecordingContext recording(stats, []()
		{
			return 0.0;
		});
		recording.BeginStage(RenderStage::Shadows);
		recording.BeginStage(RenderStage::Forward);
	}

	ENGINE_DEATH_TEST("Renderer/StageEndedInsidePass")
	{
		RenderStats stats;
		RenderRecordingContext recording(stats, []()
		{
			return 0.0;
		});
		recording.BeginStage(RenderStage::Forward);
		recording.BeginPass("ForwardOpaque");
		recording.EndStage();
	}

	namespace {

		// The names of the zones of `zones` on the CPU threads (not Profiler::GpuThreadIndex) that start with "Stage:", in
		// timeline order.
		std::vector<std::string> GetCpuStageZoneNames(const std::vector<ProfileZone>& zones)
		{
			std::vector<std::string> names;
			for (const ProfileZone& zone : zones)
			{
				if (zone.ThreadIndex != Profiler::GpuThreadIndex && zone.Name.starts_with("Stage:"))
					names.emplace_back(zone.Name);
			}
			return names;
		}

	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("RenderRecordingContext: each stage is one GPU zone around its passes")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			REQUIRE(Profiler::IsInitialized());
			GraphicsDevice& device = gpu.GetDevice();
			GpuProfiler profiler(device, 1);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			RenderStats stats{ .Available = true, .FrameIndex = 5 };
			double nowSeconds = 0.0;
			const uint64_t since = Profiler::GetTimeNs();
			{
				RenderRecordingContext recording(stats, [&nowSeconds]()
				{
					return nowSeconds;
				}, &profiler);
				profiler.BeginFrame(0, 5);
				(*commandList)->open();
				// Prepare records no GPU work: its stage has no command list.
				recording.BeginStage(RenderStage::Prepare);
				recording.BeginPass("Prepare");
				nowSeconds += 0.001;
				recording.EndPass({});
				recording.EndStage();
				// A stage that records no pass still opens and closes its zone.
				recording.BeginStage(RenderStage::Shadows, *commandList);
				recording.EndStage();
				recording.BeginStage(RenderStage::Forward, *commandList);
				recording.BeginPass("ForwardOpaque", *commandList);
				recording.EndPass({ .DrawCalls = 1 });
				recording.BeginPass("Skybox", *commandList);
				recording.EndPass({ .DrawCalls = 1 });
				recording.EndStage();
				(*commandList)->close();
			}
			device.ExecuteCommandList(**commandList);
			device.WaitForIdle();
			profiler.BeginFrame(0, 6);
			const GpuTimingFrame result = profiler.GetLastFrameResult();
			REQUIRE(result.Available);
			REQUIRE(result.Samples.size() == 4);
			CHECK(result.Samples[0].Name == "Stage:Shadows");
			CHECK(result.Samples[0].Depth == 0);
			CHECK(result.Samples[1].Name == "Stage:Forward");
			CHECK(result.Samples[1].Depth == 0);
			CHECK(result.Samples[2].Name == "ForwardOpaque");
			CHECK(result.Samples[2].Depth == 1);
			CHECK(result.Samples[3].Name == "Skybox");
			CHECK(result.Samples[3].Depth == 1);
			// A stage is never a stats row.
			REQUIRE(stats.Passes.size() == 3);
			CHECK(stats.Passes[0].Name == "Prepare");
			CHECK(stats.Passes[1].Name == "ForwardOpaque");
			CHECK(stats.Passes[2].Name == "Skybox");
			// Every stage, Prepare's included, is a CPU zone of the process profiler.
			CHECK(GetCpuStageZoneNames(Profiler::CollectZones(since)) == std::vector<std::string>{ "Stage:Prepare", "Stage:Shadows", "Stage:Forward" });
		}

		TEST_CASE("Profiler: a rendered view's GPU zones reach the profiler timeline")
		{
			// R-PROF: the per-pass and per-stage GPU timings of a view are merged into the process profiler's timeline (on the
			// GPU thread index) once their frame retires, so profiler.capture's Chrome trace has CPU and GPU zones.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			REQUIRE(Profiler::IsInitialized());
			Test::SceneRendererIntegrationFixture setup(gpu);
			const uint64_t since = Profiler::GetTimeNs();
			RenderSnapshot snapshot = setup.Snapshot();
			// The renderer collects a frame's timings when its timing slot comes round again.
			for (uint32_t frame = 0; frame <= gpu.GetDevice().GetFramesInFlight(); ++frame)
			{
				snapshot.FrameIndex = 100 + frame;
				setup.Submit(snapshot);
				gpu.GetDevice().WaitForIdle();
			}
			const std::vector<ProfileZone> zones = Profiler::CollectZones(since);
			const auto isGpuZone = [](std::string_view name)
			{
				return [name](const ProfileZone& zone)
				{
					return zone.ThreadIndex == Profiler::GpuThreadIndex && zone.Name == name;
				};
			};
			const auto forward = std::find_if(zones.begin(), zones.end(), isGpuZone("ForwardOpaque"));
			const auto stage = std::find_if(zones.begin(), zones.end(), isGpuZone("Stage:Forward"));
			REQUIRE(forward != zones.end());
			REQUIRE(stage != zones.end());
			CHECK(forward->BeginNs >= since);
			CHECK(stage->BeginNs <= forward->BeginNs);
			CHECK(forward->EndNs <= stage->EndNs);
			CHECK(forward->Depth == stage->Depth + 1);
			// Each retired frame is merged once.
			CHECK(static_cast<uint32_t>(std::count_if(zones.begin(), zones.end(), isGpuZone("Stage:Forward"))) <= gpu.GetDevice().GetFramesInFlight());
		}
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderRecordingContext: every stage's zones are named after the stage")
		{
			REQUIRE(Profiler::IsInitialized());
			RenderStats stats;
			const uint64_t since = Profiler::GetTimeNs();
			{
				RenderRecordingContext recording(stats, []()
				{
					return 0.0;
				});
				for (uint32_t index = 0; index < RenderStageCount; ++index)
				{
					recording.BeginStage(static_cast<RenderStage>(index));
					recording.EndStage();
				}
				// A pass outside every stage stays valid.
				recording.BeginPass("Prepare");
				recording.EndPass({});
			}
			std::vector<std::string> expected;
			for (uint32_t index = 0; index < RenderStageCount; ++index)
				expected.push_back("Stage:" + std::string(RenderStageToString(static_cast<RenderStage>(index))));
			CHECK(GetCpuStageZoneNames(Profiler::CollectZones(since)) == expected);
			// A stage is never a stats row.
			REQUIRE(stats.Passes.size() == 1);
			CHECK(stats.Passes[0].Name == "Prepare");
		}

		TEST_CASE("RenderRecordingContext: a stage begun inside a stage or ended inside a pass is a programmer error")
		{
			ENGINE_CHECK_DEATH("Renderer/StageInsideStage", "RenderRecordingContext::BeginStage");
			ENGINE_CHECK_DEATH("Renderer/StageEndedInsidePass", "RenderRecordingContext::EndStage");
		}

		TEST_CASE("RenderStatsHistory: an available GPU frame's samples reach the profiler timeline once")
		{
			// R-PROF: PublishGpu places each sample at the frame's CPU anchor plus its start, on the GPU thread index, with
			// its depth, and merges a frame once however often it is published.
			REQUIRE(Profiler::IsInitialized());
			const uint64_t anchor = Profiler::GetTimeNs() + 1000;
			const GpuTimingFrame frame{
				.Available = true,
				.FrameIndex = 7,
				.CpuBeginNs = anchor,
				.Samples = {
					{ .Name = "RenderStageTests.Stage", .StartMilliseconds = 0.0, .Milliseconds = 2.0, .Depth = 0 },
					{ .Name = "RenderStageTests.Pass", .StartMilliseconds = 0.5, .Milliseconds = 1.25, .Depth = 1 },
				},
			};
			RenderStatsHistory history;
			history.PublishGpu(frame);
			history.PublishGpu(frame);
			// An older frame and an unavailable result are not merged.
			GpuTimingFrame older = frame;
			older.FrameIndex = 6;
			history.PublishGpu(older);
			history.PublishGpu(GpuTimingFrame{});
			std::vector<ProfileZone> merged;
			for (const ProfileZone& zone : Profiler::CollectZones(anchor))
			{
				if (zone.Name.starts_with("RenderStageTests."))
					merged.push_back(zone);
			}
			REQUIRE(merged.size() == 2);
			CHECK(merged[0].Name == "RenderStageTests.Stage");
			CHECK(merged[0].ThreadIndex == Profiler::GpuThreadIndex);
			CHECK(merged[0].BeginNs == anchor);
			CHECK(merged[0].EndNs == anchor + 2'000'000);
			CHECK(merged[0].Depth == 0);
			CHECK(merged[1].Name == "RenderStageTests.Pass");
			CHECK(merged[1].ThreadIndex == Profiler::GpuThreadIndex);
			CHECK(merged[1].BeginNs == anchor + 500'000);
			CHECK(merged[1].EndNs == anchor + 1'750'000);
			CHECK(merged[1].Depth == 1);
			// A newer frame is merged in turn.
			GpuTimingFrame newer = frame;
			newer.FrameIndex = 8;
			newer.CpuBeginNs = anchor + 10'000'000;
			history.PublishGpu(newer);
			const std::vector<ProfileZone> later = Profiler::CollectZones(anchor + 10'000'000);
			CHECK(std::count_if(later.begin(), later.end(), [](const ProfileZone& zone)
			{
				return zone.Name == "RenderStageTests.Pass";
			}) == 1);
		}
	}

}
