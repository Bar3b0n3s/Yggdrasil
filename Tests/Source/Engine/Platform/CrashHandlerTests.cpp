#include "TestsPCH.h"

#include "Engine/Platform/CrashHandler.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Process.h"
#include "Support/ChildOutput.h"
#include "Support/DeathTest.h"
#include "Support/PlatformProbes.h"
#include "Support/TempDirectory.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>

namespace Engine {

	// The application name and build line the handler children install with.
	static constexpr std::string_view TestAppName = "CrashTest";
	static constexpr std::string_view TestBuildInfo = "Test build";

	// What a crash looks like in a report's Reason line and the stderr line of each host.
#if defined(ENGINE_PLATFORM_WINDOWS)
	static constexpr std::string_view SimulatedCrashReason = "Access violation (0xc0000005)";
	static constexpr std::string_view AbortReason = "abort() was called (SIGABRT)";
	static constexpr std::string_view StackOverflowReason = "Stack overflow (0xc00000fd)";
#else
	static constexpr std::string_view SimulatedCrashReason = "SIGSEGV (segmentation fault)";
	static constexpr std::string_view AbortReason = "SIGABRT (abort)";
#endif

	// Replaces the crash handler the Tests main installed, if any, with one that writes reports into the directory named
	// by --child-argument (no reports when it is empty). Returns false, having logged why, when that fails.
	static bool InstallTestCrashHandler()
	{
		CrashHandler::Uninstall();
		const std::string& directory = Test::GetTestOptions().ChildArgument;
		const Status installed = CrashHandler::Install({
			.ReportDirectory = directory.empty() ? std::filesystem::path() : Test::PathFromUtf8(directory),
			.AppName = std::string(TestAppName),
			.BuildInfo = std::string(TestBuildInfo),
		});
		if (!installed.has_value())
		{
			ENGINE_CORE_ERROR("The crash handler test child could not install the handler: {}", installed.error());
			return false;
		}
		return true;
	}

	// "written", or the name of the error code.
	static std::string_view DescribeReportResult(const Result<std::filesystem::path>& report)
	{
		return report.has_value() ? std::string_view("written") : ErrorCodeToString(report.error().GetCode());
	}

	// Recursion that only a stack overflow ends. Each frame keeps a volatile buffer that it reads again after the call
	// returns, so the compiler can neither drop the frames nor turn the calls into a loop, and the depth limit is a
	// parameter, so no compiler can prove the recursion endless (MSVC's C4717).
	static uint64_t RecurseUntilTheStackOverflows(uint64_t depth, uint64_t limit)
	{
		std::array<volatile char, 1024> frame{};
		frame[depth % frame.size()] = static_cast<char>(depth);
		if (depth >= limit)
			return static_cast<uint64_t>(frame[0]);
		return RecurseUntilTheStackOverflows(depth + 1, limit) + static_cast<uint64_t>(frame[(depth + 1) % frame.size()]);
	}

	// Records breadcrumbs and more log lines than a report keeps, then crashes: the child of the report test.
	ENGINE_DEATH_TEST("Platform/CrashesWithReport")
	{
		if (!InstallTestCrashHandler())
			return;
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::Scene, "Levels/Level1.scene");
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::AutomationMethod, std::string(CrashHandler::MaxBreadcrumbLength + 44, 'm'));
		for (int line = 0; line < 300; ++line)
			ENGINE_CORE_INFO("Captured log line [{}]", line);
		ENGINE_CORE_INFO("Long line {}", std::string(CrashHandler::MaxLogLineLength * 2, 'L'));
		CrashHandler::SimulateCrash();
	}

	// abort() after installing the handler: the child of the abort test.
	ENGINE_DEATH_TEST("Platform/AbortsWithReport")
	{
		if (!InstallTestCrashHandler())
			return;
		std::abort();
	}

	// Crashes on a thread other than the one that installed the handler.
	ENGINE_DEATH_TEST("Platform/CrashesOnAnotherThread")
	{
		if (!InstallTestCrashHandler())
			return;
		std::thread crashing([]()
		{
			CrashHandler::SimulateCrash();
		});
		crashing.join();
	}

	// Overflows the stack of the thread that installed the handler: on POSIX the handler runs on its alternate signal
	// stack, on Windows the reporter thread writes the report on its own stack.
	ENGINE_DEATH_TEST("Platform/OverflowsTheStack")
	{
		if (!InstallTestCrashHandler())
			return;
		ENGINE_CORE_ERROR("The recursion ended: {}", RecurseUntilTheStackOverflows(0, std::numeric_limits<uint64_t>::max()));
	}

