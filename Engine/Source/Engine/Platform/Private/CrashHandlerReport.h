#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/CrashHandler.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>

// The host-independent half of the crash handler (Platform/CrashHandler.cpp), shared by Platform/Windows/
// CrashHandlerWindows.cpp and Platform/Posix/CrashHandlerPosix.cpp: the process-level state that Install sets up, the
// report text and the guard that serializes the crash paths. Everything a crash path calls here is allocation-free and
// lock-free, so it may run in a POSIX signal handler and in a Windows process whose heap is corrupt.

namespace Engine {

	namespace Utils {

		// Formats unsigned integers without allocating (std::to_chars).
		[[nodiscard]] std::string_view FormatCrashDecimal(uint64_t value, std::array<char, 24>& storage);
		// "0x" and the lowercase hexadecimal digits of `value`, at least `minimumDigits` of them.
		[[nodiscard]] std::string_view FormatCrashHex(uint64_t value, size_t minimumDigits, std::array<char, 24>& storage);

		// Text in a fixed buffer; whatever does not fit is dropped. For reasons and file names on the crash paths.
		template<size_t Capacity>
		class CrashText
		{
		public:
			void Append(std::string_view text)
			{
				const size_t count = std::min(text.size(), Capacity - m_Size);
				for (size_t index = 0; index < count; ++index)
					m_Buffer[m_Size + index] = text[index];
				m_Size += count;
			}

			void AppendDecimal(uint64_t value)
			{
				std::array<char, 24> storage{};
				Append(FormatCrashDecimal(value, storage));
			}

			void AppendHex(uint64_t value, size_t minimumDigits = 1)
			{
				std::array<char, 24> storage{};
				Append(FormatCrashHex(value, minimumDigits, storage));
			}

			void Clear() { m_Size = 0; }

			[[nodiscard]] std::string_view GetText() const { return std::string_view(m_Buffer.data(), m_Size); }
		private:
			std::array<char, Capacity> m_Buffer{};
			size_t m_Size = 0;
		};

		// Text collected in a fixed buffer and handed to a sink whenever the buffer fills and at Flush, without allocating.
		class CrashReportWriter
		{
		public:
			// Receives the text in order. Must be async-signal-safe where the writer runs in a signal handler.
			using Sink = void (*)(void* context, std::string_view text);

			CrashReportWriter(Sink sink, void* context);
			// Flushes.
			~CrashReportWriter();

			CrashReportWriter(const CrashReportWriter&) = delete;
			CrashReportWriter& operator=(const CrashReportWriter&) = delete;
			CrashReportWriter(CrashReportWriter&&) = delete;
			CrashReportWriter& operator=(CrashReportWriter&&) = delete;

			void Append(std::string_view text);
			void AppendDecimal(uint64_t value);
			void AppendHex(uint64_t value, size_t minimumDigits = 1);
			void Flush();
		private:
			Sink m_Sink = nullptr;
			void* m_Context = nullptr;
			std::array<char, 2048> m_Buffer{};
			size_t m_Size = 0;
		};

		// Install, main thread. Checks that no handler is installed, creates the report directory, records the
		// specification's text for reports and, when reports are enabled, allocates the log-line buffer, fills it with the
		// latest entries of the log's ring buffer and registers the log listener that keeps it current. The OS half calls it
		// before installing its handlers, then CompleteCrashHandlerInstall, or ResetCrashHandlerState when that fails.
		[[nodiscard]] Status BeginCrashHandlerInstall(const CrashHandlerSpecification& specification);
		void CompleteCrashHandlerInstall();

		// Uninstall, main thread, after the OS handlers were removed: removes the listener, frees the buffer, clears the
		// breadcrumbs and stops accepting new ones. Idempotent.
		void ResetCrashHandlerState();

		// Whether the installed handler writes reports (a non-empty ReportDirectory), and where.
		[[nodiscard]] bool AreCrashReportsEnabled();
		[[nodiscard]] const std::filesystem::path& GetCrashReportDirectory();

		// The report from its first line through the breadcrumbs: "Crash report", "Reason: " followed by `reasonParts`, App,
		// Build, "Process: <processId>" and "Breadcrumbs:" with one line per breadcrumb. The OS half writes "Stack trace:"
		// and its frames next.
		void WriteCrashReportPreamble(CrashReportWriter& writer, std::span<const std::string_view> reasonParts, uint64_t processId);

		// "Last log lines:" and the captured entries, oldest first.
		void WriteCrashReportLogLines(CrashReportWriter& writer);

		// A report's file name without its extension: "crash-<epochSeconds>-<processId>", plus "-<attempt>" after the
		// first attempt (a name taken by an earlier report of the same second and process).
		void FormatCrashReportStem(CrashText<64>& stem, uint64_t epochSeconds, uint64_t processId, uint32_t attempt);

		// How many names the OS halves try for one report: the attempts 1 to MaxCrashReportAttempts of
		// FormatCrashReportStem, so "-2" to "-16" for reports written in the same second by the same process.
		inline constexpr uint32_t MaxCrashReportAttempts = 16;

		// One crash path at a time: the first thread to enter writes the report and ends the process.
		enum class CrashReportEntry : uint8_t
		{
			Entered,   // this thread writes the report; LeaveCrashReport when done (WriteFatalErrorReport returns)
			Recursive, // this thread is already writing one: the report path itself crashed
			Busy       // another thread is writing one and will end the process
		};

		// `threadId` is the OS's nonzero ID of the calling thread.
		[[nodiscard]] CrashReportEntry EnterCrashReport(uint64_t threadId);
		void LeaveCrashReport();

	}

}
