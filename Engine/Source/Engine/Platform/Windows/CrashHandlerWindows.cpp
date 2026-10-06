#include "EnginePCH.h"
#include "Engine/Platform/CrashHandler.h"

// The crash handler on Windows. SetUnhandledExceptionFilter catches structured exceptions no code handled, and a
// vectored handler catches stack overflows before the unhandled-exception path uses up the few pages a thread has left
// after one. The C runtime's abort (SIGABRT), invalid-parameter and pure-virtual-call paths, and in Debug builds the
// debug runtime's assertion and error reports (_CrtDbgReport: failed parameter checks, MSVC STL checks, debug-heap
// corruption), are routed to the same place.
//
// Everything after the crash runs on a reporter thread that Install starts: it describes the crash, writes the report
// and the minidump on a full stack of its own, prints the one stderr line and ends the process with exit code 4. The
// crashing thread only hands it the request and waits, because after a stack overflow it has nothing left but the stack
// the thread keeps for that case. The installing thread and every thread started while the handler is installed keep
// CrashStackGuaranteeBytes for it, but at most a sixteenth of their stack (SetThreadStackGuarantee, in a TLS callback
// before a new thread runs). Without that a thread keeps only the system's 12 KiB guard region, which the exception
// dispatch shares with the crash path: the dispatch takes about 3 KiB of it, and up to about 11 KiB on a thread that
// has used AMX tiles.
//
// The crashing thread allocates nothing and takes no lock, but the reporter cannot avoid either: CreateFileW and DbgHelp
// allocate from the process heap, and DbgHelp may take the loader lock. A crash inside HeapAlloc on a corrupt heap, or
// inside DllMain (a Vulkan driver or layer that faults while it loads), leaves such a lock held forever. Install
// therefore starts the reporter and waits until it runs (a thread cannot even start while the loader lock is held), and
// the crashing thread waits for it at most CrashReportTimeoutMilliseconds before it prints the line and ends the process
// itself. The line then names the text report as incomplete when the file exists (the reason and breadcrumbs reach it
// before the stack trace is symbolized), or says "no report written"; a missing minidump alone leaves the text report
// complete.
//
// A crash inside the crash handler does not end the process silently either. When the crashing thread crashes again
// (on the stack its overflow left, or while it writes a fatal-error report), or the reporter crashes, the thread that is
// left finishes the crash, and the stderr line and the report's reason name the second fault after the reason:
// "Crash: <reason>; crash handler fault on the <crashing|reporter> thread: <fault>; ...". A crash inside
// WriteFatalErrorReport keeps the fatal error as the reason once its text is copied, and the reporter writes the
// interrupted report again under its name. Only a thread that crashes again while it prints the line ends the process
// at once.

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

		// A crash inside the crash handler, named in the stderr line.
		enum class HandlerFault : uint8_t
		{
			None,
			CrashingThread, // the thread handling a crash (or writing a fatal-error report) crashed again
			ReporterThread  // the reporter crashed while it wrote the report
		};

		// The longest fatal-error reason that a crash inside WriteFatalErrorReport keeps (the rest of the message is
		// dropped), and the longest reason of a crash, which also names a fault of the crash handler itself.
		constexpr size_t MaxFatalErrorReasonLength = 1024;
		constexpr size_t MaxCrashReasonLength = MaxFatalErrorReasonLength + 256;

		// The files of one report while WriteReportFiles writes them. A crash inside WriteFatalErrorReport leaves its report
		// half written, with a file still open; the reporter closes it and writes the report again under the same name.
		struct ReportFiles
		{
			Utils::CrashText<64> Stem{}; // the file name without its extension; empty until the text report's file exists
			std::atomic<ReportProgress> Progress{ ReportProgress::None };
			HANDLE OpenFile = nullptr; // the text report or the minidump while it is written
		};

		// What a crash report needs from the crashing thread.
		struct CrashReportRequest
		{
			EXCEPTION_POINTERS* Exception = nullptr; // for the stack trace, the minidump and, without ReasonParts, the reason
			// The thread whose stack the trace walks: opened by the reporter for a crash, GetCurrentThread() for a
			// fatal-error report.
			HANDLE Thread = nullptr;
			DWORD ThreadId = 0;
			std::span<const std::string_view> ReasonParts{}; // empty: the reason describes Exception's record
			ReportFiles* InterruptedFiles = nullptr;         // a fatal-error report the crash interrupted, written again
		};

		// The fatal-error report that WriteFatalErrorReport is writing. A crash inside it hands the reporter this report
		// instead of the crash, which becomes the crash handler's fault. Only the thread that entered the report writes it.
		struct FatalErrorReport
		{
			// Set once Reason holds the copy, so neither the reporter nor the stderr line reads a message that faults;
			// cleared before LeaveCrashReport.
			std::atomic<bool> IsActive{ false };
			CrashReportRequest Request{};
			Utils::CrashText<MaxFatalErrorReasonLength> Reason{};
			std::array<std::string_view, 1> ReasonParts{};
			ReportFiles Files{};
		};

		// The OS handlers this one replaced, restored by Uninstall; the report paths, built at Install; and the reporter
		// thread with its three events. Written only while no handler is installed (Install and Uninstall, main thread),
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
			// The reporter thread (while a handler is installed) sets ReporterStarted (manual-reset) as soon as it runs,
			// waits for ReportRequested (auto-reset) and ends the process; it sets ReporterCrashed (manual-reset) only when
			// it crashes itself.
			HANDLE ReporterThread = nullptr;
			DWORD ReporterThreadId = 0;
			HANDLE ReporterStarted = nullptr;
			HANDLE ReportRequested = nullptr;
			HANDLE ReporterCrashed = nullptr;
			CrashReportRequest Request{};
		};

	}

	namespace Utils {

		constexpr uint32_t MaxStackFrames = 64;
		constexpr ULONG MaxSymbolNameLength = 512;
		constexpr SIZE_T CrashReporterStackSize = 1024 * 1024;
		// The stack every thread keeps for its own stack overflow (SetThreadStackGuarantee): room for the exception dispatch,
		// whose frame holds the processor's extended state (about 11 KiB once a thread has used AMX tiles), and for the crash
		// path up to the hand-off to the reporter, with a wide margin. A thread never gives more than 1/OverflowStackShare of
		// its stack to it, so a thread with a small stack keeps what its own work needs.
		constexpr ULONG CrashStackGuaranteeBytes = 64 * 1024;
		constexpr ULONG_PTR OverflowStackShare = 16;
		// How long a crash waits for the reporter. A complete report with a symbolized stack trace and a minidump takes well
		// under a second; a reporter that needs this long is stuck on a lock the crash left held.
		constexpr DWORD CrashReportTimeoutMilliseconds = 10000;
		// The longest file name a report path appends to its directory: "\crash-<20 digits>-<10 digits>-<2 digits>.txt".
		constexpr size_t MaxReportFileNameLength = 64;
		// The exception codes recorded in the minidump for crashes that are not structured exceptions.
		constexpr DWORD StatusFatalAppExit = 0x40000015;             // STATUS_FATAL_APP_EXIT: abort, pure virtual call, FatalError
		constexpr DWORD StatusInvalidCRuntimeParameter = 0xc0000417; // STATUS_INVALID_CRUNTIME_PARAMETER
		constexpr DWORD StatusAssertionFailure = 0xc0000420;         // STATUS_ASSERTION_FAILURE: a debug runtime assertion
		constexpr DWORD StatusHeapCorruption = 0xc0000374;
		constexpr DWORD StatusStackBufferOverrun = 0xc0000409;
		constexpr DWORD MicrosoftCppException = 0xe06d7363; // what MSVC's throw raises
		constexpr MINIDUMP_TYPE MinidumpType = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

		static WindowsCrashState s_State;
		// The report the reporter writes for the crash, which the stderr line names, and the fatal-error report being
		// written, which a crash inside it hands to the reporter.
		static constinit ReportFiles s_ReporterFiles{};
		static constinit FatalErrorReport s_FatalErrorReport{};
		// Set by Uninstall to end the reporter thread without a report.
		static constinit std::atomic<bool> s_IsReporterStopping{ false };
		// Whether s_State.Request holds the crash and the reporter was woken for it. Only the thread that entered the
		// report writes it.
		static constinit std::atomic<bool> s_IsReportRequested{ false };
		// The ID of the one thread that prints the stderr line and ends the process: the reporter, or the crashing thread
		// when the reporter did not finish. 0 until one of them gets there.
		static constinit std::atomic<DWORD> s_FinishingThreadId{ 0 };
		// The first crash inside the crash handler, if any: which thread (published last), its exception code and address.
		static constinit std::atomic<bool> s_IsHandlerFaultRecorded{ false };
		static constinit std::atomic<HandlerFault> s_HandlerFault{ HandlerFault::None };
		static constinit std::atomic<DWORD> s_HandlerFaultCode{ 0 };
		static constinit std::atomic<uintptr_t> s_HandlerFaultAddress{ 0 };
		// Whether threads that start get CrashStackGuaranteeBytes (while a handler is installed), read by the TLS callback.
		static constinit std::atomic<bool> s_ReservesOverflowStack{ false };
		// The reason in the report, composed by the reporter, and the reason and stderr line composed by the thread that
		// finishes the crash. None of them is on a crashing thread's stack.
		static CrashText<MaxCrashReasonLength> s_ReportReason;
		static std::array<std::string_view, 1> s_ReportReasonParts{};
		static CrashText<MaxCrashReasonLength> s_LineReason;
		static std::array<std::string_view, 1> s_LineReasonParts{};
		static CrashText<4096> s_StandardErrorLine;

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

		// Gives the calling thread CrashStackGuaranteeBytes (at most 1/OverflowStackShare of its stack) that stay usable after
		// its own stack overflow. A guarantee is never lowered: nothing changes for a thread that already keeps as much.
		static void ReserveOverflowStack()
		{
			ULONG_PTR low = 0;
			ULONG_PTR high = 0;
			GetCurrentThreadStackLimits(&low, &high);
			const auto wanted = static_cast<ULONG>(std::min<ULONG_PTR>(CrashStackGuaranteeBytes, (high - low) / OverflowStackShare));
			ULONG current = 0; // 0 asks for the current guarantee
			if (SetThreadStackGuarantee(&current) == FALSE || current >= wanted)
				return;
			// It fails only for a stack too small to spare `wanted`, which the share rules out; the thread keeps the system's
			// guard region then.
			ULONG requested = wanted;
			SetThreadStackGuarantee(&requested);
		}

		// The TLS callback (registered after the Utils namespace): runs on every new thread before the thread's own code.
		static void NTAPI OnThreadNotification(PVOID /*module*/, DWORD reason, PVOID /*reserved*/)
		{
			if (reason == DLL_THREAD_ATTACH && s_ReservesOverflowStack.load(std::memory_order_acquire))
				ReserveOverflowStack();
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
				case StatusFatalAppExit:                 return "Fatal application exit";
				case StatusInvalidCRuntimeParameter:     return "Invalid C runtime parameter";
				case StatusAssertionFailure:             return "C runtime assertion";
				default:                                 return "Unhandled exception";
			}
		}

		// "Access violation (0xc0000005) reading address 0x0000000000000010 at 0x00007ff6c1d21234".
		static void DescribeException(const EXCEPTION_RECORD& record, CrashText<MaxCrashReasonLength>& reason)
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

		// "; crash handler fault on the <crashing|reporter> thread: <exception> (<code>) at <address>" after `reason`, when
		// the crash handler itself crashed.
		static void AppendHandlerFault(CrashText<MaxCrashReasonLength>& reason)
		{
			const HandlerFault fault = s_HandlerFault.load(std::memory_order_acquire);
			if (fault == HandlerFault::None)
				return;
			reason.Append(fault == HandlerFault::ReporterThread ? "; crash handler fault on the reporter thread: "
																: "; crash handler fault on the crashing thread: ");
			const DWORD code = s_HandlerFaultCode.load(std::memory_order_relaxed);
			reason.Append(GetExceptionName(code));
			reason.Append(" (");
			reason.AppendHex(code, 8);
			reason.Append(") at ");
			reason.AppendHex(s_HandlerFaultAddress.load(std::memory_order_relaxed), 16);
		}

		// The reason of the requested crash in `reason`, as the one part in `storage`: the request's own parts, or else a
		// description of its exception, followed by the crash handler's own fault if it crashed.
		static std::span<const std::string_view> DescribeRequestedCrash(CrashText<MaxCrashReasonLength>& reason,
			std::array<std::string_view, 1>& storage)
		{
			reason.Clear();
			if (s_State.Request.ReasonParts.empty())
				DescribeException(*s_State.Request.Exception->ExceptionRecord, reason);
			for (const std::string_view part : s_State.Request.ReasonParts)
				reason.Append(part);
			AppendHandlerFault(reason);
			storage[0] = reason.GetText();
			return storage;
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

		// Creates the text report's file, under `files.Stem` when the report already has a name (an interrupted fatal-error
		// report, replaced), otherwise under the first free name of this second and process, which goes into `files.Stem`.
		// INVALID_HANDLE_VALUE when that fails.
		static HANDLE CreateTextReportFile(ReportFiles& files)
		{
			if (!files.Stem.GetText().empty())
			{
				ComposeReportPath(files.Stem, ".txt", true);
				const HANDLE file = CreateFileW(s_State.ReportPath.data(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
					FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file != INVALID_HANDLE_VALUE)
					return file;
				// Another process holds the file (a virus scanner, say): the report gets a name of its own.
			}

			FILETIME now{};
			GetSystemTimeAsFileTime(&now);
			// FILETIME counts 100-nanosecond intervals since 1601-01-01; the Unix epoch is 11644473600 seconds later.
			const uint64_t ticks = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
			const uint64_t epochSeconds = ticks / 10000000u - 11644473600u;
			CrashText<64> stem;
			for (uint32_t attempt = 1; attempt <= MaxCrashReportAttempts; ++attempt)
			{
				FormatCrashReportStem(stem, epochSeconds, GetCurrentProcessId(), attempt);
				ComposeReportPath(stem, ".txt", true);
				const HANDLE file = CreateFileW(s_State.ReportPath.data(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
					FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file != INVALID_HANDLE_VALUE)
				{
					files.Stem = stem;
					return file;
				}
				if (GetLastError() != ERROR_FILE_EXISTS)
					break;
			}
			return INVALID_HANDLE_VALUE;
		}

		// Writes the text report and then the minidump next to it, recording in `files` the name, the text report's
		// progress and the file open for writing. The progress is what the stderr line names; the text report exists even
		// when the minidump fails or never finishes. The UTF-8 path, which the stderr line reads, is final once the file
		// exists: the minidump's path is composed in UTF-16 only.
		static void WriteReportFiles(const CrashReportRequest& request, ReportFiles& files)
		{
			const HANDLE file = CreateTextReportFile(files);
			if (file == INVALID_HANDLE_VALUE)
				return;
			files.OpenFile = file;
			const DWORD processId = GetCurrentProcessId();
			{
				CrashReportWriter writer(&WriteToHandle, file);
				WriteCrashReportPreamble(writer, request.ReasonParts, processId);
				// The reason and the breadcrumbs reach the file before DbgHelp runs, which is where a reporter blocks on a
				// lock the crash left held.
				writer.Flush();
				files.Progress.store(ReportProgress::Started, std::memory_order_release);
				WriteStackTrace(writer, request.Thread, *request.Exception->ContextRecord);
				WriteCrashReportLogLines(writer);
			}
			FlushFileBuffers(file);
			files.OpenFile = nullptr;
			CloseHandle(file);
			files.Progress.store(ReportProgress::Written, std::memory_order_release);

			// The text report's name is this report's own, so a minidump of that name is a leftover of it: replaced.
			ComposeReportPath(files.Stem, ".dmp", false);
			const HANDLE dump = CreateFileW(s_State.ReportPath.data(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
				nullptr);
			if (dump == INVALID_HANDLE_VALUE)
				return;
			files.OpenFile = dump;
			MINIDUMP_EXCEPTION_INFORMATION exception{};
			exception.ThreadId = request.ThreadId;
			exception.ExceptionPointers = request.Exception;
			exception.ClientPointers = FALSE;
			MiniDumpWriteDump(GetCurrentProcess(), processId, dump, MinidumpType, &exception, nullptr, nullptr);
			files.OpenFile = nullptr;
			CloseHandle(dump);
		}

		// Clears `files` for a new report.
		static void ResetReportFiles(ReportFiles& files)
		{
			files.Stem.Clear();
			files.Progress.store(ReportProgress::None);
			files.OpenFile = nullptr;
		}

		// "Crash: <reason>; report written to <path>", "...; incomplete report written to <path>" or "...; no report
		// written", on stderr. The reason names a fault of the crash handler itself (DescribeRequestedCrash).
		static void WriteStandardErrorLine(std::span<const std::string_view> reasonParts, ReportProgress progress)
		{
			const HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
			if (output == nullptr || output == INVALID_HANDLE_VALUE)
				return;
			CrashText<4096>& line = s_StandardErrorLine;
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

		// The end of every crash: prints the stderr line and ends the process, on the first thread to get here (the
		// reporter, or the crashing thread when the reporter did not finish). Any other thread waits for the process to end,
		// unless it is the finishing thread itself, crashed while printing: that ends the process at once.
		[[noreturn]] static void FinishCrash()
		{
			DWORD expected = 0;
			if (!s_FinishingThreadId.compare_exchange_strong(expected, GetCurrentThreadId()))
			{
				if (expected == GetCurrentThreadId())
					TerminateCrashedProcess();
				while (true)
					Sleep(INFINITE);
			}
			WriteStandardErrorLine(DescribeRequestedCrash(s_LineReason, s_LineReasonParts),
				s_ReporterFiles.Progress.load(std::memory_order_acquire));
			TerminateCrashedProcess();
		}

		// The reporter thread: tells Install that it runs, waits for the process's one crash, or for Uninstall, writes the
		// report (when enabled) and finishes the crash.
		static DWORD WINAPI RunCrashReporter(LPVOID /*parameter*/)
		{
			// Install waits for this: the thread is past the loader's thread initialization, the one step that a lock
			// held by a crash could block.
			SetEvent(s_State.ReporterStarted);
			if (WaitForSingleObject(s_State.ReportRequested, INFINITE) != WAIT_OBJECT_0 || s_IsReporterStopping.load())
				return 0;
			if (AreCrashReportsEnabled())
			{
				CrashReportRequest request = s_State.Request;
				request.ReasonParts = DescribeRequestedCrash(s_ReportReason, s_ReportReasonParts);
				// The stack trace walks the crashing thread's stack; StackWalk64 takes a handle to it.
				request.Thread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT, FALSE, request.ThreadId);
				if (request.InterruptedFiles != nullptr)
				{
					// The crashing thread never returns to the fatal-error report it was writing: its file is closed here, and
					// the report is written again under its name, with the crash handler's fault in the reason.
					ReportFiles& interrupted = *request.InterruptedFiles;
					if (interrupted.OpenFile != nullptr)
						CloseHandle(interrupted.OpenFile);
					interrupted.OpenFile = nullptr;
					s_ReporterFiles.Stem = interrupted.Stem;
				}
				WriteReportFiles(request, s_ReporterFiles);
				if (request.Thread != nullptr)
					CloseHandle(request.Thread);
			}
			FinishCrash();
		}

		// Creates the events and starts the reporter thread, which runs before this returns. The error when that fails;
		// StopCrashReporter then releases what was created.
		static std::optional<std::string> StartCrashReporter()
		{
			const auto describeLastError = []()
			{
				const DWORD error = GetLastError();
				return std::system_category().message(static_cast<int>(error));
			};
			s_State.ReportRequested = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (s_State.ReportRequested == nullptr)
				return std::format("creating an event failed: {}", describeLastError());
			s_State.ReporterCrashed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (s_State.ReporterCrashed == nullptr)
				return std::format("creating an event failed: {}", describeLastError());
			s_State.ReporterStarted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (s_State.ReporterStarted == nullptr)
				return std::format("creating an event failed: {}", describeLastError());
			s_State.ReporterThread = CreateThread(nullptr, CrashReporterStackSize, &RunCrashReporter, nullptr, 0,
				&s_State.ReporterThreadId);
			if (s_State.ReporterThread == nullptr)
				return std::format("starting the reporter thread failed: {}", describeLastError());

			// Without this wait a crash inside DllMain right after Install would find the reporter still waiting for the
			// loader lock, and a process without reports would print its line only after CrashReportTimeoutMilliseconds.
			// The wait has no limit, like the thread start it waits for: only a thread that loads or unloads a DLL holds
			// the loader lock, and Install never runs inside DllMain (main thread).
			const std::array<HANDLE, 2> handles = { s_State.ReporterStarted, s_State.ReporterThread };
			switch (WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE))
			{
				case WAIT_OBJECT_0:     return std::nullopt;
				case WAIT_OBJECT_0 + 1: return std::string("the reporter thread ended before it ran");
				default:                return std::format("waiting for the reporter thread failed: {}", describeLastError());
			}
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
			const auto closeEvent = [](HANDLE& event)
			{
				if (event != nullptr)
					CloseHandle(event);
				event = nullptr;
			};
			closeEvent(s_State.ReporterStarted);
			closeEvent(s_State.ReportRequested);
			closeEvent(s_State.ReporterCrashed);
			s_State.ReporterThread = nullptr;
			s_State.ReporterThreadId = 0;
			s_IsReporterStopping.store(false);
			ResetReportFiles(s_ReporterFiles);
			s_IsReportRequested.store(false);
			s_FinishingThreadId.store(0);
			s_IsHandlerFaultRecorded.store(false);
			s_HandlerFault.store(HandlerFault::None);
		}

		// Records the first crash inside the crash handler for the stderr line.
		static void RecordHandlerFault(HandlerFault fault, DWORD code, uintptr_t address)
		{
			if (s_IsHandlerFaultRecorded.exchange(true))
				return;
			s_HandlerFaultCode.store(code, std::memory_order_relaxed);
			s_HandlerFaultAddress.store(address, std::memory_order_relaxed);
			s_HandlerFault.store(fault, std::memory_order_release);
		}

		// The start of every crash path (`code` and `address` describe the crash). Returns only on the thread that handles
		// the crash, the first time and when that thread crashes again inside the crash path (recorded as a handler fault).
		// The reporter crashing (inside DbgHelp, say) tells the crashing thread that waits for it at once, instead of at the
		// timeout; a crash on any other thread waits for the process to end.
		static void EnterCrashPath(DWORD code, uintptr_t address)
		{
			if (s_State.ReporterThread != nullptr && GetCurrentThreadId() == s_State.ReporterThreadId)
			{
				RecordHandlerFault(HandlerFault::ReporterThread, code, address);
				// The reporter crashed while it printed the line: nobody is left to print it.
				if (s_FinishingThreadId.load() == GetCurrentThreadId())
					TerminateCrashedProcess();
				SetEvent(s_State.ReporterCrashed);
				while (true)
					Sleep(INFINITE);
			}

			switch (EnterCrashReport(GetCurrentThreadId()))
			{
				case CrashReportEntry::Entered:
					return;
				case CrashReportEntry::Recursive:
				{
					RecordHandlerFault(HandlerFault::CrashingThread, code, address);
					// This thread crashed while it printed the line, after the reporter failed to: nobody is left to print it.
					if (s_FinishingThreadId.load() == GetCurrentThreadId())
						TerminateCrashedProcess();
					return;
				}
				case CrashReportEntry::Busy:
				{
					// Another thread is handling its crash and the process ends with it.
					while (true)
						Sleep(INFINITE);
				}
			}
		}

		// The rest of every crash path on the thread that handles the crash, after EnterCrashPath: hands the crash to the
		// reporter, which writes the report and the stderr line and ends the process, and waits. Up to the wait nothing
		// allocates, takes a user-mode lock or needs much stack (about 1.3 KiB below the vectored handler in a Debug build):
		// after a stack overflow, the stack the thread keeps for it is all there is. Only when the reporter does not finish
		// in time, or crashed, does this thread describe the crash and print the line itself. A second crash on this thread
		// hands the reporter only what it was not handed yet, and waits the same way: when the thread was writing a
		// fatal-error report, that report, whose reason names the fatal error, instead of the second crash.
		[[noreturn]] static void HandOffCrash(EXCEPTION_POINTERS* exception, std::span<const std::string_view> reasonParts)
		{
			if (!s_IsReportRequested.load(std::memory_order_relaxed))
			{
				if (s_FatalErrorReport.IsActive.load(std::memory_order_acquire))
				{
					s_State.Request = s_FatalErrorReport.Request;
				}
				else
				{
					s_State.Request = CrashReportRequest{
						.Exception = exception,
						.ThreadId = GetCurrentThreadId(),
						.ReasonParts = reasonParts,
					};
				}
				s_IsReportRequested.store(true, std::memory_order_relaxed);
				SetEvent(s_State.ReportRequested);
			}
			// A reporter stuck on the heap or loader lock never finishes; the bounded wait ends the process all the same.
			WaitForSingleObject(s_State.ReporterCrashed, CrashReportTimeoutMilliseconds);
			FinishCrash();
		}

		// A crash found by the C runtime rather than by an exception: the context of the caller stands in for the exception
		// context.
		[[noreturn]] static void HandleRuntimeCrash(DWORD code, std::string_view reason)
		{
			CONTEXT context{};
			RtlCaptureContext(&context);
			EnterCrashPath(code, context.Rip);
			EXCEPTION_RECORD record{};
			record.ExceptionCode = code;
			record.ExceptionAddress = reinterpret_cast<PVOID>(context.Rip);
			EXCEPTION_POINTERS pointers{ &record, &context };
			const std::array<std::string_view, 1> parts = { reason };
			HandOffCrash(&pointers, parts);
		}

		// A structured exception: the reporter describes it.
		[[noreturn]] static void HandleException(EXCEPTION_POINTERS* exception)
		{
			const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
			EnterCrashPath(record.ExceptionCode, reinterpret_cast<uintptr_t>(record.ExceptionAddress));
			HandOffCrash(exception, {});
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
			// Threads started from now on keep the system's default; threads that got the guarantee keep it.
			s_ReservesOverflowStack.store(false, std::memory_order_release);
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

		// WriteFatalErrorReport, after it entered the report: from here until EndFatalErrorReport a crash on this thread
		// hands the reporter this report (`pointers` and the reason `parts`) instead of the crash. The reason is copied
		// first, so a message that faults when read faults here, before the report is handed to anyone.
		static void BeginFatalErrorReport(EXCEPTION_POINTERS* pointers, std::span<const std::string_view> parts)
		{
			FatalErrorReport& report = s_FatalErrorReport;
			report.Reason.Clear();
			for (const std::string_view part : parts)
				report.Reason.Append(part);
			report.ReasonParts[0] = report.Reason.GetText();
			ResetReportFiles(report.Files);
			report.Request = CrashReportRequest{
				.Exception = pointers,
				.ThreadId = GetCurrentThreadId(),
				.ReasonParts = report.ReasonParts,
				.InterruptedFiles = &report.Files,
			};
			report.IsActive.store(true, std::memory_order_release);
		}

		// Before LeaveCrashReport: a crash from here on is a crash of its own.
		static void EndFatalErrorReport()
		{
			s_FatalErrorReport.IsActive.store(false, std::memory_order_release);
		}

	}

	// OnThreadNotification as a TLS callback of the executable: the C runtime's TLS directory (_tls_used) calls every
	// pointer in the sections from .CRT$XLA to .CRT$XLZ when a thread starts or ends. Nothing references the pointer, so
	// the linker is told to keep it, and the directory.
	#pragma section(".CRT$XLB", read)
	extern "C" __declspec(allocate(".CRT$XLB")) const PIMAGE_TLS_CALLBACK CrashHandlerThreadCallback = &Utils::OnThreadNotification;
	#pragma comment(linker, "/INCLUDE:_tls_used")
	#pragma comment(linker, "/INCLUDE:CrashHandlerThreadCallback")

	Status CrashHandler::Install(const CrashHandlerSpecification& specification)
	{
		ENGINE_TRY(Utils::BeginCrashHandlerInstall(specification));
		if (Utils::AreCrashReportsEnabled())
		{
			Utils::PrepareReportPaths();
			Utils::InitializeSymbolHandler();
		}
		// The reporter finishes every crash, with or without a report: the crashing thread may have no stack left to print
		// the line itself.
		if (const std::optional<std::string> failure = Utils::StartCrashReporter())
			return Utils::FailInstall(*failure);

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
		// The stack for a stack overflow: on this thread now, on every thread that starts from now on in the TLS callback.
		Utils::ReserveOverflowStack();
		Utils::s_ReservesOverflowStack.store(true, std::memory_order_release);
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
		Utils::BeginFatalErrorReport(&pointers, parts);

		const CrashReportRequest request = {
			.Exception = &pointers,
			.Thread = GetCurrentThread(),
			.ThreadId = GetCurrentThreadId(),
			.ReasonParts = parts,
		};
		ReportFiles& files = Utils::s_FatalErrorReport.Files;
		Utils::WriteReportFiles(request, files);

		// The path is copied before LeaveCrashReport, after which a crash on another thread may reuse the buffer.
		// ReportPath names the minidump by now; the UTF-8 path is the text report's.
		std::optional<std::filesystem::path> report;
		if (files.Progress.load() == ReportProgress::Written)
			report = Utils::NativePathFromUtf8(std::string_view(Utils::s_State.ReportPathUtf8.data(), Utils::s_State.ReportPathUtf8Length));
		Utils::EndFatalErrorReport();
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
