#include "TestsPCH.h"

#include "Engine/Core/Log.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/FileSystem.h"
#include "Support/DeathTest.h"
#include "Support/ExpectLog.h"
#include "Support/TempDirectory.h"

#include <mutex>

namespace Engine {

	// Entries appended to the process ring buffer since `cursor`.
	static std::vector<LogEntry> EntriesSince(uint64_t cursor)
	{
		LogQuery query;
		query.Cursor = cursor;
		query.Limit = 1000;
		return Log::GetRingBuffer().Read(query).Entries;
	}

	// Restores the configuration the Tests main runs with (console sink only) after a test re-initialized the log. It
	// runs before the test's REQUIREs, so a failing test never leaves the process without its logger.
	static void RestoreTestLog()
	{
		Log::Shutdown();
		const Status restored = Log::Initialize(LogSpecification());
		REQUIRE_MESSAGE(restored.has_value(), "the Tests log configuration could not be restored");
	}

	ENGINE_DEATH_TEST("Core/LogInitializeTwice")
	{
		// The Tests main has initialized the log already.
		const Status second = Log::Initialize(LogSpecification());
		static_cast<void>(second);
	}

	// After Shutdown only the fallback logger is left: Warn and above reach stderr, Info is discarded. The final assert
	// ends the child with exit code 4; its Critical line goes through the fallback logger as well.
	ENGINE_DEATH_TEST("Core/LogFallbackAfterShutdown")
	{
		Log::Shutdown();
		ENGINE_CORE_INFO("Fallback info is discarded");
		ENGINE_CORE_WARN("Fallback warning reaches stderr");
		ENGINE_CORE_ASSERT(false, "End of the fallback logger test");
	}

