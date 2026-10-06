#include "EnginePCH.h"
#include "Engine/Platform/CrashHandler.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Private/CrashHandlerReport.h"
#include "Engine/Platform/Private/PathsUtf8.h"

#include <charconv>

// The host-independent parts of the crash handler: the breadcrumb and log-line buffers, the report text and the guard
// that serializes the crash paths (Platform/Private/CrashHandlerReport.h). Installing, the OS handlers and writing report
// files live in Platform/Windows/CrashHandlerWindows.cpp and Platform/Posix/CrashHandlerPosix.cpp.
//
// The buffers are read by crash paths that may interrupt a writer at any instruction (a POSIX signal handler on the
// writing thread) or run while a writer is stopped forever (another thread crashed). They are therefore seqlocks over
// atomic characters: a writer makes the sequence odd, stores the text and makes it even again; a reader copies the text
// and keeps it only when the sequence was the same even value before and after. A reader never waits: after a few
// attempts it reports the entry as unreadable.

namespace Engine {

	namespace {

		// One breadcrumb (§4.13). Constant-initialized, so SetBreadcrumb works at any point of the process lifetime.
		struct BreadcrumbSlot
		{
			std::atomic<uint32_t> Sequence{ 0 };
			std::atomic<uint32_t> Length{ 0 };
			std::array<std::atomic<char>, CrashHandler::MaxBreadcrumbLength> Text{};
		};

		// One captured log line, written by the log listener; Index is the number of the line it holds (lines are numbered
		// from 0 in capture order and line i lives in slot i % LogLineCount).
		struct LogLineSlot
		{
			std::atomic<uint32_t> Sequence{ 0 };
			std::atomic<uint64_t> Index{ 0 };
			std::atomic<uint32_t> Length{ 0 };
			std::array<std::atomic<char>, CrashHandler::MaxLogLineLength> Text{};
		};

	}

	namespace Utils {

		// How often a reader retries an entry whose writer is active before reporting it as unreadable.
		constexpr int SeqlockReadAttempts = 3;

		// Process-level state (Architecture §3 rule 5).
		static constinit std::array<BreadcrumbSlot, CrashBreadcrumbCount> s_Breadcrumbs{};
		static constinit std::atomic<bool> s_AcceptsBreadcrumbs{ true };
		static constinit std::atomic<bool> s_IsInstalled{ false };
		static constinit std::atomic<uint64_t> s_ReportingThread{ 0 };
		// The specification of the installed handler and the log-line buffer. Written by Install and Uninstall on the main
		// thread while no handler is installed; read-only while one is.
		static std::string s_AppName;
		static std::string s_BuildInfo;
		static std::filesystem::path s_ReportDirectory;
		static Scope<LogLineSlot[]> s_LogLines;
		static constinit std::atomic<uint64_t> s_NextLogLine{ 0 };
		static uint64_t s_LogListenerID = 0;

		// Stores `text` into a seqlock-protected character array; concurrent writers of one slot take turns.
		template<size_t Capacity>
		static void WriteSlotText(std::atomic<uint32_t>& sequence, std::atomic<uint32_t>& length,
			std::array<std::atomic<char>, Capacity>& slot, std::string_view text)
		{
			uint32_t expected = sequence.load(std::memory_order_relaxed);
			while (true)
			{
				if ((expected & 1u) == 0
					&& sequence.compare_exchange_weak(expected, expected + 1, std::memory_order_acquire, std::memory_order_relaxed))
					break;
				expected = sequence.load(std::memory_order_relaxed);
			}

			const size_t count = std::min(text.size(), Capacity);
			for (size_t index = 0; index < count; ++index)
				slot[index].store(text[index], std::memory_order_relaxed);
			length.store(static_cast<uint32_t>(count), std::memory_order_relaxed);
			sequence.store(expected + 2, std::memory_order_release);
		}

