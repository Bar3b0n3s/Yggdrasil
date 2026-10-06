#include "TestsPCH.h"

#include "Engine/Core/Log.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Profiler.h"
#include "Support/DeathTest.h"
#include "Support/ExpectLog.h"
#include "Support/TempDirectory.h"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

namespace Engine {

	namespace {

		// A static whose destructor, once armed, logs, reads the ring buffer, flushes and ends the process with
		// FatalError. It is constant-initialized, so it is destroyed after every function-local static, the ones the log
		// creates lazily included (Log.h: nothing crashes after Shutdown; FatalError.h: it works during static
		// destruction).
		class StaticDestructorLogger
		{
		public:
			constexpr StaticDestructorLogger() = default;

			~StaticDestructorLogger()
			{
				if (!m_Armed)
					return;
				ENGINE_CORE_WARN("Warning from a static destructor");
				const size_t ringBufferSize = Log::GetRingBuffer().GetSize();
				Log::Flush();
				FatalError(FatalErrorKind::Gpu, std::format("Fatal error from a static destructor, ring buffer size {}", ringBufferSize));
			}

			StaticDestructorLogger(const StaticDestructorLogger&) = delete;
			StaticDestructorLogger& operator=(const StaticDestructorLogger&) = delete;
			StaticDestructorLogger(StaticDestructorLogger&&) = delete;
			StaticDestructorLogger& operator=(StaticDestructorLogger&&) = delete;

			void Arm() { m_Armed = true; }
		private:
			bool m_Armed = false;
		};

	}

	// Armed only by the death test Core/LogFromStaticDestructorAfterShutdown.
	static constinit StaticDestructorLogger s_StaticDestructorLogger;

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

	// The whole content of a file; nullopt when it cannot be opened. The log is tested independently of Core/FileSystem.
	static std::optional<std::string> ReadWholeFile(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream.is_open())
			return std::nullopt;

