#include "TestsPCH.h"

#include "Engine/Core/RingBufferSink.h"

#include <thread>

namespace Engine {

	static LogEntry MakeEntry(LogLevel level, LogChannel channel, std::string message)
	{
		LogEntry entry;
		entry.Level = level;
		entry.Logger = channel;
		entry.Message = std::move(message);
		return entry;
	}

	static LogQuery MakeQuery(uint64_t cursor, size_t limit)
	{
		LogQuery query;
		query.Cursor = cursor;
		query.Limit = limit;
		return query;
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("RingBufferSink: sequence numbers start at 1 and increase by one per entry")
		{
			RingBufferSink sink(8);
			CHECK(sink.GetNextSeq() == 1);
			CHECK(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, "a")) == 1);
			CHECK(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, "b")) == 2);
			CHECK(sink.GetNextSeq() == 3);
			CHECK(sink.GetSize() == 2);
			CHECK(sink.GetCapacity() == 8);

			const LogReadResult result = sink.Read(MakeQuery(0, 100));
			REQUIRE(result.Entries.size() == 2);
			CHECK(result.Entries[0].Seq == 1);
			CHECK(result.Entries[0].Message == "a");
			CHECK(result.Entries[1].Seq == 2);
			CHECK(result.NextCursor == 3);
			CHECK(result.DroppedCount == 0);
		}

		TEST_CASE("RingBufferSink: Append replaces the Seq of the entry and keeps every other field")
		{
			RingBufferSink sink(4);
			LogEntry entry = MakeEntry(LogLevel::Warn, LogChannel::Script, "kept");
			entry.Seq = 999;
			entry.TimeNs = 1234;
			entry.Tick = 77;
			entry.File = "Ball.luau";
			entry.Line = 5;
			entry.EntityId = UUID(42);
			entry.ScriptFile = "Assets/Scripts/Ball.luau";
			entry.ScriptLine = 6;
			CHECK(sink.Append(entry) == 1);

			const std::vector<LogEntry> stored = sink.ReadLast(1);
			REQUIRE(stored.size() == 1);
			CHECK(stored[0].Seq == 1);
			CHECK(stored[0].TimeNs == 1234);
			CHECK(stored[0].Tick == 77);
			CHECK(stored[0].Level == LogLevel::Warn);
			CHECK(stored[0].Logger == LogChannel::Script);
			CHECK(stored[0].Message == "kept");
			CHECK(stored[0].File == "Ball.luau");
			CHECK(stored[0].Line == 5);
			CHECK(stored[0].EntityId == UUID(42));
			CHECK(stored[0].ScriptFile == "Assets/Scripts/Ball.luau");
			CHECK(stored[0].ScriptLine == 6);
		}

		TEST_CASE("RingBufferSink: a full buffer overwrites the oldest entries and reports them as dropped")
		{
			RingBufferSink sink(4);
			for (int index = 1; index <= 10; ++index)
				static_cast<void>(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, std::format("entry {}", index))));

			CHECK(sink.GetSize() == 4);
			const LogReadResult result = sink.Read(MakeQuery(3, 100));
			REQUIRE(result.Entries.size() == 4);
			CHECK(result.Entries.front().Seq == 7);
			CHECK(result.Entries.back().Seq == 10);
			CHECK(result.Entries.back().Message == "entry 10");
			CHECK(result.DroppedCount == 4); // entries 3 to 6 were overwritten
			CHECK(result.NextCursor == 11);
		}

		TEST_CASE("RingBufferSink: cursor 0 starts at the oldest held entry and reports nothing dropped")
		{
			RingBufferSink sink(3);
			for (int index = 1; index <= 5; ++index)
				static_cast<void>(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, std::to_string(index))));

			const LogReadResult result = sink.Read(MakeQuery(0, 100));
			REQUIRE(result.Entries.size() == 3);
			CHECK(result.Entries[0].Seq == 3);
			CHECK(result.Entries[0].Message == "3");
			CHECK(result.Entries[2].Seq == 5);
			CHECK(result.DroppedCount == 0);
			CHECK(result.NextCursor == 6);
		}

		TEST_CASE("RingBufferSink: a cursor at or past the end returns nothing and resumes at the next entry")
		{
			RingBufferSink sink(4);
			static_cast<void>(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, "only")));

			const LogReadResult atEnd = sink.Read(MakeQuery(sink.GetNextSeq(), 100));
			CHECK(atEnd.Entries.empty());
			CHECK(atEnd.NextCursor == 2);
			CHECK(atEnd.DroppedCount == 0);

			// A cursor from an earlier, longer-lived buffer.
			const LogReadResult pastEnd = sink.Read(MakeQuery(500, 100));
			CHECK(pastEnd.Entries.empty());
			CHECK(pastEnd.NextCursor == 2);
			CHECK(pastEnd.DroppedCount == 0);

			const LogReadResult empty = RingBufferSink(2).Read(MakeQuery(0, 100));
			CHECK(empty.Entries.empty());
			CHECK(empty.NextCursor == 1);
		}

		TEST_CASE("RingBufferSink: queries filter by level, channel and substring and resume at NextCursor")
		{
			RingBufferSink sink(16);
			static_cast<void>(sink.Append(MakeEntry(LogLevel::Trace, LogChannel::Engine, "trace noise")));
			static_cast<void>(sink.Append(MakeEntry(LogLevel::Warn, LogChannel::Script, "script warning")));
			static_cast<void>(sink.Append(MakeEntry(LogLevel::Error, LogChannel::Engine, "engine error")));
			static_cast<void>(sink.Append(MakeEntry(LogLevel::Error, LogChannel::App, "app error")));
			static_cast<void>(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, "engine info")));

			LogQuery query = MakeQuery(0, 100);
			query.MinimumLevel = LogLevel::Warn;
			query.Channels = { LogChannel::Engine, LogChannel::Script };
			query.Contains = "r";
			const LogReadResult first = sink.Read(query);
			REQUIRE(first.Entries.size() == 2);
			CHECK(first.Entries[0].Message == "script warning");
			CHECK(first.Entries[1].Message == "engine error");
			CHECK(first.NextCursor == 6); // every entry was scanned

			static_cast<void>(sink.Append(MakeEntry(LogLevel::Critical, LogChannel::Engine, "engine critical error")));
			query.Cursor = first.NextCursor;
			const LogReadResult second = sink.Read(query);
			REQUIRE(second.Entries.size() == 1);
			CHECK(second.Entries[0].Seq == 6);
			CHECK(second.NextCursor == 7);
		}

		TEST_CASE("RingBufferSink: the substring filter is case-sensitive")
		{
			RingBufferSink sink(4);
			static_cast<void>(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, "Loaded Level1")));

			LogQuery query = MakeQuery(0, 100);
			query.Contains = "level1";
			CHECK(sink.Read(query).Entries.empty());
			query.Contains = "Level1";
			CHECK(sink.Read(query).Entries.size() == 1);
		}

		TEST_CASE("RingBufferSink: the limit stops the scan after the last returned entry")
		{
			RingBufferSink sink(16);
			for (int index = 0; index < 5; ++index)
				static_cast<void>(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, "line")));

			const LogReadResult result = sink.Read(MakeQuery(0, 2));
			REQUIRE(result.Entries.size() == 2);
			CHECK(result.Entries[1].Seq == 2);
			CHECK(result.NextCursor == 3);

			const LogReadResult none = sink.Read(MakeQuery(0, 0));
			CHECK(none.Entries.empty());
			CHECK(none.NextCursor == 1);
		}

		TEST_CASE("RingBufferSink: ReadLast returns the newest entries in ascending order")
		{
			RingBufferSink sink(8);
			CHECK(sink.ReadLast(3).empty());
			for (int index = 1; index <= 5; ++index)
				static_cast<void>(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, std::to_string(index))));

			const std::vector<LogEntry> last = sink.ReadLast(3);
			REQUIRE(last.size() == 3);
			CHECK(last[0].Message == "3");
			CHECK(last[2].Message == "5");
			CHECK(sink.ReadLast(100).size() == 5);
			CHECK(sink.ReadLast(0).empty());
		}

		TEST_CASE("RingBufferSink: a capacity of 1 keeps only the newest entry")
		{
			RingBufferSink sink(1);
			static_cast<void>(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, "old")));
			static_cast<void>(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, "new")));

			CHECK(sink.GetSize() == 1);
			const LogReadResult result = sink.Read(MakeQuery(1, 100));
			REQUIRE(result.Entries.size() == 1);
			CHECK(result.Entries[0].Message == "new");
			CHECK(result.Entries[0].Seq == 2);
			CHECK(result.DroppedCount == 1);
		}

		TEST_CASE("RingBufferSink: concurrent appends keep every sequence number unique")
		{
			constexpr int ThreadCount = 4;
			constexpr int EntriesPerThread = 500;
			RingBufferSink sink(ThreadCount * EntriesPerThread);

			std::vector<std::thread> threads;
			for (int thread = 0; thread < ThreadCount; ++thread)
			{
				threads.emplace_back([&sink]()
				{
					for (int index = 0; index < EntriesPerThread; ++index)
						static_cast<void>(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, "concurrent")));
				});
			}
			for (std::thread& thread : threads)
				thread.join();

			const LogReadResult result = sink.Read(MakeQuery(0, ThreadCount * EntriesPerThread));
			REQUIRE(result.Entries.size() == static_cast<size_t>(ThreadCount * EntriesPerThread));
			for (size_t index = 0; index < result.Entries.size(); ++index)
				CHECK(result.Entries[index].Seq == index + 1);
		}

		TEST_CASE("RingBufferSink: level and channel names round-trip case-insensitively")
		{
			for (const LogLevel level : { LogLevel::Trace, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Critical })
				CHECK(LogLevelFromString(LogLevelToString(level)) == level);
			for (const LogChannel channel : { LogChannel::Engine, LogChannel::App, LogChannel::Script })
				CHECK(LogChannelFromString(LogChannelToString(channel)) == channel);

			CHECK(LogLevelToString(LogLevel::Warn) == "Warn");
			CHECK(LogLevelToString(LogLevel::Critical) == "Critical");
			CHECK(LogChannelToString(LogChannel::App) == "App");
			CHECK(LogLevelFromString("error") == LogLevel::Error);
			CHECK(LogLevelFromString("tRaCe") == LogLevel::Trace);
			CHECK(LogChannelFromString("SCRIPT") == LogChannel::Script);
			CHECK_FALSE(LogLevelFromString("Fatal").has_value());
			CHECK_FALSE(LogLevelFromString("Warning").has_value());
			CHECK_FALSE(LogLevelFromString("War").has_value());
			CHECK_FALSE(LogChannelFromString("").has_value());
			CHECK_FALSE(LogChannelFromString("Engine ").has_value());
		}
	}

}