#if defined(ENGINE_PLATFORM_WINDOWS)
	// The readable start of the faulting message of Platform/FaultsWhileWritingAFatalErrorReport: far more than the reason
	// a crash inside WriteFatalErrorReport keeps, so copying that reason never reaches the bytes that fault.
	static constexpr size_t FaultingMessageReadableBytes = 16 * 1024;

	// The guarantee in bytes, or the error.
	static std::string DescribeStackGuarantee(const Result<uint32_t>& guarantee)
	{
		return guarantee.has_value() ? std::to_string(*guarantee) : guarantee.error().ToString();
	}

	// Overflows the stack of a thread other than the one that installed the handler (POSIX covers only the installing
	// thread with its alternate stack, ADR 0005 decision 22).
	ENGINE_DEATH_TEST("Platform/OverflowsAnotherThreadsStack")
	{
		if (!InstallTestCrashHandler())
			return;
		std::thread overflowing([]()
		{
			ENGINE_CORE_ERROR("The recursion ended: {}", RecurseUntilTheStackOverflows(0, std::numeric_limits<uint64_t>::max()));
		});
		overflowing.join();
	}

	// Overflows the stack of a thread that was started before the handler was installed, so it keeps only the system's
	// guard region (ADR 0005 decision 25): the hand-off to the reporter must fit in what the exception dispatch leaves.
	ENGINE_DEATH_TEST("Platform/OverflowsAThreadStartedBeforeInstall")
	{
		CrashHandler::Uninstall();
		std::atomic<bool> released{ false };
		bool isInstalled = false; // written before `released`
		std::thread overflowing([&released, &isInstalled]()
		{
			const Result<uint32_t> guarantee = Test::GetThreadStackGuarantee();
			released.wait(false);
			ENGINE_CORE_WARN("Guarantee of the overflowing thread: [{}]", DescribeStackGuarantee(guarantee));
			if (isInstalled)
				ENGINE_CORE_ERROR("The recursion ended: {}", RecurseUntilTheStackOverflows(0, std::numeric_limits<uint64_t>::max()));
		});
		isInstalled = InstallTestCrashHandler();
		released.store(true);
		released.notify_one();
		overflowing.join();
	}

	// Logs the stack that the installing thread, threads started while the handler is installed (one with a 256 KiB
	// stack, one with a 4 MiB stack) and one started after Uninstall keep for their own stack overflow, then returns.
	ENGINE_DEATH_TEST("Platform/ReportsStackGuarantees")
	{
		if (!InstallTestCrashHandler())
			return;
		const auto getNewThreadsGuarantee = []()
		{
			Result<uint32_t> guarantee = 0u;
			std::thread thread([&guarantee]()
			{
				guarantee = Test::GetThreadStackGuarantee();
			});
			thread.join();
			return guarantee;
		};
		ENGINE_CORE_WARN("Installing thread: [{}]", DescribeStackGuarantee(Test::GetThreadStackGuarantee()));
		ENGINE_CORE_WARN("Started while installed: [{}]", DescribeStackGuarantee(getNewThreadsGuarantee()));
		ENGINE_CORE_WARN("Started with a small stack: [{}]", DescribeStackGuarantee(Test::GetNewThreadStackGuarantee(256 * 1024)));
		ENGINE_CORE_WARN("Started with a large stack: [{}]", DescribeStackGuarantee(Test::GetNewThreadStackGuarantee(4 * 1024 * 1024)));
		CrashHandler::Uninstall();
		ENGINE_CORE_WARN("Started after Uninstall: [{}]", DescribeStackGuarantee(getNewThreadsGuarantee()));
	}

	// Hands WriteFatalErrorReport a message that faults when read: the crash handler itself crashes before it holds a copy
	// of the fatal error's reason.
	ENGINE_DEATH_TEST("Platform/FaultsReadingAFatalErrorMessage")
	{
		if (!InstallTestCrashHandler())
			return;
		const Result<Test::UnreadableText> message = Test::AllocateTextEndingInUnreadableBytes(0, 16);
		if (!message.has_value())
		{
			ENGINE_CORE_ERROR("The crash child cannot allocate the message: {}", message.error());
			return;
		}
		const Result<std::filesystem::path> report = CrashHandler::WriteFatalErrorReport(FatalErrorKind::Assert, message->Text);
		ENGINE_CORE_ERROR("The fatal error report returned: [{}]", DescribeReportResult(report));
	}

	// Hands WriteFatalErrorReport a message of FaultingMessageReadableBytes readable bytes followed by bytes that fault when
	// read: the crash handler copies the start of the reason, then crashes while it writes the report.
	ENGINE_DEATH_TEST("Platform/FaultsWhileWritingAFatalErrorReport")
	{
		if (!InstallTestCrashHandler())
			return;
		const Result<Test::UnreadableText> message = Test::AllocateTextEndingInUnreadableBytes(FaultingMessageReadableBytes, 16);
		if (!message.has_value())
		{
			ENGINE_CORE_ERROR("The crash child cannot allocate the message: {}", message.error());
			return;
		}
		const Result<std::filesystem::path> report = CrashHandler::WriteFatalErrorReport(FatalErrorKind::Assert, message->Text);
		ENGINE_CORE_ERROR("The fatal error report returned: [{}]", DescribeReportResult(report));
	}

	// Crashes while this thread holds the process heap's lock, as an access violation inside HeapAlloc on a corrupt heap
	// does. The reporter thread blocks on the lock as soon as it allocates.
	ENGINE_DEATH_TEST("Platform/CrashesHoldingTheHeapLock")
	{
		if (!InstallTestCrashHandler())
			return;
		const Status locked = Test::LockProcessHeap();
		if (!locked.has_value())
		{
			ENGINE_CORE_ERROR("The crash child cannot lock the process heap: {}", locked.error());
			return;
		}
		CrashHandler::SimulateCrash();
	}

	// Passes a null stream to fclose: the C runtime's parameter check (a debug runtime assertion in Debug builds, the
	// invalid-parameter handler otherwise).
	ENGINE_DEATH_TEST("Platform/PassesAnInvalidParameterToTheCRuntime")
	{
		if (!InstallTestCrashHandler())
			return;
		std::FILE* volatile stream = nullptr;
		ENGINE_CORE_ERROR("fclose returned {}", std::fclose(stream));
	}
