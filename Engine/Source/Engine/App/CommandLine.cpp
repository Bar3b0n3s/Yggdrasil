#include "EnginePCH.h"
#include "Engine/App/CommandLine.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <system_error>
#include <utility>

namespace Engine {

	namespace Utils {

		// The option names without their leading dashes, so that "-headless" and "--headles" both compare well with
		// "--headless".
		static std::string_view StripDashes(std::string_view text)
		{
			const size_t first = text.find_first_not_of('-');
			return first == std::string_view::npos ? std::string_view() : text.substr(first);
		}

		// The Levenshtein distance of two short ASCII strings (option names).
		static size_t GetEditDistance(std::string_view first, std::string_view second)
		{
			std::vector<size_t> previous(second.size() + 1);
			std::vector<size_t> current(second.size() + 1);
			for (size_t column = 0; column <= second.size(); ++column)
				previous[column] = column;
			for (size_t row = 1; row <= first.size(); ++row)
			{
				current[0] = row;
				for (size_t column = 1; column <= second.size(); ++column)
				{
					const size_t substitution = previous[column - 1] + (first[row - 1] == second[column - 1] ? 0 : 1);
					current[column] = std::min({ previous[column] + 1, current[column - 1] + 1, substitution });
				}
				std::swap(previous, current);
			}
			return previous[second.size()];
		}

		// "did you mean '--headless'?" for the declared option closest to `given`, or else the list of declared options.
		static std::string MakeOptionHint(std::string_view given, std::span<const CommandLineOption> options)
		{
			if (options.empty())
				return "this program accepts no options";

			const std::string_view stripped = StripDashes(given);
			const CommandLineOption* closest = nullptr;
			size_t closestDistance = 0;
			for (const CommandLineOption& option : options)
			{
				const size_t distance = GetEditDistance(stripped, StripDashes(option.Name));
				if (closest == nullptr || distance < closestDistance)
				{
					closest = &option;
					closestDistance = distance;
				}
			}
			// A suggestion only for a near miss: up to a third of the name's length mistyped, at least two characters.
			const size_t allowed = std::max<size_t>(2, StripDashes(closest->Name).size() / 3);
			if (closestDistance <= allowed)
				return std::format("did you mean '{}'?", closest->Name);

			std::string list = "the options are ";
			for (size_t index = 0; index < options.size(); ++index)
			{
				if (index > 0)
					list += ", ";
				list += options[index].Name;
			}
			return list;
		}

		static std::unexpected<Error> MakeOptionError(std::string message, std::string_view given,
			std::span<const CommandLineOption> options)
		{
			return std::unexpected(Error(ErrorCode::InvalidArgument, std::move(message)).WithHint(MakeOptionHint(given, options)));
		}

		static const CommandLineOption* FindOption(std::span<const CommandLineOption> options, std::string_view name)
		{
			const auto found = std::ranges::find(options, name, &CommandLineOption::Name);
			return found == options.end() ? nullptr : &*found;
		}

		// "--frames N", "--automation[=port]" or "--headless".
		static std::string FormatOptionSpelling(const CommandLineOption& option)
		{
			const std::string_view valueName = option.ValueName.empty() ? std::string_view("value") : option.ValueName;
			switch (option.Value)
			{
				case CommandLineValue::None:     return std::string(option.Name);
				case CommandLineValue::Required: return std::format("{} {}", option.Name, valueName);
				case CommandLineValue::Optional: return std::format("{}[={}]", option.Name, valueName);
			}

			ENGINE_CORE_ASSERT(false, "Unknown CommandLineValue {}", std::to_underlying(option.Value));
			return std::string(option.Name);
		}

	}

