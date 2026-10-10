#pragma once

#include "Engine/Core/Json/Json.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	enum class ScriptErrorKind : uint8_t
	{
		Compile,
		Type,
		Runtime,
		Timeout,
		Memory
	};

	struct ScriptTraceFrame
	{
		std::string Script{};
		uint32_t Line = 0;
		std::string Function{};
	};

	// Owned diagnostic data: no VM strings or entity pointers survive a protected call or a scene restart (§11.7).
	struct ScriptError
	{
		uint64_t ID = 0;
		ScriptErrorKind Kind = ScriptErrorKind::Runtime;
		std::string Script{};
		uint32_t Line = 0;
		uint32_t Column = 0;
		std::string Message{};
		std::string Callback{};
		UUID Entity{};
		std::string EntityName{};
		uint64_t Tick = 0;
		uint64_t Count = 1;
		std::vector<ScriptTraceFrame> Traceback{};
		std::string JsonPointer{}; // embedded source, e.g. /Expect/0/Luau; Line/Column refer to its decoded Luau bytes
	};

	// Lowercase protocol spelling and the §11.7 JSON representation. Pure; ToJson never consults a live scene.
	[[nodiscard]] std::string_view ScriptErrorKindToString(ScriptErrorKind kind);
	[[nodiscard]] Json ScriptErrorToJson(const ScriptError& error);

	// Main-thread error stream of a VM/session. Each Add gets a new monotonically increasing ID, including repeats.
	// Deduplicates by (Script, JsonPointer, Line, Message): replaces that entry's location/context with its latest occurrence and
	// increments Count. Read returns latest entries with ID > since in ascending ID order, so a repeated error is
	// observable after a previous cursor. GetErrors has the same order; references/spans expire on the next Add.
	class ScriptErrorStream
	{
	public:
		[[nodiscard]] const ScriptError& Add(ScriptError error);
		[[nodiscard]] std::vector<ScriptError> Read(uint64_t since = 0, uint32_t limit = 100) const;
		[[nodiscard]] uint64_t GetCursor() const;
		[[nodiscard]] std::span<const ScriptError> GetErrors() const;
	private:
		std::vector<ScriptError> m_Errors{};
	};

}
