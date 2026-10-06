#include "EnginePCH.h"
#include "Engine/Platform/Private/ProcessOutput.h"

namespace Engine {

	namespace Utils {

		// Timeouts beyond this behave like it: long enough to mean "forever", short enough that adding it to now and
		// converting it for the OS never overflows.
		static constexpr std::chrono::milliseconds MaxProcessTimeout = std::chrono::hours(24 * 365);

		static size_t StreamIndex(ProcessStream stream)
		{
			return stream == ProcessStream::StandardOutput ? 0 : 1;
		}

		static std::string_view StreamName(ProcessStream stream)
		{
			return stream == ProcessStream::StandardOutput ? "standard output" : "standard error";
		}

		ProcessClock::time_point MakeProcessDeadline(std::chrono::milliseconds timeout)
		{
			return ProcessClock::now() + std::clamp(timeout, std::chrono::milliseconds(0), MaxProcessTimeout);
		}

		int64_t GetMillisecondsUntil(ProcessClock::time_point deadline, int64_t maximum)
		{
			const ProcessClock::duration remaining = deadline - ProcessClock::now();
			const int64_t milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
			return std::clamp<int64_t>(milliseconds, 0, maximum);
		}

		void ProcessOutput::Append(ProcessStream stream, std::string_view text)
		{
			{
				std::scoped_lock lock(m_Mutex);
				if (!m_IsTaken)
					m_Text[StreamIndex(stream)].append(text);
			}
			m_Changed.notify_all();
		}

		void ProcessOutput::MarkEnded(ProcessStream stream)
		{
			{
				std::scoped_lock lock(m_Mutex);
				m_Ended[StreamIndex(stream)] = true;
			}
			m_Changed.notify_all();
		}

		void ProcessOutput::SetExitCode(int exitCode)
		{
			{
				std::scoped_lock lock(m_Mutex);
				if (!m_ExitCode.has_value())
					m_ExitCode = exitCode;
			}
			m_Changed.notify_all();
		}

		bool ProcessOutput::HasEnded(ProcessStream stream) const
		{
			std::scoped_lock lock(m_Mutex);
			return m_Ended[StreamIndex(stream)];
		}

		std::optional<int> ProcessOutput::GetExitCode() const
		{
			std::scoped_lock lock(m_Mutex);
			return m_ExitCode;
		}

		bool ProcessOutput::WaitUntilEnded(ProcessStream stream, ProcessClock::time_point deadline)
		{
			std::unique_lock lock(m_Mutex);
			const size_t index = StreamIndex(stream);
			return m_Changed.wait_until(lock, deadline, [this, index]()
			{
				return m_Ended[index];
			});
		}

		bool ProcessOutput::WaitUntilFinished(ProcessClock::time_point deadline, bool requireExitCode)
		{
			std::unique_lock lock(m_Mutex);
			return m_Changed.wait_until(lock, deadline, [this, requireExitCode]()
			{
				return m_Ended[0] && m_Ended[1] && (!requireExitCode || m_ExitCode.has_value());
			});
		}

		bool ProcessOutput::WaitForExitCode(ProcessClock::time_point deadline)
		{
			std::unique_lock lock(m_Mutex);
			return m_Changed.wait_until(lock, deadline, [this]()
			{
				return m_ExitCode.has_value();
			});
		}

		Status ProcessOutput::WaitForText(ProcessStream stream, std::string_view text, ProcessClock::time_point deadline)
		{
			const size_t index = StreamIndex(stream);
			std::unique_lock lock(m_Mutex);
			// Only the part appended since the previous look can complete a match, so each wake-up searches from just
			// before it instead of from the start.
			size_t searchFrom = 0;
			while (true)
			{
				if (m_IsTaken)
					return MakeError(ErrorCode::InvalidState, "the output of the process was already collected by Wait");

				const std::string& captured = m_Text[index];
				if (captured.find(text, searchFrom) != std::string::npos)
					return {};
				searchFrom = captured.size() >= text.size() ? captured.size() - text.size() + 1 : 0;

				if (m_Ended[index])
					return MakeError(ErrorCode::NotFound, "the {} of the process ended without \"{}\"", StreamName(stream), text);
				if (ProcessClock::now() >= deadline)
					return MakeError(ErrorCode::Timeout, "the {} of the process did not contain \"{}\" in time", StreamName(stream), text);
				static_cast<void>(m_Changed.wait_until(lock, deadline)); // the loop re-checks every condition
			}
		}

		std::array<std::string, 2> ProcessOutput::TakeText()
		{
			std::array<std::string, 2> text;
			{
				std::scoped_lock lock(m_Mutex);
				text = std::move(m_Text);
				m_Text = {};
				m_IsTaken = true;
			}
			m_Changed.notify_all();
			return text;
		}

		bool ProcessOutput::IsTaken() const
		{
			std::scoped_lock lock(m_Mutex);
			return m_IsTaken;
		}

	}

}