		const std::streamoff size = stream.tellg();
		if (size < 0)
			return std::nullopt;
		std::string content(static_cast<size_t>(size), '\0');
		stream.seekg(0);
		stream.read(content.data(), static_cast<std::streamsize>(size));
		if (!stream)
			return std::nullopt;
		return content;
	}

	static bool WriteWholeFile(const std::filesystem::path& path, std::string_view content)
	{
		std::ofstream stream(path, std::ios::binary | std::ios::trunc);
		stream.write(content.data(), static_cast<std::streamsize>(content.size()));
		return stream.good();
	}

	// A specification that writes only to `filePath` (no console output from tests that log a lot).
	static LogSpecification MakeFileOnlySpecification(const std::filesystem::path& filePath)
	{
		LogSpecification specification;
		specification.FilePath = filePath;
		specification.Console = false;
		return specification;
	}

	static std::filesystem::path RotatedLogPath(const std::filesystem::path& path, size_t index)
	{
		std::filesystem::path name = path.stem();
		name += std::format(".{}", index);
		name += path.extension();
		return path.parent_path() / name;
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

	// The fallback logger, the empty ring buffer and the listener registry are created here, during main, so they would be
	// destroyed before s_StaticDestructorLogger if they were plain function-local statics. The process then ends like a
	// normal exit (the profiler and the log shut down, std::exit runs the static destructors); the armed destructor logs
	// and calls FatalError, which must still exit with code 4.
	ENGINE_DEATH_TEST("Core/LogFromStaticDestructorAfterShutdown")
	{
		Profiler::Shutdown();
		Log::Shutdown();
		ENGINE_CORE_WARN("Fallback logger in use");
		static_cast<void>(Log::GetRingBuffer().GetSize());
		const uint64_t listener = Log::AddListener([](const LogEntry& /*entry*/) {});
		Log::RemoveListener(listener);
		s_StaticDestructorLogger.Arm();
		std::exit(0);
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

	// Console = false: the warning reaches the ring buffer but not stderr. Back on the fallback logger after Shutdown, the
	// assert reports on stderr how many entries were stored.
	ENGINE_DEATH_TEST("Core/LogWithoutConsoleWritesNothingToStderr")
	{
		Log::Shutdown();
		LogSpecification specification;
		specification.Console = false;
		const Status initialized = Log::Initialize(specification);
		ENGINE_CORE_ASSERT(initialized.has_value(), "Initialize failed");
		ENGINE_CORE_WARN("Warning without a console");
		const size_t stored = Log::GetRingBuffer().GetSize();

		// Back on the fallback logger, the assert message reaches stderr again.
		Log::Shutdown();
		ENGINE_CORE_ASSERT(false, "Stored {} entries without a console", stored);
	}

	// An assertion that fails inside a listener logs its report from within the dispatch. The report is stored and
	// printed but not dispatched again, so the process ends with code 4 instead of deadlocking or recursing.
	ENGINE_DEATH_TEST("Core/LogAssertInsideListener")
	{
		const uint64_t listener = Log::AddListener([](const LogEntry& entry)
		{
			ENGINE_CORE_ASSERT(!entry.Message.contains("trigger"), "Listener rejected '{}'", entry.Message);
		});
		static_cast<void>(listener);
		ENGINE_CORE_WARN("Listener trigger entry");
	}

	ENGINE_DEATH_TEST("Core/LogAddListenerFromListener")
	{
		const uint64_t listener = Log::AddListener([](const LogEntry& /*entry*/)
		{
			const uint64_t nested = Log::AddListener([](const LogEntry& /*nestedEntry*/) {});
			static_cast<void>(nested);
		});
		static_cast<void>(listener);
		ENGINE_CORE_WARN("Entry that registers a listener");
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("Log: macros reach the ring buffer with level, logger, location and context")
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
			CHECK(warning.Line == info.Line + 2);
			CHECK_FALSE(warning.Tick.has_value());
			CHECK_FALSE(warning.EntityId.IsValid());
			CHECK(warning.ScriptFile.empty());
			CHECK(warning.ScriptLine == 0);
			CHECK(warning.TimeNs >= info.TimeNs);
		}

		TEST_CASE("Log: Trace is recorded in Debug and Release")
		{
			const uint64_t cursor = Log::GetRingBuffer().GetNextSeq();
			ENGINE_CORE_TRACE("Trace detail {}", 1);
			ENGINE_TRACE("Client trace detail");

			const std::vector<LogEntry> entries = EntriesSince(cursor);
			REQUIRE(entries.size() == 2);
			CHECK(entries[0].Level == LogLevel::Trace);
			CHECK(entries[0].Logger == LogChannel::Engine);
			CHECK(entries[0].Message == "Trace detail 1");
			CHECK(entries[1].Logger == LogChannel::App);
		}

		TEST_CASE("Log: declared errors are recorded at Error and Critical level")
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

		TEST_CASE("Log: the Script logger records Script entries")
		{
			const uint64_t cursor = Log::GetRingBuffer().GetNextSeq();
			const spdlog::source_loc scriptLocation{ "Assets/Scripts/Ball.luau", 12, "OnUpdate" };
			Log::GetLogger(LogChannel::Script).log(scriptLocation, spdlog::level::info, "Ball speed {}", 3);

			const std::vector<LogEntry> entries = EntriesSince(cursor);
			REQUIRE(entries.size() == 1);
			CHECK(entries[0].Logger == LogChannel::Script);
			CHECK(entries[0].Level == LogLevel::Info);
			CHECK(entries[0].Message == "Ball speed 3");
			CHECK(entries[0].File == "Assets/Scripts/Ball.luau");
			CHECK(entries[0].Line == 12);
		}

		TEST_CASE("Log: listeners receive every stored entry until removed")
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

		TEST_CASE("Log: listeners run in registration order, see the stored Seq and get distinct IDs")
		{
			std::vector<std::string> calls;
			const uint64_t first = Log::AddListener([&calls](const LogEntry& entry)
			{
				calls.push_back(std::format("first {}", entry.Seq));
			});
			const uint64_t second = Log::AddListener([&calls](const LogEntry& entry)
			{
				calls.push_back(std::format("second {}", entry.Seq));
			});
			const uint64_t seq = Log::GetRingBuffer().GetNextSeq();
			ENGINE_CORE_INFO("Ordered listeners");
			Log::RemoveListener(first);
			Log::RemoveListener(second);
			Log::RemoveListener(second); // unknown IDs are ignored

			CHECK(first != second);
			REQUIRE(calls.size() == 2);
			CHECK(calls[0] == std::format("first {}", seq));
			CHECK(calls[1] == std::format("second {}", seq));
		}

		TEST_CASE("Log: listeners stay registered across Shutdown and Initialize and receive nothing in between")
		{
			std::vector<std::string> received;
			const uint64_t listener = Log::AddListener([&received](const LogEntry& entry)
			{
				received.push_back(entry.Message);
			});

			Log::Shutdown();
			ENGINE_CORE_INFO("Logged while shut down"); // discarded by the fallback logger
			RestoreTestLog();
			ENGINE_CORE_INFO("Logged after Initialize");
			Log::RemoveListener(listener);

			REQUIRE(received.size() == 1);
			CHECK(received[0] == "Logged after Initialize");
		}

		TEST_CASE("Log: an assertion inside a listener ends the process with code 4")
		{
			ENGINE_CHECK_DEATH("Core/LogAssertInsideListener", "Listener rejected 'Listener trigger entry'");
		}

		TEST_CASE("Log: AddListener from inside a listener is a programmer error")
		{
			ENGINE_CHECK_DEATH("Core/LogAddListenerFromListener", "Log::AddListener must not be called from a log listener");
		}

		TEST_CASE("Log: each channel has its own named logger")
		{
			CHECK(Log::GetLogger(LogChannel::Engine).name() == "Engine");
			CHECK(Log::GetLogger(LogChannel::App).name() == "App");
			CHECK(Log::GetLogger(LogChannel::Script).name() == "Script");
		}

		TEST_CASE("Log: Initialize while initialized is a programmer error")
		{
			ENGINE_CHECK_DEATH("Core/LogInitializeTwice", "Assertion failed: !IsInitialized()");
		}

		TEST_CASE("Log: Initialize starts a new ring buffer with the requested capacity")
		{
			ENGINE_CORE_INFO("An entry in the previous ring buffer");

			Log::Shutdown();
			LogSpecification specification;
			specification.Console = false;
			specification.RingBufferCapacity = 8;
			const Status initialized = Log::Initialize(specification);
			const uint64_t firstSeq = Log::GetRingBuffer().GetNextSeq();
			const size_t capacity = Log::GetRingBuffer().GetCapacity();
			for (int index = 0; index < 10; ++index)
				ENGINE_CORE_INFO("Entry {}", index);
			const size_t size = Log::GetRingBuffer().GetSize();
			const std::vector<LogEntry> last = Log::GetRingBuffer().ReadLast(1);
			RestoreTestLog();

			REQUIRE(initialized.has_value());
			CHECK(firstSeq == 1);
			CHECK(capacity == 8);
			CHECK(size == 8);
			REQUIRE(last.size() == 1);
			CHECK(last[0].Seq == 10);
			CHECK(last[0].Message == "Entry 9");
		}

		TEST_CASE("Log: the file sink limits are 10 MB per file and 5 files")
		{
			CHECK(LogFileMaxBytes == 10u * 1024u * 1024u);
			CHECK(LogFileCount == 5u);
		}

		TEST_CASE("Log: FilePath creates the log directory and file and Flush writes the entries")
		{
			Test::TempDirectory directory("LogFile");
			const std::filesystem::path logPath = directory / "Logs/Nested/Tests.log";

			Log::Shutdown();
			const Status initialized = Log::Initialize(MakeFileOnlySpecification(logPath));
			ENGINE_CORE_INFO("Written to the log file {}", 42);
			ENGINE_TRACE("Client trace in the file");
			Log::Flush();
			const std::optional<std::string> flushed = ReadWholeFile(logPath);
			RestoreTestLog(); // also closes the file, so the temporary directory can be removed on every host

			REQUIRE(initialized.has_value());
			REQUIRE(flushed.has_value());
			CHECK(flushed->contains("Written to the log file 42"));
			CHECK(flushed->contains("Client trace in the file"));
			CHECK(flushed->contains("[Engine]"));
			CHECK(flushed->contains("[App]"));
		}

		TEST_CASE("Log: an existing log file is appended to")
		{
			Test::TempDirectory directory("LogAppend");
			const std::filesystem::path logPath = directory / "Tests.log";

			Log::Shutdown();
			const Status first = Log::Initialize(MakeFileOnlySpecification(logPath));
			ENGINE_CORE_INFO("First session");
			Log::Shutdown();
			const Status second = Log::Initialize(MakeFileOnlySpecification(logPath));
			ENGINE_CORE_INFO("Second session");
			RestoreTestLog();

			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			const std::optional<std::string> content = ReadWholeFile(logPath);
			REQUIRE(content.has_value());
			const size_t firstPosition = content->find("First session");
			const size_t secondPosition = content->find("Second session");
			REQUIRE(firstPosition != std::string::npos);
			REQUIRE(secondPosition != std::string::npos);
			CHECK(firstPosition < secondPosition);
		}

		TEST_CASE("Log: the file rotates before LogFileMaxBytes and keeps LogFileCount files")
		{
			Test::TempDirectory directory("LogRotation");
			const std::filesystem::path logPath = directory / "Logs/Rotation.log";

			// Large entries reach the limits quickly. A file holds fewer than LogFileMaxBytes / PayloadSize of them, so the
			// files together hold fewer than EntryCount: the oldest entries must have been dropped.
			constexpr size_t PayloadSize = 256 * 1024;
			constexpr size_t EntryCount = LogFileCount * (LogFileMaxBytes / PayloadSize) + 16;
			const std::string payload(PayloadSize, 'x');

			Log::Shutdown();
			LogSpecification specification = MakeFileOnlySpecification(logPath);
			specification.RingBufferCapacity = 1; // the entries are large and only the files matter here
			const Status initialized = Log::Initialize(specification);
			for (size_t index = 0; index < EntryCount; ++index)
				ENGINE_CORE_INFO("Entry {:04} {}", index, payload);
			RestoreTestLog();
			REQUIRE(initialized.has_value());

			std::vector<std::string> files; // newest first
			for (size_t index = 0; index < LogFileCount; ++index)
			{
				const std::filesystem::path path = index == 0 ? logPath : RotatedLogPath(logPath, index);
				std::optional<std::string> content = ReadWholeFile(path);
				REQUIRE_MESSAGE(content.has_value(), "missing log file ", path.generic_string());
				CHECK(content->size() <= LogFileMaxBytes);
				files.push_back(std::move(*content));
			}
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(RotatedLogPath(logPath, LogFileCount), error));

			const std::string newest = std::format("Entry {:04} ", EntryCount - 1);
			CHECK(files.front().contains(newest));
			for (const std::string& content : files)
				CHECK_FALSE(content.contains("Entry 0000 "));

			// Each older file ends before the next newer one starts.
			for (size_t index = 0; index + 1 < files.size(); ++index)
			{
				const size_t newerFirst = files[index].find("Entry ");
				const size_t olderLast = files[index + 1].rfind("Entry ");
				REQUIRE(newerFirst != std::string::npos);
				REQUIRE(olderLast != std::string::npos);
				CHECK(files[index + 1].substr(olderLast, 10) < files[index].substr(newerFirst, 10));
			}
		}

		TEST_CASE("Log: Initialize fails with Io when the log file cannot be created")
		{
			Test::TempDirectory directory("LogFileBlocked");
			REQUIRE(WriteWholeFile(directory / "Blocker", "a regular file"));

			std::filesystem::path blockedPath;
			SUBCASE("a file is in the way of the directory")
			{
				blockedPath = directory / "Blocker/Logs/Tests.log";
			}
			SUBCASE("the path is a file name of an existing directory")
			{
				std::error_code error;
				REQUIRE(std::filesystem::create_directories(directory / "Logs/Tests.log", error));
				blockedPath = directory / "Logs/Tests.log";
			}
			SUBCASE("a directory name is not valid in the host's path encoding")
			{
				// An unpaired UTF-16 surrogate on Windows, an invalid UTF-8 byte elsewhere: the error message must still be
				// produced, without the exception the standard UTF-8 accessors throw for such a Windows name.
				using NativeChar = std::filesystem::path::value_type;
				std::basic_string<NativeChar> name;
				name += static_cast<NativeChar>('B');
				name += static_cast<NativeChar>(sizeof(NativeChar) == 1 ? 0xff : 0xd800);
				blockedPath = directory / "Blocker" / std::filesystem::path(name) / "Logs/Tests.log";
			}

			Log::Shutdown();
			const Status failed = Log::Initialize(MakeFileOnlySpecification(blockedPath));
			const bool initializedAfterFailure = Log::IsInitialized();
			RestoreTestLog();

			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::Io);
			CHECK(failed.error().GetMessageText().contains("Logs"));
			CHECK_FALSE(initializedAfterFailure);
		}

		TEST_CASE("Log: Shutdown is idempotent and logging afterwards is safe")
		{
			Log::Shutdown();
			Log::Shutdown();
			const bool initialized = Log::IsInitialized();
			const uint64_t nextSeq = Log::GetRingBuffer().GetNextSeq();
			const size_t size = Log::GetRingBuffer().GetSize();
			ENGINE_CORE_INFO("Logged while shut down");
			Log::Flush();
			const uint64_t nextSeqAfterLogging = Log::GetRingBuffer().GetNextSeq();
			RestoreTestLog();

			CHECK_FALSE(initialized);
			CHECK(size == 0);
			CHECK(nextSeqAfterLogging == nextSeq);
		}

		TEST_CASE("Log: concurrent logging stores every entry once with consecutive Seq")
		{
			constexpr int ThreadCount = 4;
			constexpr int EntriesPerThread = 200;

			// Without the console sink, so the 800 entries do not flood the test output.
			Log::Shutdown();
			LogSpecification specification;
			specification.Console = false;
			const Status initialized = Log::Initialize(specification);
			const uint64_t cursor = Log::GetRingBuffer().GetNextSeq();

			std::vector<std::thread> threads;
			for (int thread = 0; thread < ThreadCount; ++thread)
			{
				threads.emplace_back([thread]()
				{
					for (int index = 0; index < EntriesPerThread; ++index)
						ENGINE_CORE_TRACE("Thread {} entry {}", thread, index);
				});
			}
			for (std::thread& thread : threads)
				thread.join();

			const std::vector<LogEntry> entries = EntriesSince(cursor);
			RestoreTestLog();

			REQUIRE(initialized.has_value());
			REQUIRE(entries.size() == static_cast<size_t>(ThreadCount * EntriesPerThread));
			std::set<std::string> messages;
			for (size_t index = 0; index < entries.size(); ++index)
			{
				CHECK(entries[index].Seq == cursor + index);
				messages.insert(entries[index].Message);
			}
			CHECK(messages.size() == entries.size());
		}

		TEST_CASE("Log: after Shutdown a fallback logger writes Warn and above to stderr")
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Core/LogFallbackAfterShutdown");
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == FatalCrashExitCode);
			CHECK(result->StandardError.contains("Fallback warning reaches stderr"));
			CHECK(result->StandardError.contains("End of the fallback logger test"));
			CHECK_FALSE(result->StandardError.contains("Fallback info is discarded"));
		}

		TEST_CASE("Log: a static destructor can log and call FatalError after Shutdown")
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Core/LogFromStaticDestructorAfterShutdown");
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == FatalCrashExitCode);
			CHECK(result->StandardError.contains("Warning from a static destructor"));
			CHECK(result->StandardError.contains("Fatal error (Gpu): Fatal error from a static destructor, ring buffer size 0"));
		}

		TEST_CASE("Log: ConsoleLevel filters the console but not the ring buffer")
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Core/LogConsoleLevelFiltersOnlyTheConsole");
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == FatalCrashExitCode);
			CHECK(result->StandardError.contains("Console-visible warning"));
			CHECK_FALSE(result->StandardError.contains("Console-filtered info"));
			CHECK(result->StandardError.contains("The ring buffer stored 2 entries"));
		}

		TEST_CASE("Log: Console false writes nothing to stderr")
		{
			const Result<Test::DeathTestResult> result = Test::RunDeathTest("Core/LogWithoutConsoleWritesNothingToStderr");
			REQUIRE(result.has_value());
			CHECK(result->ExitCode == FatalCrashExitCode);
			CHECK_FALSE(result->StandardError.contains("Warning without a console"));
			CHECK(result->StandardError.contains("Stored 1 entries without a console"));
		}
	}

}
