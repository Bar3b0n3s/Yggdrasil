#include "TestsPCH.h"
#include "Support/TestOptions.h"

#include "Support/ChildProcess.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace Engine {

	namespace Test {

		namespace Utils {

			constexpr std::string_view DeathTestOption = "--death-test";
			constexpr std::string_view TimeoutOption = "--test-timeout";

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

			// A finite number of seconds > 0 in plain decimal notation.
			static std::optional<double> ParsePositiveSeconds(std::string_view text)
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

			static std::filesystem::path PathFromUtf8(std::string_view text)
			{
				return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
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
				else if (const std::optional<std::string_view> seconds = Utils::MatchOption(argument, Utils::TimeoutOption))
				{
					const std::optional<double> parsed = Utils::ParsePositiveSeconds(*seconds);
					if (!parsed.has_value())
					{
						return MakeError(ErrorCode::InvalidArgument, "{} needs a positive number of seconds, got '{}'",
							Utils::TimeoutOption, *seconds);
					}
					options.DefaultTimeoutSeconds = *parsed;
				}
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
			std::filesystem::path fallback = std::filesystem::absolute(Utils::PathFromUtf8(argv[0]), error);
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

	}

}
