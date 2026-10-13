#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace Engine {

	enum class EditorHostErrorLevel : uint8_t
	{
		Warning,
		Error
	};

	// One recurring host failure channel of EditorApp (a UI draw, the UI's safe-point actions, the host services,
	// autosave). A failure that repeats every frame is logged once, when it first appears or its error changes, instead of
	// flooding the log ring buffer at the frame rate (CodeStyle §11); a success re-arms the channel, so the same failure
	// returning later is logged again. Main thread.
	class EditorHostErrorLog
	{
	public:
		// Logs "<what>: <error>" at `level` when `status` failed with an error other than the one this channel last logged,
		// and clears the channel when it succeeded. Returns whether it logged.
		bool Report(std::string_view what, const Status& status, EditorHostErrorLevel level = EditorHostErrorLevel::Error);
		// The error last logged and not yet cleared by a success; empty while the channel is healthy.
		[[nodiscard]] const std::string& GetLastError() const { return m_LastError; }
	private:
		std::string m_LastError{};
	};

}
