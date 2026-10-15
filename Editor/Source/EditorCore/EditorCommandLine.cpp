#include "EditorPCH.h"
#include "EditorCore/EditorCommandLine.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Utf8.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace Engine {

	namespace {

		constexpr std::string_view ProjectOption = "--project";
		constexpr std::string_view ReadOnlyOption = "--read-only";
		constexpr std::string_view AutomationOption = "--automation";
		constexpr std::string_view TestHooksOption = "--automation-test-hooks";
		constexpr std::string_view BatchOption = "--batch";
		constexpr std::string_view UpgradeOption = "--upgrade";
		constexpr std::string_view DumpReferenceOption = "--dump-reference";
		constexpr std::string_view BakeEngineAssetsOption = "--bake-engine-assets";
		constexpr std::string_view EngineCacheDirectoryOption = "--engine-cache-dir";
		constexpr std::string_view CheckScriptsOption = "--check-scripts";
		constexpr std::string_view RunTestsOption = "--run-tests";
		constexpr std::string_view FilterOption = "--filter";
		constexpr std::string_view IdSeedOption = "--id-seed";
		constexpr std::string_view JunitOption = "--junit";
		constexpr std::string_view ExportOption = "--export";
		constexpr std::string_view ConfigOption = "--config";
		constexpr std::string_view TestingOption = "--testing";
		constexpr std::string_view SmokeTestOption = "--smoke-test";
		constexpr std::string_view ExportReportOption = "--export-report";
		constexpr std::string_view ExportRootOption = "--export-root";
		constexpr std::string_view StrictOption = "--strict";
		constexpr std::string_view HelpOption = "--help";
		// --help's short spelling, the editor's one single-dash option (ExpandEditorShortOptions).
		constexpr std::string_view ShortHelpOption = "-h";
		// The engine option (GetEngineCommandLineOptions) that ends the run after N frames, which an export must not do.
		constexpr std::string_view FramesOption = "--frames";

		constexpr std::array EditorOptions = {
			CommandLineOption{ .Name = HelpOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Print this list of options and exit (also -h)." },
			CommandLineOption{ .Name = ProjectOption, .Value = CommandLineValue::Required, .ValueName = "path", .Description = "Open the project (.eproj or its directory)." },
			CommandLineOption{ .Name = ReadOnlyOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Open the project read-only, without the lock." },
			CommandLineOption{ .Name = AutomationOption, .Value = CommandLineValue::Optional, .ValueName = "port", .Description = "Start the automation server (127.0.0.1)." },
			CommandLineOption{ .Name = TestHooksOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Register the debug.* test hooks." },
			CommandLineOption{ .Name = BatchOption, .Value = CommandLineValue::Required, .ValueName = "file.jsonl", .Description = "Run the automation requests of a batch file and exit." },
			CommandLineOption{ .Name = UpgradeOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Run project.refreshAssets and project.upgrade on the project, transcribed, and exit." },
			CommandLineOption{ .Name = DumpReferenceOption, .Value = CommandLineValue::Required, .ValueName = "dir", .Description = "Write the generated reference (method catalogues, Markdown references, schemas, Engine.d.luau) into dir and exit." },
			CommandLineOption{ .Name = BakeEngineAssetsOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Bake the engine resources into the engine cooked cache and exit." },
			CommandLineOption{ .Name = EngineCacheDirectoryOption, .Value = CommandLineValue::Required, .ValueName = "dir", .Description = "Use this engine cooked cache instead of the checkout's bin/EngineCache." },
			CommandLineOption{ .Name = CheckScriptsOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Check every project script and exit with failure on errors." },
			CommandLineOption{ .Name = RunTestsOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Run configured script suites and replays and exit with their result." },
			CommandLineOption{ .Name = FilterOption, .Value = CommandLineValue::Required, .ValueName = "pattern", .Description = "Select script test suites/cases for --run-tests." },
			CommandLineOption{ .Name = IdSeedOption, .Value = CommandLineValue::Required, .ValueName = "n", .Description = "Seed the entity id and asset handle generator of a --batch run (0 to 2^64-1)." },
			CommandLineOption{ .Name = JunitOption, .Value = CommandLineValue::Required, .ValueName = "file.xml", .Description = "Write the --run-tests JUnit report here and its JSON report beside it (.json)." },
			CommandLineOption{ .Name = ExportOption, .Value = CommandLineValue::Required, .ValueName = "dir", .Description = "Export the project into this package directory and exit (needs --project and --config)." },
			CommandLineOption{ .Name = ConfigOption, .Value = CommandLineValue::Required, .ValueName = "Debug|Release|Dist", .Description = "The configuration an --export packages." },
			CommandLineOption{ .Name = TestingOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Make the --export a testing export (FeatureTest suites and replays included)." },
			CommandLineOption{ .Name = SmokeTestOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Run the exported game headless before the --export completes." },
			CommandLineOption{ .Name = ExportReportOption, .Value = CommandLineValue::Required, .ValueName = "file.json", .Description = "Write the --export result or error to this JSON file, whatever the outcome." },
			CommandLineOption{ .Name = ExportRootOption, .Value = CommandLineValue::Required, .ValueName = "dir", .Description = "Make project.export write below this directory instead of the project's Build/." },
			CommandLineOption{ .Name = StrictOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Treat unknown keys, fields and components in the project's .eproj, scenes and prefabs as errors, and refuse the project when any scene or prefab does not load strictly (CI)." },
		};

	}

	namespace Utils {

		// The path value of `option`, or nullopt when it was not given. std::filesystem::path throws on ill-formed UTF-8 on
		// Windows, and a command line is external input, so the text is checked first.
		[[nodiscard]] static Result<std::optional<std::filesystem::path>> ReadPathOption(const CommandLine& commandLine, std::string_view option)
		{
			const std::optional<std::string_view> value = commandLine.GetValue(option);
			if (!value.has_value())
				return std::optional<std::filesystem::path>();
			if (value->empty())
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a path", option);
			if (!IsValidUtf8(*value))
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a path in UTF-8", option);
			return std::optional<std::filesystem::path>(FileSystem::PathFromUtf8(*value));
		}

		// The configuration named by --config: exactly "Debug", "Release" or "Dist".
		[[nodiscard]] static Result<std::optional<ExportConfiguration>> ReadConfigurationOption(const CommandLine& commandLine)
		{
			const std::optional<std::string_view> value = commandLine.GetValue(ConfigOption);
			if (!value.has_value())
				return std::optional<ExportConfiguration>();
			for (const ExportConfiguration configuration : { ExportConfiguration::Debug, ExportConfiguration::Release, ExportConfiguration::Dist })
			{
				if (*value == ExportConfigurationToString(configuration))
					return std::optional<ExportConfiguration>(configuration);
			}
			return MakeError(ErrorCode::InvalidArgument, "option '{}' takes Debug, Release or Dist", ConfigOption);
		}

		// The export options' rules (§12.1; ADR 0024 decision 3). `commandLine` holds the engine options too.
		[[nodiscard]] static Status CheckExportOptions(const EditorLaunchOptions& options, const CommandLine& commandLine)
		{
			if (!options.ExportDirectory.has_value())
			{
				const bool exportOnly = options.ExportConfig.has_value() || options.ExportTesting || options.ExportSmokeTest || options.ExportReportPath.has_value();
				if (exportOnly)
				{
					return MakeError(ErrorCode::InvalidArgument, "options '{}', '{}', '{}' and '{}' need '{}'", ConfigOption, TestingOption, SmokeTestOption,
						ExportReportOption, ExportOption);
				}
				return {};
			}
			if (!options.ExportConfig.has_value())
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs '{}' (Debug, Release or Dist)", ExportOption, ConfigOption);
			if (!options.Project.has_value())
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs '{}'", ExportOption, ProjectOption);
			if (options.ReadOnly)
				return MakeError(ErrorCode::InvalidArgument, "option '{}' writes the export's staging files, which '{}' forbids", ExportOption, ReadOnlyOption);
			if (options.Automation || options.AutomationTestHooks)
				return MakeError(ErrorCode::InvalidArgument, "option '{}' is a one-shot run, which '{}' and '{}' cannot serve", ExportOption, AutomationOption, TestHooksOption);
			if (options.ExportRoot.has_value())
			{
				return MakeError(ErrorCode::InvalidArgument, "option '{}' names the package directory itself, so '{}' does not apply", ExportOption,
					ExportRootOption);
			}
			// --frames ends the run with exit code 0 after N frames, which would cut the export short and report success
			// without a package or a report (the M15 review's M03): the export decides when the run ends.
			if (commandLine.Has(FramesOption))
			{
				return MakeError(ErrorCode::InvalidArgument, "option '{}' runs until the export finishes, so '{}' does not apply", ExportOption,
					FramesOption);
			}
			return {};
		}

	}

	std::span<const CommandLineOption> GetEditorCommandLineOptions()
	{
		return EditorOptions;
	}

	std::vector<std::string> ExpandEditorShortOptions(std::span<const std::string> arguments, std::span<const CommandLineOption> options)
	{
		std::vector<std::string> expanded(arguments.begin(), arguments.end());
		for (size_t index = 0; index < expanded.size(); ++index)
		{
			const std::string& argument = expanded[index];
			if (argument == "--")
				break;
			if (argument == ShortHelpOption)
			{
				expanded[index] = std::string(HelpOption);
				continue;
			}
			// "--name value": the next argument is the Required option's value, whatever it looks like (CommandLine::Parse).
			if (argument.starts_with("--") && !argument.contains('='))
			{
				const auto option = std::ranges::find(options, std::string_view(argument), &CommandLineOption::Name);
				if (option != options.end() && option->Value == CommandLineValue::Required)
					++index;
			}
		}
		return expanded;
	}

	Result<EditorLaunchOptions> ParseEditorLaunchOptions(const CommandLine& commandLine)
	{
		EditorLaunchOptions options;
		if (commandLine.Has(HelpOption))
		{
			options.Help = true;
			return options;
		}
		ENGINE_TRY_ASSIGN(options.Project, Utils::ReadPathOption(commandLine, ProjectOption));
		options.ReadOnly = commandLine.Has(ReadOnlyOption);

		options.Automation = commandLine.Has(AutomationOption);
		if (commandLine.GetValue(AutomationOption).has_value())
		{
			ENGINE_TRY_ASSIGN(const std::optional<uint64_t> port, commandLine.GetUnsigned(AutomationOption, std::numeric_limits<uint16_t>::max()));
			if (!port.has_value() || *port == 0)
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes a port from 1 to 65535 (without one, the OS assigns it)", AutomationOption);
			options.AutomationPort = static_cast<uint16_t>(*port);
		}
		options.AutomationTestHooks = commandLine.Has(TestHooksOption);
		ENGINE_TRY_ASSIGN(options.BatchFile, Utils::ReadPathOption(commandLine, BatchOption));
		options.Upgrade = commandLine.Has(UpgradeOption);
		options.CheckScripts = commandLine.Has(CheckScriptsOption);
		options.RunTests = commandLine.Has(RunTestsOption);
		if (const auto filter = commandLine.GetValue(FilterOption))
		{
			if (!options.RunTests || !IsValidUtf8(*filter))
				return MakeError(ErrorCode::InvalidArgument, "option '{}' requires '{}' and a UTF-8 pattern", FilterOption, RunTestsOption);
			options.TestFilter = *filter;
		}
		ENGINE_TRY_ASSIGN(options.TestJunitPath, Utils::ReadPathOption(commandLine, JunitOption));
		if (options.TestJunitPath.has_value() && !options.RunTests)
			return MakeError(ErrorCode::InvalidArgument, "option '{}' requires '{}'", JunitOption, RunTestsOption);
		ENGINE_TRY_ASSIGN(options.IdSeed, commandLine.GetUnsigned(IdSeedOption, std::numeric_limits<uint64_t>::max()));
		ENGINE_TRY_ASSIGN(options.DumpReferenceDirectory, Utils::ReadPathOption(commandLine, DumpReferenceOption));
		options.BakeEngineAssets = commandLine.Has(BakeEngineAssetsOption);
		ENGINE_TRY_ASSIGN(options.EngineCacheDirectory, Utils::ReadPathOption(commandLine, EngineCacheDirectoryOption));
		ENGINE_TRY_ASSIGN(options.ExportDirectory, Utils::ReadPathOption(commandLine, ExportOption));
		ENGINE_TRY_ASSIGN(options.ExportConfig, Utils::ReadConfigurationOption(commandLine));
		options.ExportTesting = commandLine.Has(TestingOption);
		options.ExportSmokeTest = commandLine.Has(SmokeTestOption);
		ENGINE_TRY_ASSIGN(options.ExportReportPath, Utils::ReadPathOption(commandLine, ExportReportOption));
		ENGINE_TRY_ASSIGN(options.ExportRoot, Utils::ReadPathOption(commandLine, ExportRootOption));
		if (options.ExportRoot.has_value())
		{
			// project.export needs an absolute root (Exporter::Start), and the working directory is the user's meaning.
			std::error_code error;
			std::filesystem::path absolute = std::filesystem::absolute(*options.ExportRoot, error);
			if (error)
				return MakeError(ErrorCode::InvalidArgument, "option '{}' names '{}', which cannot be made absolute: {}", ExportRootOption,
					FileSystem::PathToUtf8(*options.ExportRoot), error.message());
			options.ExportRoot = absolute.lexically_normal();
		}
		options.Strict = commandLine.Has(StrictOption);
		ENGINE_TRY(Utils::CheckExportOptions(options, commandLine));

		if (options.ReadOnly && !options.Project.has_value())
			return MakeError(ErrorCode::InvalidArgument, "option '{}' needs '{}'", ReadOnlyOption, ProjectOption);
		if (options.Upgrade && !options.Project.has_value())
			return MakeError(ErrorCode::InvalidArgument, "option '{}' needs '{}'", UpgradeOption, ProjectOption);
		if ((options.CheckScripts || options.RunTests) && !options.Project.has_value())
			return MakeError(ErrorCode::InvalidArgument, "options '{}' and '{}' require '{}'", CheckScriptsOption, RunTestsOption, ProjectOption);
		if (options.Upgrade && options.ReadOnly)
			return MakeError(ErrorCode::InvalidArgument, "option '{}' rewrites project files, which '{}' forbids", UpgradeOption, ReadOnlyOption);
		const int oneShotModes = (options.BatchFile.has_value() ? 1 : 0) + (options.Upgrade ? 1 : 0) + (options.DumpReferenceDirectory.has_value() ? 1 : 0)
			+ (options.BakeEngineAssets ? 1 : 0) + (options.CheckScripts ? 1 : 0) + (options.RunTests ? 1 : 0) + (options.ExportDirectory.has_value() ? 1 : 0);
		if (oneShotModes > 1)
		{
			return MakeError(ErrorCode::InvalidArgument, "options '{}', '{}', '{}', '{}', '{}', '{}' and '{}' exclude each other", BatchOption, UpgradeOption,
				DumpReferenceOption, BakeEngineAssetsOption, CheckScriptsOption, RunTestsOption, ExportOption);
		}
		if ((options.Automation || options.AutomationTestHooks) && (options.DumpReferenceDirectory.has_value() || options.Upgrade || options.BakeEngineAssets || options.CheckScripts || options.RunTests))
		{
			return MakeError(ErrorCode::InvalidArgument, "options '{}' and '{}' cannot be combined with '{}', '{}', '{}', '{}' or '{}'", AutomationOption,
				TestHooksOption, DumpReferenceOption, UpgradeOption, BakeEngineAssetsOption, CheckScriptsOption, RunTestsOption);
		}
		if (options.AutomationTestHooks && !options.Automation && !options.BatchFile.has_value())
			return MakeError(ErrorCode::InvalidArgument, "option '{}' needs '{}' or '{}'", TestHooksOption, AutomationOption, BatchOption);
		if (options.IdSeed.has_value() && !options.BatchFile.has_value())
			return MakeError(ErrorCode::InvalidArgument, "option '{}' needs '{}'", IdSeedOption, BatchOption);
		return options;
	}

}
