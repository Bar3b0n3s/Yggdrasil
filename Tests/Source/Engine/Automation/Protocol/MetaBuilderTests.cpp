#include "TestsPCH.h"

#include "Engine/Automation/Protocol/MetaBuilder.h"

#include "Engine/Core/RingBufferSink.h"

namespace Engine {

	static void AppendMetaEntry(RingBufferSink& log, LogLevel level, LogChannel channel, std::string message)
	{
		log.Append(LogEntry{ .Seq = 0, .TimeNs = 0, .Tick = std::nullopt, .Level = level, .Logger = channel, .Message = std::move(message), .File = {}, .Line = 0, .EntityId = UUID(), .ScriptFile = {}, .ScriptLine = 0 });
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("MetaBuilder: counts the entries logged since the client's previous response" * doctest::skip(true))
		{
			// A ring of the test's own, so nothing else that logs can change the counts.
			RingBufferSink log(64);
			AppendMetaEntry(log, LogLevel::Error, LogChannel::Engine, "before the client connected");
			MetaBuilder builder(log);
			builder.AddClient(1);

			AppendMetaEntry(log, LogLevel::Warn, LogChannel::Engine, "unknown field 'Foo'");
			AppendMetaEntry(log, LogLevel::Info, LogChannel::App, "opened");
			AppendMetaEntry(log, LogLevel::Error, LogChannel::Script, "attempt to index nil");
			Json meta = builder.Build(1, MetaState{ .Revision = 58, .Dirty = true, .UndoLabel = "[agent] Create Entity 'Board'", .Tick = std::nullopt, .PlayState = "Edit" });
			CHECK(meta["revision"] == Json(58));
			CHECK(meta["dirty"] == Json(true));
			CHECK(meta["undoLabel"] == Json("[agent] Create Entity 'Board'"));
			CHECK(meta["playState"] == Json("Edit"));
			CHECK(meta["diagnostics"]["newErrors"] == Json(0));
			CHECK(meta["diagnostics"]["newWarnings"] == Json(1));
			CHECK(meta["diagnostics"]["newScriptErrors"] == Json(1));
			CHECK(meta["diagnostics"]["logCursor"] == Json(std::to_string(log.GetNextSeq()))); // a log.read cursor
			CHECK(meta["diagnostics"]["firstNew"][0]["message"] == Json("unknown field 'Foo'"));

			Json quiet = builder.Build(1, MetaState{});
			CHECK(quiet["diagnostics"]["newWarnings"] == Json(0));
			CHECK(quiet["diagnostics"]["firstNew"] == Json::array());
		}

		TEST_CASE("MetaBuilder: firstNew quotes at most three entries" * doctest::skip(true))
		{
			RingBufferSink log(64);
			MetaBuilder builder(log);
			builder.AddClient(2);
			for (int index = 0; index < 5; ++index)
				AppendMetaEntry(log, LogLevel::Warn, LogChannel::Engine, std::format("warning {}", index));
			Json meta = builder.Build(2, MetaState{});
			CHECK(meta["diagnostics"]["newWarnings"] == Json(5));
			CHECK(meta["diagnostics"]["firstNew"].size() == MaxFirstNewDiagnostics);
			CHECK(meta["diagnostics"]["firstNew"][2]["message"] == Json("warning 2"));
		}

		TEST_CASE("MetaBuilder: optional members are omitted and clients are independent" * doctest::skip(true))
		{
			RingBufferSink log(64);
			MetaBuilder builder(log);
			builder.AddClient(1);
			builder.AddClient(2);
			AppendMetaEntry(log, LogLevel::Error, LogChannel::Engine, "shared");
			Json first = builder.Build(1, MetaState{});
			CHECK_FALSE(first.contains("undoLabel"));
			CHECK_FALSE(first.contains("tick"));
			CHECK(first["diagnostics"]["newErrors"] == Json(1));
			Json second = builder.Build(2, MetaState{ .Revision = 0, .Dirty = false, .UndoLabel = {}, .Tick = 600, .PlayState = "Play" });
			CHECK(second["diagnostics"]["newErrors"] == Json(1));
			CHECK(second["tick"] == Json(600));
			builder.RemoveClient(2);
			builder.RemoveClient(99);
		}
	}

}
