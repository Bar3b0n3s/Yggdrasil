#include "EnginePCH.h"
#include "Engine/Platform/CrashHandler.h"

// The crash handler on Windows. SetUnhandledExceptionFilter catches structured exceptions no code handled, and a
// vectored handler catches stack overflows before the unhandled-exception path uses up the few pages a thread has left
// after one. The C runtime's abort (SIGABRT), invalid-parameter and pure-virtual-call paths, and in Debug builds the
// debug runtime's assertion and error reports (_CrtDbgReport: failed parameter checks, MSVC STL checks, debug-heap
// corruption), are routed to the same place. The report is written by a reporter thread that Install starts: it has a
// full stack of its own (a stack overflow leaves the crashing thread almost none), which is also where MiniDumpWriteDump
// should run. The crashing thread only hands it the request, waits for it, prints one line to stderr (composed in a
// static buffer, not on its stack) and ends the process with exit code 4.
//
// The crashing thread allocates nothing and takes no lock, but the reporter cannot avoid either: CreateFileW and DbgHelp
// allocate from the process heap, and DbgHelp may take the loader lock. A crash inside HeapAlloc on a corrupt heap, or
// inside DllMain (a Vulkan driver or layer that faults while it loads), leaves such a lock held forever. The reporter
// therefore exists before any crash (a thread cannot even start while the loader lock is held), and the crashing thread
// waits for it at most CrashReportTimeoutMilliseconds. When the reporter is stuck, the stderr line names the text report
// as incomplete when the file exists (the reason and breadcrumbs reach it before the stack trace is symbolized), says
// "no report written" otherwise, and the process still exits with code 4. A missing minidump alone leaves the text
// report complete.

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include "Engine/Platform/Process.h"
	#include "Engine/Platform/Private/CrashHandlerReport.h"
	#include "Engine/Platform/Private/PathsUtf8.h"

	#include <windows.h>
	#include <crtdbg.h>
	#include <dbghelp.h>

	#include <atomic>
	#include <csignal>
	#include <cstdlib>
	#include <cwchar>

namespace Engine {

	namespace {

		// How far the text report has come, which decides what the stderr line says.
		enum class ReportProgress : uint8_t
		{
			None,    // no report file
			Started, // the file exists and holds at least the reason and the breadcrumbs
			Written  // the text report is complete (the minidump may still be missing)
		};

		// What a crash report needs from the crashing thread.
		struct CrashReportRequest
		{
			EXCEPTION_POINTERS* Exception = nullptr; // for the minidump
			HANDLE Thread = nullptr;                 // the crashing thread, whose stack the trace walks
			DWORD ThreadId = 0;
			std::span<const std::string_view> ReasonParts{};
		};

		// The OS handlers this one replaced, restored by Uninstall; the report paths, built at Install; and the reporter
		// thread with its two events. Written only while no handler is installed (Install and Uninstall, main thread),
		// except Request, which the crash path that entered the report (EnterCrashReport) fills before it wakes the reporter.
		struct WindowsCrashState
		{
			LPTOP_LEVEL_EXCEPTION_FILTER PreviousFilter = nullptr;
			_invalid_parameter_handler PreviousInvalidParameterHandler = nullptr;
			_purecall_handler PreviousPurecallHandler = nullptr;
			_crt_signal_t PreviousAbortHandler = SIG_DFL;
			unsigned int PreviousAbortBehavior = 0;
			PVOID VectoredHandler = nullptr; // AddVectoredExceptionHandler's handle
			bool AreCrtReportHooksInstalled = false;
			bool IsSymbolHandlerInitialized = false;
			// "<ReportDirectory>\", and buffers large enough for it plus a report file name, in UTF-16 for the file system
			// and in UTF-8 for messages.
			std::wstring ReportPathPrefix;
			std::string ReportPathPrefixUtf8;
			std::vector<wchar_t> ReportPath;
			std::vector<char> ReportPathUtf8;
			size_t ReportPathUtf8Length = 0;
			// The reporter thread (only while reports are enabled) waits for ReportRequested (auto-reset) and sets
			// ReportFinished (manual-reset) once the text report and the minidump are written.
			HANDLE ReporterThread = nullptr;
			DWORD ReporterThreadId = 0;
			HANDLE ReportRequested = nullptr;
			HANDLE ReportFinished = nullptr;
			CrashReportRequest Request{};
		};

	}