		// Copies a seqlock-protected text into `output` and returns its length, or nullopt when every attempt met a writer.
		template<size_t Capacity>
		static std::optional<size_t> ReadSlotText(const std::atomic<uint32_t>& sequence, const std::atomic<uint32_t>& length,
			const std::array<std::atomic<char>, Capacity>& slot, std::array<char, Capacity>& output)
		{
			for (int attempt = 0; attempt < SeqlockReadAttempts; ++attempt)
			{
				const uint32_t before = sequence.load(std::memory_order_acquire);
				if ((before & 1u) != 0)
					continue;
				const size_t count = std::min<size_t>(length.load(std::memory_order_relaxed), Capacity);
				for (size_t index = 0; index < count; ++index)
					output[index] = slot[index].load(std::memory_order_relaxed);
				std::atomic_thread_fence(std::memory_order_acquire);
				if (sequence.load(std::memory_order_relaxed) == before)
					return count;
			}
			return std::nullopt;
		}

		// At most `maximum` bytes of `text`, cut at a UTF-8 character boundary.
		static std::string_view TruncateUtf8(std::string_view text, size_t maximum)
		{
			if (text.size() <= maximum)
				return text;
			size_t length = maximum;
			while (length > 0 && (static_cast<uint8_t>(text[length]) & 0xc0) == 0x80)
				--length;
			return text.substr(0, length);
		}

		// One log entry as a report line: "[<seconds since Log::Initialize>] [<Level>] [<Logger>] <Message>", on one line.
		static std::string FormatLogLine(const LogEntry& entry)
		{
			std::string line = std::format("[{}.{:06}] [{}] [{}] {}", entry.TimeNs / 1000000000u, (entry.TimeNs / 1000u) % 1000000u,
				LogLevelToString(entry.Level), LogChannelToString(entry.Logger), entry.Message);
			std::ranges::replace(line, '\n', ' ');
			std::ranges::replace(line, '\r', ' ');
			return line;
		}

		static void CaptureLogLine(const LogEntry& entry)
		{
			const std::string line = FormatLogLine(entry);
			const uint64_t index = s_NextLogLine.fetch_add(1, std::memory_order_relaxed);
			LogLineSlot& slot = s_LogLines[static_cast<size_t>(index % CrashHandler::LogLineCount)];
			// The index is part of the protected text: a reader that sees an older line in the slot skips it.
			uint32_t expected = slot.Sequence.load(std::memory_order_relaxed);
			while ((expected & 1u) != 0
				|| !slot.Sequence.compare_exchange_weak(expected, expected + 1, std::memory_order_acquire, std::memory_order_relaxed))
			{
				expected = slot.Sequence.load(std::memory_order_relaxed);
			}
			slot.Index.store(index, std::memory_order_relaxed);
			const std::string_view text = TruncateUtf8(line, CrashHandler::MaxLogLineLength);
			for (size_t character = 0; character < text.size(); ++character)
				slot.Text[character].store(text[character], std::memory_order_relaxed);
			slot.Length.store(static_cast<uint32_t>(text.size()), std::memory_order_relaxed);
			slot.Sequence.store(expected + 2, std::memory_order_release);
		}

		std::string_view FormatCrashDecimal(uint64_t value, std::array<char, 24>& storage)
		{
			const std::to_chars_result result = std::to_chars(storage.data(), storage.data() + storage.size(), value);
			return std::string_view(storage.data(), static_cast<size_t>(result.ptr - storage.data()));
		}

		std::string_view FormatCrashHex(uint64_t value, size_t minimumDigits, std::array<char, 24>& storage)
		{
			std::array<char, 16> digits{};
			const std::to_chars_result result = std::to_chars(digits.data(), digits.data() + digits.size(), value, 16);
			const auto digitCount = static_cast<size_t>(result.ptr - digits.data());
			const size_t padding = minimumDigits > digitCount ? std::min(minimumDigits, digits.size()) - digitCount : 0;

			size_t size = 0;
			storage[size++] = '0';
			storage[size++] = 'x';
			for (size_t index = 0; index < padding; ++index)
				storage[size++] = '0';
			for (size_t index = 0; index < digitCount; ++index)
				storage[size++] = digits[index];
			return std::string_view(storage.data(), size);
		}

		CrashReportWriter::CrashReportWriter(Sink sink, void* context)
			: m_Sink(sink), m_Context(context)
		{
		}

		CrashReportWriter::~CrashReportWriter()
		{
			Flush();
		}

		void CrashReportWriter::Append(std::string_view text)
		{
			while (!text.empty())
			{
				if (m_Size == m_Buffer.size())
					Flush();
				const size_t count = std::min(text.size(), m_Buffer.size() - m_Size);
				for (size_t index = 0; index < count; ++index)
					m_Buffer[m_Size + index] = text[index];
				m_Size += count;
				text.remove_prefix(count);
			}
		}

