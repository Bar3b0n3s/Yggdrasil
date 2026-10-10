#include "EnginePCH.h"
#include "Engine/Testing/TestResults.h"

#include "Engine/Core/Error.h"

#include <nlohmann/json.hpp>

namespace Engine {

	void RegisterTestResultTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<Json> TestRunResultToJson(const TestRunResult& /*result*/, const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 shared test result JSON contract"));
	}

	Result<std::string> TestRunResultToJUnit(const TestRunResult& /*result*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M13 shared test result JUnit contract"));
	}

}