	Result<CommandLine> CommandLine::Parse(std::span<const std::string> arguments, std::span<const CommandLineOption> options)
	{
		CommandLine commandLine;
		commandLine.m_Arguments.assign(arguments.begin(), arguments.end());

		bool optionsEnded = false;
		for (size_t index = 0; index < arguments.size(); ++index)
		{
			const std::string& argument = arguments[index];
			if (optionsEnded)
			{
				commandLine.m_Positional.push_back(argument);
				continue;
			}
			if (argument == "--")
			{
				optionsEnded = true;
				continue;
			}
			if (!argument.starts_with("--"))
			{
				if (argument.size() > 1 && argument.front() == '-')
				{
					return Utils::MakeOptionError(std::format("unknown option '{}': options start with '--'", argument), argument,
						options);
				}
				commandLine.m_Positional.push_back(argument);
				continue;
			}

			const size_t equals = argument.find('=');
			const std::string_view name = std::string_view(argument).substr(0, equals);
			const CommandLineOption* option = Utils::FindOption(options, name);
			if (option == nullptr)
				return Utils::MakeOptionError(std::format("unknown option '{}'", name), name, options);
			if (commandLine.FindGiven(name) != nullptr)
				return MakeError(ErrorCode::InvalidArgument, "option '{}' is given more than once", name);

			GivenOption given{ .Name = std::string(name) };
			switch (option->Value)
			{
				case CommandLineValue::None:
				{
					if (equals != std::string::npos)
						return MakeError(ErrorCode::InvalidArgument, "option '{}' takes no value, got '{}'", name, argument);
					break;
				}
				case CommandLineValue::Required:
				{
					if (equals != std::string::npos)
						given.Value = argument.substr(equals + 1);
					else if (index + 1 < arguments.size())
						given.Value = arguments[++index];
					if (!given.Value.has_value() || given.Value->empty())
					{
						return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a value: {}", name,
							Utils::FormatOptionSpelling(*option));
					}
					break;
				}
				case CommandLineValue::Optional:
				{
					if (equals != std::string::npos)
					{
						given.Value = argument.substr(equals + 1);
						if (given.Value->empty())
						{
							return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a value after '=': {}", name,
								Utils::FormatOptionSpelling(*option));
						}
					}
					break;
				}
			}
			commandLine.m_Options.push_back(std::move(given));
		}
		return commandLine;
	}

	bool CommandLine::Has(std::string_view name) const
	{
		return FindGiven(name) != nullptr;
	}

	std::optional<std::string_view> CommandLine::GetValue(std::string_view name) const
	{
		const GivenOption* given = FindGiven(name);
		if (given == nullptr || !given->Value.has_value())
			return std::nullopt;
		return std::string_view(*given->Value);
	}

	Result<std::optional<uint64_t>> CommandLine::GetUnsigned(std::string_view name, uint64_t maximum) const
	{
		const GivenOption* given = FindGiven(name);
		if (given == nullptr)
			return std::optional<uint64_t>();
		if (!given->Value.has_value())
			return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a value", name);

		const std::string& text = *given->Value;
		const bool digitsOnly = !text.empty() && std::ranges::all_of(text, [](char character)
		{
			return character >= '0' && character <= '9';
		});
		if (!digitsOnly)
			return MakeError(ErrorCode::InvalidArgument, "option '{}' needs an unsigned decimal integer, got '{}'", name, text);

		uint64_t value = 0;
		const std::from_chars_result parsed = std::from_chars(text.data(), text.data() + text.size(), value);
		if (parsed.ec != std::errc() || value > maximum)
			return MakeError(ErrorCode::InvalidArgument, "option '{}' must be at most {}, got '{}'", name, maximum, text);
		return std::optional<uint64_t>(value);
	}

	const std::vector<std::string>& CommandLine::GetPositional() const
	{
		return m_Positional;
	}

	const std::vector<std::string>& CommandLine::GetArguments() const
	{
		return m_Arguments;
	}

	std::string CommandLine::FormatUsage(std::string_view programName, std::span<const CommandLineOption> options)
	{
		std::vector<std::string> spellings;
		spellings.reserve(options.size());
		size_t width = 0;
		for (const CommandLineOption& option : options)
		{
			spellings.push_back(Utils::FormatOptionSpelling(option));
			width = std::max(width, spellings.back().size());
		}

		std::string usage = std::format("Usage: {} [options]\n", programName);
		for (size_t index = 0; index < options.size(); ++index)
			usage += std::format("  {:<{}}  {}\n", spellings[index], width, options[index].Description);
		return usage;
	}

	const CommandLine::GivenOption* CommandLine::FindGiven(std::string_view name) const
	{
		const auto found = std::ranges::find(m_Options, name, &GivenOption::Name);
		return found == m_Options.end() ? nullptr : &*found;
	}

}
