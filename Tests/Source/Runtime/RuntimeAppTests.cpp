#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Platform/Process.h"
#include "Support/ChildOutput.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TempDirectory.h"
#include "Support/TestGame.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

// The runtime executable as a whole process (Architecture §14.3; Roadmap M2, M6, M7) on test games written like exported
// ones (Support/TestGame.h) and given with --manifest (Docs/Decisions/0012-m7-decisions.md decision 10). Runs that need no
// GPU pass --renderer none; the Vulkan renderer is covered in the GPU suite. Every run gets --user-data-dir, so nothing is
// written into the real user-data folder.

namespace Engine {

	namespace {

		// The Runtime with `arguments`, its user data in `userData`.
		Result<ProcessResult> RunRuntime(const Test::TempDirectory& userData, std::vector<std::string> arguments)
		{
			ENGINE_TRY_ASSIGN(std::filesystem::path runtime, Test::GetBuiltExecutablePath("Runtime"));
			arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
			return Process::Run({ .Executable = std::move(runtime), .Arguments = std::move(arguments) }, std::chrono::seconds(120));
		}

		// A headless runtime rendering with the Vulkan renderer, validated like every GPU test's process
		// (Test::GetGpuApplicationArguments).
		Result<ProcessResult> RunRenderingRuntime(const Test::TempDirectory& userData, std::vector<std::string> arguments)
		{
			const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			return RunRuntime(userData, std::move(arguments));
		}

		std::string ManifestArgument(const std::filesystem::path& manifest)
		{
			return "--manifest=" + Test::PathToUtf8(manifest);
		}

		// The text of the one crash report of the game `name` (<userData>/<name>/Crashes: the manifest names the user-data
		// folder). Errors: NotFound unless there is exactly one report; those of FileSystem::ReadText.
		Result<std::string> ReadGameCrashReport(const Test::TempDirectory& userData, std::string_view name)
		{
			const std::vector<std::filesystem::path> reports = Test::ListCrashFiles(userData / (std::string(name) + "/Crashes"), ".txt");
			if (reports.size() != 1)
				return MakeError(ErrorCode::NotFound, "expected one crash report of '{}', found {}", name, reports.size());
			return FileSystem::ReadText(reports.front());
		}

		// Replaces the first `from` in `path`'s text with `to`.
		void EditFile(const std::filesystem::path& path, std::string_view from, std::string_view to)
		{
			Result<std::string> text = FileSystem::ReadText(path);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			const size_t position = text->find(from);
			REQUIRE_MESSAGE(position != std::string::npos, std::string(from));
			text->replace(position, from.size(), to);
			REQUIRE(FileSystem::WriteFileAtomic(path, std::as_bytes(std::span(text->data(), text->size()))).has_value());
		}

	}