	// ConsoleLevel filters only the console: both entries reach the ring buffer, only the warning reaches stderr. The
	// assert reports what the ring buffer received.
	ENGINE_DEATH_TEST("Core/LogConsoleLevelFiltersOnlyTheConsole")
	{
		Log::Shutdown();
		LogSpecification specification;
		specification.ConsoleLevel = LogLevel::Warn;
		const Status initialized = Log::Initialize(specification);
		ENGINE_CORE_ASSERT(initialized.has_value(), "Initialize failed");

		const uint64_t cursor = Log::GetRingBuffer().GetNextSeq();
		ENGINE_CORE_INFO("Console-filtered info");
		ENGINE_CORE_WARN("Console-visible warning");
		LogQuery query;
		query.Cursor = cursor;
		const size_t stored = Log::GetRingBuffer().Read(query).Entries.size();
		ENGINE_CORE_ASSERT(false, "The ring buffer stored {} entries", stored);
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("Log: macros reach the ring buffer with level, logger, location and context" * doctest::skip(true))
		{
			REQUIRE(Log::IsInitialized());
			const uint64_t cursor = Log::GetRingBuffer().GetNextSeq();

			LogContext context;
			context.Tick = 120;
			context.Entity = UUID(0x1234567890abcdef);
			context.ScriptFile = "Assets/Scripts/Game.luau";
			context.ScriptLine = 9;
			{
				LogContextScope scope(context);
				ENGINE_CORE_INFO("Loaded {} entities", 14);
			}
			ENGINE_WARN("Client warning without context");

			const std::vector<LogEntry> entries = EntriesSince(cursor);
			REQUIRE(entries.size() == 2);

			const LogEntry& info = entries[0];
			CHECK(info.Seq == cursor);
			CHECK(info.Level == LogLevel::Info);
			CHECK(info.Logger == LogChannel::Engine);
			CHECK(info.Message == "Loaded 14 entities");
			CHECK(info.File.ends_with("LogTests.cpp"));
			CHECK(info.Line > 0);
			REQUIRE(info.Tick.has_value());
			CHECK(*info.Tick == 120);
			CHECK(info.EntityId == UUID(0x1234567890abcdef));
			CHECK(info.ScriptFile == "Assets/Scripts/Game.luau");
			CHECK(info.ScriptLine == 9);

			const LogEntry& warning = entries[1];
			CHECK(warning.Seq == cursor + 1);
			CHECK(warning.Level == LogLevel::Warn);
			CHECK(warning.Logger == LogChannel::App);
			CHECK_FALSE(warning.Tick.has_value());
			CHECK_FALSE(warning.EntityId.IsValid());
			CHECK(warning.ScriptFile.empty());
		}

		TEST_CASE("Log: Trace is recorded in Debug and Release" * doctest::skip(true))
		{
			const uint64_t cursor = Log::GetRingBuffer().GetNextSeq();
			ENGINE_CORE_TRACE("Trace detail {}", 1);
			ENGINE_TRACE("Client trace detail");

			const std::vector<LogEntry> entries = EntriesSince(cursor);
			REQUIRE(entries.size() == 2);
			CHECK(entries[0].Level == LogLevel::Trace);
			CHECK(entries[0].Logger == LogChannel::Engine);
			CHECK(entries[1].Logger == LogChannel::App);
		}

		TEST_CASE("Log: declared errors are recorded at Error and Critical level" * doctest::skip(true))
		{
			const uint64_t cursor = Log::GetRingBuffer().GetNextSeq();
			{
				Test::ExpectLog expectedError(LogLevel::Error, "Failed to load 'Missing.scene'");
				Test::ExpectLog expectedCritical(LogLevel::Critical, "Cannot continue");
				ENGINE_CORE_ERROR("Failed to load '{}'", "Missing.scene");
				ENGINE_CRITICAL("Cannot continue");
			}

			const std::vector<LogEntry> entries = EntriesSince(cursor);
			REQUIRE(entries.size() == 2);
			CHECK(entries[0].Level == LogLevel::Error);
			CHECK(entries[1].Level == LogLevel::Critical);
			CHECK(entries[1].Logger == LogChannel::App);
		}

		TEST_CASE("Log: listeners receive every stored entry until removed" * doctest::skip(true))
		{
			std::vector<std::string> received;
			std::mutex receivedMutex;
			const uint64_t listener = Log::AddListener([&received, &receivedMutex](const LogEntry& entry)
			{
				std::scoped_lock lock(receivedMutex);
				received.push_back(entry.Message);
			});
			CHECK(listener != 0);

			ENGINE_CORE_INFO("First message");
			ENGINE_INFO("Second message");
			Log::RemoveListener(listener);
			ENGINE_CORE_INFO("Not received");

			std::scoped_lock lock(receivedMutex);
			REQUIRE(received.size() == 2);
			CHECK(received[0] == "First message");
			CHECK(received[1] == "Second message");
		}

		TEST_CASE("Log: each channel has its own named logger" * doctest::skip(true))
		{
			CHECK(Log::GetLogger(LogChannel::Engine).name() == "Engine");
			CHECK(Log::GetLogger(LogChannel::App).name() == "App");
			CHECK(Log::GetLogger(LogChannel::Script).name() == "Script");
		}

		TEST_CASE("Log: Initialize while initialized is a programmer error" * doctest::skip(true))
		{
			Test::CheckDeath("Core/LogInitializeTwice", "Assertion failed");
		}

		TEST_CASE("Log: the file sink limits are 10 MB per file and 5 files")
		{
			CHECK(LogFileMaxBytes == 10u * 1024u * 1024u);
			CHECK(LogFileCount == 5u);
		}

		TEST_CASE("Log: FilePath creates the log directory and file and Flush writes the entries" * doctest::skip(true))
		{
			Test::TempDirectory directory("LogFile");
			const std::filesystem::path logPath = directory / "Logs/Nested/Tests.log";

			Log::Shutdown();
			LogSpecification specification;
			specification.FilePath = logPath;
			specification.Console = false;
			const Status initialized = Log::Initialize(specification);
			ENGINE_CORE_INFO("Written to the log file {}", 42);
			Log::Flush();
			const Result<std::string> flushed = FileSystem::ReadText(logPath);
			RestoreTestLog(); // also closes the file, so the temporary directory can be removed on every host

			REQUIRE(initialized.has_value());
			REQUIRE(flushed.has_value());
			CHECK(flushed->contains("Written to the log file 42"));
		}

		TEST_CASE("Log: Initialize fails with Io when the log file cannot be created" * doctest::skip(true))
		{
			Test::TempDirectory directory("LogFileBlocked");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Blocker", AsBytes("a regular file")).has_value());

			Log::Shutdown();
			LogSpecification specification;
			specification.FilePath = directory / "Blocker/Logs/Tests.log"; // a directory would have to replace the file
			const Status failed = Log::Initialize(specification);
			const bool initializedAfterFailure = Log::IsInitialized();
			RestoreTestLog();

			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::Io);
			CHECK_FALSE(initializedAfterFailure);
		}

		TEST_CASE("Log: Shutdown is idempotent and logging afterwards is safe" * doctest::skip(true))
		{
			Log::Shutdown();
			Log::Shutdown();
			const bool initialized = Log::IsInitialized();
			const uint64_t nextSeq = Log::GetRingBuffer().GetNextSeq();
			ENGINE_CORE_INFO("Logged while shut down");
			const uint64_t nextSeqAfterLogging = Log::GetRingBuffer().GetNextSeq();
			RestoreTestLog();

			CHECK_FALSE(initialized);
			CHECK(nextSeqAfterLogging == nextSeq);
		}

		TEST_CASE("Log: after Shutdown a fallback logger writes Warn and above to stderr" * doctest::skip(true))
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Core/LogFallbackAfterShutdown");
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == FatalCrashExitCode);
			CHECK(result->StandardError.contains("Fallback warning reaches stderr"));
			CHECK(result->StandardError.contains("End of the fallback logger test"));
			CHECK_FALSE(result->StandardError.contains("Fallback info is discarded"));
		}

		TEST_CASE("Log: ConsoleLevel filters the console but not the ring buffer" * doctest::skip(true))
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Core/LogConsoleLevelFiltersOnlyTheConsole");
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == FatalCrashExitCode);
			CHECK(result->StandardError.contains("Console-visible warning"));
			CHECK_FALSE(result->StandardError.contains("Console-filtered info"));
			CHECK(result->StandardError.contains("The ring buffer stored 2 entries"));
		}
	}

}
