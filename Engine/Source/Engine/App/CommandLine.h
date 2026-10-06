#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The command line of an application (Architecture §4.1 ApplicationSpecification::Args, §12.1 Editor CLI, §13.9
// Runtime CLI), parsed against the options the application declares, so a typo is a usage error (exit code 2), never a
// silently ignored flag.

namespace Engine {

	// Whether an option takes a value.
	enum class CommandLineValue : uint8_t
	{
		None,     // a flag: "--headless"; "--headless=x" is an error
		Required, // "--frames 10" or "--frames=10"
		Optional  // "--automation" or "--automation=5000": the value only after '='
	};

	// One option an application accepts. The views must refer to text that outlives every Parse and FormatUsage call
	// given the option, in practice string literals in a static option table (GetEngineCommandLineOptions).
	struct CommandLineOption
	{
		std::string_view Name{}; // with the leading dashes: "--frames"
		CommandLineValue Value = CommandLineValue::None;
		std::string_view ValueName{};   // for usage text: "N", "port"
		std::string_view Description{}; // one sentence, for usage text
	};

	// A parsed command line: which options were given, their values, and the positional arguments. A value type.
	class CommandLine
	{
	public:
		// An empty command line: no options, no positional arguments.
		CommandLine() = default;

		// Parses `arguments` (argv[1..], UTF-8) against `options`. An argument that starts with "--" is an option: "--name",
		// "--name=value", or "--name value" for a Required option (the next argument is the value even when it starts with
		// "-"). A lone "--" ends option parsing: every later argument is positional. Every other argument that starts with
		// '-' and is longer than "-" is a mistyped option ("-headless"): there are no single-dash options. Any other
		// argument, a lone "-" included, is positional, in order; an application that declares no positional arguments
		// rejects them itself (the editor's and runtime's factories do). Errors: InvalidArgument naming the argument for an
		// unknown or single-dash option (with a hint naming the closest declared option), a missing value, a value given to
		// a flag, or an option given twice.
		[[nodiscard]] static Result<CommandLine> Parse(std::span<const std::string> arguments, std::span<const CommandLineOption> options);

		// Whether option `name` ("--headless") was given, with or without a value.
		[[nodiscard]] bool Has(std::string_view name) const;

		// The value of option `name`; nullopt when it was not given or was given without a value (an Optional option). The
		// view points into this CommandLine's arguments and is valid while the CommandLine lives, unmoved and unassigned.
		[[nodiscard]] std::optional<std::string_view> GetValue(std::string_view name) const;

		// The value of option `name` as an unsigned decimal integer; nullopt when the option was not given. Errors:
		// InvalidArgument naming the option when the value is missing, not plain decimal digits, or above `maximum`.
		[[nodiscard]] Result<std::optional<uint64_t>> GetUnsigned(std::string_view name, uint64_t maximum = UINT64_MAX) const;

		[[nodiscard]] const std::vector<std::string>& GetPositional() const;

		// The arguments as given to Parse.
		[[nodiscard]] const std::vector<std::string>& GetArguments() const;

		// Usage text: the line "Usage: <programName> [options]", then one line per option in declaration order: its
		// spelling ("--headless", "--frames N" for a Required option, "--automation[=port]" for an Optional one), padded to
		// a common width, then its description.
		[[nodiscard]] static std::string FormatUsage(std::string_view programName, std::span<const CommandLineOption> options);
	private:
		// One option as given: its declared name and its value (nullopt for a flag or a valueless Optional option).
		struct GivenOption
		{
			std::string Name{};
			std::optional<std::string> Value{};
		};

		[[nodiscard]] const GivenOption* FindGiven(std::string_view name) const;
	private:
		std::vector<std::string> m_Arguments;
		std::vector<std::string> m_Positional;
		std::vector<GivenOption> m_Options; // in the order given
	};

}
