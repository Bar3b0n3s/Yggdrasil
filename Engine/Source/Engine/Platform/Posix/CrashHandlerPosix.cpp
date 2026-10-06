#include "EnginePCH.h"
#include "Engine/Platform/CrashHandler.h"

// The crash handler on Linux and macOS: sigaction for SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT, running on an
// alternate signal stack so that a stack overflow on the installing (main) thread is reported too. The handler uses only
// async-signal-safe calls (open, write, close, clock_gettime, getpid, _exit) plus backtrace and dladdr, which Install
// warms up so that they do not allocate in the handler. It writes the report, prints one line to stderr and ends the
// process with exit code 4.

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

	#include "Engine/Platform/Private/CrashHandlerReport.h"

	#include <dlfcn.h>
	#include <execinfo.h>
	#include <fcntl.h>
	#include <pthread.h>
	#include <signal.h>
	#include <time.h>
	#include <unistd.h>

	#include <cerrno>
	#include <cstdlib>
	#include <system_error>

namespace Engine {

	namespace {

		constexpr std::array<int, 5> CrashSignals = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };

		// The OS handlers this one replaced, restored by Uninstall, the alternate stack and the report path buffer.
		// Written only while no handler is installed (Install and Uninstall, main thread).
		struct PosixCrashState
		{
			std::array<struct sigaction, CrashSignals.size()> PreviousActions{};
			size_t InstalledActionCount = 0;
			stack_t PreviousAlternateStack{};
			bool IsAlternateStackInstalled = false;
			Scope<std::byte[]> AlternateStack;
			// "<ReportDirectory>/" and a NUL-terminated buffer large enough for it plus a report file name.
			std::string ReportPathPrefix;
			std::vector<char> ReportPath;
			size_t ReportPathLength = 0;
		};

	}

	namespace Utils {

		constexpr int MaxStackFrames = 64;
		// The longest file name a report path appends to its directory: "/crash-<20 digits>-<10 digits>-<2 digits>.txt".
		constexpr size_t MaxReportFileNameLength = 64;
		// Room for the handler's own frames, the report writer and backtrace on any host (macOS's SIGSTKSZ is 128 KiB).
		constexpr size_t AlternateStackSize = 256 * 1024;

		static PosixCrashState s_State;

		// The kernel's ID of the calling thread: nonzero, and safe to read in a signal handler.
		static uint64_t GetCurrentThreadId()
		{
	#if defined(ENGINE_PLATFORM_LINUX)
			return static_cast<uint64_t>(gettid());
	#elif defined(ENGINE_PLATFORM_MACOS)
			uint64_t id = 0;
			pthread_threadid_np(nullptr, &id);
			return id;
	#endif
		}

		static void WriteToDescriptor(void* context, std::string_view text)
		{
			const int descriptor = *static_cast<const int*>(context);
			while (!text.empty())
			{
				const ssize_t written = write(descriptor, text.data(), text.size());
				if (written < 0 && errno == EINTR)
					continue;
				if (written <= 0)
					return;
				text.remove_prefix(static_cast<size_t>(written));
			}
		}

		static std::string_view GetSignalDescription(int signalNumber)
		{
			switch (signalNumber)
			{
				case SIGSEGV: return "SIGSEGV (segmentation fault)";
				case SIGBUS:  return "SIGBUS (bus error)";
				case SIGILL:  return "SIGILL (illegal instruction)";
				case SIGFPE:  return "SIGFPE (arithmetic exception)";
				case SIGABRT: return "SIGABRT (abort)";
				default:      return "Fatal signal";
			}
		}

		// Whether the signal was sent (raise, kill) rather than raised by a faulting instruction, whose address then means
		// nothing.
		static bool IsSentSignal(const siginfo_t& information)
		{
	#if defined(ENGINE_PLATFORM_LINUX)
			return information.si_code <= 0; // SI_USER, SI_QUEUE, SI_TKILL and the other sender codes
	#elif defined(ENGINE_PLATFORM_MACOS)
			return information.si_code == SI_USER || information.si_code == SI_QUEUE;
	#endif
		}

		// "SIGSEGV (segmentation fault) at address 0x0000000000000010", or "..., sent by a process".
		static void DescribeSignal(int signalNumber, const siginfo_t* information, CrashText<256>& reason)
		{
			reason.Append(GetSignalDescription(signalNumber));
			if (information == nullptr)
				return;
			if (IsSentSignal(*information))
			{
				reason.Append(", sent by a process");
				return;
			}
			if (signalNumber != SIGABRT)
			{
				reason.Append(" at address ");
				reason.AppendHex(reinterpret_cast<uintptr_t>(information->si_addr), 16);
			}
		}

		// One line per frame: "  #<n> <address> <module>(<function>+<offset>)", or "<module>+<offset>" when the function is
		// not exported (an offset into the module, for addr2line or atos).
		static void WriteStackTrace(CrashReportWriter& writer)
		{
			writer.Append("Stack trace:\n");
			std::array<void*, MaxStackFrames> frames{};
			const int frameCount = backtrace(frames.data(), MaxStackFrames);
			for (int index = 0; index < frameCount; ++index)
			{
				const auto address = reinterpret_cast<uintptr_t>(frames[static_cast<size_t>(index)]);
				writer.Append("  #");
				writer.AppendDecimal(static_cast<uint64_t>(index));
				writer.Append(" ");
				writer.AppendHex(address, 16);

				Dl_info information{};
				if (dladdr(frames[static_cast<size_t>(index)], &information) != 0 && information.dli_fname != nullptr)
				{
					std::string_view module(information.dli_fname);
					const size_t slash = module.rfind('/');
					if (slash != std::string_view::npos)
						module.remove_prefix(slash + 1);
					writer.Append(" ");
					writer.Append(module);
					if (information.dli_sname != nullptr)
					{
						writer.Append("(");
						writer.Append(std::string_view(information.dli_sname));
						writer.Append("+");
						writer.AppendHex(address - reinterpret_cast<uintptr_t>(information.dli_saddr));
						writer.Append(")");
					}
					else
					{
						writer.Append("+");
						writer.AppendHex(address - reinterpret_cast<uintptr_t>(information.dli_fbase));
					}
				}
				writer.Append("\n");
			}
			if (frameCount <= 0)
				writer.Append("  (unavailable)\n");
		}

		// Composes "<prefix><stem>.txt" in s_State.ReportPath (NUL-terminated).
		static void ComposeReportPath(const CrashText<64>& stem)
		{
			size_t size = 0;
			for (const char character : s_State.ReportPathPrefix)
				s_State.ReportPath[size++] = character;
			for (const char character : stem.GetText())
				s_State.ReportPath[size++] = character;
			for (const char character : std::string_view(".txt"))
				s_State.ReportPath[size++] = character;
			s_State.ReportPath[size] = '\0';
			s_State.ReportPathLength = size;
		}

		// Writes the text report; true when it was written. Async-signal-safe.
		static bool WriteReportFile(std::span<const std::string_view> reasonParts)
		{
			timespec now{};
			clock_gettime(CLOCK_REALTIME, &now);
			const auto processId = static_cast<uint64_t>(getpid());

			CrashText<64> stem;
			int file = -1;
			for (uint32_t attempt = 1; attempt <= MaxCrashReportAttempts && file < 0; ++attempt)
			{
				FormatCrashReportStem(stem, static_cast<uint64_t>(now.tv_sec), processId, attempt);
				ComposeReportPath(stem);
				do
				{
					file = open(s_State.ReportPath.data(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
				} while (file < 0 && errno == EINTR);
				if (file < 0 && errno != EEXIST)
					return false;
			}
			if (file < 0)
				return false;

			{
				CrashReportWriter writer(&WriteToDescriptor, &file);
				WriteCrashReportPreamble(writer, reasonParts, processId);
				WriteStackTrace(writer);
				WriteCrashReportLogLines(writer);
			}
			close(file);
			return true;
		}

		// "Crash: <reason>; report written to <path>" or "...; no report written", on stderr.
		static void WriteStandardErrorLine(std::span<const std::string_view> reasonParts, bool isWritten)
		{
			int output = STDERR_FILENO;
			CrashReportWriter writer(&WriteToDescriptor, &output);
			writer.Append("Crash: ");
			for (const std::string_view part : reasonParts)
				writer.Append(part);
			if (isWritten)
			{
				writer.Append("; report written to ");
				writer.Append(std::string_view(s_State.ReportPath.data(), s_State.ReportPathLength));
			}
			else
			{
				writer.Append("; no report written");
			}
			writer.Append("\n");
		}

		static void OnCrashSignal(int signalNumber, siginfo_t* information, void* /*context*/)
		{
			switch (EnterCrashReport(GetCurrentThreadId()))
			{
				case CrashReportEntry::Entered:
					break;
				case CrashReportEntry::Recursive:
					_exit(FatalCrashExitCode);
				case CrashReportEntry::Busy:
					// Another thread is reporting its crash and will end the process.
					while (true)
						pause();
			}

			CrashText<256> reason;
			DescribeSignal(signalNumber, information, reason);
			const std::array<std::string_view, 1> parts = { reason.GetText() };
			const bool isWritten = AreCrashReportsEnabled() && WriteReportFile(parts);
			WriteStandardErrorLine(parts, isWritten);
			_exit(FatalCrashExitCode);
		}

		static void RemoveHandlers()
		{
			for (size_t index = s_State.InstalledActionCount; index > 0; --index)
				sigaction(CrashSignals[index - 1], &s_State.PreviousActions[index - 1], nullptr);
			if (s_State.IsAlternateStackInstalled)
				sigaltstack(&s_State.PreviousAlternateStack, nullptr);
			s_State = {};
		}

	}

	Status CrashHandler::Install(const CrashHandlerSpecification& specification)
	{
		ENGINE_TRY(Utils::BeginCrashHandlerInstall(specification));
		if (Utils::AreCrashReportsEnabled())
		{
			Utils::s_State.ReportPathPrefix = Utils::GetCrashReportDirectory().native();
			if (!Utils::s_State.ReportPathPrefix.ends_with('/'))
				Utils::s_State.ReportPathPrefix += '/';
			Utils::s_State.ReportPath.assign(Utils::s_State.ReportPathPrefix.size() + Utils::MaxReportFileNameLength + 1, '\0');

			// The first backtrace loads the unwinder (and may allocate); the handler must find it loaded.
			std::array<void*, 1> warmUp{};
			static_cast<void>(backtrace(warmUp.data(), 1));
			Dl_info information{};
			static_cast<void>(dladdr(warmUp[0], &information));
		}

		const auto fail = [](std::string_view what, int error) -> Status
		{
			Utils::RemoveHandlers();
			Utils::ResetCrashHandlerState();
			return MakeError(ErrorCode::Io, "cannot install the crash handler: {} failed: {}", what, std::generic_category().message(error));
		};

		Utils::s_State.AlternateStack = CreateScope<std::byte[]>(Utils::AlternateStackSize);
		stack_t alternateStack{};
		alternateStack.ss_sp = Utils::s_State.AlternateStack.get();
		alternateStack.ss_size = Utils::AlternateStackSize;
		alternateStack.ss_flags = 0;
		if (sigaltstack(&alternateStack, &Utils::s_State.PreviousAlternateStack) != 0)
			return fail("sigaltstack", errno);
		Utils::s_State.IsAlternateStackInstalled = true;

		for (const int signalNumber : CrashSignals)
		{
			struct sigaction action{};
			action.sa_sigaction = &Utils::OnCrashSignal;
			sigemptyset(&action.sa_mask);
			action.sa_flags = SA_SIGINFO | SA_ONSTACK;
			if (sigaction(signalNumber, &action, &Utils::s_State.PreviousActions[Utils::s_State.InstalledActionCount]) != 0)
				return fail("sigaction", errno);
			++Utils::s_State.InstalledActionCount;
		}
		Utils::CompleteCrashHandlerInstall();
		return {};
	}

	void CrashHandler::Uninstall()
	{
		if (!IsInstalled())
			return;
		Utils::RemoveHandlers();
		Utils::ResetCrashHandlerState();
	}

	Result<std::filesystem::path> CrashHandler::WriteFatalErrorReport(FatalErrorKind kind, std::string_view message)
	{
		if (!IsInstalled())
			return MakeError(ErrorCode::InvalidState, "no crash handler is installed");
		if (!Utils::AreCrashReportsEnabled())
			return MakeError(ErrorCode::InvalidState, "crash reports are disabled (the crash handler has no report directory)");

		switch (Utils::EnterCrashReport(Utils::GetCurrentThreadId()))
		{
			case Utils::CrashReportEntry::Entered:
				break;
			case Utils::CrashReportEntry::Recursive:
				return MakeError(ErrorCode::InvalidState, "a crash report is already being written on this thread");
			case Utils::CrashReportEntry::Busy:
				// Another thread is reporting a crash and will end the process.
				while (true)
					pause();
		}

		const std::array<std::string_view, 4> parts = { "Fatal error (", FatalErrorKindToString(kind), "): ", message };
		// The path is copied before LeaveCrashReport, after which a crash on another thread may reuse the buffer.
		std::optional<std::filesystem::path> report;
		if (Utils::WriteReportFile(parts))
			report = std::filesystem::path(std::string_view(Utils::s_State.ReportPath.data(), Utils::s_State.ReportPathLength));
		Utils::LeaveCrashReport();
		if (!report.has_value())
			return MakeError(ErrorCode::Io, "cannot write a crash report into '{}'", Utils::GetCrashReportDirectory().native());
		return std::move(*report);
	}

	void CrashHandler::SimulateCrash()
	{
		raise(SIGSEGV);
		// Not reached with a handler installed (it exits) or without one (SIGSEGV's default action ends the process).
		// abort() keeps the [[noreturn]] promise if some other handler returned.
		std::abort();
	}

}

#endif