#endif

#if defined(ENGINE_PLATFORM_WINDOWS) && defined(_DEBUG)
	// Indexes a vector out of range: the MSVC STL's debug check, which reports through the debug runtime and then ends the
	// process with __fastfail.
	ENGINE_DEATH_TEST("Platform/FailsAnStlDebugCheck")
	{
		if (!InstallTestCrashHandler())
			return;
		const std::vector<int> values(1);
		const volatile size_t index = values.size();
		ENGINE_CORE_ERROR("The out-of-range element is {}", values[index]);
	}
#endif

	// Installs the handler without a report directory, tries a fatal-error report, then crashes.
	ENGINE_DEATH_TEST("Platform/CrashesWithoutReports")
	{
		if (!InstallTestCrashHandler())
			return;
		const Result<std::filesystem::path> report = CrashHandler::WriteFatalErrorReport(FatalErrorKind::Gpu, "Not written");
		ENGINE_CORE_WARN("Fatal error report: [{}]", DescribeReportResult(report));
		CrashHandler::SimulateCrash();
	}

	// Writes fatal-error reports around an Uninstall and a second Install, then returns.
	ENGINE_DEATH_TEST("Platform/WritesFatalErrorReports")
	{
		const auto write = [](std::string_view label)
		{
			const Result<std::filesystem::path> report = CrashHandler::WriteFatalErrorReport(FatalErrorKind::DeviceLost,
				"Simulated device loss");
			if (report.has_value())
				ENGINE_CORE_WARN("{} report: [{}]", label, Test::PathToUtf8(*report));
			else
				ENGINE_CORE_WARN("{} report failed: [{}]", label, report.error());
		};

		if (!InstallTestCrashHandler())
			return;
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::PlayState, "Play");
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::Scene, "Levels/Level2.scene");
		write("First");
		write("Second");

		CrashHandler::Uninstall();
		ENGINE_CORE_WARN("Installed after Uninstall: [{}]", CrashHandler::IsInstalled());
		const Result<std::filesystem::path> afterUninstall = CrashHandler::WriteFatalErrorReport(FatalErrorKind::DeviceLost,
			"Ignored");
		ENGINE_CORE_WARN("Report after Uninstall: [{}]", DescribeReportResult(afterUninstall));
		CrashHandler::SetBreadcrumb(CrashBreadcrumb::Scene, "Ignored after Uninstall");

		if (!InstallTestCrashHandler())
			return;
		write("Third");
	}

	// Installs with a report directory below a regular file, which cannot be created.
	ENGINE_DEATH_TEST("Platform/InstallFailsOnBadDirectory")
	{
		CrashHandler::Uninstall();
		const std::filesystem::path file = Test::PathFromUtf8(Test::GetTestOptions().ChildArgument);
		const Status installed = CrashHandler::Install({ .ReportDirectory = file / "Crashes", .AppName = std::string(TestAppName) });
		const std::string_view outcome = installed.has_value() ? std::string_view("succeeded")
															   : ErrorCodeToString(installed.error().GetCode());
		ENGINE_CORE_WARN("Install: [{}] installed: [{}]", outcome, CrashHandler::IsInstalled());
	}

	namespace {

		// A handler child's process ID and result.
		struct HandlerChildResult
		{
			uint32_t Id = 0;
			ProcessResult Result{};
		};

	}

	// Runs `Tests --death-test=<deathTest> --child-argument=<childArgument>`, and waits for it at most a minute.
	static Result<HandlerChildResult> RunHandlerChild(std::string_view deathTest, const std::string& childArgument)
	{
		const ProcessSpecification specification = Test::MakeTestsChildSpecification({
			std::format("--death-test={}", deathTest),
			"--child-argument=" + childArgument,
		});
		ENGINE_TRY_ASSIGN(Process child, Process::Spawn(specification));
		HandlerChildResult result;
		result.Id = child.GetId();
		ENGINE_TRY_ASSIGN(result.Result, child.Wait(std::chrono::seconds(60)));
		return result;
	}

	// Runs `Tests --crash-child` with its user data in `directory` and returns the child's result.
	static Result<ProcessResult> RunCrashChild(const Test::TempDirectory& directory, std::vector<std::string> extraArguments = {})
	{
		std::vector<std::string> arguments = { "--crash-child", "--user-data-dir=" + Test::PathToUtf8(directory.GetPath()) };
		for (std::string& argument : extraArguments)
			arguments.push_back(std::move(argument));
		return Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), std::chrono::seconds(60));
	}

	// The text from just after `prefix` to the end of its line; empty when `prefix` is missing.
	static std::string FindLineValue(const std::string& text, std::string_view prefix)
	{
		const size_t start = text.find(prefix);
		if (start == std::string::npos)
			return {};
		const size_t valueStart = start + prefix.size();
		const size_t end = text.find_first_of("\r\n", valueStart);
		return text.substr(valueStart, end == std::string::npos ? std::string::npos : end - valueStart);
	}

	// The one crash report a handler child wrote into `reports`, after the checks every such report must pass: its name
	// ends with the child's process ID, and the child's stderr line names it.
	static Result<std::string> ReadHandlerChildReport(const HandlerChildResult& child, const std::filesystem::path& reports)
	{
		const std::vector<std::filesystem::path> files = Test::ListCrashFiles(reports, ".txt");
		if (files.size() != 1)
			return MakeError(ErrorCode::NotFound, "expected one crash report in '{}', found {}", Test::PathToUtf8(reports), files.size());
		const std::string fileName = Test::PathToUtf8(files.front().filename());
		if (!fileName.ends_with(std::format("-{}.txt", child.Id)))
			return MakeError(ErrorCode::Validation, "the report '{}' does not name process {}", fileName, child.Id);
		if (Test::PathFromUtf8(FindLineValue(child.Result.StandardError, "; report written to ")) != files.front())
			return MakeError(ErrorCode::Validation, "the stderr line does not name the report '{}'", fileName);
		return FileSystem::ReadText(files.front());
	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("CrashHandler: child crash produces report and exit code 4")
		{
			Test::TempDirectory directory("CrashChild");
			const Result<ProcessResult> child = RunCrashChild(directory);
			REQUIRE(child.has_value());
			CHECK(child->ExitCode == ExitCode::Crash);
			CHECK(child->StandardError.contains("report written to"));

			const Result<std::string> report = Test::ReadOnlyCrashReport(directory.GetPath());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->starts_with("Crash report"));
			CHECK(report->contains("Reason: "));
			CHECK(report->contains(std::string("App: ") + ENGINE_PRODUCT_NAME));
			CHECK(report->contains("Build: "));
			CHECK(report->contains("Process: "));
			CHECK(report->contains("Breadcrumbs:"));
			CHECK(report->contains("FramePhase: Crash child"));
			CHECK(report->contains("Stack trace:"));
			CHECK(report->contains("Last log lines:"));
			// The child's log reached the report: ProcessContext logs every step it initialized.
			CHECK(report->contains("Process context: CrashHandler initialized"));
		}

		TEST_CASE("CrashHandler: a fatal error in a child writes a report naming its kind")
		{
			Test::TempDirectory directory("FatalChild");
			const Result<ProcessResult> child = RunCrashChild(directory, { "--child-argument=fatal-error" });
			REQUIRE(child.has_value());
			CHECK(child->ExitCode == ExitCode::Crash);
			CHECK(child->StandardError.contains("Fatal error (DeviceLost): Simulated device loss"));

			const Result<std::string> report = Test::ReadOnlyCrashReport(directory.GetPath());
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("Reason: Fatal error (DeviceLost): Simulated device loss"));
			CHECK(report->contains("FramePhase: Crash child"));
		}

		TEST_CASE("CrashHandler: the Tests process has the handler installed")
		{
			// Truncation of overlong breadcrumbs is checked in a report by "an installed handler turns a crash into a
			// complete report and exit code 4".
			CHECK(CrashHandler::IsInstalled());
		}

		TEST_CASE("CrashHandler: an installed handler turns a crash into a complete report and exit code 4")
		{
			Test::TempDirectory directory("CrashReport");
			const std::filesystem::path reports = directory / "Crashes"; // Install creates it
			const Result<HandlerChildResult> child = RunHandlerChild("Platform/CrashesWithReport", Test::PathToUtf8(reports));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			const std::string& standardError = child->Result.StandardError;
			INFO("child stderr: ", standardError);
			CHECK(child->Result.ExitCode == ExitCode::Crash);
			CHECK(standardError.contains(std::format("Crash: {}", SimulatedCrashReason)));

			const std::vector<std::filesystem::path> files = Test::ListCrashFiles(reports, ".txt");
			REQUIRE(files.size() == 1);
			const Result<std::string> report = ReadHandlerChildReport(*child, reports);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			INFO("report: ", *report);
			CHECK(report->starts_with(std::format("Crash report\nReason: {}", SimulatedCrashReason)));
			const std::array<std::string, 7> sections = {
				"\nReason: ",
				std::format("\nApp: {}\n", TestAppName),
				std::format("Build: {}\n", TestBuildInfo),
				std::format("Process: {}\n", child->Id),
				"Breadcrumbs:\n",
				"\nStack trace:\n  #0 0x",
				"\nLast log lines:\n",
			};
			CHECK(Test::ContainsInOrder(*report, sections));

			// Every breadcrumb has a line; long values are cut at MaxBreadcrumbLength.
			CHECK(report->contains("\n  Scene: Levels/Level1.scene\n"));
			CHECK(report->contains("\n  PlayState: (none)\n"));
			CHECK(report->contains(std::format("\n  AutomationMethod: {}\n", std::string(CrashHandler::MaxBreadcrumbLength, 'm'))));
			CHECK(report->contains("\n  ScriptCallback: (none)\n"));
			CHECK(report->contains("\n  FramePhase: (none)\n"));

			// The last LogLineCount entries: lines 45 to 299 and the long line, which is cut at MaxLogLineLength.
			CHECK(report->contains("Captured log line [45]"));
			CHECK(report->contains("Captured log line [299]"));
			CHECK_FALSE(report->contains("Captured log line [44]"));
			const size_t longStart = report->find("Long line L");
			REQUIRE(longStart != std::string::npos);
			const size_t lineStart = report->rfind('\n', longStart) + 1;
			const size_t lineEnd = report->find('\n', longStart);
			CHECK(lineEnd - lineStart == CrashHandler::MaxLogLineLength + 2); // "  " and a line cut at the limit

#if defined(ENGINE_PLATFORM_WINDOWS)
			// The frames are symbolized from the PDB, and a minidump lies next to the report.
			CHECK(report->contains("SimulateCrash"));
			std::filesystem::path dump = files.front();
			dump.replace_extension(".dmp");
			const Result<FileInfo> dumpInfo = FileSystem::GetInfo(dump);
			REQUIRE(dumpInfo.has_value());
			CHECK(dumpInfo->Size > 0);
#endif
		}

		TEST_CASE("CrashHandler: abort() is reported as a crash with exit code 4")
		{
			Test::TempDirectory directory("CrashAbort");
			const std::filesystem::path reports = directory / "Crashes";
			const Result<HandlerChildResult> child = RunHandlerChild("Platform/AbortsWithReport", Test::PathToUtf8(reports));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stderr: ", child->Result.StandardError);
			CHECK(child->Result.ExitCode == ExitCode::Crash);
			CHECK(child->Result.StandardError.contains(std::format("Crash: {}", AbortReason)));
			CHECK(child->Result.StandardError.contains("; report written to "));

			const Result<std::string> report = ReadHandlerChildReport(*child, reports);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains(std::format("\nReason: {}", AbortReason)));
		}

		TEST_CASE("CrashHandler: a crash on another thread is reported like one on the main thread")
		{
			Test::TempDirectory directory("CrashThread");
			const std::filesystem::path reports = directory / "Crashes";
			const Result<HandlerChildResult> child = RunHandlerChild("Platform/CrashesOnAnotherThread", Test::PathToUtf8(reports));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stderr: ", child->Result.StandardError);
			CHECK(child->Result.ExitCode == ExitCode::Crash);
			CHECK(child->Result.StandardError.contains(std::format("Crash: {}", SimulatedCrashReason)));
			CHECK(Test::ListCrashFiles(reports, ".txt").size() == 1);
		}

		TEST_CASE("CrashHandler: a stack overflow on the thread that installed the handler is reported")
		{
			Test::TempDirectory directory("CrashOverflow");
			const std::filesystem::path reports = directory / "Crashes";
			const Result<HandlerChildResult> child = RunHandlerChild("Platform/OverflowsTheStack", Test::PathToUtf8(reports));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			const std::string& standardError = child->Result.StandardError;
			INFO("child stderr: ", standardError);
			CHECK(child->Result.ExitCode == ExitCode::Crash);
			CHECK_FALSE(standardError.contains("The recursion ended"));
			// The handler finished on its normal path: nothing faulted inside it.
			CHECK_FALSE(standardError.contains("crash handler fault"));
#if defined(ENGINE_PLATFORM_WINDOWS)
			CHECK(standardError.contains(std::format("Crash: {}", StackOverflowReason)));
#else
			// The guard page below the stack faults: SIGSEGV on Linux, SIGSEGV or SIGBUS on macOS.
			CHECK((standardError.contains("Crash: SIGSEGV") || standardError.contains("Crash: SIGBUS")));
#endif
			const Result<std::string> report = ReadHandlerChildReport(*child, reports);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("\nStack trace:\n  #0 0x"));
		}

