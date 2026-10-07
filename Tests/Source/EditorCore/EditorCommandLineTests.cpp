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
		TEST_CASE("EditorCommandLine: the option table names every M4 editor option")
		{
			// --renderer is an engine option (GetEngineCommandLineOptions), which the Runtime takes too (ADR 0009 decisions 3 and 33).
			std::vector<std::string_view> names;
			for (const CommandLineOption& option : GetEditorCommandLineOptions())
			{
				names.push_back(option.Name);
				CHECK_FALSE(option.Description.empty());
			}
			const std::vector<std::string_view> expected = { "--project", "--read-only", "--automation", "--automation-test-hooks", "--batch",
				"--upgrade", "--dump-reference" };
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

		TEST_CASE("EditorCommandLine: --bake-engine-assets is a one-shot run that excludes the other runs" * doctest::skip(true))
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
	}

}
