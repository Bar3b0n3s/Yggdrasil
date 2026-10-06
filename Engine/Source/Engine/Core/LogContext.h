#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace Engine {

	// The per-thread context recorded into every LogEntry (Architecture §4.4): the simulation tick (set by the play
	// session around a step), and the entity and script location (set by the script engine around callbacks).
	struct LogContext
	{
		std::optional<uint64_t> Tick;
		UUID Entity;                 // invalid = none
		std::string_view ScriptFile; // empty = none; must outlive every scope that carries it
		uint32_t ScriptLine = 0;     // 0 = none
	};

	// Replaces the calling thread's log context for its lifetime and restores the previous one on destruction. Scopes
	// nest; to change one field, copy GetCurrent(), modify the copy and open a scope with it:
	//     LogContext context = LogContextScope::GetCurrent();
	//     context.Entity = entity.GetUUID();
	//     LogContextScope scope(context);
	// A scope must be destroyed on the thread that created it, in reverse order of creation (it is a stack object).
	// The context is thread-local: other threads, and jobs started from inside a scope, do not see it.
	class LogContextScope
	{
	public:
		explicit LogContextScope(const LogContext& context);
		~LogContextScope();

		LogContextScope(const LogContextScope&) = delete;
		LogContextScope& operator=(const LogContextScope&) = delete;
		LogContextScope(LogContextScope&&) = delete;
		LogContextScope& operator=(LogContextScope&&) = delete;

		// The calling thread's current context; a default-constructed LogContext when no scope is open. The reference is
		// valid until the calling thread opens or closes a scope.
		[[nodiscard]] static const LogContext& GetCurrent();
	private:
		LogContext m_Previous;
	};

}