#if defined(ENGINE_PLATFORM_WINDOWS)
		TEST_CASE("CrashHandler: a stack overflow on another thread is reported on Windows")
		{
			Test::TempDirectory directory("CrashOverflowThread");
			const std::filesystem::path reports = directory / "Crashes";
			const Result<HandlerChildResult> child = RunHandlerChild("Platform/OverflowsAnotherThreadsStack", Test::PathToUtf8(reports));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stderr: ", child->Result.StandardError);
			CHECK(child->Result.ExitCode == ExitCode::Crash);
			CHECK(child->Result.StandardError.contains(std::format("Crash: {}", StackOverflowReason)));
			// The overflowed thread kept enough stack to hand the crash over: nothing faulted inside the handler.
			CHECK_FALSE(child->Result.StandardError.contains("crash handler fault"));
			const Result<std::string> report = ReadHandlerChildReport(*child, reports);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains(std::format("\nReason: {}", StackOverflowReason)));
		}

		TEST_CASE("CrashHandler: a stack overflow on a thread started before Install is reported on Windows")
		{
			// The thread keeps only the system's guard region, which the exception dispatch shares with the crash path up to
			// the hand-off to the reporter.
			Test::TempDirectory directory("CrashOverflowEarlyThread");
			const std::filesystem::path reports = directory / "Crashes";
			const Result<HandlerChildResult> child =
				RunHandlerChild("Platform/OverflowsAThreadStartedBeforeInstall", Test::PathToUtf8(reports));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			const std::string& standardError = child->Result.StandardError;
			INFO("child stderr: ", standardError);
			CHECK(Test::FindBracketedValue(standardError, "Guarantee of the overflowing thread: ") == "0");
			CHECK(child->Result.ExitCode == ExitCode::Crash);
			CHECK_FALSE(standardError.contains("The recursion ended"));
			CHECK(standardError.contains(std::format("Crash: {}", StackOverflowReason)));
			CHECK_FALSE(standardError.contains("crash handler fault"));
			const Result<std::string> report = ReadHandlerChildReport(*child, reports);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains(std::format("\nReason: {}", StackOverflowReason)));
		}

		TEST_CASE("CrashHandler: threads started while the handler is installed keep 64 KiB of stack for their own overflow on Windows")
		{
			// Without it a thread keeps only the system's guard region (12 KiB), which the exception dispatch and the crash
			// path share after an overflow; the dispatch alone takes more of it on CPUs with larger processor state.
			const Result<HandlerChildResult> child = RunHandlerChild("Platform/ReportsStackGuarantees", "");
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			const std::string& standardError = child->Result.StandardError;
			INFO("child stderr: ", standardError);
			CHECK(child->Result.ExitCode == ExitCode::Failed); // the body returns: "returned without dying"
			CHECK(Test::FindBracketedValue(standardError, "Installing thread: ") == "65536");
			CHECK(Test::FindBracketedValue(standardError, "Started while installed: ") == "65536");
			// A thread gives at most a sixteenth of its stack, and never more than 64 KiB (a sixteenth of 4 MiB is 256 KiB).
			CHECK(Test::FindBracketedValue(standardError, "Started with a small stack: ") == "16384");
			CHECK(Test::FindBracketedValue(standardError, "Started with a large stack: ") == "65536");
			CHECK(Test::FindBracketedValue(standardError, "Started after Uninstall: ") == "0");
		}

		TEST_CASE("CrashHandler: a fault inside the crash handler still prints the line and exits with code 4 on Windows")
		{
			// Before, the second fault on the thread that held the crash report ended the process at once with code 4,
			// without the line or a report.
			Test::TempDirectory directory("CrashHandlerFault");
			const std::filesystem::path reports = directory / "Crashes";
			const Result<HandlerChildResult> child = RunHandlerChild("Platform/FaultsReadingAFatalErrorMessage", Test::PathToUtf8(reports));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			const std::string& standardError = child->Result.StandardError;
			INFO("child stderr: ", standardError);
			CHECK(child->Result.ExitCode == ExitCode::Crash);
			CHECK_FALSE(standardError.contains("The fatal error report returned"));
			// The handler faulted before it held the fatal error's reason, so the fault is the reason too.
			constexpr std::string_view Fault = "Access violation (0xc0000005)";
			const std::string handlerFault = std::format("; crash handler fault on the crashing thread: {} at 0x", Fault);
			CHECK(standardError.contains(std::format("Crash: {} reading address 0x", Fault)));
			CHECK(standardError.contains(handlerFault));
			const Result<std::string> report = ReadHandlerChildReport(*child, reports);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->starts_with(std::format("Crash report\nReason: {} reading address 0x", Fault)));
			CHECK(FindLineValue(*report, "\nReason: ").contains(handlerFault));
			CHECK(report->contains("\nLast log lines:\n"));
		}

		TEST_CASE("CrashHandler: a fault while a fatal-error report is written keeps the fatal error as the reason on Windows")
		{
			Test::TempDirectory directory("CrashFatalFault");
			const std::filesystem::path reports = directory / "Crashes";
			const Result<HandlerChildResult> child =
				RunHandlerChild("Platform/FaultsWhileWritingAFatalErrorReport", Test::PathToUtf8(reports));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			const std::string& standardError = child->Result.StandardError;
			INFO("child stderr: ", standardError);
			CHECK(child->Result.ExitCode == ExitCode::Crash);
			CHECK_FALSE(standardError.contains("The fatal error report returned"));
			// The reason keeps the start of the message; the fault that interrupted the report follows it.
			const std::string reason = "Fatal error (Assert): " + std::string(64, 'x');
			const std::string handlerFault = "; crash handler fault on the crashing thread: Access violation (0xc0000005) at 0x";
			const std::string line = FindLineValue(standardError, "Crash: ");
			CHECK(line.starts_with(reason));
			CHECK(line.contains(handlerFault));
			CHECK(line.size() < FaultingMessageReadableBytes);

			// The interrupted report was written again under its own name: one complete report and one minidump.
			const Result<std::string> report = ReadHandlerChildReport(*child, reports);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			INFO("report: ", report->substr(0, 4096));
			CHECK(report->starts_with("Crash report\nReason: " + reason));
			CHECK(FindLineValue(*report, "\nReason: ").contains(handlerFault));
			CHECK(report->contains("\nStack trace:\n  #0 0x"));
			CHECK(report->contains("\nLast log lines:\n"));
			CHECK(Test::ListCrashFiles(reports, ".dmp").size() == 1);
		}

		TEST_CASE("CrashHandler: a crash that leaves the heap locked still exits with code 4")
		{
			// The reporter blocks on the heap lock the crashing thread holds; the crashing thread waits for it only for a
			// bounded time, then prints its line and ends the process. Before, the process hung forever.
			Test::TempDirectory directory("CrashHeapLock");
			const std::filesystem::path reports = directory / "Crashes";
			const Result<HandlerChildResult> child = RunHandlerChild("Platform/CrashesHoldingTheHeapLock", Test::PathToUtf8(reports));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			const std::string& standardError = child->Result.StandardError;
			INFO("child stderr: ", standardError);
			CHECK(child->Result.ExitCode == ExitCode::Crash);
			CHECK(standardError.contains(std::format("Crash: {}", SimulatedCrashReason)));
			CHECK_FALSE(standardError.contains("; report written to "));

			// Where the reporter blocks depends on what the OS allocates: once the file exists (on current Windows it does,
			// and the reporter blocks in DbgHelp), the report holds the reason and the breadcrumbs and the stderr line calls
			// it incomplete; before that, nothing is written.
			const std::vector<std::filesystem::path> files = Test::ListCrashFiles(reports, ".txt");
			REQUIRE(files.size() <= 1);
			if (files.empty())
			{
				CHECK(standardError.contains("; no report written\n"));
			}
			else
			{
				CHECK(Test::PathFromUtf8(FindLineValue(standardError, "; incomplete report written to ")) == files.front());
				const Result<std::string> report = FileSystem::ReadText(files.front());
				REQUIRE(report.has_value());
				CHECK(report->starts_with(std::format("Crash report\nReason: {}", SimulatedCrashReason)));
				CHECK(report->contains("\nBreadcrumbs:\n"));
				CHECK_FALSE(report->contains("\nLast log lines:\n"));
			}
		}

		TEST_CASE("CrashHandler: an invalid parameter passed to the C runtime is a crash with exit code 4")
		{
	#if defined(_DEBUG)
			// The debug runtime reports the failed check first; the handler takes that report.
			constexpr std::string_view Reason = "C runtime assertion: ";
	#else
			constexpr std::string_view Reason = "Invalid parameter passed to a C runtime function";
	#endif
			Test::TempDirectory directory("CrashInvalidParameter");
			const std::filesystem::path reports = directory / "Crashes";
			const Result<HandlerChildResult> child =
				RunHandlerChild("Platform/PassesAnInvalidParameterToTheCRuntime", Test::PathToUtf8(reports));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stderr: ", child->Result.StandardError);
			CHECK(child->Result.ExitCode == ExitCode::Crash);
			CHECK_FALSE(child->Result.StandardError.contains("fclose returned"));
			CHECK(child->Result.StandardError.contains(std::format("Crash: {}", Reason)));
			const Result<std::string> report = ReadHandlerChildReport(*child, reports);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains(std::format("\nReason: {}", Reason)));
		}
