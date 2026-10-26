#include "TestsPCH.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Automation/Protocol/Framing.h"
#include "Engine/Automation/Protocol/SessionFile.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Graphics/GraphicsSpecification.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/Socket.h"
#include "Support/ChildOutput.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/PlatformProbes.h"
#include "Support/TempDirectory.h"
#include "Support/TestGame.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"
#include "Support/WaitUntil.h"

#include <nlohmann/json.hpp>

#include <array>
#include <optional>

// The runtime executable as a whole process (Architecture §14.3; Roadmap M2, M6, M7) on test games written like exported
// ones (Support/TestGame.h) and given with --manifest (Docs/Decisions/0012-m7-decisions.md decision 10). Runs that need no
// GPU pass --renderer none; the Vulkan renderer is covered in the GPU suite. Runs get --user-data-dir, except the
// feature-test fallback regression whose Testing:true manifest confines user data beside its temporary export.

namespace Engine {

	namespace {

		// The Runtime with `arguments`, its user data in `userData`; a windowed run gets --audio-device none
		// (Test::WithoutAudioDevice). `workingDirectory` empty: the test process's. A --feature-test run writes its reports
		// below its working directory (ADR 0022 decision 7), so such a run passes a temporary one.
		Result<ProcessResult> RunRuntime(const Test::TempDirectory& userData, std::vector<std::string> arguments, std::filesystem::path workingDirectory = {})
		{
			ENGINE_TRY_ASSIGN(std::filesystem::path runtime, Test::GetBuiltExecutablePath("Runtime"));
			arguments = Test::WithoutAudioDevice(std::move(arguments));
			arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
			return Process::Run({ .Executable = std::move(runtime), .Arguments = std::move(arguments), .WorkingDirectory = std::move(workingDirectory) },
				std::chrono::seconds(120));
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

		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decisions 40 to 46): the pixels of the tick-10 screenshot of a
		// headless run of the game at `manifest` with --gpu-inject-fault=`fault` (no fault when empty), written to
		// <userData>/Shot-<fault or "none">.png; the run must exit 0 with --expect-no-errors. Its standard error goes to
		// `standardError`.
		std::vector<std::byte> RunFaultScreenshot(const Test::TempDirectory& userData, const std::filesystem::path& manifest, std::string_view fault,
			std::string& standardError)
		{
			CAPTURE(fault);
			const std::filesystem::path png = userData / std::format("Shot-{}.png", fault.empty() ? "none" : fault);
			std::vector<std::string> arguments = { "--headless", "--frames", "12", "--expect-no-errors",
				"--screenshot-at=10:" + Test::PathToUtf8(png), ManifestArgument(manifest) };
			if (!fault.empty())
				arguments.push_back(std::format("--gpu-inject-fault={}", fault));
			const Result<ProcessResult> result = RunRenderingRuntime(userData, std::move(arguments));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			standardError = result->StandardError;
			const Result<Image> image = ReadPng(png);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			return image->Pixels;
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

#if defined(ENGINE_PLATFORM_WINDOWS)
		// A JSON-RPC client of a Runtime's automation server (--automation) over its loopback port, as Tools/Automation/
		// engine_client.py speaks it (Architecture §13.2): the framing, session.hello with the token of its session file,
		// then one request at a time, skipping notifications.
		class RuntimeAutomationConnection
		{
		public:
			static constexpr std::chrono::seconds Timeout{ 60 };

			// Connects to the server whose session file <sessionsDirectory>/<pid>.json exists, and says hello.
			[[nodiscard]] static Result<RuntimeAutomationConnection> Open(const std::filesystem::path& sessionsDirectory, uint32_t pid)
			{
				ENGINE_TRY_ASSIGN(const std::string text, FileSystem::ReadText(SessionFile::GetPath(sessionsDirectory, pid)));
				ENGINE_TRY_ASSIGN(const SessionFileContent session, SessionFile::FromText(text));
				ENGINE_TRY_ASSIGN(Socket socket, Socket::Connect(session.Port, Timeout));
				RuntimeAutomationConnection connection(std::move(socket));
				const Json client{ { "name", "engine-tests" }, { "version", "1" } };
				ENGINE_TRY(connection.Call("session.hello", Json{ { "token", session.Token }, { "protocolVersion", "1.0" }, { "client", client } }));
				return connection;
			}

			// The result of `method`, or an Error carrying the response.
			[[nodiscard]] Result<Json> Call(std::string_view method, const Json& params)
			{
				const int64_t id = m_NextId++;
				const std::string frame = EncodeFrame(Json{ { "jsonrpc", "2.0" }, { "id", id }, { "method", method }, { "params", params } }.dump());
				ENGINE_TRY(m_Socket.Send(std::as_bytes(std::span(frame.data(), frame.size())), Timeout));
				while (true)
				{
					ENGINE_TRY_ASSIGN(const Json message, Receive());
					const JsonReader reader(message);
					if (reader.ReadMember<int64_t>("id").value_or(0) != id)
						continue; // a notification
					if (const std::optional<JsonReader> result = reader.FindMember("result"))
						return result->GetValue();
					return MakeError(ErrorCode::InvalidState, "{} failed: {}", method, message.dump());
				}
			}
		private:
			explicit RuntimeAutomationConnection(Socket socket)
				: m_Socket(std::move(socket))
			{
			}

			[[nodiscard]] Result<Json> Receive()
			{
				while (true)
				{
					ENGINE_TRY_ASSIGN(const std::optional<std::string> payload, m_Decoder.Next());
					if (payload.has_value())
						return JsonReader::Parse(*payload);
					std::array<std::byte, 4096> buffer{};
					ENGINE_TRY_ASSIGN(const size_t received, m_Socket.Receive(buffer, Timeout));
					if (received == 0)
						return MakeError(ErrorCode::Io, "the Runtime closed the automation connection");
					m_Decoder.Append(std::span(buffer.data(), received));
				}
			}

			Socket m_Socket;
			FrameDecoder m_Decoder{};
			int64_t m_NextId = 1;
		};
#endif

		// Replaces the first `from` in `path`'s text with `to`.
		void EditFile(const std::filesystem::path& path, std::string_view from, std::string_view to)
		{
			Result<std::string> text = FileSystem::ReadText(path);
			REQUIRE_MESSAGE(text.has_value(), (text ? "" : text.error().ToString()));
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
				{ { "--filter", "one" }, "--filter" },
				{ { "--verify" }, "--verify" },
				{ { "--replay=" }, "--replay" },
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

		TEST_CASE("RuntimeApp: feature-test stays inert for an ordinary exported game")
		{
			Test::TempDirectory userData("RuntimeTestGate");
			const auto manifest = Test::WriteTestGame(userData / "Game");
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const auto result = RunRuntime(userData, { "--headless", "--renderer", "none", "--feature-test", ManifestArgument(*manifest) }, userData.GetPath());
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK(result->ExitCode == ExitCode::UsageError);
			CHECK(result->StandardError.contains("--feature-test requires a testing export"));
		}

		TEST_CASE("RuntimeApp: feature-test user data stays beside its manifest unless explicitly overridden")
		{
			for (const bool explicitRoot : { false, true })
			{
				CAPTURE(explicitRoot);
				Test::TempDirectory directory("RuntimeTestUserData");
				Test::TestGameSpecification specification;
				specification.Name = "IsolatedTests";
				const auto manifest = Test::WriteTestGame(directory / "Game", specification);
				REQUIRE_MESSAGE(manifest.has_value(), (manifest ? "" : manifest.error().ToString()));
				EditFile(*manifest, "\"Testing\": false", "\"Testing\": true");
				const auto executable = Test::GetBuiltExecutablePath("Runtime");
				REQUIRE_MESSAGE(executable.has_value(), (executable ? "" : executable.error().ToString()));
				std::vector<std::string> arguments = { "--headless", "--renderer", "none", "--feature-test", ManifestArgument(*manifest) };
				if (explicitRoot)
					arguments.push_back("--user-data-dir=" + Test::PathToUtf8(directory / "Explicit"));
				ProcessSpecification process;
				process.Executable = *executable;
				process.Arguments = std::move(arguments);
				process.WorkingDirectory = directory.GetPath();
				const auto result = Process::Run(process, std::chrono::seconds(120));
				REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
				INFO("runtime stderr: ", result->StandardError);
				CHECK(result->ExitCode == ExitCode::Success);
				const auto fallback = directory / "Game/bin/TestUserData/IsolatedTests/Logs/Runtime.log";
				const auto overridePath = directory / "Explicit/IsolatedTests/Logs/Runtime.log";
				std::error_code error;
				CHECK(std::filesystem::is_regular_file(explicitRoot ? overridePath : fallback, error));
				CHECK_FALSE(error);
				CHECK_FALSE(std::filesystem::exists(explicitRoot ? fallback : overridePath, error));
				CHECK_FALSE(error);
			}
		}

		// ADR 0022 decision 7: the exported runner's reports have fixed names, which M15's export stage reads, below the
		// working directory: FeatureTest-Release in development builds (FeatureTest-Dist in Dist).
		TEST_CASE("RuntimeApp: feature-test writes FeatureTest-Release reports below its working directory")
		{
			Test::TempDirectory directory("RuntimeTestReports");
			Test::TestGameSpecification specification;
			specification.Name = "ReportedTests";
			const auto manifest = Test::WriteTestGame(directory / "Game", specification);
			REQUIRE_MESSAGE(manifest.has_value(), (manifest ? "" : manifest.error().ToString()));
			EditFile(*manifest, "\"Testing\": false", "\"Testing\": true");
			const auto executable = Test::GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(executable.has_value(), (executable ? "" : executable.error().ToString()));
			ProcessSpecification process;
			process.Executable = *executable;
			process.Arguments = { "--headless", "--renderer", "none", "--feature-test", ManifestArgument(*manifest), "--user-data-dir=" + Test::PathToUtf8(directory / "UserData") };
			process.WorkingDirectory = directory.GetPath();
			const auto result = Process::Run(process, std::chrono::seconds(120));
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			const auto json = FileSystem::ReadText(directory / "bin/TestResults/FeatureTest-Release.json");
			REQUIRE_MESSAGE(json.has_value(), json.error().ToString());
			CHECK(json->contains(R"("mode": "Release")"));
			CHECK(json->contains(R"("passed": true)"));
			std::error_code error;
			CHECK(std::filesystem::is_regular_file(directory / "bin/TestResults/FeatureTest-Release.xml", error));
			// The development report name of M13 is gone.
			size_t reports = 0;
			for (std::filesystem::directory_iterator entry(directory / "bin/TestResults", error); !error && entry != std::filesystem::directory_iterator(); entry.increment(error))
			{
				CHECK(entry->path().filename().string().starts_with("FeatureTest-Release."));
				++reports;
			}
			CHECK_FALSE(error);
			CHECK(reports == 2);
		}

		// Review finding M01 (ADR 0022 decision 7): a --feature-test run that cannot begin (here its suite's script is not in
		// the pak) still writes its reports, as a failed run naming the error, over a passing report an earlier run left at
		// the fixed paths M15's export stage reads.
		TEST_CASE("RuntimeApp: feature-test writes failed reports over stale ones when the run cannot begin")
		{
			Test::TempDirectory directory("RuntimeTestReportsBegin");
			Test::TestGameSpecification specification;
			specification.Name = "UnstartableTests";
			specification.TestSuiteScripts = { "Assets/Tests/Missing.test.luau" };
			const auto manifest = Test::WriteTestGame(directory / "Game", specification);
			REQUIRE_MESSAGE(manifest.has_value(), (manifest ? "" : manifest.error().ToString()));
			EditFile(*manifest, "\"Testing\": false", "\"Testing\": true");
			const std::filesystem::path reports = directory / "bin/TestResults";
			REQUIRE(FileSystem::CreateDirectories(reports));
			const std::string stale = R"({"mode": "Release", "passed": true, "stale": true})";
			REQUIRE(FileSystem::WriteFileAtomic(reports / "FeatureTest-Release.json", std::as_bytes(std::span(stale.data(), stale.size()))));
			REQUIRE(FileSystem::WriteFileAtomic(reports / "FeatureTest-Release.xml", std::as_bytes(std::span(stale.data(), stale.size()))));
			const auto result = RunRuntime(directory, { "--headless", "--renderer", "none", "--feature-test", ManifestArgument(*manifest) }, directory.GetPath());
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode != ExitCode::Success);
			const auto json = FileSystem::ReadText(reports / "FeatureTest-Release.json");
			REQUIRE_MESSAGE(json.has_value(), json.error().ToString());
			CHECK_FALSE(json->contains("stale"));
			CHECK(json->contains(R"("mode": "Release")"));
			CHECK(json->contains(R"("passed": false)"));
			CHECK(json->contains("Missing.test.luau"));
			const auto junit = FileSystem::ReadText(reports / "FeatureTest-Release.xml");
			REQUIRE_MESSAGE(junit.has_value(), junit.error().ToString());
			CHECK(junit->contains("<error message="));
			CHECK(junit->contains("Missing.test.luau"));
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

		TEST_CASE("RuntimeApp: --gpu-inject-fault=device-lost-unrecoverable exits 4 with a crash report"
			* doctest::test_suite(Test::GpuSuite))
		{
			// §8.14 item 8: the device is reported lost after a submission, and its recovery fails, which ends the process
			// through GraphicsDevice::RaiseDeviceLost. From M18 a plain device loss is recovered
			// (Docs/Decisions/0029-m18-contract.md decision 30; "RuntimeApp: --gpu-inject-fault=device-lost recovers the
			// device and writes the same screenshot").
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeDeviceLost");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "DeviceLostGame", .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const Result<ProcessResult> result =
				RunRenderingRuntime(userData, { "--headless", "--frames", "10", "--gpu-inject-fault", "device-lost-unrecoverable", ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Crash);
			const Result<std::string> report = ReadGameCrashReport(userData, "DeviceLostGame");
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (DeviceLost)"));
			CHECK(report->contains("device lost"));
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("RuntimeApp: --gpu-inject-fault=device-lost recovers the device and writes the same screenshot"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, Docs/Decisions/0029-m18-contract.md decisions 29 and 30): the loss strikes the second
			// frame and is recovered, the run goes on to its end, and the screenshot of tick 10 equals a run's without the loss.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeDeviceLostRecovered");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "RecoveredGame", .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			std::vector<std::vector<std::byte>> images;
			for (const bool lose : { false, true })
			{
				CAPTURE(lose);
				const std::filesystem::path png = userData / (lose ? "Lost.png" : "Kept.png");
				std::vector<std::string> arguments = { "--headless", "--frames", "12", "--expect-no-errors",
					"--screenshot-at=10:" + Test::PathToUtf8(png), ManifestArgument(*manifest) };
				if (lose)
				{
					arguments.push_back("--gpu-inject-fault");
					arguments.push_back("device-lost");
				}
				const Result<ProcessResult> result = RunRenderingRuntime(userData, std::move(arguments));
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				INFO("runtime stderr: ", result->StandardError);
				CHECK(result->ExitCode == ExitCode::Success);
				CHECK(result->StandardError.contains("GPU device recovered") == lose);
				const Result<Image> image = ReadPng(png);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				images.push_back(image->Pixels);
			}
			CHECK(images[1] == images[0]);
		}

		TEST_CASE("RuntimeApp: --gpu-inject-fault=hang-reset recovers and writes the same screenshot"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, Docs/Decisions/0029-m18-contract.md decisions 40 and 46): a hang the driver resets is
			// recovered like a loss, and the screenshot of tick 10 equals a run's without the fault; --expect-no-errors holds,
			// since the recovered hang logs warnings only.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeHangReset");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "HangResetGame", .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			std::string plainOutput;
			const std::vector<std::byte> plain = RunFaultScreenshot(userData, *manifest, "", plainOutput);
			std::string resetOutput;
			const std::vector<std::byte> reset = RunFaultScreenshot(userData, *manifest, "hang-reset", resetOutput);
			CHECK_FALSE(plainOutput.contains("GPU device recovered"));
			CHECK(resetOutput.contains("GPU device recovered"));
			CHECK(reset == plain);
		}

		TEST_CASE("RuntimeApp: --gpu-inject-fault=oom-rendertarget recovers and writes the same screenshot"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, decisions 42, 44 and 46): an out-of-memory request after startup (the capture's new size)
			// is retried after a trim, and the screenshot of tick 10 equals a run's without the fault; --expect-no-errors holds,
			// since the recovered request logs warnings only.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeOomRenderTarget");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "OomRetriedGame", .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			std::string plainOutput;
			const std::vector<std::byte> plain = RunFaultScreenshot(userData, *manifest, "", plainOutput);
			std::string retriedOutput;
			const std::vector<std::byte> retried = RunFaultScreenshot(userData, *manifest, "oom-rendertarget", retriedOutput);
			CHECK_FALSE(plainOutput.contains("injected by --gpu-inject-fault"));
			// The fault struck a request after startup, whose retry after its trim succeeded.
			CHECK(retriedOutput.contains("injected by --gpu-inject-fault=oom-rendertarget"));
			CHECK_FALSE(retriedOutput.contains("GPU device recovered"));
			CHECK(retried == plain);
		}

		TEST_CASE("RuntimeApp: --gpu-inject-fault=hang-reset-persistent recovers every hang and runs to the end"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, decisions 40 and 46): every bounded wait after the first frame hangs and is reset, the
			// recovered device's included, and every loss is recovered: the run reaches its last frame and exits 0.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeHangResetPersistent");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "HangResetGame", .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const Result<ProcessResult> result = RunRenderingRuntime(userData,
				{ "--headless", "--frames", "20", "--expect-no-errors", "--gpu-inject-fault", "hang-reset-persistent", ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			size_t recoveries = 0;
			for (size_t at = result->StandardError.find("GPU device recovered"); at != std::string::npos;
				at = result->StandardError.find("GPU device recovered", at + 1))
				++recoveries;
			CHECK(recoveries >= 2);
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("RuntimeApp: --gpu-inject-fault=oom-rendertarget-persistent fails only the screenshot, with exit code 1"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, decisions 42, 44 and 45): "a persistent one fails only its request". The capture's
			// targets run out of memory again after the trim, so --screenshot-at writes nothing and fails the run with
			// OutOfMemory (exit code 1), while the game runs to its last frame: no crash, no crash report.
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeOomRenderTargetPersistent");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "OomGame", .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const std::filesystem::path png = userData / "Shots/Tick10.png";
			const Result<ProcessResult> result = RunRenderingRuntime(userData, { "--headless", "--frames", "12", "--screenshot-at=10:" + Test::PathToUtf8(png), "--gpu-inject-fault", "oom-rendertarget-persistent", ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Failed);
			CHECK(result->StandardError.contains("OutOfMemory"));
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(png, error));
			CHECK_FALSE(ReadGameCrashReport(userData, "OomGame").has_value());
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("RuntimeApp: out-of-memory at startup still exits 4 with a crash report"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.4, decisions 42 and 46): ENGINE_GPU_FAULT_AT_STARTUP arms oom-pipeline-persistent at the
			// device's creation, so the game's startup pipelines run out of memory, which stays FatalError(OutOfMemory).
			if (!Test::ProbeGpuForProcess())
				return;
			Test::TempDirectory userData("RuntimeOomAtStartup");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "OomStartupGame", .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const Result<std::filesystem::path> runtime = Test::GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
			std::vector<std::string> arguments = { "--headless", "--frames", "10", "--gpu-inject-fault", "oom-pipeline-persistent", ManifestArgument(*manifest) };
			const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			arguments = Test::WithoutAudioDevice(std::move(arguments));
			arguments.push_back("--user-data-dir=" + Test::PathToUtf8(userData.GetPath()));
			const Result<ProcessResult> result = Process::Run({ .Executable = *runtime, .Arguments = std::move(arguments), .Environment = { { std::string(GpuFaultAtStartupVariable), "1" } } },
				std::chrono::seconds(120));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Crash);
			const Result<std::string> report = ReadGameCrashReport(userData, "OomStartupGame");
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (OutOfMemory)"));
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
		}

		TEST_CASE("RuntimeApp: F1 toggles the stats overlay in a windowed run and the game still receives F1"
			* doctest::test_suite(Test::GpuSuite))
		{
			// Docs/Decisions/0024-m15-contract.md decision 17: a windowed Debug or Release Runtime enables ImGui for the overlay,
			// each F1 press that is not a repeat toggles it, and the key still reaches the game (OnEvent consumes nothing). The
			// keystrokes are real window messages, which only the Windows probe can post (Support/PlatformProbes.h); the
			// paused session (--paused) applies each one at the tick that play.step runs, and script.eval reads the game's
			// Input there.
			if (!Test::ProbeGpuForProcess())
				return;
#if defined(ENGINE_PLATFORM_WINDOWS)
			Test::TempDirectory userData("RuntimeOverlay");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "OverlayGame", .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			Result<std::filesystem::path> runtime = Test::GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
			std::vector<std::string> arguments = Test::WithoutAudioDevice({ "--automation", "--paused", ManifestArgument(*manifest),
				"--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) });
			const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			Result<Process> process = Process::Spawn({ .Executable = std::move(*runtime), .Arguments = std::move(arguments) });
			REQUIRE_MESSAGE(process.has_value(), process.error().ToString());
			const uint32_t pid = process->GetId();

			const std::filesystem::path sessions = userData / "OverlayGame" / "Automation" / "Sessions";
			REQUIRE(Test::WaitUntil([&sessions, pid]()
			{
				return FileSystem::Exists(SessionFile::GetPath(sessions, pid));
			}));
			Result<RuntimeAutomationConnection> connection = RuntimeAutomationConnection::Open(sessions, pid);
			REQUIRE_MESSAGE(connection.has_value(), connection.error().ToString());

			// One keystroke message (a press or a release of F1) to the Runtime's window.
			const auto postF1 = [pid](bool pressed)
			{
				const Status posted = Test::PostKeyToProcessWindow(pid, Key::F1, pressed);
				REQUIRE_MESSAGE(posted.has_value(), posted.error().ToString());
			};
			// One tick, then whether the game holds F1 down.
			const auto stepAndReadF1 = [&connection]() -> bool
			{
				const Result<Json> stepped = connection->Call("play.step", Json{ { "ticks", 1 } });
				REQUIRE_MESSAGE(stepped.has_value(), stepped.error().ToString());
				const Result<Json> evaluated = connection->Call("script.eval", Json{ { "code", "return Input.IsKeyDown(\"F1\")" }, { "context", "Play" } });
				REQUIRE_MESSAGE(evaluated.has_value(), evaluated.error().ToString());
				return JsonReader(*evaluated).ReadMember<bool>("value").value_or(false);
			};

			// The Runtime configured the overlay's ImGui context so that it never changes the OS cursor (the game's cursor
			// mode holds as in Dist; the M15 review's M33). The OS cursor of another process's window cannot be read back,
			// so the Runtime's line after ConfigureStatsOverlayContext is the observable.
			REQUIRE(process->WaitForOutput(ProcessStream::StandardError, "Runtime: stats overlay ready (F1); its ImGui context leaves the OS cursor to the game",
				RuntimeAutomationConnection::Timeout));

			// Every keystroke is waited for in the Runtime's output before the tick that must see it: posting is
			// asynchronous, and a play.step read in the same frame as an unpolled message would tick without it (the M15
			// review's M28). Each line is logged where OnEvent hands the key to the game's input queue.
			CHECK_FALSE(stepAndReadF1());
			// The first press shows the overlay, and the game receives it.
			postF1(true);
			REQUIRE(process->WaitForOutput(ProcessStream::StandardError, "Runtime: stats overlay shown (F1)", RuntimeAutomationConnection::Timeout));
			CHECK(stepAndReadF1());
			// A release reaches the game and toggles nothing; the next press hides the overlay, and the game receives it too.
			postF1(false);
			REQUIRE(process->WaitForOutput(ProcessStream::StandardError, "Runtime: F1 released (stats overlay shown)", RuntimeAutomationConnection::Timeout));
			CHECK_FALSE(stepAndReadF1());
			postF1(true);
			REQUIRE(process->WaitForOutput(ProcessStream::StandardError, "Runtime: stats overlay hidden (F1)", RuntimeAutomationConnection::Timeout));
			CHECK(stepAndReadF1());
			postF1(false);
			REQUIRE(process->WaitForOutput(ProcessStream::StandardError, "Runtime: F1 released (stats overlay hidden)", RuntimeAutomationConnection::Timeout));
			CHECK_FALSE(stepAndReadF1());

			REQUIRE(connection->Call("session.shutdown", Json::object()).has_value());
			const Result<ProcessResult> result = process->Wait(std::chrono::seconds(120));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			// One line per toggle, and nothing a clean run must not print (validation messages included).
			const auto count = [&result](std::string_view text)
			{
				size_t found = 0;
				for (size_t position = result->StandardError.find(text); position != std::string::npos; position = result->StandardError.find(text, position + 1))
					++found;
				return found;
			};
			CHECK(count("Runtime: stats overlay ready (F1)") == 1);
			CHECK(count("Runtime: stats overlay shown (F1)") == 1);
			CHECK(count("Runtime: stats overlay hidden (F1)") == 1);
			CHECK(count("Runtime: F1 released") == 2);
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());
#else
			MESSAGE("RuntimeApp F1 overlay: posting a real keystroke to another process's window is implemented on Windows only "
					"(Test::PostKeyToProcessWindow); the overlay's toggle, drawing and cursor rule are covered by the StatsOverlay cases");
#endif
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
