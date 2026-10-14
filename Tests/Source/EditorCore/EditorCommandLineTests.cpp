#include "TestsPCH.h"

#include "EditorCore/EditorCommandLine.h"

#include "Engine/App/Application.h"

namespace Engine {

	// Parses `arguments` with the engine's and the editor's options, as the editor's factory does.
	static Result<EditorLaunchOptions> ParseEditorArguments(std::vector<std::string> arguments)
	{
		std::vector<CommandLineOption> options(GetEngineCommandLineOptions().begin(), GetEngineCommandLineOptions().end());
		options.insert(options.end(), GetEditorCommandLineOptions().begin(), GetEditorCommandLineOptions().end());
		ENGINE_TRY_ASSIGN(const CommandLine commandLine, CommandLine::Parse(arguments, options));
		return ParseEditorLaunchOptions(commandLine);
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorCommandLine: script checking and test runs are exclusive project one-shots")
		{
			const auto check = ParseEditorArguments({ "--project", "Game", "--check-scripts" });
			REQUIRE(check);
			CHECK(check->CheckScripts);
			CHECK(check->IsOneShot());
			CHECK_FALSE(check->ListensForAutomation(true));
			const auto tests = ParseEditorArguments({ "--project", "Game", "--run-tests", "--filter", "Movement/*" });
			REQUIRE(tests);
			CHECK(tests->RunTests);
			CHECK(tests->TestFilter == "Movement/*");
			CHECK(tests->IsOneShot());
			CHECK_FALSE(ParseEditorArguments({ "--check-scripts" }));
			CHECK_FALSE(ParseEditorArguments({ "--run-tests" }));
			CHECK_FALSE(ParseEditorArguments({ "--project", "Game", "--check-scripts", "--run-tests" }));
			CHECK_FALSE(ParseEditorArguments({ "--project", "Game", "--check-scripts", "--automation" }));
			CHECK_FALSE(ParseEditorArguments({ "--project", "Game", "--run-tests", "--batch", "run.jsonl" }));
			CHECK_FALSE(ParseEditorArguments({ "--project", "Game", "--filter", "Movement" }));
		}

		// ADR 0022 decision 7: --run-tests reports go to bin/TestResults/FeatureTest-Editor.{xml,json} or beside --junit.
		TEST_CASE("EditorCommandLine: --junit names the reports of a test run")
		{
			const auto tests = ParseEditorArguments({ "--project", "Game", "--run-tests", "--junit", "bin/TestResults/FeatureTest-Editor-Debug.xml" });
			REQUIRE_MESSAGE(tests.has_value(), tests.error().ToString());
			CHECK(tests->TestJunitPath == std::filesystem::path("bin/TestResults/FeatureTest-Editor-Debug.xml"));
			const auto plain = ParseEditorArguments({ "--project", "Game", "--run-tests" });
			REQUIRE(plain);
			CHECK_FALSE(plain->TestJunitPath.has_value());
			CHECK_FALSE(ParseEditorArguments({ "--project", "Game", "--junit", "out.xml" }));
			CHECK_FALSE(ParseEditorArguments({ "--project", "Game", "--check-scripts", "--junit", "out.xml" }));
		}

		// ADR 0022 decision 8 (implemented by the contract): committed scaffolds run with a fixed id seed, so their output is
		// byte-identical on every machine; EditorAppTests proves two seeded batch runs write identical ids.
		TEST_CASE("EditorCommandLine: --id-seed seeds the ids of a batch run")
		{
			const auto batch = ParseEditorArguments({ "--project", "Projects/FeatureTest", "--batch", "Scaffold/Formats.jsonl", "--id-seed", "1401" });
			REQUIRE_MESSAGE(batch.has_value(), batch.error().ToString());
			CHECK(batch->IdSeed == std::optional<uint64_t>(1401));
			const auto largest = ParseEditorArguments({ "--batch", "s.jsonl", "--id-seed", "18446744073709551615" });
			REQUIRE(largest);
			CHECK(largest->IdSeed == std::optional<uint64_t>(18446744073709551615ull));
			CHECK_FALSE(ParseEditorArguments({ "--project", "Game", "--id-seed", "1" }));
			CHECK_FALSE(ParseEditorArguments({ "--project", "Game", "--run-tests", "--id-seed", "1" }));
			for (const char* invalid : { "-1", "abc", "0x10", "18446744073709551616", "" })
			{
				CAPTURE(invalid);
				CHECK_FALSE(ParseEditorArguments({ "--batch", "s.jsonl", "--id-seed", invalid }));
			}
		}

		TEST_CASE("EditorCommandLine: the option table names every editor launch option")
		{
			// --renderer is an engine option (GetEngineCommandLineOptions), which the Runtime takes too (ADR 0009 decisions 3 and 33).
			std::vector<std::string_view> names;
			for (const CommandLineOption& option : GetEditorCommandLineOptions())
			{
				names.push_back(option.Name);
				CHECK_FALSE(option.Description.empty());
			}
			const std::vector<std::string_view> expected = { "--project", "--read-only", "--automation", "--automation-test-hooks", "--batch",
				"--upgrade", "--dump-reference", "--bake-engine-assets", "--engine-cache-dir", "--check-scripts", "--run-tests", "--filter", "--id-seed", "--junit" };
			CHECK(names == expected);
		}

		TEST_CASE("EditorCommandLine: every option parses into the launch options")
		{
			const Result<EditorLaunchOptions> options =
				ParseEditorArguments({ "--project", "Projects/Tetris", "--read-only", "--renderer", "none", "--automation=50123", "--automation-test-hooks" });
			REQUIRE_MESSAGE(options.has_value(), options.error().ToString());
			CHECK(options->Project == std::filesystem::path("Projects/Tetris"));
			CHECK(options->ReadOnly);
			CHECK(options->Automation);
			CHECK(options->AutomationPort == 50123);
			CHECK(options->AutomationTestHooks);
			CHECK_FALSE(options->IsOneShot());

			const Result<EditorLaunchOptions> defaults = ParseEditorArguments({});
			REQUIRE(defaults.has_value());
			CHECK_FALSE(defaults->Project.has_value());
			CHECK_FALSE(defaults->Automation);

			const Result<EditorLaunchOptions> batch = ParseEditorArguments({ "--headless", "--batch", "Scaffold.jsonl" });
			REQUIRE(batch.has_value());
			CHECK(batch->BatchFile == std::filesystem::path("Scaffold.jsonl"));
			CHECK(batch->IsOneShot());

			const Result<EditorLaunchOptions> upgrade = ParseEditorArguments({ "--headless", "--project", "P", "--upgrade" });
			REQUIRE(upgrade.has_value());
			CHECK(upgrade->Upgrade);

			const Result<EditorLaunchOptions> dump = ParseEditorArguments({ "--headless", "--renderer", "none", "--dump-reference", "out" });
			REQUIRE(dump.has_value());
			CHECK(dump->DumpReferenceDirectory == std::filesystem::path("out"));
			const Result<EditorLaunchOptions> automation = ParseEditorArguments({ "--automation" });
			REQUIRE(automation.has_value());
			CHECK(automation->AutomationPort == std::nullopt);
		}

		TEST_CASE("EditorCommandLine: the server listens with --automation, or headless outside one-shot runs")
		{
			EditorLaunchOptions interactive;
			CHECK_FALSE(interactive.ListensForAutomation(false)); // a windowed editor holds the lock without listening
			CHECK(interactive.ListensForAutomation(true));

			EditorLaunchOptions automation;
			automation.Automation = true;
			CHECK(automation.ListensForAutomation(false));

			EditorLaunchOptions batch;
			batch.BatchFile = std::filesystem::path("run.jsonl");
			CHECK(batch.IsOneShot());
			CHECK_FALSE(batch.ListensForAutomation(true));
			batch.Automation = true; // an explicit request still listens
			CHECK(batch.ListensForAutomation(true));

			EditorLaunchOptions upgrade;
			upgrade.Upgrade = true;
			CHECK_FALSE(upgrade.ListensForAutomation(true));

			EditorLaunchOptions dump;
			dump.DumpReferenceDirectory = std::filesystem::path("out");
			CHECK_FALSE(dump.ListensForAutomation(true));
		}

		TEST_CASE("EditorCommandLine: conflicting and incomplete options are InvalidArgument")
		{
			const std::vector<std::vector<std::string>> invalid = {
				{ "--automation=0" },
				{ "--automation=70000" },
				{ "--automation=port" },
				{ "--read-only" },
				{ "--upgrade" },
				{ "--project", "P", "--upgrade", "--read-only" },
				{ "--batch", "a.jsonl", "--dump-reference", "out" },
				{ "--project", "P", "--upgrade", "--batch", "a.jsonl" },
				{ "--automation", "--dump-reference", "out" },
				{ "--automation-test-hooks" },
			};
			for (const std::vector<std::string>& arguments : invalid)
			{
				std::string joined;
				for (const std::string& argument : arguments)
					joined += argument + ' ';
				INFO(joined);
				const Result<EditorLaunchOptions> options = ParseEditorArguments(arguments);
				REQUIRE_FALSE(options.has_value());
				CHECK(options.error().GetCode() == ErrorCode::InvalidArgument);
			}
		}

		TEST_CASE("EditorCommandLine: --bake-engine-assets is a one-shot run that excludes the other runs")
		{
			const Result<EditorLaunchOptions> bake = ParseEditorArguments({ "--headless", "--bake-engine-assets" });
			REQUIRE_MESSAGE(bake.has_value(), bake.error().ToString());
			CHECK(bake->BakeEngineAssets);
			CHECK(bake->IsOneShot());
			// A one-shot run never listens, even headless (ADR 0008 decision 14).
			CHECK_FALSE(bake->ListensForAutomation(true));
			// It needs no project, and with one it still bakes.
			CHECK(ParseEditorArguments({ "--bake-engine-assets", "--project", "Projects/Tetris" }).has_value());
			for (const std::vector<std::string>& conflicting : std::vector<std::vector<std::string>>{
					 { "--bake-engine-assets", "--batch", "Scaffold.jsonl" },
					 { "--bake-engine-assets", "--project", "Projects/Tetris", "--upgrade" },
					 { "--bake-engine-assets", "--dump-reference", "Reference" },
					 { "--bake-engine-assets", "--automation" },
				 })
			{
				const Result<EditorLaunchOptions> parsed = ParseEditorArguments(conflicting);
				REQUIRE_FALSE(parsed.has_value());
				CHECK(parsed.error().GetCode() == ErrorCode::InvalidArgument);
			}
		}

		TEST_CASE("EditorCommandLine: --engine-cache-dir names the engine cooked cache of any run")
		{
			const Result<EditorLaunchOptions> plain = ParseEditorArguments({ "--headless" });
			REQUIRE(plain.has_value());
			CHECK_FALSE(plain->EngineCacheDirectory.has_value());
			const Result<EditorLaunchOptions> bake = ParseEditorArguments({ "--headless", "--bake-engine-assets", "--engine-cache-dir", "Temp/EngineCache" });
			REQUIRE_MESSAGE(bake.has_value(), bake.error().ToString());
			CHECK(bake->EngineCacheDirectory == std::filesystem::path("Temp/EngineCache"));
			const Result<EditorLaunchOptions> automation = ParseEditorArguments({ "--automation", "--engine-cache-dir=Cache" });
			REQUIRE(automation.has_value());
			CHECK(automation->EngineCacheDirectory == std::filesystem::path("Cache"));
			const Result<EditorLaunchOptions> empty = ParseEditorArguments({ "--engine-cache-dir=" });
			REQUIRE_FALSE(empty.has_value());
			CHECK(empty.error().GetCode() == ErrorCode::InvalidArgument);
		}

		// M15 (§12.1, §13.4; Docs/Decisions/0024-m15-contract.md decision 3, stream A).
		TEST_CASE("EditorCommandLine: --export parses the directory, configuration, testing, smoke-test and report options" * doctest::skip(true))
		{
			const Result<EditorLaunchOptions> full = ParseEditorArguments({ "--headless", "--project", "Game", "--export", "Out/Game", "--config", "Dist",
				"--testing", "--smoke-test", "--export-report", "Out/Report.json" });
			REQUIRE_MESSAGE(full.has_value(), full.error().ToString());
			CHECK(full->ExportDirectory == std::filesystem::path("Out/Game"));
			CHECK(full->ExportConfig == ExportConfiguration::Dist);
			CHECK(full->ExportTesting);
			CHECK(full->ExportSmokeTest);
			CHECK(full->ExportReportPath == std::filesystem::path("Out/Report.json"));
			CHECK(full->IsOneShot());
			CHECK_FALSE(full->ListensForAutomation(true));

			const Result<EditorLaunchOptions> plain = ParseEditorArguments({ "--project", "Game", "--export", "Out", "--config", "Release" });
			REQUIRE_MESSAGE(plain.has_value(), plain.error().ToString());
			CHECK(plain->ExportConfig == ExportConfiguration::Release);
			CHECK_FALSE(plain->ExportTesting);
			CHECK_FALSE(plain->ExportSmokeTest);
			CHECK_FALSE(plain->ExportReportPath.has_value());

			for (const std::vector<std::string>& refused : std::vector<std::vector<std::string>>{
					 { "--export", "Out", "--config", "Release" },                       // no project
					 { "--project", "Game", "--export", "Out" },                         // no configuration
					 { "--project", "Game", "--export", "Out", "--config", "Shipping" }, // unknown configuration
					 { "--project", "Game", "--config", "Release" },                     // --config without --export
					 { "--project", "Game", "--testing" },                               // --testing without --export
					 { "--project", "Game", "--smoke-test" },                            // --smoke-test without --export
					 { "--project", "Game", "--export-report", "r.json" },               // --export-report without --export
					 { "--project", "Game", "--read-only", "--export", "Out", "--config", "Release" },
					 { "--project", "Game", "--export", "Out", "--config", "Release", "--run-tests" },
					 { "--project", "Game", "--export", "Out", "--config", "Release", "--automation" },
					 { "--project", "Game", "--export", "Out", "--config", "Release", "--export-root", "Root" } })
			{
				std::string joined;
				for (const std::string& argument : refused)
					joined += argument + " ";
				CAPTURE(joined);
				const Result<EditorLaunchOptions> options = ParseEditorArguments(refused);
				REQUIRE_FALSE(options.has_value());
				CHECK(options.error().GetCode() == ErrorCode::InvalidArgument);
			}
		}

		TEST_CASE("EditorCommandLine: --export-root names the root project.export writes under" * doctest::skip(true))
		{
			const Result<EditorLaunchOptions> serving = ParseEditorArguments({ "--headless", "--project", "Game", "--export-root", "Exports" });
			REQUIRE_MESSAGE(serving.has_value(), serving.error().ToString());
			REQUIRE(serving->ExportRoot.has_value());
			CHECK(serving->ExportRoot->is_absolute());
			CHECK(serving->ExportRoot->filename() == "Exports");
			CHECK_FALSE(serving->IsOneShot());
			const Result<EditorLaunchOptions> empty = ParseEditorArguments({ "--export-root=" });
			REQUIRE_FALSE(empty.has_value());
			CHECK(empty.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
