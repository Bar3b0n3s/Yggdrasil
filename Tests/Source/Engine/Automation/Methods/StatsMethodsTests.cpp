#include "TestsPCH.h"

#include "Engine/Automation/Methods/StatsMethods.h"

#include "Engine/Automation/Methods/Private/RenderQueryTestContext.h"
#include "Engine/Automation/Methods/RaycastMethods.h"
#include "Engine/Renderer/RenderStats.h"
#include "Support/AutomationTestClient.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <cmath>
#include <limits>
#include <variant>

namespace Engine {

	TEST_CASE("StatsGet: renderer summaries preserve exact frame identifiers and hide unavailable samples" * doctest::test_suite("Automation"))
	{
		RenderStats stats;
		stats.Available = true;
		stats.FrameIndex = std::numeric_limits<uint64_t>::max();
		stats.GpuAvailable = true;
		stats.GpuFrameIndex = stats.FrameIndex - 2;
		stats.Width = 320;
		stats.Height = 240;
		stats.Passes.push_back({ .Name = "Prepare", .CpuMilliseconds = 2.5, .GpuMilliseconds = 7.0, .GpuAvailable = false });
		stats.Passes.push_back({ .Name = "Opaque", .CpuMilliseconds = 1.5, .GpuMilliseconds = 3.0, .GpuAvailable = true, .DrawCalls = 9 });
		const auto summary = MakeStatsViewSummary("scene", stats);
		CHECK(summary.Frame == "18446744073709551615");
		CHECK(summary.GpuFrame == "18446744073709551613");
		CHECK(summary.Width == 320);
		REQUIRE(summary.Passes.size() == 2);
		CHECK_FALSE(summary.Passes[0].GpuAvailable);
		CHECK(summary.Passes[0].GpuMilliseconds == 0.0f);
		CHECK(summary.Passes[1].GpuMilliseconds == 3.0f);
		CHECK(summary.Passes[1].DrawCalls == 9);
		stats.GpuAvailable = false;
		const auto cpuOnly = MakeStatsViewSummary("game", stats);
		CHECK(cpuOnly.GpuFrame.empty());
		CHECK_FALSE(cpuOnly.Passes[1].GpuAvailable);
		CHECK(cpuOnly.Passes[1].GpuMilliseconds == 0.0f);
		stats.Available = false;
		const auto unavailable = MakeStatsViewSummary("game", stats);
		CHECK(unavailable.Name == "game");
		CHECK_FALSE(unavailable.Available);
		CHECK(unavailable.Passes.empty());
		CHECK(unavailable.Frame.empty());
		CHECK(unavailable.Width == 0);
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("StatsGet: play reports the live script heap and limits without advancing time")
		{
			Test::AutomationFixture setup("ScriptHeapStatistics");
			REQUIRE(setup.Call("play.start", Json{ { "lockstep", true } }));
			const auto before = setup.Call("play.state", Json::object());
			REQUIRE(before);
			const auto stats = setup.Call("stats.get", Json::object());
			REQUIRE(stats);
			CHECK((*stats)["scriptAvailable"] == Json(true));
			CHECK((*stats)["scriptHeapBytes"] > Json(0));
			CHECK((*stats)["scriptSoftLimitBytes"] > (*stats)["scriptHeapBytes"]);
			CHECK((*stats)["scriptHardLimitBytes"] > (*stats)["scriptSoftLimitBytes"]);
			const auto after = setup.Call("play.state", Json::object());
			REQUIRE(after);
			CHECK((*after)["tick"] == (*before)["tick"]);
			REQUIRE(setup.Call("play.stop", Json::object()));
			const auto stopped = setup.Call("stats.get", Json::object());
			REQUIRE(stopped);
			CHECK((*stopped)["scriptAvailable"] == Json(false));
			CHECK((*stopped)["scriptHeapBytes"] == Json(0));
		}

		TEST_CASE("StatsGet: telemetry conversion stays finite and range safe")
		{
			CHECK(ToStatsTelemetry(1.25) == 1.25f);
			CHECK(ToStatsTelemetry(16777217.0) == 16777216.0f); // byte telemetry is approximate
			CHECK(ToStatsTelemetry(std::numeric_limits<double>::max()) == std::numeric_limits<float>::max());
			CHECK(ToStatsTelemetry(std::numeric_limits<double>::infinity()) == std::numeric_limits<float>::max());
			CHECK(ToStatsTelemetry(-std::numeric_limits<double>::infinity()) == 0.0f);
			CHECK(ToStatsTelemetry(std::numeric_limits<double>::quiet_NaN()) == 0.0f);
			CHECK(ToStatsTelemetry(-1.0) == 0.0f);
			CHECK_FALSE(std::signbit(ToStatsTelemetry(-0.0)));
			CHECK(std::isfinite(ToStatsTelemetry(std::numeric_limits<double>::max())));
		}
		TEST_CASE("StatsGet: renderer none reports counts with unavailable GPU samples")
		{
			Test::AutomationFixture setup("StatsWithoutGpu");
			REQUIRE(setup.Call("entity.create", Json{ { "name", "One" } }));
			const auto before = setup.Call("scene.tree", Json::object());
			REQUIRE(before);
			const auto result = setup.Call("stats.get", Json::object());
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK((*result)["entities"] == Json(1));
			CHECK((*result)["bodies"] == Json(0));
			CHECK((*result)["voices"] == Json(0));
			CHECK((*result)["scriptAvailable"] == Json(false));
			CHECK((*result)["memoryAllocationCount"] == Json(0));
			REQUIRE((*result)["views"].size() == 2);
			for (const auto& view : (*result)["views"])
			{
				CHECK(view["available"] == Json(false));
				CHECK(view["gpuAvailable"] == Json(false));
			}
			const auto after = setup.Call("scene.tree", Json::object());
			REQUIRE(after);
			CHECK((*after)["scene"] == (*before)["scene"]);
		}
		TEST_CASE("StatsGet: per-view pass summaries retain delayed GPU frame identities")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			ProjectSettings settings;
			TypeRegistry types;
			RegisterAutomationSharedTypes(types);
			RegisterStatsMethodTypes(types);
			types.Freeze();
			MethodRegistry methods(types);
			RegisterStatsMethods(methods);
			methods.Freeze();
			const auto* method = methods.Find("stats.get");
			REQUIRE(method != nullptr);
			Test::RenderQueryTestContext context(fixture.GetScene(), assets, settings, { .Method = method, .Params = Json::object(), .Registry = &methods });
			context.Statistics.Fps = ToStatsTelemetry(59.123456789);
			context.Statistics.Views = { { .Name = "scene", .Available = true, .Frame = "9007199254740997", .GpuFrame = "9007199254740993", .GpuAvailable = true, .Width = 640, .Height = 360, .CpuMilliseconds = 1.25, .Passes = { { .Name = "Forward", .CpuMilliseconds = 0.25, .GpuMilliseconds = 1.75, .GpuAvailable = true, .DrawCalls = 2, .Triangles = 24 } } },
				{ .Name = "game", .Available = true, .Frame = "42", .GpuFrame = "40", .GpuAvailable = true, .Width = 320, .Height = 180 } };
			const auto result = methods.Invoke(context);
			REQUIRE(std::holds_alternative<Json>(result));
			const auto& json = std::get<Json>(result);
			CHECK(json["fps"] == Json(context.Statistics.Fps));
			CHECK(json["views"][0]["frame"] == Json("9007199254740997"));
			CHECK(json["views"][0]["gpuFrame"] == Json("9007199254740993"));
			CHECK(json["views"][1]["frame"] == Json("42"));
			CHECK(json["views"][0]["passes"][0]["gpuMilliseconds"] == Json(1.75));
			CHECK(context.StatisticsReads == 1);
			CHECK(method->Specification.SupportsDryRun);
			CHECK(method->Specification.AllowedInBatch);
			CHECK(method->Specification.AvailableInRuntime);
			context.Statistics.Fps = std::numeric_limits<float>::infinity();
			context.Statistics.CpuMilliseconds = -1.0f;
			context.Statistics.DroppedSeconds = std::numeric_limits<float>::quiet_NaN();
			context.Statistics.ScriptHeapBytes = std::numeric_limits<float>::infinity();
			context.Statistics.ScriptSoftLimitBytes = -1.0f;
			context.Statistics.ScriptHardLimitBytes = std::numeric_limits<float>::quiet_NaN();
			context.Statistics.Views[0].CpuMilliseconds = -1.0f;
			context.Statistics.Views[0].Passes[0].CpuMilliseconds = std::numeric_limits<float>::quiet_NaN();
			context.Statistics.Views[0].Passes[0].GpuMilliseconds = std::numeric_limits<float>::infinity();
			const auto sanitized = methods.Invoke(context);
			REQUIRE(std::holds_alternative<Json>(sanitized));
			const auto& safe = std::get<Json>(sanitized);
			CHECK(safe["fps"] == Json(std::numeric_limits<float>::max()));
			CHECK(safe["cpuMilliseconds"] == Json(0.0f));
			CHECK(safe["droppedSeconds"] == Json(0.0f));
			CHECK(safe["scriptHeapBytes"] == Json(std::numeric_limits<float>::max()));
			CHECK(safe["scriptSoftLimitBytes"] == Json(0.0f));
			CHECK(safe["scriptHardLimitBytes"] == Json(0.0f));
			CHECK(safe["views"][0]["cpuMilliseconds"] == Json(0.0f));
			CHECK(safe["views"][0]["passes"][0]["cpuMilliseconds"] == Json(0.0f));
			CHECK(safe["views"][0]["passes"][0]["gpuMilliseconds"] == Json(std::numeric_limits<float>::max()));
			CHECK(safe["views"][0]["frame"] == Json("9007199254740997"));
			CHECK(safe["views"][0]["gpuFrame"] == Json("9007199254740993"));
			context.HasStatistics = false;
			const auto absent = Automation::StatsGet(context, {});
			REQUIRE_FALSE(absent);
			CHECK(absent.error().GetCode() == ErrorCode::Unsupported);
		}
	}

}
