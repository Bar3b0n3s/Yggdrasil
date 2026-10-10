#include "EnginePCH.h"
#include "Engine/Scripting/ScriptError.h"

#include "Engine/Core/Assert.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace Engine {

	std::string_view ScriptErrorKindToString(ScriptErrorKind kind)
	{
		switch (kind)
		{
			case ScriptErrorKind::Compile: return "compile";
			case ScriptErrorKind::Type:    return "type";
			case ScriptErrorKind::Runtime: return "runtime";
			case ScriptErrorKind::Timeout: return "timeout";
			case ScriptErrorKind::Memory:  return "memory";
		}
		ENGINE_CORE_ASSERT(false, "Unknown ScriptErrorKind {}", std::to_underlying(kind));
		return "runtime";
	}

	Json ScriptErrorToJson(const ScriptError& error)
	{
		Json traceback = Json::array();
		for (const ScriptTraceFrame& frame : error.Traceback)
			traceback.push_back({ { "script", frame.Script }, { "line", frame.Line }, { "function", frame.Function } });
		Json result = {
			{ "id", error.ID },
			{ "kind", ScriptErrorKindToString(error.Kind) },
			{ "script", error.Script },
			{ "line", error.Line },
			{ "column", error.Column },
			{ "message", error.Message },
			{ "callback", error.Callback },
			{ "entity", { { "id", error.Entity.ToString() }, { "name", error.EntityName } } },
			{ "tick", error.Tick },
			{ "count", error.Count },
			{ "traceback", std::move(traceback) },
		};
		if (!error.JsonPointer.empty())
			result["jsonPointer"] = error.JsonPointer;
		return result;
	}

	const ScriptError& ScriptErrorStream::Add(ScriptError error)
	{
		const uint64_t cursor = GetCursor();
		ENGINE_CORE_VERIFY(cursor != std::numeric_limits<uint64_t>::max(), "Script error cursor exhausted");
		error.ID = cursor + 1;
		error.Count = 1;
		const auto previous = std::ranges::find_if(m_Errors, [&error](const ScriptError& candidate)
		{
			return candidate.Script == error.Script && candidate.JsonPointer == error.JsonPointer
				&& candidate.Line == error.Line && candidate.Message == error.Message;
		});
		if (previous != m_Errors.end())
		{
			ENGINE_CORE_VERIFY(previous->Count != std::numeric_limits<uint64_t>::max(), "Script error count exhausted");
			error.Count = previous->Count + 1;
			m_Errors.erase(previous);
		}
		m_Errors.push_back(std::move(error));
		return m_Errors.back();
	}

	std::vector<ScriptError> ScriptErrorStream::Read(uint64_t since, uint32_t limit) const
	{
		std::vector<ScriptError> result;
		result.reserve(std::min(m_Errors.size(), static_cast<size_t>(limit)));
		for (const ScriptError& error : m_Errors)
		{
			if (result.size() == limit)
				break;
			if (error.ID > since)
				result.push_back(error);
		}
		return result;
	}

	uint64_t ScriptErrorStream::GetCursor() const
	{
		return m_Errors.empty() ? 0 : m_Errors.back().ID;
	}

	std::span<const ScriptError> ScriptErrorStream::GetErrors() const
	{
		return m_Errors;
	}

}