#endif

#if defined(ENGINE_PLATFORM_WINDOWS) && defined(_DEBUG)
		TEST_CASE("CrashHandler: a failed STL debug check is a crash with exit code 4 and a report")
		{
			Test::TempDirectory directory("CrashStlCheck");
			const std::filesystem::path reports = directory / "Crashes";
			const Result<HandlerChildResult> child = RunHandlerChild("Platform/FailsAnStlDebugCheck", Test::PathToUtf8(reports));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			const std::string& standardError = child->Result.StandardError;
			INFO("child stderr: ", standardError);
			CHECK(child->Result.ExitCode == ExitCode::Crash);
			CHECK_FALSE(standardError.contains("The out-of-range element is"));
			CHECK(standardError.contains("Crash: C runtime assertion: "));
			CHECK(standardError.contains("vector subscript out of range"));
			const Result<std::string> report = ReadHandlerChildReport(*child, reports);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->contains("\nReason: C runtime assertion: "));
		}
#endif

		TEST_CASE("CrashHandler: without a report directory a crash still exits with code 4 and writes nothing")
		{
			const Result<HandlerChildResult> child = RunHandlerChild("Platform/CrashesWithoutReports", "");
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			const std::string& standardError = child->Result.StandardError;
			INFO("child stderr: ", standardError);
			CHECK(child->Result.ExitCode == ExitCode::Crash);
			CHECK(Test::FindBracketedValue(standardError, "Fatal error report: ") == "InvalidState");
			CHECK(standardError.contains(std::format("Crash: {}", SimulatedCrashReason)));
			CHECK(standardError.contains("; no report written\n"));
		}

		TEST_CASE("CrashHandler: WriteFatalErrorReport writes a report naming the kind and returns its path")
		{
			Test::TempDirectory directory("CrashFatal");
			const std::filesystem::path reports = directory / "Crashes";
			const Result<HandlerChildResult> child = RunHandlerChild("Platform/WritesFatalErrorReports", Test::PathToUtf8(reports));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			const std::string& standardError = child->Result.StandardError;
			INFO("child stderr: ", standardError);
			CHECK(child->Result.ExitCode == ExitCode::Failed); // the body returns: "returned without dying"

			const std::filesystem::path first = Test::PathFromUtf8(Test::FindBracketedValue(standardError, "First report: "));
			const std::filesystem::path second = Test::PathFromUtf8(Test::FindBracketedValue(standardError, "Second report: "));
			const std::filesystem::path third = Test::PathFromUtf8(Test::FindBracketedValue(standardError, "Third report: "));
			CHECK(first.parent_path() == reports);
			CHECK(second.parent_path() == reports);
			CHECK(third.parent_path() == reports);
			// Reports of the same second and process get distinct names.
			CHECK(first != second);
			CHECK(second != third);
			CHECK(Test::ListCrashFiles(reports, ".txt").size() == 3);
#if defined(ENGINE_PLATFORM_WINDOWS)
			CHECK(Test::ListCrashFiles(reports, ".dmp").size() == 3);
#endif

			const Result<std::string> firstReport = FileSystem::ReadText(first);
			REQUIRE(firstReport.has_value());
			CHECK(firstReport->starts_with("Crash report\nReason: Fatal error (DeviceLost): Simulated device loss\n"));
			CHECK(firstReport->contains(std::format("Process: {}\n", child->Id)));
			CHECK(firstReport->contains("\n  Scene: Levels/Level2.scene\n"));
			CHECK(firstReport->contains("\n  PlayState: Play\n"));
			CHECK(firstReport->contains("\nStack trace:\n  #0 0x"));
			CHECK(firstReport->contains("\nLast log lines:\n"));

			// Uninstall clears the breadcrumbs and ignores new ones until the next Install.
			CHECK(Test::FindBracketedValue(standardError, "Installed after Uninstall: ") == "false");
			CHECK(Test::FindBracketedValue(standardError, "Report after Uninstall: ") == "InvalidState");
			const Result<std::string> thirdReport = FileSystem::ReadText(third);
			REQUIRE(thirdReport.has_value());
			CHECK(thirdReport->contains("\n  Scene: (none)\n"));
			CHECK(thirdReport->contains("\n  PlayState: (none)\n"));
		}

		TEST_CASE("CrashHandler: Install fails with Io when the report directory cannot be created and installs nothing")
		{
			Test::TempDirectory directory("CrashInstall");
			const std::filesystem::path file = directory / "File";
			const std::string text = "in the way";
			REQUIRE(FileSystem::WriteFileAtomic(file, AsBytes(text)).has_value());

			const Result<HandlerChildResult> child = RunHandlerChild("Platform/InstallFailsOnBadDirectory", Test::PathToUtf8(file));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stderr: ", child->Result.StandardError);
			CHECK(child->Result.StandardError.contains("Install: [Io] installed: [false]"));
		}

		TEST_CASE("CrashHandler: CrashBreadcrumbToString names every breadcrumb")
		{
			CHECK(CrashBreadcrumbToString(CrashBreadcrumb::Scene) == "Scene");
			CHECK(CrashBreadcrumbToString(CrashBreadcrumb::PlayState) == "PlayState");
			CHECK(CrashBreadcrumbToString(CrashBreadcrumb::AutomationMethod) == "AutomationMethod");
			CHECK(CrashBreadcrumbToString(CrashBreadcrumb::ScriptCallback) == "ScriptCallback");
			CHECK(CrashBreadcrumbToString(CrashBreadcrumb::FramePhase) == "FramePhase");
		}
	}

}
