#include "TestsPCH.h"

#include "Engine/App/DisplaySettingsFile.h"
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
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

			// Connects to the server whose session file <sessionsDirectory>/<pid>.json exists, and says hello. The file is read
			// as every client reads it (SessionFile::Read), since the rename that published it may still be in flight.
			[[nodiscard]] static Result<RuntimeAutomationConnection> Open(const std::filesystem::path& sessionsDirectory, uint32_t pid)
			{
				ENGINE_TRY_ASSIGN(const SessionFileContent session, SessionFile::Read(sessionsDirectory, pid));
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

		// The Runtime of the game `name` (its manifest at `manifest`) with `arguments`, --automation and --paused, its user
		// data in `userData` and no audio device: evaluates each of `codes` in the play session (script.eval), then ends the
		// run with session.shutdown and checks that it exits with Success. Returns the values; its standard error goes to
		// `standardError`.
		std::vector<Json> EvaluateInRuntime(const Test::TempDirectory& userData, std::string_view name, const std::filesystem::path& manifest,
			std::vector<std::string> arguments, std::span<const std::string_view> codes, std::string& standardError)
		{
			Result<std::filesystem::path> runtime = Test::GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
			arguments.insert(arguments.end(), { "--automation", "--paused", ManifestArgument(manifest), "--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) });
			Result<Process> process = Process::Spawn({ .Executable = std::move(*runtime), .Arguments = Test::WithoutAudioDevice(std::move(arguments)) });
			REQUIRE_MESSAGE(process.has_value(), process.error().ToString());
			const uint32_t pid = process->GetId();
			const std::filesystem::path sessions = userData / std::string(name) / "Automation" / "Sessions";
			REQUIRE(Test::WaitUntil([&sessions, pid]()
			{
				return FileSystem::Exists(SessionFile::GetPath(sessions, pid));
			}));
			Result<RuntimeAutomationConnection> connection = RuntimeAutomationConnection::Open(sessions, pid);
			REQUIRE_MESSAGE(connection.has_value(), connection.error().ToString());
			std::vector<Json> values;
			for (const std::string_view code : codes)
			{
				const Result<Json> evaluated = connection->Call("script.eval", Json{ { "code", code }, { "context", "Play" } });
				REQUIRE_MESSAGE(evaluated.has_value(), evaluated.error().ToString());
				const std::optional<JsonReader> value = JsonReader(*evaluated).FindMember("value");
				values.push_back(value.has_value() ? value->GetValue() : Json());
			}
			REQUIRE(connection->Call("session.shutdown", Json::object()).has_value());
			const Result<ProcessResult> result = process->Wait(std::chrono::seconds(120));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			standardError = result->StandardError;
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			return values;
		}
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
				// M38 (Docs/Decisions/0055-m38-runtime-locale.md decision 8): --locale takes a tag.
				{ { "--locale", "de_DE" }, "--locale" },
				{ { "--locale=" }, "--locale" },
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

		// M38 (Docs/Decisions/0055-m38-runtime-locale.md decision 8; Roadmap M38's acceptance): --locale starts the live session in
		// its tag (the start line names the session's locale) and, with --feature-test, every suite whose entry names no locale,
		// while an entry's own Locale still wins; --replay refuses it, since a replay starts in its header's locale.
		TEST_CASE("RuntimeApp: --locale starts the sessions and the suites that name no locale in its tag, and --replay refuses it")
		{
			Test::TempDirectory directory("RuntimeLocale");
			Test::TestGameSpecification specification;
			specification.Name = "LocaleGame";
			specification.Locale = "fr";
			specification.Scripts = { { .Handle = AssetHandle(0x38C0001), .Path = "Assets/Tests/Locale.test.luau", .Source = R"(return Test.Suite("Locale", function()
	Test.Case("starts in its locale", function()
		Test.ExpectEqual(Loc.GetLocale(), Scene.GetLoadParameters().Expected)
	end)
end)
)" } };
			TestSuiteSettings inherits;
			inherits.Script = "Assets/Tests/Locale.test.luau";
			inherits.Parameters = VariantValue(Json{ { "Expected", "ar" } });
			TestSuiteSettings german = inherits;
			german.Locale = "de";
			german.Parameters = VariantValue(Json{ { "Expected", "de" } });
			specification.TestSuites = { inherits, german };
			const auto manifest = Test::WriteTestGame(directory / "Game", specification);
			REQUIRE_MESSAGE(manifest.has_value(), (manifest ? "" : manifest.error().ToString()));

			// A live session.
			const Result<ProcessResult> live = RunRuntime(directory, { "--headless", "--renderer", "none", "--frames", "2", "--locale", "ar", ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(live.has_value(), live.error().ToString());
			INFO("runtime stderr: ", live->StandardError);
			CHECK(live->ExitCode == ExitCode::Success);
			CHECK(live->StandardError.contains("Runtime: 'LocaleGame' started with scene 'Assets/Scenes/Main.scene' (3 entities, seed 7, locale 'ar')"));
			const Result<ProcessResult> fallback = RunRuntime(directory, { "--headless", "--renderer", "none", "--frames", "2", ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(fallback.has_value(), fallback.error().ToString());
			CHECK(fallback->StandardError.contains("locale 'fr')"));

			// The testing export's suites: the entry without a locale runs in --locale's, the German entry in its own.
			EditFile(*manifest, "\"Testing\": false", "\"Testing\": true");
			const Result<ProcessResult> suites = RunRuntime(directory, { "--headless", "--renderer", "none", "--feature-test", "--locale", "ar", ManifestArgument(*manifest) }, directory.GetPath());
			REQUIRE_MESSAGE(suites.has_value(), suites.error().ToString());
			INFO("feature-test stderr: ", suites->StandardError);
			CHECK(suites->ExitCode == ExitCode::Success);
			const auto json = FileSystem::ReadText(directory / "bin/TestResults/FeatureTest-Release.json");
			REQUIRE_MESSAGE(json.has_value(), json.error().ToString());
			const auto report = JsonReader::Parse(*json);
			REQUIRE(report.has_value());
			CHECK((*report)["passed"] == Json(true));
			REQUIRE((*report)["suites"].size() == 2);
			CHECK((*report)["suites"][0]["suite"] == Json("Locale"));
			CHECK((*report)["suites"][0]["locale"] == Json("ar"));
			CHECK((*report)["suites"][1]["suite"] == Json("Locale (de)"));
			CHECK((*report)["suites"][1]["locale"] == Json("de"));

			// --replay owns its locale.
			const Result<ProcessResult> replay = RunRuntime(directory, { "--headless", "--renderer", "none", "--replay", "Recorded.replay", "--locale", "ar", ManifestArgument(*manifest) });
			REQUIRE_MESSAGE(replay.has_value(), replay.error().ToString());
			CHECK(replay->ExitCode == ExitCode::UsageError);
			CHECK(replay->StandardError.contains("--locale"));
		}

		// M38 (Docs/Decisions/0055-m38-runtime-locale.md decisions 3, 5 and 8; the W38.1 review's finding 2): in the Runtime's
		// automation subset, a recording that names no locale starts in the launch locale (--locale), one that names a locale in
		// it, and the header records the start locale; input.replay starts its playback in the header's locale, neither the
		// launch locale nor the project's.
		TEST_CASE("RuntimeApp: a Runtime recording starts in --locale unless it names one, and input.replay plays in its header's locale")
		{
#if defined(ENGINE_PLATFORM_WINDOWS)
			Test::TempDirectory userData("RuntimeLocaleRecord");
			Test::TestGameSpecification specification;
			specification.Name = "LocaleRecordGame";
			specification.Locale = "fr";
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", specification);
			REQUIRE_MESSAGE(manifest.has_value(), (manifest ? "" : manifest.error().ToString()));
			Result<std::filesystem::path> runtime = Test::GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
			Result<Process> process = Process::Spawn({ .Executable = std::move(*runtime),
				.Arguments = Test::WithoutAudioDevice({ "--headless", "--renderer", "none", "--automation", "--paused", "--locale", "de", ManifestArgument(*manifest),
					"--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) }) });
			REQUIRE_MESSAGE(process.has_value(), process.error().ToString());
			const uint32_t pid = process->GetId();
			const std::filesystem::path game = userData / "LocaleRecordGame";
			const std::filesystem::path sessions = game / "Automation" / "Sessions";
			REQUIRE(Test::WaitUntil([&sessions, pid]()
			{
				return FileSystem::Exists(SessionFile::GetPath(sessions, pid));
			}));
			Result<RuntimeAutomationConnection> connection = RuntimeAutomationConnection::Open(sessions, pid);
			REQUIRE_MESSAGE(connection.has_value(), connection.error().ToString());
			const auto playStateLocale = [&connection]()
			{
				const Result<Json> state = connection->Call("play.state", Json::object());
				REQUIRE_MESSAGE(state.has_value(), state.error().ToString());
				return JsonReader(*state).ReadMember<std::string>("locale").value_or("<none>");
			};
			// Records two ticks from a restart with `start`'s members and returns the written header's Locale.
			const auto record = [&connection, &game, &playStateLocale](Json start, std::string_view file, std::string_view sessionLocale)
			{
				start["action"] = "Start";
				start["restart"] = true;
				const Result<Json> started = connection->Call("input.record", start);
				REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
				CHECK(playStateLocale() == sessionLocale);
				REQUIRE(connection->Call("play.step", Json{ { "ticks", 2 }, { "render", "none" } }).has_value());
				const Result<Json> stopped = connection->Call("input.record", Json{ { "action", "Stop" }, { "path", file } });
				REQUIRE_MESSAGE(stopped.has_value(), stopped.error().ToString());
				const Result<std::string> text = FileSystem::ReadText(game / "Replays" / Test::PathFromUtf8(file));
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				const Result<Json> document = JsonReader::Parse(*text);
				REQUIRE(document.has_value());
				return JsonReader(*document).ReadMember<std::string>("Locale").value_or("<null>");
			};
			// No locale named: the launch locale, not the project's fr.
			CHECK(record(Json::object(), "Inherited.replay", "de") == "de");
			// A named locale wins over the launch locale.
			CHECK(record(Json{ { "locale", "ar" } }, "Arabic.replay", "ar") == "ar");

			// The Arabic replay plays in its header's locale: a session in de or fr would be refused at its start.
			const Result<Json> replayed = connection->Call("input.replay", Json{ { "path", "user://Replays/Arabic.replay" }, { "verify", true }, { "strictHash", true } });
			REQUIRE_MESSAGE(replayed.has_value(), replayed.error().ToString());
			CHECK((*replayed)["passed"] == Json(true));
			CHECK(playStateLocale() == "ar");

			REQUIRE(connection->Call("session.shutdown", Json::object()).has_value());
			const Result<ProcessResult> result = process->Wait(std::chrono::seconds(120));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
#else
			MESSAGE("RuntimeApp locale recording: the Runtime's automation client in this file is implemented on Windows only "
					"(RuntimeAutomationConnection); the editor's recordings are covered by the ReplayMethods case");
#endif
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

		TEST_CASE("RuntimeApp: a windowed run restores Display.json, logs an invalid one in the game's log and saves an applied request")
		{
			// M28 (Docs/Decisions/0043-m28-game-services.md decision 3; the W28.1 review's findings 2 and 6). The factory reads
			// user://Display.json before logging exists, so an invalid file's problem reaches the game's log only from
			// OnInitialize; a headless run never reads it. A valid file makes the first window in the chosen settings, which a
			// script reads back, and a request the frame boundary applies is written back. Windowed runs without a renderer, on
			// Windows, whose display backend alone exists (Docs/Decisions/0028-backlog-program.md decision 10).
#if defined(ENGINE_PLATFORM_WINDOWS)
			const auto writeDisplay = [](const std::filesystem::path& gameData, std::string_view text)
			{
				REQUIRE(FileSystem::CreateDirectories(gameData).has_value());
				REQUIRE(FileSystem::WriteFileAtomic(gameData / "Display.json", std::as_bytes(std::span(text.data(), text.size()))).has_value());
			};
			const std::string_view invalid = "{\"Format\": \"DisplaySettings\", \"Version\": 1, \"WindowMode\": ";
			{
				Test::TempDirectory userData("RuntimeDisplayHeadless");
				const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "DisplayGame" });
				REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
				writeDisplay(userData / "DisplayGame", invalid);
				const Result<ProcessResult> result = RunRuntime(userData, { "--headless", "--renderer", "none", "--frames", "3", ManifestArgument(*manifest) });
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				INFO("runtime stderr: ", result->StandardError);
				CHECK(result->ExitCode == ExitCode::Success);
				CHECK_FALSE(result->StandardError.contains("Display.json"));
			}
			{
				Test::TempDirectory userData("RuntimeDisplayInvalid");
				const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "DisplayGame" });
				REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
				writeDisplay(userData / "DisplayGame", invalid);
				const Result<ProcessResult> result = RunRuntime(userData, { "--renderer", "none", "--frames", "3", ManifestArgument(*manifest) });
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				INFO("runtime stderr: ", result->StandardError);
				CHECK(result->ExitCode == ExitCode::Success);
				const Result<std::string> log = FileSystem::ReadText(userData / "DisplayGame/Logs/Runtime.log");
				REQUIRE_MESSAGE(log.has_value(), log.error().ToString());
				// The problem's line, at Warn level, in the game's own log.
				const size_t found = log->find("user://Display.json");
				REQUIRE(found != std::string::npos);
				const size_t lineStart = log->find_last_of('\n', found);
				const size_t start = lineStart == std::string::npos ? 0 : lineStart + 1;
				CHECK(std::string_view(*log).substr(start, found - start).contains("[warning]"));
			}

			Test::TempDirectory userData("RuntimeDisplay");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "DisplayGame" });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const std::filesystem::path display = userData / "DisplayGame/Display.json";
			writeDisplay(userData / "DisplayGame",
				"{\"Format\": \"DisplaySettings\", \"Version\": 1, \"WindowMode\": \"Windowed\", \"Width\": 200, \"Height\": 150, \"RefreshRate\": 0, \"VSync\": false}");
			Result<std::filesystem::path> runtime = Test::GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
			Result<Process> process = Process::Spawn({ .Executable = std::move(*runtime),
				.Arguments = Test::WithoutAudioDevice({ "--renderer", "none", "--automation", "--paused", ManifestArgument(*manifest),
					"--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) }) });
			REQUIRE_MESSAGE(process.has_value(), process.error().ToString());
			const uint32_t pid = process->GetId();
			const std::filesystem::path sessions = userData / "DisplayGame" / "Automation" / "Sessions";
			REQUIRE(Test::WaitUntil([&sessions, pid]()
			{
				return FileSystem::Exists(SessionFile::GetPath(sessions, pid));
			}));
			Result<RuntimeAutomationConnection> connection = RuntimeAutomationConnection::Open(sessions, pid);
			REQUIRE_MESSAGE(connection.has_value(), connection.error().ToString());
			const auto evaluate = [&connection](std::string_view code) -> Json
			{
				const Result<Json> evaluated = connection->Call("script.eval", Json{ { "code", code }, { "context", "Play" } });
				REQUIRE_MESSAGE(evaluated.has_value(), evaluated.error().ToString());
				const std::optional<JsonReader> value = JsonReader(*evaluated).FindMember("value");
				return value.has_value() ? value->GetValue() : Json();
			};

			// The project's window is 160 x 90 with VSync; the restored file's settings are in force.
			CHECK(evaluate("local s = Application.GetDisplaySettings() return string.format('%s %dx%d %s', s.WindowMode, s.Width, s.Height, tostring(s.VSync))")
				== Json("Windowed 200x150 false"));
			// A request applies at the next frame boundary, and what was applied is written back in that boundary, so once the
			// window shows the new size the file holds it. The file is read only then: a reader polling it would hold it open
			// while the Runtime renames the new file over it, and that write would fail.
			CHECK(evaluate("local accepted = Application.SetDisplaySettings({ Width = 240, Height = 180 }) return accepted") == Json(true));
			CHECK(Test::WaitUntil([&evaluate]()
			{
				return evaluate("local s = Application.GetDisplaySettings() return s.Width == 240 and s.Height == 180") == Json(true);
			}));
			const Result<std::string> written = FileSystem::ReadText(display);
			REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
			CHECK(ParseDisplaySettingsFile(*written).value_or(DisplaySettings{})
				== DisplaySettings{ .WindowMode = DisplayWindowMode::Windowed, .Width = 240, .Height = 180, .RefreshRate = 0, .VSync = false });

			REQUIRE(connection->Call("session.shutdown", Json::object()).has_value());
			const Result<ProcessResult> result = process->Wait(std::chrono::seconds(120));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(Test::FindProblemLogLines(result->StandardError).empty());
#else
			MESSAGE("RuntimeApp display settings: the display backend is Windows-only (Docs/Decisions/0028-backlog-program.md decision 10); "
					"the restore's gate is covered by the DisplaySettingsFile cases");
#endif
		}

		TEST_CASE("RuntimeApp: a live run reads the game's store, a headless run never writes it, and a corrupt one waits for a windowed run")
		{
			// M28 (Docs/Decisions/0043-m28-game-services.md decision 8; the W28.2 review's findings 1 and 6): a live session, a
			// headless one included, starts with user://SaveData/Store.json; a headless run (the Dist smoke test, on the player's
			// real user data) never writes it: its Commit returns false and the file keeps its bytes, and a store it cannot read
			// stays where it is, reported to its scripts, so the next windowed run moves it aside and reports it to the player.
			// The windowed run has no renderer; the Runtime's automation is reached on Windows only.
#if defined(ENGINE_PLATFORM_WINDOWS)
			Test::TempDirectory userData("RuntimeSaveData");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "StoreGame" });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			const std::filesystem::path directory = userData / "StoreGame" / "SaveData";
			const std::filesystem::path store = directory / "Store.json";
			const auto writeStore = [&directory, &store](std::string_view text)
			{
				REQUIRE(FileSystem::CreateDirectories(directory).has_value());
				REQUIRE(FileSystem::WriteFileAtomic(store, std::as_bytes(std::span(text.data(), text.size()))).has_value());
			};
			std::string standardError;

			const std::string_view saved = "{\"Format\": \"SaveData\", \"Version\": 1, \"Values\": {\"Realm\": 2}}";
			writeStore(saved);
			const std::array headlessCodes = { std::string_view("return SaveData.Get('Realm')"),
				std::string_view("SaveData.Set('Realm', 3) local committed, reason = SaveData.Commit() return { committed, type(reason) }") };
			const std::vector<Json> headless = EvaluateInRuntime(userData, "StoreGame", *manifest, { "--headless", "--renderer", "none" }, headlessCodes, standardError);
			REQUIRE(headless.size() == 2);
			CHECK(headless[0] == Json(2));
			CHECK(headless[1] == Json::array({ false, "string" }));
			CHECK(FileSystem::ReadText(store).value_or("") == saved);

			const std::string_view truncated = "{\"Format\": \"SaveData\", \"Version\": 1, \"Values\": {\"Realm\"";
			writeStore(truncated);
			const std::array loadErrorCode = { std::string_view("return SaveData.GetLoadError()") };
			const std::vector<Json> left = EvaluateInRuntime(userData, "StoreGame", *manifest, { "--headless", "--renderer", "none" }, loadErrorCode, standardError);
			REQUIRE(left.size() == 1);
			CHECK(left[0].is_string());
			CHECK(FileSystem::ReadText(store).value_or("") == truncated);
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(directory / "Store.corrupt-1.json", error));
			CHECK(standardError.contains("Store.json"));

			const std::vector<Json> moved = EvaluateInRuntime(userData, "StoreGame", *manifest, { "--renderer", "none" }, loadErrorCode, standardError);
			REQUIRE(moved.size() == 1);
			REQUIRE(moved[0].is_string());
			CHECK(moved[0].dump().contains("Store.corrupt-1.json"));
			CHECK(FileSystem::ReadText(directory / "Store.corrupt-1.json").value_or("") == truncated);
			CHECK_FALSE(std::filesystem::exists(store, error));
#else
			MESSAGE("RuntimeApp save data: the Runtime's automation is reached on Windows only (RuntimeAutomationConnection)");
#endif
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
			* doctest::test_suite(Test::GpuSuite))
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
			* doctest::test_suite(Test::GpuSuite))
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
			* doctest::test_suite(Test::GpuSuite))
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
			* doctest::test_suite(Test::GpuSuite))
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

		TEST_CASE("RuntimeApp: a game-view render that runs out of memory warns once, renders again after the trim, and runs without errors"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, decisions 44 and 46): the game view's per-frame render fails alone. After the first frame
			// armed the fault, script.eval adds a shadow-casting spot light, so the next frame's render creates the spot shadow
			// atlas, a render target, which runs out of memory: the Runtime logs one warning for the run of failing frames
			// (never an error: --expect-no-errors holds, exit code 0) and trims after the frame's submission. Under
			// oom-rendertarget the next frame creates the atlas and renders the shadowed spot (stats.get's shadowedSpotLights);
			// under oom-rendertarget-persistent every later frame fails the same way, still with one warning.
			if (!Test::ProbeGpuForProcess())
				return;
#if defined(ENGINE_PLATFORM_WINDOWS)
			for (const std::string_view fault : { "oom-rendertarget", "oom-rendertarget-persistent" })
			{
				CAPTURE(fault);
				const bool persistent = fault == "oom-rendertarget-persistent";
				Test::TempDirectory userData("RuntimeOomFrame");
				const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "OomFrameGame", .Shaders = true });
				REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
				Result<std::filesystem::path> runtime = Test::GetBuiltExecutablePath("Runtime");
				REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
				std::vector<std::string> arguments = Test::WithoutAudioDevice({ "--headless", "--automation", "--paused", "--expect-no-errors",
					std::format("--gpu-inject-fault={}", fault), ManifestArgument(*manifest), "--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) });
				const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
				arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
				Result<Process> process = Process::Spawn({ .Executable = std::move(*runtime), .Arguments = std::move(arguments) });
				REQUIRE_MESSAGE(process.has_value(), process.error().ToString());
				const uint32_t pid = process->GetId();
				const std::filesystem::path sessions = userData / "OomFrameGame" / "Automation" / "Sessions";
				REQUIRE(Test::WaitUntil([&sessions, pid]()
				{
					return FileSystem::Exists(SessionFile::GetPath(sessions, pid));
				}));
				Result<RuntimeAutomationConnection> connection = RuntimeAutomationConnection::Open(sessions, pid);
				REQUIRE_MESSAGE(connection.has_value(), connection.error().ToString());

				// The game view's shadowed spot lights in the last frame it rendered.
				const auto readSpotShadows = [&connection]() -> uint32_t
				{
					const Result<Json> stats = connection->Call("stats.get", Json::object());
					REQUIRE_MESSAGE(stats.has_value(), stats.error().ToString());
					const Result<JsonReader> views = JsonReader(*stats).GetMember("views");
					REQUIRE_MESSAGE(views.has_value(), views.error().ToString());
					for (size_t index = 0; index < views->GetArraySize().value_or(0); ++index)
					{
						const Result<JsonReader> view = views->GetElement(index);
						if (view.has_value() && view->ReadMember<std::string>("name").value_or("") == "game")
							return view->ReadMember<uint32_t>("shadowedSpotLights").value_or(0);
					}
					FAIL_CHECK("stats.get reports no game view");
					return 0;
				};
				// The requests run in later frames than the first, whose garbage collection armed the fault.
				CHECK(readSpotShadows() == 0);
				const Result<Json> added = connection->Call("script.eval",
					Json{ { "code", "local spot = Scene.CreateEntity(\"Spot\")\n"
									"spot.Transform:Teleport(vector.create(0, 1.5, 3))\n"
									"spot:AddComponent(\"SpotLight\", { CastShadows = true })\n"
									"return true" },
						{ "context", "Play" } });
				REQUIRE_MESSAGE(added.has_value(), added.error().ToString());
				const Result<Json> stepped = connection->Call("play.step", Json{ { "ticks", 1 } });
				REQUIRE_MESSAGE(stepped.has_value(), stepped.error().ToString());
				REQUIRE(process->WaitForOutput(ProcessStream::StandardError, "The game view ran out of GPU memory", RuntimeAutomationConnection::Timeout));
				if (persistent)
				{
					// Every later frame needs the atlas and fails again, in the same run of failing frames.
					for (int frame = 0; frame < 4; ++frame)
						CHECK(readSpotShadows() == 0);
				}
				else
				{
					// After the trim the next frame creates the atlas and renders the shadowed spot.
					CHECK(Test::WaitUntil([&readSpotShadows]()
					{
						return readSpotShadows() == 1;
					}));
				}

				REQUIRE(connection->Call("session.shutdown", Json::object()).has_value());
				const Result<ProcessResult> result = process->Wait(std::chrono::seconds(120));
				REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
				INFO("runtime stderr: ", result->StandardError);
				CHECK(result->ExitCode == ExitCode::Success);
				size_t warnings = 0;
				for (size_t at = result->StandardError.find("The game view ran out of GPU memory"); at != std::string::npos;
					at = result->StandardError.find("The game view ran out of GPU memory", at + 1))
					++warnings;
				CHECK(warnings == 1);
				CHECK(result->StandardError.contains(std::format("injected by --gpu-inject-fault={}", fault)));
				CHECK_FALSE(result->StandardError.contains("Cannot render the game view"));
				CHECK_FALSE(ReadGameCrashReport(userData, "OomFrameGame").has_value());
				CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
			}
#else
			MESSAGE("RuntimeApp per-frame out-of-memory: the Runtime's automation client in this file is implemented on Windows only "
					"(RuntimeAutomationConnection); the editor's per-frame case is covered by the EditorHostViewports and EditorApp cases");
#endif
		}

		TEST_CASE("RuntimeApp: the game view continues its temporal history between renders and starts it afresh in a new session"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M26 (Docs/Decisions/0037-m26-contract.md decision 10; W26.2's review): the Runtime applies the game view's
			// history to every render, so stats.get's historyFrames counts the renders since it started afresh, and a new
			// session (input.record's restart of the start scene) starts it afresh. A paused session shows its last extracted
			// view, which a step makes, so each session is stepped once first. The game view renders every frame, so a
			// render's frame minus its historyFrames is the frame its history last started at.
			if (!Test::ProbeGpuForProcess())
				return;
#if defined(ENGINE_PLATFORM_WINDOWS)
			Test::TempDirectory userData("RuntimeViewHistory");
			const Result<std::filesystem::path> manifest = Test::WriteTestGame(userData / "Game", { .Name = "HistoryGame", .Shaders = true });
			REQUIRE_MESSAGE(manifest.has_value(), manifest.error().ToString());
			Result<std::filesystem::path> runtime = Test::GetBuiltExecutablePath("Runtime");
			REQUIRE_MESSAGE(runtime.has_value(), runtime.error().ToString());
			std::vector<std::string> arguments = Test::WithoutAudioDevice(
				{ "--headless", "--automation", "--paused", ManifestArgument(*manifest), "--user-data-dir=" + Test::PathToUtf8(userData.GetPath()) });
			const std::vector<std::string> gpuArguments = Test::GetGpuApplicationArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			Result<Process> process = Process::Spawn({ .Executable = std::move(*runtime), .Arguments = std::move(arguments) });
			REQUIRE_MESSAGE(process.has_value(), process.error().ToString());
			const uint32_t pid = process->GetId();
			const std::filesystem::path sessions = userData / "HistoryGame" / "Automation" / "Sessions";
			REQUIRE(Test::WaitUntil([&sessions, pid]()
			{
				return FileSystem::Exists(SessionFile::GetPath(sessions, pid));
			}));
			Result<RuntimeAutomationConnection> connection = RuntimeAutomationConnection::Open(sessions, pid);
			REQUIRE_MESSAGE(connection.has_value(), connection.error().ToString());

			// The game view's last render: its frame and its historyFrames (0 before the first); the last stats read, for a
			// failure's message.
			std::string lastStats;
			const auto readHistory = [&connection, &lastStats]() -> std::pair<uint64_t, uint32_t>
			{
				const Result<Json> stats = connection->Call("stats.get", Json::object());
				REQUIRE_MESSAGE(stats.has_value(), stats.error().ToString());
				lastStats = stats->dump();
				const Result<JsonReader> views = JsonReader(*stats).GetMember("views");
				REQUIRE_MESSAGE(views.has_value(), views.error().ToString());
				for (size_t index = 0; index < views->GetArraySize().value_or(0); ++index)
				{
					const Result<JsonReader> view = views->GetElement(index);
					if (!view.has_value() || view->ReadMember<std::string>("name").value_or("") != "game" || !view->ReadMember<bool>("available").value_or(false))
						continue;
					const std::string frame = view->ReadMember<std::string>("frame").value_or("");
					uint64_t value = 0;
					REQUIRE(std::from_chars(frame.data(), frame.data() + frame.size(), value).ec == std::errc());
					return { value, view->ReadMember<uint32_t>("historyFrames").value_or(0) };
				}
				return { 0, 0 };
			};
			INFO("the last stats.get: ", lastStats);
			REQUIRE(connection->Call("play.step", Json{ { "ticks", 1 } }).has_value());
			CHECK(Test::WaitUntil([&readHistory]()
			{
				return readHistory().second >= 3;
			}));
			const auto [frame, history] = readHistory();
			REQUIRE(history >= 3);
			REQUIRE(frame >= history);
			const Result<Json> restarted = connection->Call("input.record", Json{ { "action", "Start" }, { "restart", true } });
			REQUIRE_MESSAGE(restarted.has_value(), restarted.error().ToString());
			REQUIRE(connection->Call("play.step", Json{ { "ticks", 1 } }).has_value());
			CHECK(Test::WaitUntil([&readHistory, start = frame]()
			{
				const auto [rendered, frames] = readHistory();
				return rendered > start && rendered - frames > start && frames >= 2;
			}));

			REQUIRE(connection->Call("session.shutdown", Json::object()).has_value());
			const Result<ProcessResult> result = process->Wait(std::chrono::seconds(120));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			INFO("runtime stderr: ", result->StandardError);
			CHECK(result->ExitCode == ExitCode::Success);
			CHECK(Test::FindGpuMessageLines(result->StandardError).empty());
#else
			MESSAGE("RuntimeApp view history: the Runtime's automation client in this file is implemented on Windows only "
					"(RuntimeAutomationConnection); the editor's views are covered by the EditorHostViewports case");
#endif
		}

		TEST_CASE("RuntimeApp: out-of-memory at startup still exits 4 with a crash report"
			* doctest::test_suite(Test::GpuSuite))
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
			// The frame pacer's first wait for a slot with a submission spends its budget of timed-out slices and then its
			// reset wait's, since no loss report arrives (Docs/Decisions/0029-m18-contract.md decision 40: the hang that stays
			// fatal), without waiting for wall time (FramePacer.h), and ends the process with GpuHang during the render phase.
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
