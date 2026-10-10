#include "TestsPCH.h"
#include "Engine/Asset/ReplayData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Scripting/ScriptCompiler.h"

#include <nlohmann/json.hpp>

#include <array>
#include <limits>

namespace Engine {

	namespace {

		ReplayDocument Document()
		{
			ReplayDocument d;
			d.Header.Scene = { UUID(19), "Assets/Scenes/Test.scene" };
			d.Header.Parameters.Set(Json{ { "level", 2 }, { "name", "snow ☃" }, { "nested", Json::array({ true, 0.25f }) } });
			d.Header.Seed = std::numeric_limits<uint64_t>::max();
			d.Header.EngineVersion = "1";
			d.Header.Config = "Debug";
			d.FinalTick = 2;
			d.FinalStateHash = "0123456789abcdef";
			d.Events = {
				{ .Tick = 0, .Type = "Action", .Name = "Jump", .State = "Tap" },
				{ .Tick = 0, .Type = "Action", .Name = "Move", .Value = 0.25f },
				{ .Tick = 0, .Type = "Key", .State = "Down", .KeyName = "Space" },
				{ .Tick = 0, .Type = "MouseButton", .State = "Up", .ButtonName = "Left", .Position = glm::vec2(1, 2) },
				{ .Tick = 0, .Type = "MouseMove", .Position = glm::vec2(2, 3) },
				{ .Tick = 0, .Type = "MouseDelta", .Delta = glm::vec2(-1, 4) },
				{ .Tick = 0, .Type = "Scroll", .Delta = glm::vec2(0, 2) },
				{ .Tick = 0, .Type = "GamepadButton", .State = "Down", .ButtonName = "South", .Gamepad = 3 },
				{ .Tick = 1, .Type = "GamepadAxis", .Value = 0.7f, .Gamepad = 2, .AxisName = "LeftTrigger" },
				{ .Tick = 1, .Type = "Text", .Text = "hello ☃" }
			};
			return d;
		}

