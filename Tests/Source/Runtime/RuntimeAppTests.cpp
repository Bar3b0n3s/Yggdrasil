#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Platform/Process.h"
#include "Support/ChildOutput.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

// The runtime executable as a whole process (Roadmap M2, M6): RunApplication with the runtime's application, which builds
// its RuntimeAssetManager (the game manifest and its paks arrive with M7). Runs that need no GPU pass --renderer none;
// the Vulkan renderer is covered in the GPU suite.

namespace Engine {

	// A headless runtime rendering with the Vulkan renderer, validated like every GPU test's process
	// (Test::GetGpuApplicationArguments), with its logs and crash reports in `userData`.
	static Result<ProcessResult> RunRenderingRuntime(const Test::TempDirectory& userData, std::vector<std::string> arguments)
	{
		ENGINE_TRY_ASSIGN(std::filesystem::path runtime, Test::GetBuiltExecutablePath("Runtime"));
		const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
		arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
		arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
		return Process::Run({ .Executable = std::move(runtime), .Arguments = std::move(arguments) }, std::chrono::seconds(60));
	}

	TEST_SUITE("Runtime")
	{
		TEST_CASE("RuntimeApp: --headless --frames 10 exits 0 using ManualClock")
		{
			const Result<std::filesystem::path> runtime = Test::GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
			Test::TempDirectory userData("RuntimeHeadless");
			const ProcessSpecification specification = {
				.Executable = *runtime,
				.Arguments = { "--headless", "--renderer", "none", "--frames", "10", "--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) },
			};
			const Result<ProcessResult> result = Process::Run(specification, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK_MESSAGE(result->ExitCode == ExitCode::Success, result->StandardError);
			// Headless play without lockstep is paced at FixedHz (§4.2), never run flat out.
			CHECK(result->StandardError.contains("Frame loop started: Manual clock, 60 Hz, throttled"));
			// The log file follows --user-data-dir, never the real user-data folder.
			std::error_code error;
			CHECK(std::filesystem::is_regular_file(userData / ENGINE_PRODUCT_NAME / "Logs" / "Runtime.log", error));
		}

		TEST_CASE("RuntimeApp: builds and injects its RuntimeAssetManager at startup")
		{
			const Result<std::filesystem::path> runtime = Test::GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
			Test::TempDirectory userData("RuntimeAssets");
			const ProcessSpecification specification = {
				.Executable = *runtime,
				.Arguments = { "--headless", "--renderer", "none", "--frames", "2", "--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) },
			};
			const Result<ProcessResult> result = Process::Run(specification, std::chrono::seconds(60));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK_MESSAGE(result->ExitCode == ExitCode::Success, result->StandardError);
			// Until the game manifest adds paks (M7), the manager serves the procedural built-ins.
			CHECK(result->StandardError.contains(std::format("Runtime asset manager: no paks, {} procedural built-ins", GetProceduralBuiltinEntries().size())));
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());
		}

		TEST_CASE("RuntimeApp: --headless --frames 10 renders with the Vulkan renderer and exits 0"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeRenders");
			const Result<ProcessResult> result = RunRenderingRuntime(userData, { "--headless", "--frames", "10" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(result->StandardError.contains("Process context: VulkanLoader initialized"));
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());
		}

		TEST_CASE("RuntimeApp: --gpu-inject-fault=device-lost exits 4 with a crash report"
			* doctest::test_suite(Test::GpuSuite))
		{
			// §8.14 item 8: the device is reported lost after the first submission, and the check that follows it ends the
			// process through GraphicsDevice::RaiseDeviceLost.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeDeviceLost");
			const Result<ProcessResult> result =
				RunRenderingRuntime(userData, { "--headless", "--frames", "10", "--gpu-inject-fault", "device-lost" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Crash);
			const Result<std::string> report = Test::ReadOnlyCrashReport(userData.GetPath());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (DeviceLost)"));
			CHECK(report->contains("device lost"));
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("RuntimeApp: --gpu-inject-fault=hang exits 4 with a crash report after the bounded wait"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The frame pacer's first wait for a slot with a submission spends its budget of timed-out slices without waiting
			// for wall time (FramePacer.h) and ends the process with GpuHang during the render phase.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeHang");
			const Result<ProcessResult> result = RunRenderingRuntime(userData, { "--headless", "--frames", "10", "--gpu-inject-fault", "hang" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Crash);
			const Result<std::string> report = Test::ReadOnlyCrashReport(userData.GetPath());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (GpuHang)"));
			CHECK(report->contains("FramePhase: Render"));
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}
	}

}
