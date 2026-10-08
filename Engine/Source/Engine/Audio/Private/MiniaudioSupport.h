#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Result.h"

#include <miniaudio.h>

#include <string_view>

// Small pieces of miniaudio glue shared by the Audio module's translation units (Docs/Decisions/0015-m12-decisions.md
// decision 1: miniaudio stays behind Audio/Private and the module's .cpp files).

namespace Engine {

	// miniaudio's description of `result` ("Does not exist", "Invalid data", ...).
	[[nodiscard]] std::string_view DescribeMiniaudioResult(ma_result result);

	// An error for a failed miniaudio call: Unsupported when the platform or build lacks the feature (no backend, not
	// implemented), Io otherwise; the message is "<action>: <description> (miniaudio result <n>)".
	[[nodiscard]] Error MakeMiniaudioError(ma_result result, std::string_view action);

	// A miniaudio log (ma_log) whose messages go to the engine log at Trace: miniaudio reports every failure through a
	// result too, which the engine turns into an Error or a warning of its own, so its own messages are diagnostics only.
	// Thread-safe (ma_log has its own lock; the engine log is thread-safe). Not copyable or movable: miniaudio keeps its
	// address.
	class MiniaudioLog
	{
	public:
		MiniaudioLog() = default;
		~MiniaudioLog();

		MiniaudioLog(const MiniaudioLog&) = delete;
		MiniaudioLog& operator=(const MiniaudioLog&) = delete;

		// Initializes the log and registers the forwarding callback. Errors: as MakeMiniaudioError.
		[[nodiscard]] Status Initialize();

		// The log for miniaudio's configs; null before Initialize succeeded.
		[[nodiscard]] ma_log* Get() { return m_Initialized ? &m_Log : nullptr; }
	private:
		ma_log m_Log{};
		bool m_Initialized = false;
	};

}