	namespace Utils {

		constexpr uint32_t MaxStackFrames = 64;
		constexpr ULONG MaxSymbolNameLength = 512;
		constexpr SIZE_T CrashReporterStackSize = 1024 * 1024;
		// How long a crash waits for the reporter. A complete report with a symbolized stack trace and a minidump takes well
		// under a second; a reporter that needs this long is stuck on a lock the crash left held.
		constexpr DWORD CrashReportTimeoutMilliseconds = 10000;
		// The longest file name a report path appends to its directory: "\crash-<20 digits>-<10 digits>-<2 digits>.txt".
		constexpr size_t MaxReportFileNameLength = 64;
		// The exception codes recorded in the minidump for crashes that are not structured exceptions.
		constexpr DWORD StatusFatalAppExit = 0x40000015;             // STATUS_FATAL_APP_EXIT: abort, pure virtual call, FatalError
		constexpr DWORD StatusInvalidCRuntimeParameter = 0xc0000417; // STATUS_INVALID_CRUNTIME_PARAMETER
	#if defined(_DEBUG)
		constexpr DWORD StatusAssertionFailure = 0xc0000420; // STATUS_ASSERTION_FAILURE: a debug runtime assertion
	#endif
		constexpr DWORD StatusHeapCorruption = 0xc0000374;
		constexpr DWORD StatusStackBufferOverrun = 0xc0000409;
		constexpr DWORD MicrosoftCppException = 0xe06d7363; // what MSVC's throw raises
		constexpr MINIDUMP_TYPE MinidumpType = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

		static WindowsCrashState s_State;
		// The reporter's progress with the crash report, read by the crashing thread when its wait ends.
		static constinit std::atomic<ReportProgress> s_ReportProgress{ ReportProgress::None };
		// Set by Uninstall to end the reporter thread without a report.
		static constinit std::atomic<bool> s_IsReporterStopping{ false };
		// The reason of a structured exception and the stderr line, composed by the one crash path that entered the report
		// (EnterCrashReport). They are not on the crashing thread's stack, which a stack overflow leaves almost empty.
		static CrashText<512> s_ExceptionReason;
		static CrashText<2048> s_StandardErrorLine;

		[[noreturn]] static void TerminateCrashedProcess()
		{
			TerminateProcess(GetCurrentProcess(), FatalCrashExitCode);
			ExitProcess(FatalCrashExitCode);
		}

		static void WriteToHandle(void* context, std::string_view text)
		{
			const HANDLE file = context;
			while (!text.empty())
			{
				DWORD written = 0;
				const auto chunk = static_cast<DWORD>(std::min<size_t>(text.size(), 1u << 30));
				if (!WriteFile(file, text.data(), chunk, &written, nullptr) || written == 0)
					return;
				text.remove_prefix(written);
			}
		}

		// The report path of `stem` and `extension` in s_State.ReportPath (UTF-16, NUL-terminated) and, for the text report,
		// s_State.ReportPathUtf8.
		static void ComposeReportPath(const CrashText<64>& stem, std::string_view extension, bool updateUtf8)
		{
			size_t size = 0;
			for (const wchar_t character : s_State.ReportPathPrefix)
				s_State.ReportPath[size++] = character;
			for (const char character : stem.GetText())
				s_State.ReportPath[size++] = static_cast<wchar_t>(character);
			for (const char character : extension)
				s_State.ReportPath[size++] = static_cast<wchar_t>(character);
			s_State.ReportPath[size] = L'\0';

			if (!updateUtf8)
				return;
			size = 0;
			for (const char character : s_State.ReportPathPrefixUtf8)
				s_State.ReportPathUtf8[size++] = character;
			for (const char character : stem.GetText())
				s_State.ReportPathUtf8[size++] = character;
			for (const char character : extension)
				s_State.ReportPathUtf8[size++] = character;
			s_State.ReportPathUtf8Length = size;
		}

