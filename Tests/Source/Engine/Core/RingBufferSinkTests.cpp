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
		TEST_CASE("RingBufferSink: sequence numbers start at 1 and increase by one per entry" * doctest::skip(true))
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

		TEST_CASE("RingBufferSink: a full buffer overwrites the oldest entries and reports them as dropped" * doctest::skip(true))
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

		TEST_CASE("RingBufferSink: queries filter by level, channel and substring and resume at NextCursor" * doctest::skip(true))
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

		TEST_CASE("RingBufferSink: the limit stops the scan after the last returned entry" * doctest::skip(true))
		{
			RingBufferSink sink(16);
			for (int index = 0; index < 5; ++index)
				static_cast<void>(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, "line")));

			const LogReadResult result = sink.Read(MakeQuery(0, 2));
			REQUIRE(result.Entries.size() == 2);
			CHECK(result.Entries[1].Seq == 2);
			CHECK(result.NextCursor == 3);
		}

		TEST_CASE("RingBufferSink: ReadLast returns the newest entries in ascending order" * doctest::skip(true))
		{
			RingBufferSink sink(8);
			for (int index = 1; index <= 5; ++index)
				static_cast<void>(sink.Append(MakeEntry(LogLevel::Info, LogChannel::Engine, std::to_string(index))));

			const std::vector<LogEntry> last = sink.ReadLast(3);
			REQUIRE(last.size() == 3);
			CHECK(last[0].Message == "3");
			CHECK(last[2].Message == "5");
			CHECK(sink.ReadLast(100).size() == 5);
		}

		TEST_CASE("RingBufferSink: concurrent appends keep every sequence number unique" * doctest::skip(true))
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

		TEST_CASE("RingBufferSink: level and channel names round-trip case-insensitively" * doctest::skip(true))
		{
			for (const LogLevel level : { LogLevel::Trace, LogLevel::Info, LogLevel::Warn, LogLevel::Error, LogLevel::Critical })
				CHECK(LogLevelFromString(LogLevelToString(level)) == level);
			for (const LogChannel channel : { LogChannel::Engine, LogChannel::App, LogChannel::Script })
				CHECK(LogChannelFromString(LogChannelToString(channel)) == channel);

			CHECK(LogLevelToString(LogLevel::Warn) == "Warn");
			CHECK(LogLevelFromString("error") == LogLevel::Error);
			CHECK(LogChannelFromString("SCRIPT") == LogChannel::Script);
			CHECK_FALSE(LogLevelFromString("Fatal").has_value());
			CHECK_FALSE(LogChannelFromString("").has_value());
		}
	}

}