		void CrashReportWriter::AppendDecimal(uint64_t value)
		{
			std::array<char, 24> storage{};
			Append(FormatCrashDecimal(value, storage));
		}

		void CrashReportWriter::AppendHex(uint64_t value, size_t minimumDigits)
		{
			std::array<char, 24> storage{};
			Append(FormatCrashHex(value, minimumDigits, storage));
		}

		void CrashReportWriter::Flush()
		{
			if (m_Size == 0)
				return;
			m_Sink(m_Context, std::string_view(m_Buffer.data(), m_Size));
			m_Size = 0;
		}

		Status BeginCrashHandlerInstall(const CrashHandlerSpecification& specification)
		{
			if (s_IsInstalled.load())
			{
				ENGINE_CORE_ASSERT(false, "CrashHandler::Install called while a crash handler is installed");
				return MakeError(ErrorCode::InvalidState, "a crash handler is already installed");
			}

			if (!specification.ReportDirectory.empty())
			{
				const Status created = FileSystem::CreateDirectories(specification.ReportDirectory);
				if (!created.has_value())
				{
					return MakeError(ErrorCode::Io, "cannot create the crash report directory '{}': {}",
						NativePathToUtf8(specification.ReportDirectory), created.error().GetMessageText());
				}
			}

			s_AppName = specification.AppName;
			s_BuildInfo = specification.BuildInfo;
			s_ReportDirectory = specification.ReportDirectory;
			if (!s_ReportDirectory.empty())
			{
				// Lines logged before Install are already in the log's ring buffer; the listener keeps the buffer current
				// from here on. An entry another thread logs between the two steps is missed (Install runs on the main
				// thread during process start, while nothing else logs).
				s_LogLines = CreateScope<LogLineSlot[]>(CrashHandler::LogLineCount);
				s_NextLogLine.store(0);
				for (const LogEntry& entry : Log::GetRingBuffer().ReadLast(CrashHandler::LogLineCount))
					CaptureLogLine(entry);
				s_LogListenerID = Log::AddListener([](const LogEntry& entry)
				{
					CaptureLogLine(entry);
				});
			}
			s_AcceptsBreadcrumbs.store(true);
			return {};
		}

		void CompleteCrashHandlerInstall()
		{
			s_IsInstalled.store(true);
		}

		void ResetCrashHandlerState()
		{
			s_IsInstalled.store(false);
			if (s_LogListenerID != 0)
			{
				// When RemoveListener returns, the listener is not running and will not run again, so the buffer can go.
				Log::RemoveListener(s_LogListenerID);
				s_LogListenerID = 0;
			}
			s_LogLines.reset();
			s_NextLogLine.store(0);
			s_AppName.clear();
			s_BuildInfo.clear();
			s_ReportDirectory.clear();

			s_AcceptsBreadcrumbs.store(false);
			for (BreadcrumbSlot& slot : s_Breadcrumbs)
				WriteSlotText(slot.Sequence, slot.Length, slot.Text, {});
		}

		bool AreCrashReportsEnabled()
		{
			return !s_ReportDirectory.empty();
		}

		const std::filesystem::path& GetCrashReportDirectory()
		{
			return s_ReportDirectory;
		}

		void WriteCrashReportPreamble(CrashReportWriter& writer, std::span<const std::string_view> reasonParts, uint64_t processId)
		{
			writer.Append("Crash report\nReason: ");
			for (const std::string_view part : reasonParts)
				writer.Append(part);
			writer.Append("\nApp: ");
			writer.Append(s_AppName);
			writer.Append("\nBuild: ");
			writer.Append(s_BuildInfo);
			writer.Append("\nProcess: ");
			writer.AppendDecimal(processId);
			writer.Append("\nBreadcrumbs:\n");

			std::array<char, CrashHandler::MaxBreadcrumbLength> text{};
			for (size_t index = 0; index < s_Breadcrumbs.size(); ++index)
			{
				const BreadcrumbSlot& slot = s_Breadcrumbs[index];
				writer.Append("  ");
				writer.Append(CrashBreadcrumbToString(static_cast<CrashBreadcrumb>(index)));
				writer.Append(": ");
				const std::optional<size_t> length = ReadSlotText(slot.Sequence, slot.Length, slot.Text, text);
				if (!length.has_value())
					writer.Append("(being written)");
				else if (*length == 0)
					writer.Append("(none)");
				else
					writer.Append(std::string_view(text.data(), *length));
				writer.Append("\n");
			}
		}