		static std::string_view GetExceptionName(DWORD code)
		{
			switch (code)
			{
				case EXCEPTION_ACCESS_VIOLATION:         return "Access violation";
				case EXCEPTION_STACK_OVERFLOW:           return "Stack overflow";
				case EXCEPTION_ILLEGAL_INSTRUCTION:      return "Illegal instruction";
				case EXCEPTION_PRIV_INSTRUCTION:         return "Privileged instruction";
				case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "Integer division by zero";
				case EXCEPTION_INT_OVERFLOW:             return "Integer overflow";
				case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "Floating-point division by zero";
				case EXCEPTION_FLT_INVALID_OPERATION:    return "Invalid floating-point operation";
				case EXCEPTION_FLT_OVERFLOW:             return "Floating-point overflow";
				case EXCEPTION_FLT_UNDERFLOW:            return "Floating-point underflow";
				case EXCEPTION_FLT_INEXACT_RESULT:       return "Inexact floating-point result";
				case EXCEPTION_FLT_DENORMAL_OPERAND:     return "Denormal floating-point operand";
				case EXCEPTION_FLT_STACK_CHECK:          return "Floating-point stack check";
				case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "Array bounds exceeded";
				case EXCEPTION_DATATYPE_MISALIGNMENT:    return "Misaligned data access";
				case EXCEPTION_IN_PAGE_ERROR:            return "In-page error";
				case EXCEPTION_BREAKPOINT:               return "Breakpoint";
				case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "Non-continuable exception";
				case StatusHeapCorruption:               return "Heap corruption";
				case StatusStackBufferOverrun:           return "Stack buffer overrun";
				case MicrosoftCppException:              return "Unhandled C++ exception";
				default:                                 return "Unhandled exception";
			}
		}

		// "Access violation (0xc0000005) reading address 0x0000000000000010 at 0x00007ff6c1d21234".
		static void DescribeException(const EXCEPTION_RECORD& record, CrashText<512>& reason)
		{
			reason.Append(GetExceptionName(record.ExceptionCode));
			reason.Append(" (");
			reason.AppendHex(record.ExceptionCode, 8);
			reason.Append(")");
			const bool hasAccessDetails = record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION
				|| record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR;
			if (hasAccessDetails && record.NumberParameters >= 2)
			{
				switch (record.ExceptionInformation[0])
				{
					case 0:  reason.Append(" reading address "); break;
					case 1:  reason.Append(" writing address "); break;
					case 8:  reason.Append(" executing address "); break;
					default: reason.Append(" accessing address "); break;
				}
				reason.AppendHex(record.ExceptionInformation[1], 16);
			}
			reason.Append(" at ");
			reason.AppendHex(reinterpret_cast<uintptr_t>(record.ExceptionAddress), 16);
		}

		// One line per frame: "  #<n> <address> <module>!<function>+<offset> (<file>:<line>)", symbolized where DbgHelp
		// finds symbols (the PDBs next to the executable).
		static void WriteStackTrace(CrashReportWriter& writer, HANDLE thread, const CONTEXT& context)
		{
			writer.Append("Stack trace:\n");
			const HANDLE process = GetCurrentProcess();
			if (s_State.IsSymbolHandlerInitialized)
				SymRefreshModuleList(process); // modules loaded after Install

			CONTEXT walkContext = context; // StackWalk64 updates it frame by frame
			STACKFRAME64 frame{};
			frame.AddrPC.Offset = walkContext.Rip;
			frame.AddrPC.Mode = AddrModeFlat;
			frame.AddrFrame.Offset = walkContext.Rbp;
			frame.AddrFrame.Mode = AddrModeFlat;
			frame.AddrStack.Offset = walkContext.Rsp;
			frame.AddrStack.Mode = AddrModeFlat;

			uint32_t frameCount = 0;
			for (uint32_t index = 0; index < MaxStackFrames; ++index)
			{
				if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &walkContext, nullptr, SymFunctionTableAccess64,
						SymGetModuleBase64, nullptr))
					break;
				const DWORD64 address = frame.AddrPC.Offset;
				if (address == 0)
					break;

