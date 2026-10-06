#include "EnginePCH.h"
#include "Engine/App/CommandLine.h"

// M2 contract stub (Roadmap rule 3): stream D (application and frame loop) implements parsing and the queries. Until then
// Parse fails with Unsupported and an empty command line answers every query with "not given".

namespace Engine {

	Result<CommandLine> CommandLine::Parse(std::span<const std::string> /*arguments*/, std::span<const CommandLineOption> /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CommandLine::Parse is not implemented yet");
	}

	bool CommandLine::Has(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	std::optional<std::string_view> CommandLine::GetValue(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	Result<std::optional<uint64_t>> CommandLine::GetUnsigned(std::string_view /*name*/, uint64_t /*maximum*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::optional<uint64_t>();
	}

	const std::vector<std::string>& CommandLine::GetPositional() const
	{
		ENGINE_CONTRACT_STUB();
		static const std::vector<std::string> NoArguments;
		return NoArguments;
	}

	const std::vector<std::string>& CommandLine::GetArguments() const
	{
		ENGINE_CONTRACT_STUB();
		static const std::vector<std::string> NoArguments;
		return NoArguments;
	}

	std::string CommandLine::FormatUsage(std::string_view /*programName*/, std::span<const CommandLineOption> /*options*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