	TEST_SUITE("Runtime")
	{
		TEST_CASE("RuntimeApp: a missing Game.json exits 3 naming the manifest")
		{
			// "test_runtime_missing_manifest_exits_3" (§4.1: a required file missing is InitFailed).
			Test::TempDirectory userData("RuntimeNoManifest");
			const Result<ProcessResult> result =
				RunRuntime(userData, { "--headless", "--renderer", "none", "--frames", "1", ManifestArgument(userData / "Missing/Game.json") });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::InitFailed);
			CHECK(result->StandardError.contains("Missing/Game.json"));
			// The application name comes from the manifest, so without one nothing is written into a user-data folder.
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(userData / ENGINE_PRODUCT_NAME, error));
		}

		TEST_CASE("RuntimeApp: an invalid Game.json exits 3 naming the file and the member")
		{
			Test::TempDirectory userData("RuntimeBadManifest");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game");
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			EditFile(*manifest, "\"FixedHz\": 60", "\"FixedHz\": 0");
			const Result<ProcessResult> result = RunRuntime(userData, { "--headless", "--renderer", "none", "--frames", "1", ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::InitFailed);
			CHECK(result->StandardError.contains("Game.json"));
			CHECK(result->StandardError.contains("/Simulation/FixedHz"));
		}

		TEST_CASE("RuntimeApp: a pak that does not match Game.json exits 3 naming it, logging under the manifest's name")
		{
			// §14.1 integrity: the paks are the exported ones. The process context exists by then, named after the manifest
			// (§4.4, "test_exported_user_data_folder_uses_manifest_name").
			Test::TempDirectory userData("RuntimeBadPak");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "PakCheckGame" });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			Result<Buffer> pak = FileSystem::ReadFile(userData / "Game/Data/Game.pak");
			REQUIRE(pak.has_value());
			pak->push_back(std::byte{ 0 });
			REQUIRE(FileSystem::WriteFileAtomic(userData / "Game/Data/Game.pak", *pak).has_value());

			const Result<ProcessResult> result = RunRuntime(userData, { "--headless", "--renderer", "none", "--frames", "1", ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::InitFailed);
			CHECK(result->StandardError.contains("Data/Game.pak"));
			CHECK(result->StandardError.contains("damaged"));
			std::error_code error;
			CHECK(std::filesystem::is_regular_file(userData / "PakCheckGame/Logs/Runtime.log", error));
			CHECK_FALSE(std::filesystem::exists(userData / ENGINE_PRODUCT_NAME, error));
		}

		TEST_CASE("RuntimeApp: malformed Runtime options are usage errors naming the option")
		{
			Test::TempDirectory userData("RuntimeOptions");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game");
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			struct Case
			{
				std::vector<std::string> Arguments;
				std::string Named;
			};
			const std::vector<Case> cases = {
				{ { "--screenshot-at", "ten:Shot.png" }, "--screenshot-at" },
				{ { "--screenshot-at", "60" }, "--screenshot-at" },
				{ { "--screenshot-at", "60:" }, "--screenshot-at" },
				{ { "--renderer", "none", "--screenshot-at", "60:Shot.png" }, "--screenshot-at" },
				{ { "--automation=0" }, "--automation" },
				{ { "--paused" }, "--paused" },
				{ { "--manifest=" }, "--manifest" },
			};
			for (const Case& usage : cases)
			{
				std::vector<std::string> arguments = { "--headless", "--frames", "1" };
				if (usage.Named != "--manifest")
					arguments.push_back(ManifestArgument(*manifest));
				arguments.insert(arguments.end(), usage.Arguments.begin(), usage.Arguments.end());
				CAPTURE(usage.Arguments.back());
				const Result<ProcessResult> result = RunRuntime(userData, std::move(arguments));
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				INFO("runtime stderr: ", result->StandardError);
				CHECK(result->ExitCode == ExitCode::UsageError);
				CHECK(result->StandardError.contains(usage.Named));
			}
		}

		TEST_CASE("RuntimeApp: --headless --frames 10 runs the game with ManualClock and exits 0")
		{
			Test::TempDirectory userData("RuntimeHeadless");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "HeadlessGame" });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const Result<ProcessResult> result =
				RunRuntime(userData, { "--headless", "--renderer", "none", "--frames", "10", "--expect-no-errors", ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			// Headless play without lockstep is paced at the game's FixedHz (§4.2), never run flat out.
			CHECK(result->StandardError.contains("Frame loop started: Manual clock, 60 Hz, throttled"));
			// The asset manager serves the start scene from Game.pak, and the session runs it.
			CHECK(result->StandardError.contains("Runtime: 'HeadlessGame' started with scene 'Assets/Scenes/Main.scene' (3 entities"));
			// Logs follow the manifest's name below --user-data-dir (§4.4, §14.1), never the engine's product name.
			std::error_code error;
			CHECK(std::filesystem::is_regular_file(userData / "HeadlessGame/Logs/Runtime.log", error));
			CHECK_FALSE(std::filesystem::exists(userData / ENGINE_PRODUCT_NAME, error));
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());
		}

		TEST_CASE("RuntimeApp: --frames ending before --screenshot-at fails the run"
			* doctest::test_suite(Test::GpuSuite))
		{
			// --screenshot-at needs a renderer.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeLateScreenshot");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const std::string shot = "--screenshot-at=100:" + Test::PathToUtf8(userData / "Shot.png");
			const Result<ProcessResult> result = RunRenderingRuntime(userData, { "--headless", "--frames", "5", shot, ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Failed);
			CHECK(result->StandardError.contains("before --screenshot-at 100"));
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(userData / "Shot.png", error));
		}

		TEST_CASE("RuntimeApp: --headless --frames 10 renders the game with the Vulkan renderer and exits 0"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The shaders come from the
			// game's Engine.pak (EngineContextSpecification::EnginePak), never the build's shader directory.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeRenders");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const Result<ProcessResult> result =
				RunRenderingRuntime(userData, { "--headless", "--frames", "10", "--expect-no-errors", ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(result->StandardError.contains("Process context: VulkanLoader initialized"));
			CHECK(result->StandardError.contains("mounted as engine://"));
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());
		}

		TEST_CASE("RuntimeApp: --screenshot-at writes the game view at the window's size"
			* doctest::test_suite(Test::GpuSuite))
		{
			// §8.13: re-rendered at the
			// window's framebuffer size after the frame in which the tick is reached.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeScreenshot");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Width = 160, .Height = 90, .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const std::filesystem::path png = userData / "Shots/Tick10.png";
			const std::string shot = "--screenshot-at=10:" + Test::PathToUtf8(png);
			const Result<ProcessResult> result =
				RunRenderingRuntime(userData, { "--headless", "--frames", "12", "--expect-no-errors", shot, ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(result->StandardError.contains("Screenshot of tick 10 (160x90)"));
			const Result<Image> image = ReadPng(png);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			CHECK(image->Width == 160);
			CHECK(image->Height == 90);
		}

		TEST_CASE("RuntimeApp: --gpu-inject-fault=device-lost exits 4 with a crash report"
			* doctest::test_suite(Test::GpuSuite))
		{
			// §8.14 item 8: the device is
			// reported lost after the first submission, and the check that follows it ends the process through
			// GraphicsDevice::RaiseDeviceLost.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeDeviceLost");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "DeviceLostGame", .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const Result<ProcessResult> result =
				RunRenderingRuntime(userData, { "--headless", "--frames", "10", "--gpu-inject-fault", "device-lost", ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Crash);
			const Result<std::string> report = ReadGameCrashReport(userData, "DeviceLostGame");
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (DeviceLost)"));
			CHECK(report->contains("device lost"));
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("RuntimeApp: --gpu-inject-fault=hang exits 4 with a crash report after the bounded wait"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The frame pacer's first wait for
			// a slot with a submission spends its budget of timed-out slices without waiting for wall time (FramePacer.h) and
			// ends the process with GpuHang during the render phase.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeHang");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "HangGame", .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const Result<ProcessResult> result =
				RunRenderingRuntime(userData, { "--headless", "--frames", "10", "--gpu-inject-fault", "hang", ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Crash);
			const Result<std::string> report = ReadGameCrashReport(userData, "HangGame");
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (GpuHang)"));
			CHECK(report->contains("FramePhase: Render"));
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}
	}

}