				writer.Append("  #");
				writer.AppendDecimal(index);
				writer.Append(" ");
				writer.AppendHex(address, 16);
				if (s_State.IsSymbolHandlerInitialized)
				{
					// A return address points after its call; the call itself names the right line.
					const DWORD64 lookup = index == 0 ? address : address - 1;
					IMAGEHLP_MODULE64 module{};
					module.SizeOfStruct = sizeof(module);
					if (SymGetModuleInfo64(process, lookup, &module))
					{
						writer.Append(" ");
						writer.Append(std::string_view(module.ModuleName));
						writer.Append("!");
					}

					alignas(SYMBOL_INFO) std::array<std::byte, sizeof(SYMBOL_INFO) + MaxSymbolNameLength> symbolStorage{};
					auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage.data());
					symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
					symbol->MaxNameLen = MaxSymbolNameLength;
					DWORD64 displacement = 0;
					if (SymFromAddr(process, lookup, &displacement, symbol))
					{
						writer.Append(std::string_view(symbol->Name, std::min<size_t>(symbol->NameLen, MaxSymbolNameLength)));
						writer.Append("+");
						writer.AppendHex(index == 0 ? displacement : displacement + 1);
					}

					IMAGEHLP_LINE64 line{};
					line.SizeOfStruct = sizeof(line);
					DWORD lineDisplacement = 0;
					if (SymGetLineFromAddr64(process, lookup, &lineDisplacement, &line) && line.FileName != nullptr)
					{
						writer.Append(" (");
						writer.Append(std::string_view(line.FileName));
						writer.Append(":");
						writer.AppendDecimal(line.LineNumber);
						writer.Append(")");
					}
				}
				writer.Append("\n");
				++frameCount;
			}
			if (frameCount == 0)
				writer.Append("  (unavailable)\n");
		}

		// Writes the text report and then the minidump next to it, recording the text report's progress in `progress`: it
		// is what the stderr line names, and it exists even when the minidump fails or never finishes. The UTF-8 path, which
		// the stderr line reads, is final once the file exists: the minidump's path is composed in UTF-16 only.
		static void WriteReportFiles(const CrashReportRequest& request, std::atomic<ReportProgress>& progress)
		{
			FILETIME now{};
			GetSystemTimeAsFileTime(&now);
			// FILETIME counts 100-nanosecond intervals since 1601-01-01; the Unix epoch is 11644473600 seconds later.
			const uint64_t ticks = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
			const uint64_t epochSeconds = ticks / 10000000u - 11644473600u;
			const DWORD processId = GetCurrentProcessId();

			CrashText<64> stem;
			HANDLE file = INVALID_HANDLE_VALUE;
			for (uint32_t attempt = 1; attempt <= MaxCrashReportAttempts && file == INVALID_HANDLE_VALUE; ++attempt)
			{
				FormatCrashReportStem(stem, epochSeconds, processId, attempt);
				ComposeReportPath(stem, ".txt", true);
				file = CreateFileW(s_State.ReportPath.data(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
					nullptr);
				if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS)
					return;
			}
			if (file == INVALID_HANDLE_VALUE)
				return;

			{
				CrashReportWriter writer(&WriteToHandle, file);
				WriteCrashReportPreamble(writer, request.ReasonParts, processId);
				// The reason and the breadcrumbs reach the file before DbgHelp runs, which is where a reporter blocks on a
				// lock the crash left held.
				writer.Flush();
				progress.store(ReportProgress::Started, std::memory_order_release);
				WriteStackTrace(writer, request.Thread, *request.Exception->ContextRecord);
				WriteCrashReportLogLines(writer);
			}
			FlushFileBuffers(file);
			CloseHandle(file);
			progress.store(ReportProgress::Written, std::memory_order_release);

			ComposeReportPath(stem, ".dmp", false);
			const HANDLE dump = CreateFileW(s_State.ReportPath.data(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (dump == INVALID_HANDLE_VALUE)
				return;
			MINIDUMP_EXCEPTION_INFORMATION exception{};
			exception.ThreadId = request.ThreadId;
			exception.ExceptionPointers = request.Exception;
			exception.ClientPointers = FALSE;
			MiniDumpWriteDump(GetCurrentProcess(), processId, dump, MinidumpType, &exception, nullptr, nullptr);
			CloseHandle(dump);
		}

		// The reporter thread: waits for the process's one crash report, or for Uninstall, and writes the report.
		static DWORD WINAPI RunCrashReporter(LPVOID /*parameter*/)
		{
			if (WaitForSingleObject(s_State.ReportRequested, INFINITE) != WAIT_OBJECT_0 || s_IsReporterStopping.load())
				return 0;
			WriteReportFiles(s_State.Request, s_ReportProgress);
			SetEvent(s_State.ReportFinished);
			return 0;
		}

		// Creates the events and starts the reporter thread. False, with the error in GetLastError, when that fails;
		// StopCrashReporter then releases what was created.
		static bool StartCrashReporter()
		{
			s_State.ReportRequested = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (s_State.ReportRequested == nullptr)
				return false;
			s_State.ReportFinished = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (s_State.ReportFinished == nullptr)
				return false;
			s_State.ReporterThread = CreateThread(nullptr, CrashReporterStackSize, &RunCrashReporter, nullptr, 0,
				&s_State.ReporterThreadId);
			return s_State.ReporterThread != nullptr;
		}

		// Ends the reporter thread, which still waits for a report that never came, and closes the events.
		static void StopCrashReporter()
		{
			if (s_State.ReporterThread != nullptr)
			{
				s_IsReporterStopping.store(true);
				SetEvent(s_State.ReportRequested);
				WaitForSingleObject(s_State.ReporterThread, INFINITE);
				CloseHandle(s_State.ReporterThread);
			}
			if (s_State.ReportRequested != nullptr)
				CloseHandle(s_State.ReportRequested);
			if (s_State.ReportFinished != nullptr)
				CloseHandle(s_State.ReportFinished);
			s_State.ReporterThread = nullptr;
			s_State.ReporterThreadId = 0;
			s_State.ReportRequested = nullptr;
			s_State.ReportFinished = nullptr;
			s_IsReporterStopping.store(false);
			s_ReportProgress.store(ReportProgress::None);
		}

		// "Crash: <reason>; report written to <path>", "...; incomplete report written to <path>" or "...; no report
		// written", on stderr.
		static void WriteStandardErrorLine(std::span<const std::string_view> reasonParts, ReportProgress progress)
		{
			const HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
			if (output == nullptr || output == INVALID_HANDLE_VALUE)
				return;
			CrashText<2048>& line = s_StandardErrorLine;
			line.Clear();
			line.Append("Crash: ");
			for (const std::string_view part : reasonParts)
				line.Append(part);
			switch (progress)
			{
				case ReportProgress::None:
				{
					line.Append("; no report written");
					break;
				}
				case ReportProgress::Started:
				case ReportProgress::Written:
				{
					line.Append(progress == ReportProgress::Written ? "; report written to " : "; incomplete report written to ");
					line.Append(std::string_view(s_State.ReportPathUtf8.data(), s_State.ReportPathUtf8Length));
					break;
				}
			}
			line.Append("\n");
			WriteToHandle(output, line.GetText());
		}

		// The start of every crash path: returns only on the one thread that writes the report. The reporter itself
		// crashing (inside DbgHelp, say) tells the crashing thread that waits for it at once, instead of at the timeout.
		static void EnterCrashPath()
		{
			if (s_State.ReporterThread != nullptr && GetCurrentThreadId() == s_State.ReporterThreadId)
			{
				SetEvent(s_State.ReportFinished);
				while (true)
					Sleep(INFINITE);
			}

			switch (EnterCrashReport(GetCurrentThreadId()))
			{
				case CrashReportEntry::Entered:
					return;
				case CrashReportEntry::Recursive:
					TerminateCrashedProcess();
				case CrashReportEntry::Busy:
					// Another thread is reporting its crash and will end the process.
					while (true)
						Sleep(INFINITE);
			}
		}

		// The end of every crash path, after EnterCrashPath: has the reporter write the report (when enabled), prints the
		// stderr line and ends the process. Only system calls that neither allocate nor take a user-mode lock run on the
		// crashing thread, and its stack holds no buffer.
		[[noreturn]] static void FinishCrash(EXCEPTION_POINTERS* exception, std::span<const std::string_view> reasonParts)
		{
			if (s_State.ReporterThread != nullptr)
			{
				HANDLE thread = nullptr;
				DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
				s_State.Request = CrashReportRequest{
					.Exception = exception,
					.Thread = thread,
					.ThreadId = GetCurrentThreadId(),
					.ReasonParts = reasonParts,
				};
				SetEvent(s_State.ReportRequested);
				// A reporter stuck on the heap or loader lock never finishes; the bounded wait ends the process all the same.
				WaitForSingleObject(s_State.ReportFinished, CrashReportTimeoutMilliseconds);
			}
			WriteStandardErrorLine(reasonParts, s_ReportProgress.load(std::memory_order_acquire));
			TerminateCrashedProcess();
		}

		// A crash found by the C runtime rather than by an exception: the context of the caller stands in for the exception
		// context.
		[[noreturn]] static void HandleRuntimeCrash(DWORD code, std::string_view reason)
		{
			EnterCrashPath();
			CONTEXT context{};
			RtlCaptureContext(&context);
			EXCEPTION_RECORD record{};
			record.ExceptionCode = code;
			record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
			EXCEPTION_POINTERS pointers{ &record, &context };
			const std::array<std::string_view, 1> parts = { reason };
			FinishCrash(&pointers, parts);
		}

		[[noreturn]] static void HandleException(EXCEPTION_POINTERS* exception)
		{
			EnterCrashPath();
			s_ExceptionReason.Clear();
			DescribeException(*exception->ExceptionRecord, s_ExceptionReason);
			const std::array<std::string_view, 1> parts = { s_ExceptionReason.GetText() };
			FinishCrash(exception, parts);
		}

		static LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* exception)
		{
			HandleException(exception);
		}

		// Vectored handlers run first, before the frame handlers and the unhandled-exception path, which need more stack
		// than a stack overflow leaves. Only stack overflows are taken here (no code recovers from one); everything else
		// continues to the frame handlers.
		static LONG CALLBACK OnVectoredException(EXCEPTION_POINTERS* exception)
		{
			if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
				return EXCEPTION_CONTINUE_SEARCH;
			HandleException(exception);
		}

		static void __cdecl OnAbortSignal(int /*signalNumber*/)
		{
			HandleRuntimeCrash(StatusFatalAppExit, "abort() was called (SIGABRT)");
		}

		static void __cdecl OnInvalidParameter(const wchar_t* /*expression*/, const wchar_t* /*function*/, const wchar_t* /*file*/,
			unsigned int /*line*/, uintptr_t /*reserved*/)
		{
			HandleRuntimeCrash(StatusInvalidCRuntimeParameter, "Invalid parameter passed to a C runtime function");
		}

		static void __cdecl OnPureVirtualCall()
		{
			HandleRuntimeCrash(StatusFatalAppExit, "Pure virtual function call");
		}

	#if defined(_DEBUG)
		// A report of the debug runtime as a crash: "C runtime assertion: <message>" or "C runtime error: <message>", on one
		// line (the runtime ends its messages with a line break).
		[[noreturn]] static void HandleCrtReport(int reportType, std::string_view message)
		{
			CrashText<512> reason;
			reason.Append(reportType == _CRT_ASSERT ? "C runtime assertion: " : "C runtime error: ");
			while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
				message.remove_suffix(1);
			for (const char character : message)
				reason.Append(character == '\n' || character == '\r' ? std::string_view(" ") : std::string_view(&character, 1));
			HandleRuntimeCrash(reportType == _CRT_ASSERT ? StatusAssertionFailure : StatusFatalAppExit, reason.GetText());
		}

		// The debug runtime's report hooks, narrow (_CrtDbgReport: the MSVC STL's checks) and wide (_CrtDbgReportW: the C
		// runtime's parameter checks). Without them a report opens a modal window before any other handler runs, and an STL
		// check then ends the process with __fastfail, which no handler sees. Assertions and errors are crashes; warnings
		// keep their default handling (the debugger output).
		static int __cdecl OnCrtReport(int reportType, char* message, int* /*returnValue*/)
		{
			if (reportType == _CRT_WARN)
				return FALSE;
			HandleCrtReport(reportType, message != nullptr ? std::string_view(message) : std::string_view());
		}

		static int __cdecl OnCrtReportWide(int reportType, wchar_t* message, int* /*returnValue*/)
		{
			if (reportType == _CRT_WARN)
				return FALSE;
			// UTF-8 takes at most 3 bytes per UTF-16 unit, so this many units always fit; the rest of a message is dropped.
			std::array<char, 1536> text{};
			int length = 0;
			if (message != nullptr)
			{
				const auto units = static_cast<int>(std::min<size_t>(std::wcslen(message), text.size() / 3));
				length = WideCharToMultiByte(CP_UTF8, 0, message, units, text.data(), static_cast<int>(text.size()), nullptr, nullptr);
			}
			HandleCrtReport(reportType, std::string_view(text.data(), static_cast<size_t>(std::max(length, 0))));
		}
	#endif

		// Installs the debug runtime's report hooks (Debug builds; the release runtime makes no reports). False when the
		// runtime refuses; nothing stays installed then.
		static bool InstallCrtReportHooks()
		{
	#if defined(_DEBUG)
			if (_CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, &OnCrtReport) == -1)
				return false;
			if (_CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, &OnCrtReportWide) == -1)
			{
				_CrtSetReportHook2(_CRT_RPTHOOK_REMOVE, &OnCrtReport);
				return false;
			}
			s_State.AreCrtReportHooksInstalled = true;
	#endif
			return true;
		}

		static void RemoveCrtReportHooks()
		{
	#if defined(_DEBUG)
			if (!s_State.AreCrtReportHooksInstalled)
				return;
			_CrtSetReportHookW2(_CRT_RPTHOOK_REMOVE, &OnCrtReportWide);
			_CrtSetReportHook2(_CRT_RPTHOOK_REMOVE, &OnCrtReport);
			s_State.AreCrtReportHooksInstalled = false;
	#endif
		}

		// Symbols for stack traces, searched next to the executable only (never a symbol server, which a crash must not
		// wait for). Deferred loads make this cheap at startup.
		static void InitializeSymbolHandler()
		{
			SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS
				| SYMOPT_NO_PROMPTS);
			std::wstring searchPath;
			if (const Result<std::filesystem::path> executable = Process::GetCurrentExecutablePath())
				searchPath = executable->parent_path().native();
			s_State.IsSymbolHandlerInitialized = SymInitializeW(GetCurrentProcess(), searchPath.empty() ? nullptr : searchPath.c_str(), TRUE)
				!= FALSE;
		}

		static void PrepareReportPaths()
		{
			const std::filesystem::path& directory = GetCrashReportDirectory();
			s_State.ReportPathPrefix = directory.native();
			if (!s_State.ReportPathPrefix.empty() && s_State.ReportPathPrefix.back() != L'\\' && s_State.ReportPathPrefix.back() != L'/')
				s_State.ReportPathPrefix += L'\\';
			s_State.ReportPathPrefixUtf8 = NativeStringToUtf8(s_State.ReportPathPrefix);
			s_State.ReportPath.assign(s_State.ReportPathPrefix.size() + MaxReportFileNameLength + 1, L'\0');
			s_State.ReportPathUtf8.assign(s_State.ReportPathPrefixUtf8.size() + MaxReportFileNameLength, '\0');
			s_State.ReportPathUtf8Length = 0;
		}

		// Stops the reporter, releases the symbol handler and forgets the state. The OS handlers are not installed (any more).
		static void ReleaseInstallResources()
		{
			if (s_State.VectoredHandler != nullptr)
				RemoveVectoredExceptionHandler(s_State.VectoredHandler);
			StopCrashReporter();
			if (s_State.IsSymbolHandlerInitialized)
				SymCleanup(GetCurrentProcess());
			s_State = {};
		}

		static void RemoveHandlers()
		{
			SetUnhandledExceptionFilter(s_State.PreviousFilter);
			_set_invalid_parameter_handler(s_State.PreviousInvalidParameterHandler);
			_set_purecall_handler(s_State.PreviousPurecallHandler);
			RemoveCrtReportHooks();
			signal(SIGABRT, s_State.PreviousAbortHandler);
			_set_abort_behavior(s_State.PreviousAbortBehavior, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
			ReleaseInstallResources();
		}

		// Install's failure path, while no OS handler is installed: undoes the rest and returns the Io error describing
		// `failure`.
		static std::unexpected<Error> FailInstall(const std::string& failure)
		{
			ReleaseInstallResources();
			ResetCrashHandlerState();
			return MakeError(ErrorCode::Io, "cannot install the crash handler: {}", failure);
		}

	}

	Status CrashHandler::Install(const CrashHandlerSpecification& specification)
	{
		ENGINE_TRY(Utils::BeginCrashHandlerInstall(specification));
		if (Utils::AreCrashReportsEnabled())
		{
			Utils::PrepareReportPaths();
			Utils::InitializeSymbolHandler();
			if (!Utils::StartCrashReporter())
			{
				const DWORD error = GetLastError();
				return Utils::FailInstall(std::format("starting the reporter thread failed: {}",
					std::system_category().message(static_cast<int>(error))));
			}
		}

		Utils::s_State.VectoredHandler = AddVectoredExceptionHandler(1, &Utils::OnVectoredException);
		if (Utils::s_State.VectoredHandler == nullptr)
			return Utils::FailInstall("installing the vectored exception handler failed");
		Utils::s_State.PreviousAbortHandler = signal(SIGABRT, &Utils::OnAbortSignal);
		if (Utils::s_State.PreviousAbortHandler == SIG_ERR)
			return Utils::FailInstall("installing the SIGABRT handler failed");
		if (!Utils::InstallCrtReportHooks())
		{
			signal(SIGABRT, Utils::s_State.PreviousAbortHandler);
			return Utils::FailInstall("installing the debug runtime's report hooks failed");
		}
		Utils::s_State.PreviousFilter = SetUnhandledExceptionFilter(&Utils::OnUnhandledException);
		Utils::s_State.PreviousInvalidParameterHandler = _set_invalid_parameter_handler(&Utils::OnInvalidParameter);
		Utils::s_State.PreviousPurecallHandler = _set_purecall_handler(&Utils::OnPureVirtualCall);
		// abort() must reach the SIGABRT handler without a message box or Windows Error Reporting first.
		Utils::s_State.PreviousAbortBehavior = _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
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

		switch (Utils::EnterCrashReport(GetCurrentThreadId()))
		{
			case Utils::CrashReportEntry::Entered:
				break;
			case Utils::CrashReportEntry::Recursive:
				return MakeError(ErrorCode::InvalidState, "a crash report is already being written on this thread");
			case Utils::CrashReportEntry::Busy:
				// Another thread is reporting a crash and will end the process.
				while (true)
					Sleep(INFINITE);
		}

		CONTEXT context{};
		RtlCaptureContext(&context);
		EXCEPTION_RECORD record{};
		record.ExceptionCode = Utils::StatusFatalAppExit;
		record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
		EXCEPTION_POINTERS pointers{ &record, &context };
		const std::array<std::string_view, 4> parts = { "Fatal error (", FatalErrorKindToString(kind), "): ", message };

		const CrashReportRequest request = {
			.Exception = &pointers,
			.Thread = GetCurrentThread(),
			.ThreadId = GetCurrentThreadId(),
			.ReasonParts = parts,
		};
		std::atomic<ReportProgress> progress{ ReportProgress::None };
		Utils::WriteReportFiles(request, progress);

		// The path is copied before LeaveCrashReport, after which a crash on another thread may reuse the buffer.
		// ReportPath names the minidump by now; the UTF-8 path is the text report's.
		std::optional<std::filesystem::path> report;
		if (progress.load() == ReportProgress::Written)
			report = Utils::NativePathFromUtf8(std::string_view(Utils::s_State.ReportPathUtf8.data(), Utils::s_State.ReportPathUtf8Length));
		Utils::LeaveCrashReport();
		if (!report.has_value())
		{
			return MakeError(ErrorCode::Io, "cannot write a crash report into '{}'",
				Utils::NativePathToUtf8(Utils::GetCrashReportDirectory()));
		}
		return std::move(*report);
	}

	void CrashHandler::SimulateCrash()
	{
		RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
		// Not reached: an unhandled non-continuable exception never returns. abort() keeps the [[noreturn]] promise if a
		// handler somewhere resumed it anyway.
		std::abort();
	}

}

#endif
