#include "EnginePCH.h"
#include "Engine/Scripting/ScriptError.h"

#include <nlohmann/json.hpp>

namespace Engine {

	std::string_view ScriptErrorKindToString(ScriptErrorKind /*kind*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Json ScriptErrorToJson(const ScriptError& /*error*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	const ScriptError& ScriptErrorStream::Add(ScriptError error)
	{
		ENGINE_CONTRACT_STUB();
		m_Errors.push_back(std::move(error));
		return m_Errors.back();
	}

	std::vector<ScriptError> ScriptErrorStream::Read(uint64_t /*since*/, uint32_t /*limit*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	uint64_t ScriptErrorStream::GetCursor() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::span<const ScriptError> ScriptErrorStream::GetErrors() const
	{
		ENGINE_CONTRACT_STUB();
		return m_Errors;
	}

}
