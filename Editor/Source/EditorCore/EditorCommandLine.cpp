#include "EditorPCH.h"
#include "EditorCore/EditorCommandLine.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Utf8.h"

#include <array>
#include <cstdint>
#include <format>
#include <limits>

namespace Engine {

	namespace {

		constexpr std::string_view ProjectOption = "--project";
		constexpr std::string_view ReadOnlyOption = "--read-only";
		constexpr std::string_view RendererOption = "--renderer";
		constexpr std::string_view AutomationOption = "--automation";
		constexpr std::string_view TestHooksOption = "--automation-test-hooks";
		constexpr std::string_view BatchOption = "--batch";
		constexpr std::string_view UpgradeOption = "--upgrade";
		constexpr std::string_view DumpReferenceOption = "--dump-reference";

		constexpr std::array EditorOptions = {
			CommandLineOption{ .Name = ProjectOption, .Value = CommandLineValue::Required, .ValueName = "path", .Description = "Open the project (.eproj or its directory)." },
			CommandLineOption{ .Name = ReadOnlyOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Open the project read-only, without the lock." },
			CommandLineOption{ .Name = RendererOption, .Value = CommandLineValue::Required, .ValueName = "vulkan|none", .Description = "Render with Vulkan, or not at all." },
			CommandLineOption{ .Name = AutomationOption, .Value = CommandLineValue::Optional, .ValueName = "port", .Description = "Start the automation server (127.0.0.1)." },
			CommandLineOption{ .Name = TestHooksOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Register the debug.* test hooks." },
			CommandLineOption{ .Name = BatchOption, .Value = CommandLineValue::Required, .ValueName = "file.jsonl", .Description = "Run the automation requests of a batch file and exit." },
			CommandLineOption{ .Name = UpgradeOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Run project.upgrade on the project and exit." },
			CommandLineOption{ .Name = DumpReferenceOption, .Value = CommandLineValue::Required, .ValueName = "dir", .Description = "Write the method catalogues and exit." },
		};

	}

	namespace Utils {

		[[nodiscard]] static bool EqualsIgnoreAsciiCase(std::string_view a, std::string_view b)
		{
			if (a.size() != b.size())
				return false;
			for (size_t index = 0; index < a.size(); ++index)
			{
				const auto lower = [](char character)
				{
					return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
				};
				if (lower(a[index]) != lower(b[index]))
					return false;
			}
			return true;
		}

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

	}

	std::string_view EditorRendererToString(EditorRenderer renderer)
	{
		switch (renderer)
		{
			case EditorRenderer::Vulkan: return "vulkan";
			case EditorRenderer::None:   return "none";
		}
		return "vulkan";
	}

	std::span<const CommandLineOption> GetEditorCommandLineOptions()
	{
		return EditorOptions;
	}

	Result<EditorLaunchOptions> ParseEditorLaunchOptions(const CommandLine& commandLine)
	{
		EditorLaunchOptions options;
		ENGINE_TRY_ASSIGN(options.Project, Utils::ReadPathOption(commandLine, ProjectOption));
		options.ReadOnly = commandLine.Has(ReadOnlyOption);

		if (const std::optional<std::string_view> renderer = commandLine.GetValue(RendererOption))
		{
			if (Utils::EqualsIgnoreAsciiCase(*renderer, EditorRendererToString(EditorRenderer::Vulkan)))
				options.Renderer = EditorRenderer::Vulkan;
			else if (Utils::EqualsIgnoreAsciiCase(*renderer, EditorRendererToString(EditorRenderer::None)))
				options.Renderer = EditorRenderer::None;
			else
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes vulkan or none, got '{}'", RendererOption, *renderer);
		}

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
		ENGINE_TRY_ASSIGN(options.DumpReferenceDirectory, Utils::ReadPathOption(commandLine, DumpReferenceOption));

		if (options.ReadOnly && !options.Project.has_value())
			return MakeError(ErrorCode::InvalidArgument, "option '{}' needs '{}'", ReadOnlyOption, ProjectOption);
		if (options.Upgrade && !options.Project.has_value())
			return MakeError(ErrorCode::InvalidArgument, "option '{}' needs '{}'", UpgradeOption, ProjectOption);
		if (options.Upgrade && options.ReadOnly)
			return MakeError(ErrorCode::InvalidArgument, "option '{}' rewrites project files, which '{}' forbids", UpgradeOption, ReadOnlyOption);
		const int oneShotModes = (options.BatchFile.has_value() ? 1 : 0) + (options.Upgrade ? 1 : 0) + (options.DumpReferenceDirectory.has_value() ? 1 : 0);
		if (oneShotModes > 1)
		{
			return MakeError(ErrorCode::InvalidArgument, "options '{}', '{}' and '{}' exclude each other", BatchOption, UpgradeOption,
				DumpReferenceOption);
		}
		if ((options.Automation || options.AutomationTestHooks) && (options.DumpReferenceDirectory.has_value() || options.Upgrade))
		{
			return MakeError(ErrorCode::InvalidArgument, "options '{}' and '{}' cannot be combined with '{}' or '{}'", AutomationOption,
				TestHooksOption, DumpReferenceOption, UpgradeOption);
		}
		if (options.AutomationTestHooks && !options.Automation && !options.BatchFile.has_value())
			return MakeError(ErrorCode::InvalidArgument, "option '{}' needs '{}' or '{}'", TestHooksOption, AutomationOption, BatchOption);
		return options;
	}

}
