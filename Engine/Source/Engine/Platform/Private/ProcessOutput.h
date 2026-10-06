#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/Process.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

// What a Process shares with the threads that read its child's output (Platform/Windows/ProcessWindows.cpp,
// Platform/Posix/ProcessPosix.cpp): the captured text of both streams, whether each reached end of file, and the exit code
// once a thread observed it. Every member is thread-safe; waiters are woken by every change.

namespace Engine {

	namespace Utils {

		using ProcessClock = std::chrono::steady_clock;

		// The point in time `timeout` from now. A negative timeout is now; a huge one saturates instead of overflowing.
		[[nodiscard]] ProcessClock::time_point MakeProcessDeadline(std::chrono::milliseconds timeout);

		// The milliseconds left until `deadline`, rounded up and clamped to [0, `maximum`], for OS waits.
		[[nodiscard]] int64_t GetMillisecondsUntil(ProcessClock::time_point deadline, int64_t maximum);

		class ProcessOutput
		{
		public:
			ProcessOutput() = default;

			ProcessOutput(const ProcessOutput&) = delete;
			ProcessOutput& operator=(const ProcessOutput&) = delete;
			ProcessOutput(ProcessOutput&&) = delete;
			ProcessOutput& operator=(ProcessOutput&&) = delete;

			// Reader side: appends what the child wrote, and records the end of a stream.
			void Append(ProcessStream stream, std::string_view text);
			void MarkEnded(ProcessStream stream);

			// Records the child's exit code; the first call wins.
			void SetExitCode(int exitCode);

			[[nodiscard]] bool HasEnded(ProcessStream stream) const;
			[[nodiscard]] std::optional<int> GetExitCode() const;

			// Waits until `stream` reached end of file; false when the deadline passed first.
			[[nodiscard]] bool WaitUntilEnded(ProcessStream stream, ProcessClock::time_point deadline);

			// Waits until both streams reached end of file and, with `requireExitCode`, an exit code was recorded; false when
			// the deadline passed first.
			[[nodiscard]] bool WaitUntilFinished(ProcessClock::time_point deadline, bool requireExitCode);

			// Waits until an exit code was recorded; false when the deadline passed first.
			[[nodiscard]] bool WaitForExitCode(ProcessClock::time_point deadline);

			// Process::WaitForOutput: Timeout, NotFound when the stream ended without `text`, InvalidState after TakeText.
			[[nodiscard]] Status WaitForText(ProcessStream stream, std::string_view text, ProcessClock::time_point deadline);

			// Moves the captured text out (standard output, then standard error); afterwards IsTaken is true.
			[[nodiscard]] std::array<std::string, 2> TakeText();
			[[nodiscard]] bool IsTaken() const;
		private:
			mutable std::mutex m_Mutex;
			std::condition_variable m_Changed;
			std::array<std::string, 2> m_Text{};
			std::array<bool, 2> m_Ended{};
			std::optional<int> m_ExitCode{};
			bool m_IsTaken = false;
		};

	}

}