		void WriteCrashReportLogLines(CrashReportWriter& writer)
		{
			writer.Append("Last log lines:\n");
			if (s_LogLines == nullptr)
				return;

			const uint64_t next = s_NextLogLine.load(std::memory_order_acquire);
			const uint64_t first = next > CrashHandler::LogLineCount ? next - CrashHandler::LogLineCount : 0;
			std::array<char, CrashHandler::MaxLogLineLength> text{};
			for (uint64_t index = first; index < next; ++index)
			{
				const LogLineSlot& slot = s_LogLines[static_cast<size_t>(index % CrashHandler::LogLineCount)];
				for (int attempt = 0; attempt < SeqlockReadAttempts; ++attempt)
				{
					const uint32_t before = slot.Sequence.load(std::memory_order_acquire);
					if ((before & 1u) != 0)
						continue;
					const uint64_t slotIndex = slot.Index.load(std::memory_order_relaxed);
					const size_t count = std::min<size_t>(slot.Length.load(std::memory_order_relaxed), text.size());
					for (size_t character = 0; character < count; ++character)
						text[character] = slot.Text[character].load(std::memory_order_relaxed);
					std::atomic_thread_fence(std::memory_order_acquire);
					if (slot.Sequence.load(std::memory_order_relaxed) != before)
						continue;
					// A line whose writer has not stored it yet leaves an older line in the slot; it is skipped.
					if (slotIndex == index)
					{
						writer.Append("  ");
						writer.Append(std::string_view(text.data(), count));
						writer.Append("\n");
					}
					break;
				}
			}
		}

		void FormatCrashReportStem(CrashText<64>& stem, uint64_t epochSeconds, uint64_t processId, uint32_t attempt)
		{
			stem.Clear();
			stem.Append("crash-");
			stem.AppendDecimal(epochSeconds);
			stem.Append("-");
			stem.AppendDecimal(processId);
			if (attempt > 1)
			{
				stem.Append("-");
				stem.AppendDecimal(attempt);
			}
		}

		CrashReportEntry EnterCrashReport(uint64_t threadId)
		{
			uint64_t expected = 0;
			if (s_ReportingThread.compare_exchange_strong(expected, threadId))
				return CrashReportEntry::Entered;
			return expected == threadId ? CrashReportEntry::Recursive : CrashReportEntry::Busy;
		}

		void LeaveCrashReport()
		{
			s_ReportingThread.store(0);
		}

	}

	bool CrashHandler::IsInstalled()
	{
		return Utils::s_IsInstalled.load();
	}

	void CrashHandler::SetBreadcrumb(CrashBreadcrumb breadcrumb, std::string_view value)
	{
		const auto index = static_cast<size_t>(std::to_underlying(breadcrumb));
		if (index >= Utils::s_Breadcrumbs.size())
		{
			ENGINE_CORE_ASSERT(false, "Unknown CrashBreadcrumb {}", std::to_underlying(breadcrumb));
			return;
		}
		if (!Utils::s_AcceptsBreadcrumbs.load(std::memory_order_relaxed))
			return;
		BreadcrumbSlot& slot = Utils::s_Breadcrumbs[index];
		Utils::WriteSlotText(slot.Sequence, slot.Length, slot.Text, Utils::TruncateUtf8(value, MaxBreadcrumbLength));
	}

	std::string_view CrashBreadcrumbToString(CrashBreadcrumb breadcrumb)
	{
		switch (breadcrumb)
		{
			case CrashBreadcrumb::Scene:            return "Scene";
			case CrashBreadcrumb::PlayState:        return "PlayState";
			case CrashBreadcrumb::AutomationMethod: return "AutomationMethod";
			case CrashBreadcrumb::ScriptCallback:   return "ScriptCallback";
			case CrashBreadcrumb::FramePhase:       return "FramePhase";
		}

		ENGINE_CORE_ASSERT(false, "Unknown CrashBreadcrumb {}", std::to_underlying(breadcrumb));
		return "Unknown";
	}

}
