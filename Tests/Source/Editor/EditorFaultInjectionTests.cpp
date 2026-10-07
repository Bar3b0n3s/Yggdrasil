#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Platform/Process.h"
#include "Support/ChildOutput.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

// The editor rendering through the engine's frame, and the GPU fault paths of Architecture §8.14 item 8, as whole
// processes (Roadmap M5 "Fault injection"). Every run is headless, so no error dialog can block it, and gets
// --user-data-dir, so logs and crash reports land in the test's temporary directory.

namespace Engine {

	static Result<ProcessResult> RunRenderingEditor(const Test::TempDirectory& userData, std::vector<std::string> arguments,
		std::chrono::milliseconds timeout = std::chrono::seconds(60))
	{
		ENGINE_TRY_ASSIGN(std::filesystem::path editor, Test::GetBuiltExecutablePath("Editor"));
		const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
		arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
		arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
		return Process::Run({ .Executable = std::move(editor), .Arguments = std::move(arguments) }, timeout);
	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorApp: --headless --frames 10 renders with the Vulkan renderer and exits 0"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorRenders");
			const Result<ProcessResult> result = RunRenderingEditor(userData, { "--headless", "--frames", "10" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(result->StandardError.contains("Process context: VulkanLoader initialized"));
			// Validation is on (GetGpuApplicationArguments): nothing it reports, and no other error.
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());
		}

		TEST_CASE("EditorApp: --gpu-inject-fault=device-lost exits 4 with a crash report after the fatal-error hook"
			* doctest::test_suite(Test::GpuSuite))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorDeviceLost");
			const Result<ProcessResult> result =
				RunRenderingEditor(userData, { "--headless", "--frames", "10", "--gpu-inject-fault", "device-lost" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Crash);
			const Result<std::string> report = Test::ReadOnlyCrashReport(userData.GetPath());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (DeviceLost)"));
			// The editor's hook (the place of the autosave, §8.14 item 6) ran before the report was written.
			CHECK(report->contains("Editor fatal-error hook (DeviceLost): nothing is autosaved before EditorCore/Autosave (M10)"));
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("EditorApp: --gpu-inject-fault=hang exits 4 with a crash report after the bounded wait"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The first bounded wait, the frame pacer's at the first frame whose slot has a submission, skips its early-outs and
			// spends its whole budget of timed-out slices (FramePacer.h), however fast the GPU finished: the process ends with
			// GpuHang during the run's third frame, without waiting the budget's 10 s of wall time.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorHang");
			const Result<ProcessResult> result =
				RunRenderingEditor(userData, { "--headless", "--frames", "10", "--gpu-inject-fault", "hang" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Crash);
			const Result<std::string> report = Test::ReadOnlyCrashReport(userData.GetPath());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (GpuHang)"));
			CHECK(report->contains("Editor fatal-error hook (GpuHang): nothing is autosaved before EditorCore/Autosave (M10)"));
			CHECK(report->contains("FramePhase: Render"));
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("EditorApp: --gpu-inject-fault=oom-texture reports the failed texture and keeps running"
			* doctest::test_suite(Test::GpuSuite))
		{
			// Sampled-only textures fail (the ImGui font atlas here); render targets keep working, so the editor runs its
			// frames, reports the failure once, and never crashes (§8.14 item 7). The editor draws no assets before the scene
			// renderer (M7), so the asset half of the acceptance, the placeholder plus an ASSET_UPLOAD_FAILED diagnostic, is
			// "GpuResourceCache: an injected texture OOM yields the placeholder and a diagnostic" (ADR 0010 decision 22).
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("EditorOomTexture");
			const Result<ProcessResult> result =
				RunRenderingEditor(userData, { "--headless", "--frames", "10", "--gpu-inject-fault", "oom-texture" });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("editor stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(result->StandardError.contains("cannot create texture"));
			const std::vector<std::filesystem::path> reports = Test::ListCrashFiles(userData / ENGINE_PRODUCT_NAME / "Crashes", ".txt");
			CHECK(reports.empty());
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}
	}

}
