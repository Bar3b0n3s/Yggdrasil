#include "EnginePCH.h"
#include "Engine/Scripting/ScriptError.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Scripting/ScriptLimits.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
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
		Json result = Json::object();
		result["id"] = error.ID;
		result["kind"] = ScriptErrorKindToString(error.Kind);
		result["script"] = error.Script;
		result["line"] = error.Line;
		result["column"] = error.Column;
		result["message"] = error.Message;
		result["callback"] = error.Callback;
		result["entity"] = { { "id", error.Entity.ToString() }, { "name", error.EntityName } };
		// The faulting behaviour's slot, after the entity it belongs to (Docs/Decisions/0031-m20-contract.md decision 17).
		if (error.Entity.IsValid())
			result["slot"] = error.Slot;
		result["tick"] = error.Tick;
		result["count"] = error.Count;
		result["traceback"] = std::move(traceback);
		if (!error.JsonPointer.empty())
			result["jsonPointer"] = error.JsonPointer;
		return result;
	}

	void BoundScriptError(ScriptError& error)
	{
		if (error.Message.size() > MaxScriptLogMessageBytes)
		{
			const std::string marker = std::format(" [truncated: {} bytes]", error.Message.size());
			error.Message.resize(MaxScriptLogMessageBytes - marker.size());
			error.Message.resize(CompleteUtf8Prefix(error.Message));
			error.Message += marker;
		}
		if (error.Traceback.size() > MaxScriptErrorTracebackFrames)
		{
			const size_t omitted = error.Traceback.size() - (MaxScriptErrorTracebackFrames - 1);
			error.Traceback.resize(MaxScriptErrorTracebackFrames - 1);
			error.Traceback.push_back({ .Script = {}, .Line = 0, .Function = std::format("[truncated: {} more frames]", omitted) });
		}
	}

	const ScriptError& ScriptErrorStream::Add(ScriptError error)
	{
		ENGINE_CORE_VERIFY(m_Cursor != std::numeric_limits<uint64_t>::max(), "Script error cursor exhausted");
		BoundScriptError(error);
		error.ID = ++m_Cursor;
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
		else if (m_Errors.size() == MaxScriptErrorEntries)
		{
			// Entries stay in ascending ID order, so the front is the error whose latest occurrence is the oldest.
			m_EvictedThrough = m_Errors.front().ID;
			++m_EvictedCount;
			m_Errors.erase(m_Errors.begin());
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
		return m_Cursor;
	}

	std::span<const ScriptError> ScriptErrorStream::GetErrors() const
	{
		return m_Errors;
	}

	uint64_t ScriptErrorStream::GetEvictedCount() const
	{
		return m_EvictedCount;
	}

	bool ScriptErrorStream::HasEvictedAfter(uint64_t since) const
	{
		return m_EvictedThrough > since;
	}

}
