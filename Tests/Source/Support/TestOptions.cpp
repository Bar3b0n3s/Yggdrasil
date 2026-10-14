#include "TestsPCH.h"
#include "Support/TestOptions.h"

#include "Engine/Core/Utf8.h"
#include "Support/ChildProcess.h"
#include "Support/Utf8Path.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace Engine {

	namespace Test {

		namespace Utils {

			constexpr std::string_view DeathTestOption = "--death-test";
			constexpr std::string_view WindowedChildOption = "--windowed-child";
			constexpr std::string_view CrashChildOption = "--crash-child";
			constexpr std::string_view ChildProcessFlag = ChildProcessOption;
			constexpr std::string_view UserDataOption = "--user-data-dir";
			constexpr std::string_view ChildArgumentOption = "--child-argument";
			constexpr std::string_view TimeoutOption = "--test-timeout";
			constexpr std::string_view RequireGpuOption = "--require-gpu";
			constexpr std::string_view VulkanApiOption = "--vulkan-api";
			constexpr std::string_view UpdateGoldenOption = "--update-golden";
			constexpr std::string_view FuzzIterationsOption = "--fuzz-iterations";
			constexpr std::string_view FuzzScaleOption = "--fuzz-scale";
			constexpr std::string_view FuzzShardOption = "--fuzz-shard";
			constexpr std::string_view FuzzIndexOption = "--fuzz-index";
			constexpr std::string_view TimeoutScaleOption = "--test-timeout-scale";

			// The value of `--<name>=<value>` when `argument` is that option (an empty value when the '=' is missing), or
			// nullopt when it is another argument.
			static std::optional<std::string_view> MatchOption(std::string_view argument, std::string_view option)
			{
				if (!argument.starts_with(option))
					return std::nullopt;
				const std::string_view rest = argument.substr(option.size());
				if (rest.empty())
					return std::string_view();
				if (rest.front() != '=')
					return std::nullopt; // a longer option that merely starts with the same characters
				return rest.substr(1);
			}

			// Digits with an optional fraction and exponent ("2", "2.5", ".5", "1e3"): what strtod accepts minus leading
			// spaces, signs, "inf", "nan" and hexadecimal.
			static bool IsDecimalNumber(std::string_view text)
			{
				size_t index = 0;
				const auto skipDigits = [&text, &index]()
				{
					const size_t start = index;
					while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index])))
						++index;
					return index - start;
				};

				size_t digitCount = skipDigits();
				if (index < text.size() && text[index] == '.')
				{
					++index;
					digitCount += skipDigits();
				}
				if (digitCount == 0)
					return false;
				if (index < text.size() && (text[index] == 'e' || text[index] == 'E'))
				{
					++index;
					if (index < text.size() && (text[index] == '+' || text[index] == '-'))
						++index;
					if (skipDigits() == 0)
						return false;
				}
				return index == text.size();
			}

			// A finite number > 0 in plain decimal notation (seconds, scales).
			static std::optional<double> ParsePositiveNumber(std::string_view text)
			{
				if (!IsDecimalNumber(text))
					return std::nullopt;
				const std::string terminated(text);
				char* end = nullptr;
				const double value = std::strtod(terminated.c_str(), &end);
				if (end != terminated.c_str() + terminated.size() || !std::isfinite(value) || !(value > 0.0))
					return std::nullopt;
				return value;
			}

			// Decimal digits only, without sign or spaces, that fit in 64 bits.
			static std::optional<uint64_t> ParseUnsigned(std::string_view text)
			{
				if (text.empty() || !std::ranges::all_of(text, [](char character)
				{
					return character >= '0' && character <= '9';
				}))
				{
					return std::nullopt;
				}
				uint64_t value = 0;
				const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), value);
				if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size())
					return std::nullopt;
				return value;
			}

			static TestOptions& GetMutableTestOptions()
			{
				static TestOptions s_Options;
				return s_Options;
			}

		}

		Result<TestOptions> ParseTestOptions(int argc, const char* const* argv)
		{
			TestOptions options;
			bool shardGiven = false;
			for (int index = 1; index < argc; ++index)
			{
				const std::string_view argument = argv[index] != nullptr ? std::string_view(argv[index]) : std::string_view();
				if (const std::optional<std::string_view> name = Utils::MatchOption(argument, Utils::DeathTestOption))
				{
					if (name->empty())
					{
						return MakeError(ErrorCode::InvalidArgument, "{} needs the name of a death test: {}=<Module>/<Name>",
							Utils::DeathTestOption, Utils::DeathTestOption);
					}
					options.DeathTest = std::string(*name);
				}
				else if (const std::optional<std::string_view> testCase = Utils::MatchOption(argument, Utils::WindowedChildOption))
				{
					if (testCase->empty())
					{
						return MakeError(ErrorCode::InvalidArgument, "{} needs the name of a test case: {}=<test case name>",
							Utils::WindowedChildOption, Utils::WindowedChildOption);
					}
					options.WindowedChild = std::string(*testCase);
				}
				else if (const std::optional<std::string_view> crashValue = Utils::MatchOption(argument, Utils::CrashChildOption))
				{
					if (argument != Utils::CrashChildOption)
						return MakeError(ErrorCode::InvalidArgument, "{} takes no value, got '{}'", Utils::CrashChildOption, *crashValue);
					options.CrashChild = true;
				}
				else if (const std::optional<std::string_view> childValue = Utils::MatchOption(argument, Utils::ChildProcessFlag))
				{
					if (argument != Utils::ChildProcessFlag)
						return MakeError(ErrorCode::InvalidArgument, "{} takes no value, got '{}'", Utils::ChildProcessFlag, *childValue);
					options.ChildProcess = true;
				}
				else if (const std::optional<std::string_view> directory = Utils::MatchOption(argument, Utils::UserDataOption))
				{
					if (directory->empty())
					{
						return MakeError(ErrorCode::InvalidArgument, "{} needs an absolute directory: {}=<path>", Utils::UserDataOption,
							Utils::UserDataOption);
					}
					// std::filesystem::path throws on ill-formed UTF-8 on Windows.
					if (!IsValidUtf8(*directory))
						return MakeError(ErrorCode::InvalidArgument, "{} needs a path in UTF-8", Utils::UserDataOption);
					std::filesystem::path path = PathFromUtf8(*directory);
					if (!path.is_absolute())
					{
						return MakeError(ErrorCode::InvalidArgument, "{} needs an absolute directory, got '{}'", Utils::UserDataOption,
							*directory);
					}
					options.UserDataDirectory = std::move(path);
				}
				else if (const std::optional<std::string_view> text = Utils::MatchOption(argument, Utils::ChildArgumentOption))
				{
					options.ChildArgument = std::string(*text);
				}
				else if (const std::optional<std::string_view> seconds = Utils::MatchOption(argument, Utils::TimeoutOption))
				{
					const std::optional<double> parsed = Utils::ParsePositiveNumber(*seconds);
					if (!parsed.has_value())
					{
						return MakeError(ErrorCode::InvalidArgument, "{} needs a positive number of seconds, got '{}'",
							Utils::TimeoutOption, *seconds);
					}
					options.DefaultTimeoutSeconds = *parsed;
				}
				else if (const std::optional<std::string_view> requireValue = Utils::MatchOption(argument, Utils::RequireGpuOption))
				{
					if (argument != Utils::RequireGpuOption)
						return MakeError(ErrorCode::InvalidArgument, "{} takes no value, got '{}'", Utils::RequireGpuOption, *requireValue);
					options.RequireGpu = true;
				}
				else if (const std::optional<std::string_view> api = Utils::MatchOption(argument, Utils::VulkanApiOption))
				{
					const std::optional<VulkanApiVersion> version = VulkanApiVersionFromString(*api);
					if (!version.has_value())
						return MakeError(ErrorCode::InvalidArgument, "{} takes 1.3 or 1.4, got '{}'", Utils::VulkanApiOption, *api);
					options.VulkanApi = *version;
				}
				else if (const std::optional<std::string_view> updateValue = Utils::MatchOption(argument, Utils::UpdateGoldenOption))
				{
					if (argument != Utils::UpdateGoldenOption)
						return MakeError(ErrorCode::InvalidArgument, "{} takes no value, got '{}'", Utils::UpdateGoldenOption, *updateValue);
					options.UpdateGolden = true;
				}
				else if (const std::optional<std::string_view> iterations = Utils::MatchOption(argument, Utils::FuzzIterationsOption))
				{
					const std::optional<uint64_t> parsed = Utils::ParseUnsigned(*iterations);
					if (!parsed.has_value() || *parsed == 0)
					{
						return MakeError(ErrorCode::InvalidArgument, "{} needs a positive whole number of iterations, got '{}'",
							Utils::FuzzIterationsOption, *iterations);
					}
					options.FuzzIterations = *parsed;
				}
				else if (const std::optional<std::string_view> scale = Utils::MatchOption(argument, Utils::FuzzScaleOption))
				{
					const std::optional<double> parsed = Utils::ParsePositiveNumber(*scale);
					if (!parsed.has_value() || *parsed > 1.0)
					{
						return MakeError(ErrorCode::InvalidArgument, "{} needs a number in (0, 1], got '{}'", Utils::FuzzScaleOption,
							*scale);
					}
					options.FuzzScale = *parsed;
				}
				else if (const std::optional<std::string_view> shard = Utils::MatchOption(argument, Utils::FuzzShardOption))
				{
					const size_t slash = shard->find('/');
					const std::optional<uint64_t> shardIndex = slash == std::string_view::npos ? std::nullopt : Utils::ParseUnsigned(shard->substr(0, slash));
					const std::optional<uint64_t> count = slash == std::string_view::npos ? std::nullopt : Utils::ParseUnsigned(shard->substr(slash + 1));
					if (!shardIndex.has_value() || !count.has_value() || *count == 0 || *shardIndex >= *count || *count > UINT32_MAX)
					{
						return MakeError(ErrorCode::InvalidArgument, "{} needs <k>/<n> with 0 <= k < n, got '{}'", Utils::FuzzShardOption,
							*shard);
					}
					options.FuzzShardIndex = static_cast<uint32_t>(*shardIndex);
					options.FuzzShardCount = static_cast<uint32_t>(*count);
					shardGiven = true;
				}
				else if (const std::optional<std::string_view> fuzzIndex = Utils::MatchOption(argument, Utils::FuzzIndexOption))
				{
					const std::optional<uint64_t> parsed = Utils::ParseUnsigned(*fuzzIndex);
					if (!parsed.has_value())
					{
						return MakeError(ErrorCode::InvalidArgument, "{} needs the whole-number index of one iteration, got '{}'",
							Utils::FuzzIndexOption, *fuzzIndex);
					}
					options.FuzzIndex = *parsed;
				}
				else if (const std::optional<std::string_view> timeoutScale = Utils::MatchOption(argument, Utils::TimeoutScaleOption))
				{
					const std::optional<double> parsed = Utils::ParsePositiveNumber(*timeoutScale);
					if (!parsed.has_value() || *parsed < 1.0)
					{
						return MakeError(ErrorCode::InvalidArgument, "{} needs a number >= 1, got '{}'", Utils::TimeoutScaleOption,
							*timeoutScale);
					}
					options.TimeoutScale = *parsed;
				}
			}

			if (shardGiven && options.FuzzIndex.has_value())
			{
				return MakeError(ErrorCode::InvalidArgument, "{} replays one iteration of the whole run; it does not combine with {}",
					Utils::FuzzIndexOption, Utils::FuzzShardOption);
			}

			const int childModeCount = static_cast<int>(!options.DeathTest.empty()) + static_cast<int>(!options.WindowedChild.empty())
				+ static_cast<int>(options.CrashChild);
			if (childModeCount > 1)
			{
				return MakeError(ErrorCode::InvalidArgument, "{}, {} and {} are child modes; give at most one", Utils::DeathTestOption,
					Utils::WindowedChildOption, Utils::CrashChildOption);
			}

			Result<std::filesystem::path> executable = GetCurrentExecutablePath();
			if (executable.has_value())
			{
				options.ExecutablePath = std::move(*executable);
				return options;
			}

			// The OS query failed: fall back to argv[0].
			if (argc < 1 || argv[0] == nullptr || argv[0][0] == '\0')
				return MakeError(ErrorCode::Io, "the Tests executable path is unknown ({}) and argv[0] is empty", executable.error());
			std::error_code error;
			std::filesystem::path fallback = std::filesystem::absolute(PathFromUtf8(argv[0]), error);
			if (error)
			{
				return MakeError(ErrorCode::Io, "the Tests executable path is unknown ({}) and argv[0] '{}' has no absolute form: {}",
					executable.error(), argv[0], error.message());
			}
			options.ExecutablePath = std::move(fallback);
			return options;
		}

		void SetTestOptions(TestOptions options)
		{
			Utils::GetMutableTestOptions() = std::move(options);
		}

		const TestOptions& GetTestOptions()
		{
			return Utils::GetMutableTestOptions();
		}

		Result<std::filesystem::path> GetBuiltExecutablePath(std::string_view project)
		{
			// <bin>/<Config>-<system>-<arch>/Tests/Tests[.exe] -> <bin>/<Config>-<system>-<arch>/<project>/<project>[.exe]
			const std::filesystem::path& tests = GetTestOptions().ExecutablePath;
			const std::filesystem::path outputDirectory = tests.parent_path().parent_path();
			const std::filesystem::path projectName = PathFromUtf8(project);
			std::filesystem::path executable = outputDirectory / projectName / projectName;
			executable += tests.extension();

			std::error_code error;
			if (std::filesystem::is_regular_file(executable, error))
				return executable;

			const std::string outputName = PathToUtf8(outputDirectory.filename());
			const std::string configuration = outputName.substr(0, outputName.find('-'));
			std::string message = std::format("the {} executable '{}' does not exist", project, PathToUtf8(executable));
			std::string hint = std::format("build it with python Scripts/Build.py --config {} --project {}", configuration, project);
			return std::unexpected(Error(ErrorCode::NotFound, std::move(message)).WithHint(std::move(hint)));
		}

		std::vector<std::string> WithoutAudioDevice(std::vector<std::string> arguments)
		{
			const bool decided = std::ranges::any_of(arguments, [](const std::string& argument)
			{
				return argument == "--headless" || argument.starts_with("--audio-device");
			});
			if (!decided)
				arguments.insert(arguments.end(), { "--audio-device", "none" });
			return arguments;
		}

		ProcessSpecification MakeTestsChildSpecification(std::vector<std::string> arguments)
		{
			arguments.emplace_back(ChildProcessOption);
			return ProcessSpecification{ .Executable = GetTestOptions().ExecutablePath, .Arguments = std::move(arguments) };
		}

	}

}