		ReplayBytecodeExpectation Predicate(uint64_t tick)
		{
			auto compiled = ScriptCompiler::Compile({ .Source = "true", .Mode = ScriptCompileMode::Expression, .ChunkName = "=replay-test", .JsonPointer = "/Expect/0/Luau" });
			REQUIRE(compiled.has_value());
			ReplayBytecodeExpectation e;
			e.Tick = tick;
			e.Script.Bytecode = std::move(compiled->Bytecode);
			e.Script.SourceMap = std::move(compiled->SourceMap);
			return e;
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("ReplayData: canonical source round trips every input event and header parameter")
		{
			const auto d = Document();
			auto text = ReplayToText(d);
			REQUIRE(text.has_value());
			ReplayLoadReport report;
			auto read = ReplayFromText(*text, report, true);
			REQUIRE(read.has_value());
			CHECK(report.FileVersion == 1);
			CHECK(report.Diagnostics.empty());
			CHECK(read->Header.Seed == d.Header.Seed);
			CHECK(read->Header.Parameters == d.Header.Parameters);
			auto roundTrip = ReplayToText(*read);
			REQUIRE(roundTrip);
			CHECK(*roundTrip == *text);
			auto cooked = CookReplay(d, {}, 4);
			REQUIRE(cooked.has_value());
			auto loaded = LoadCookedReplay(*cooked);
			REQUIRE(loaded.has_value());
			CHECK((*loaded)->Events.size() == d.Events.size());
			ReplayDocument restored = d;
			restored.Events = (*loaded)->Events;
			auto restoredText = ReplayToText(restored);
			REQUIRE(restoredText);
			CHECK(*restoredText == *text);
		}

		TEST_CASE("ReplayData: scene handles survive moves and configuration is informational")
		{
			auto d = Document();
			auto previous = d.Header.Scene.Handle;
			d.Header.Scene.Path = "Assets/Moved.scene";
			d.Header.Config = "Dist";
			auto cooked = CookReplay(d, {}, 1);
			REQUIRE(cooked.has_value());
			auto loaded = LoadCookedReplay(*cooked);
			REQUIRE(loaded.has_value());
			CHECK((*loaded)->Header.Scene.Handle == previous);
			CHECK((*loaded)->Header.Config == "Dist");
			CHECK((*loaded)->FinalStateHash == d.FinalStateHash);
		}

		TEST_CASE("ReplayData: malformed fields and newer versions return located errors")
		{
			auto json = ReplayToJson(Document());
			REQUIRE(json.has_value());
			ReplayLoadReport report;
			SUBCASE("new version")
			{
				(*json)["Version"] = 2;
				auto result = ReplayFromJson(*json, report);
				REQUIRE_FALSE(result);
				CHECK(result.error().GetCode() == ErrorCode::UnsupportedVersion);
			}
			SUBCASE("finite and bounded")
			{
				(*json)["FixedHz"] = 0;
				auto result = ReplayFromJson(*json, report);
				REQUIRE_FALSE(result);
				CHECK(result.error().GetLocation().JsonPointer == "/FixedHz");
			}
			SUBCASE("inappropriate event member")
			{
				(*json)["Events"][2]["Value"] = 0;
				auto result = ReplayFromJson(*json, report);
				REQUIRE_FALSE(result);
				CHECK(result.error().GetLocation().JsonPointer == "/Events/2/Value");
			}
			SUBCASE("unknown fields")
			{
				(*json)["Future"] = true;
				CHECK(ReplayFromJson(*json, report));
				CHECK(report.Diagnostics.size() == 1);
				CHECK_FALSE(ReplayFromJson(*json, report, true));
			}
			SUBCASE("object parameters")
			{
				(*json)["Parameters"] = Json::array();
				CHECK_FALSE(ReplayFromJson(*json, report));
			}
			SUBCASE("invalid text")
			{
				auto d = Document();
				d.Events.back().Text = std::string(1, static_cast<char>(0xff));
				CHECK_FALSE(ValidateReplayDocument(d));
			}
		}

		TEST_CASE("ReplayData: event and expectation tick boundaries are validated without reordering ties")
		{
			auto d = Document();
			d.Expect = { { 0, "true" }, { 2, "true" } };
			REQUIRE(ValidateReplayDocument(d));
			auto text = ReplayToText(d);
			REQUIRE(text);
			ReplayLoadReport report;
			auto result = ReplayFromText(*text, report);
			REQUIRE(result);
			CHECK(result->Events[0].Name == "Jump");
			CHECK(result->Events[1].Name == "Move");
			d.Events.back().Tick = 2;
			CHECK_FALSE(ValidateReplayDocument(d));
			d.Events.back().Tick = 0;
			CHECK_FALSE(ValidateReplayDocument(d));
			d.Events = {};
			d.Expect.back().Tick = 3;
			CHECK_FALSE(ValidateReplayDocument(d));
			d.Expect = {};
			d.FinalTick = 0;
			CHECK(ValidateReplayDocument(d));
		}

		TEST_CASE("ReplayData: cooked expectations load without a Luau compiler")
		{
			auto d = Document();
			d.Expect = { { 1, "true" } };
			const std::array bytecode{ Predicate(1) };
			auto cooked = CookReplay(d, bytecode, 7);
			REQUIRE(cooked);
			auto loaded = LoadCookedReplay(*cooked);
			REQUIRE(loaded);
			REQUIRE((*loaded)->Expect.size() == 1);
			CHECK((*loaded)->Expect[0].Script.Bytecode == bytecode[0].Script.Bytecode);
			CHECK((*loaded)->Expect[0].Script.SourceMap.JsonPointer == "/Expect/0/Luau");
			CHECK((*loaded)->Expect[0].Script.SourceMap.GeneratedPrefixLines == 1);
		}

		TEST_CASE("ReplayData: cooked payload rejects truncation corrupt hashes and mismatched expectation counts")
		{
			auto d = Document();
			auto cooked = CookReplay(d, {}, 1);
			REQUIRE(cooked);
			for (size_t i = 0; i < cooked->size(); ++i)
				CHECK_FALSE(LoadCookedReplay(std::span<const std::byte>(*cooked).first(i)));
			auto corrupt = *cooked;
			corrupt.back() ^= std::byte{ 1 };
			CHECK_FALSE(LoadCookedReplay(corrupt));
			d.Expect = { { 0, "true" } };
			CHECK_FALSE(CookReplay(d, {}, 1));
			const std::array wrongTick{ Predicate(1) };
			CHECK_FALSE(CookReplay(d, wrongTick, 1));
			auto artifact = ReadCookedArtifact(*cooked);
			REQUIRE(artifact);
			Buffer payload(artifact->Payload.begin(), artifact->Payload.end());
			payload.push_back(std::byte{ 0 });
			CHECK_FALSE(LoadCookedReplay(WriteCookedArtifact(AssetType::Replay, 1, 1, payload)));
		}

		TEST_CASE("ReplayData: seeded source and binary mutations never assert or crash")
		{
			auto d = Document();
			auto text = ReplayToText(d);
			auto cooked = CookReplay(d, {}, 1);
			REQUIRE(text);
			REQUIRE(cooked);
			Random random(731);
			for (uint32_t i = 0; i < 1000; ++i)
			{
				auto source = *text;
				source[random.NextU64() % source.size()] = static_cast<char>(random.NextU32() & 255);
				ReplayLoadReport report;
				auto parsed = ReplayFromText(source, report);
				if (parsed)
					CHECK(ValidateReplayDocument(*parsed));
				auto bytes = *cooked;
				bytes[CookedHeader::Size + random.NextU64() % (bytes.size() - CookedHeader::Size)] ^= std::byte{ 1 };
				CHECK_FALSE(LoadCookedReplay(bytes));
			}
		}
	}

}
