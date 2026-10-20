#include "TestsPCH.h"
#include "Engine/Asset/ReplayData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Support/Fuzz.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <array>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
		TEST_CASE("ReplayData: recorded parameters in the header form read back unchanged from text and cooked replays")
		{
			// 0.123456789 and 1/3 are not floats: a recording that started from them would replay with other values.
			const Json raw = { { "ratio", 0.123456789 }, { "third", 1.0 / 3.0 }, { "level", 123456789012 }, { "nested", Json{ { "half", 0.5 } } } };
			const auto canonical = CanonicalizeReplayParameters(raw);
			REQUIRE_MESSAGE(canonical, (canonical ? "" : canonical.error().ToString()));
			const auto number = [&canonical](std::string_view name)
			{
				const auto member = JsonReader(*canonical).GetMember(name);
				REQUIRE(member);
				const auto value = member->ReadDouble();
				REQUIRE(value);
				return *value;
			};
			// Floats are stored as float (§6): each reads back as the float nearest the requested value.
			CHECK(static_cast<float>(number("ratio")) == 0.123456789f);
			CHECK(number("ratio") != 0.123456789);
			CHECK(static_cast<float>(number("third")) == 1.0f / 3.0f);
			CHECK(JsonReader(*canonical).ReadMember<uint64_t>("level").value_or(0) == 123456789012u);
			// The members keep their order; each float reads back as its shortest float text.
			CHECK(*canonical == JsonReader::Parse(R"({"ratio":0.12345679,"third":0.33333334,"level":123456789012,"nested":{"half":0.5}})").value_or(Json()));
			const auto again = CanonicalizeReplayParameters(*canonical);
			REQUIRE(again);
			CHECK(*again == *canonical);

			ReplayDocument d = Document();
			d.Header.Parameters.Set(*canonical);
			const auto text = ReplayToText(d);
			REQUIRE(text);
			ReplayLoadReport report;
			const auto loaded = ReplayFromText(*text, report, true);
			REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().ToString()));
			CHECK(loaded->Header.Parameters.Get() == *canonical);
			const auto cooked = CookReplay(d, {}, 1);
			REQUIRE_MESSAGE(cooked, (cooked ? "" : cooked.error().ToString()));
			const auto decoded = LoadCookedReplay(*cooked);
			REQUIRE_MESSAGE(decoded, (decoded ? "" : decoded.error().ToString()));
			CHECK((*decoded)->Header.Parameters.Get() == *canonical);

			// A value the format cannot store fails when the recording starts, located at its member.
			const auto huge = CanonicalizeReplayParameters(Json{ { "big", 1e39 } }, "/parameters");
			REQUIRE_FALSE(huge);
			CHECK(huge.error().GetCode() == ErrorCode::Validation);
			CHECK(huge.error().GetLocation().JsonPointer == std::optional<std::string>("/parameters/big"));
			const auto array = CanonicalizeReplayParameters(Json::array({ 1 }), "/parameters");
			REQUIRE_FALSE(array);
			CHECK(array.error().GetLocation().JsonPointer == std::optional<std::string>("/parameters"));
		}

		TEST_CASE("ReplayData: canonical source round trips every input event and header parameter")
		{
			const auto d = Document();
			auto text = ReplayToText(d);
			REQUIRE(text.has_value());
			ReplayLoadReport report;
			auto read = ReplayFromText(*text, report, true);
			REQUIRE(read.has_value());
			CHECK(report.FileVersion == ReplayFormatVersion);
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
				(*json)["Version"] = ReplayFormatVersion + 1;
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
			CHECK_FALSE(LoadCookedReplay(WriteCookedArtifact(AssetType::Replay, ReplayData::FormatVersion, 1, payload)));
		}

		// M16 friction S2-01 (FX-4): version 2 adds the state-hash checkpoints. A version 1 file still loads, migrated with
		// none (only its final hash is compared, as before), and writes back canonically as version 2, the bytes
		// project.upgrade then writes (Tests/Data/Formats/Replay/v1.upgraded.replay).
		TEST_CASE("ReplayData: a version 1 replay migrates to version 2 with no checkpoints")
		{
			const Result<std::string> v1 = Test::ReadTestDataText("Formats/Replay/v1.replay");
			const Result<std::string> upgraded = Test::ReadTestDataText("Formats/Replay/v1.upgraded.replay");
			REQUIRE(v1.has_value());
			REQUIRE(upgraded.has_value());
			ReplayLoadReport report;
			const auto migrated = ReplayFromText(*v1, report, true);
			REQUIRE_MESSAGE(migrated.has_value(), migrated.error().ToString());
			CHECK(report.FileVersion == 1);
			CHECK(report.Diagnostics.empty());
			CHECK(migrated->Version == ReplayFormatVersion);
			CHECK(migrated->Checkpoints.empty());
			CHECK(migrated->FinalStateHash == "3e0c91a2b4d5f607");
			const auto text = ReplayToText(*migrated);
			REQUIRE(text.has_value());
			CHECK(*text == *upgraded);
			// The written form reads back unchanged, as version 2.
			const auto again = ReplayFromText(*upgraded, report, true);
			REQUIRE(again.has_value());
			CHECK(report.FileVersion == 2);
			CHECK(ReplayToText(*again).value_or("") == *upgraded);

			// Version 1 has no Checkpoints: one there is an unknown member (a warning, an error when strict) and is ignored.
			const auto document = JsonReader::Parse(*v1);
			REQUIRE(document.has_value());
			Json withCheckpoints = *document;
			withCheckpoints["Checkpoints"] = Json::array({ Json{ { "Tick", 60 }, { "StateHash", "0123456789abcdef" } } });
			const auto lenient = ReplayFromJson(withCheckpoints, report);
			REQUIRE(lenient.has_value());
			CHECK(lenient->Checkpoints.empty());
			REQUIRE(report.Diagnostics.size() == 1);
			CHECK(report.Diagnostics[0].JsonPointer == "/Checkpoints");
			CHECK_FALSE(ReplayFromJson(withCheckpoints, report, true).has_value());
			// Version 2 requires them.
			Json missing = JsonReader::Parse(*upgraded).value_or(Json());
			missing.erase("Checkpoints");
			CHECK_FALSE(ReplayFromJson(missing, report).has_value());
		}

		TEST_CASE("ReplayData: checkpoints round trip through the source and cooked replays and are validated")
		{
			auto d = Document();
			d.FinalTick = 200;
			d.Checkpoints = { { .Tick = 60, .StateHash = "00000000000000ff" }, { .Tick = 120, .StateHash = "fedcba9876543210" } };
			REQUIRE(ValidateReplayDocument(d));
			const auto text = ReplayToText(d);
			REQUIRE(text.has_value());
			CHECK(text->find("\"Checkpoints\": [") < text->find("\"FinalTick\""));
			CHECK(text->find("\"Expect\": [") < text->find("\"Checkpoints\": ["));
			ReplayLoadReport report;
			const auto read = ReplayFromText(*text, report, true);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			REQUIRE(read->Checkpoints.size() == 2);
			CHECK(read->Checkpoints[1].Tick == 120);
			CHECK(read->Checkpoints[1].StateHash == "fedcba9876543210");
			const auto cooked = CookReplay(d, {}, 1);
			REQUIRE_MESSAGE(cooked.has_value(), cooked.error().ToString());
			const auto loaded = LoadCookedReplay(*cooked);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			REQUIRE((*loaded)->Checkpoints.size() == 2);
			CHECK((*loaded)->Checkpoints[0].Tick == 60);
			CHECK((*loaded)->Checkpoints[0].StateHash == "00000000000000ff");
			CHECK((*loaded)->Checkpoints[1].StateHash == "fedcba9876543210");
			// Every truncation of the payload is refused.
			for (size_t size = 0; size < cooked->size(); ++size)
				CHECK_FALSE(LoadCookedReplay(std::span<const std::byte>(*cooked).first(size)));
			// A cooked replay of payload version 1 (no checkpoints) is UnsupportedVersion: the cache rebuilds it.
			const auto artifact = ReadCookedArtifact(*cooked);
			REQUIRE(artifact.has_value());
			const auto older = LoadCookedReplay(WriteCookedArtifact(AssetType::Replay, 1, 1, Buffer(artifact->Payload.begin(), artifact->Payload.end())));
			REQUIRE_FALSE(older.has_value());
			CHECK(older.error().GetCode() == ErrorCode::UnsupportedVersion);

			const auto refused = [](const ReplayDocument& document, std::string_view pointer)
			{
				const Status valid = ValidateReplayDocument(document);
				REQUIRE_FALSE(valid.has_value());
				CHECK(valid.error().GetLocation().JsonPointer == std::optional<std::string>(pointer));
			};
			auto unordered = d;
			std::swap(unordered.Checkpoints[0], unordered.Checkpoints[1]);
			refused(unordered, "/Checkpoints/1/Tick");
			auto duplicate = d;
			duplicate.Checkpoints[1].Tick = 60;
			refused(duplicate, "/Checkpoints/1/Tick");
			auto setup = d;
			setup.Checkpoints[0].Tick = 0;
			refused(setup, "/Checkpoints/0/Tick");
			auto atFinal = d;
			atFinal.Checkpoints[1].Tick = d.FinalTick;
			refused(atFinal, "/Checkpoints/1/Tick");
			auto uppercase = d;
			uppercase.Checkpoints[0].StateHash = "00000000000000FF";
			refused(uppercase, "/Checkpoints/0/StateHash");
			auto shortHash = d;
			shortHash.Checkpoints[1].StateHash = "abc";
			refused(shortHash, "/Checkpoints/1/StateHash");
		}

		TEST_CASE("ReplayData: seeded source and binary mutations never assert or crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// The seed corpus (Docs/Decisions/0024-m15-contract.md decision 13): the synthetic document, which holds every
			// event kind, and FeatureTest's committed replays (long event lists, multi-line Luau expectations), each as text
			// and cooked with its expectations compiled as ReplayImporter compiles them. Each iteration draws its seed from its
			// own Random.
			struct ReplaySeed
			{
				std::string Text;
				Buffer Cooked;
				Buffer Payload;
			};
			const auto makeSeed = [](const ReplayDocument& document)
			{
				std::vector<ReplayBytecodeExpectation> expectations;
				for (size_t index = 0; index < document.Expect.size(); ++index)
				{
					const std::string pointer = std::format("/Expect/{}/Luau", index);
					auto compiled = ScriptCompiler::Compile({ .Source = document.Expect[index].Luau, .Mode = ScriptCompileMode::ExpressionOrChunk, .ChunkName = "=replay-fuzz", .JsonPointer = pointer });
					REQUIRE_MESSAGE(compiled.has_value(), compiled.error().ToString());
					ReplayBytecodeExpectation expectation;
					expectation.Tick = document.Expect[index].Tick;
					expectation.Script.Bytecode = std::move(compiled->Bytecode);
					expectation.Script.SourceMap = std::move(compiled->SourceMap);
					expectations.push_back(std::move(expectation));
				}
				auto text = ReplayToText(document);
				auto cooked = CookReplay(document, expectations, 1);
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				REQUIRE_MESSAGE(cooked.has_value(), cooked.error().ToString());
				const auto view = ReadCookedArtifact(*cooked);
				REQUIRE(view.has_value());
				return ReplaySeed{ .Text = std::move(*text), .Cooked = *cooked, .Payload = Buffer(view->Payload.begin(), view->Payload.end()) };
			};
			std::vector<ReplaySeed> seeds;
			seeds.push_back(makeSeed(Document()));
			const std::vector<std::string> featureTestReplays = Test::ReadFuzzSeedTexts(Test::GetRepositoryRoot() / "Projects/FeatureTest/Assets", ".replay");
			REQUIRE(featureTestReplays.size() >= 2);
			for (const std::string& replay : featureTestReplays)
			{
				ReplayLoadReport report;
				const auto parsed = ReplayFromText(replay, report);
				REQUIRE_MESSAGE(parsed.has_value(), parsed.error().ToString());
				REQUIRE_FALSE(parsed->Expect.empty());
				seeds.push_back(makeSeed(*parsed));
			}

			const Test::FuzzRun run = Test::RunFuzzCase(731, 1000, [&seeds](Random& random, uint64_t /*index*/)
			{
				const ReplaySeed& seed = seeds[static_cast<size_t>(random.NextU64() % seeds.size())];
				// The text form: a document the parser accepts is valid.
				std::string source = seed.Text;
				Test::MutateText(source, random);
				ReplayLoadReport report;
				const auto parsed = ReplayFromText(source, report);
				if (parsed)
					CHECK(ValidateReplayDocument(*parsed));
				// One flipped bit after the header always fails the payload hash.
				Buffer bytes = seed.Cooked;
				bytes[CookedHeader::Size + random.NextU64() % (bytes.size() - CookedHeader::Size)] ^= std::byte{ 1 };
				CHECK_FALSE(LoadCookedReplay(bytes));
				// A mutated payload sealed again reaches the cooked decoder, which validates what it decoded.
				Buffer inner = seed.Payload;
				Test::MutateBytes(inner, random);
				const auto decoded = LoadCookedReplay(WriteCookedArtifact(ReplayData::StaticType, ReplayData::FormatVersion, 1, inner));
				if (decoded)
					CHECK((*decoded)->FinalStateHash.size() == 16);
				return parsed.has_value();
			});
			if (Test::IsFuzzSample(run.Range))
				CHECK(run.Counts.Rejected > 0);
		}

		// Docs/Decisions/0030-m19-contract.md decision 2 (A-FPRINT): version 3 adds the start fingerprint after Config. A
		// version 2 file (the reserved fixture Formats/Replay/v2) migrates to version 3 with a null one, nothing recorded, and
		// writes back as the bytes project.upgrade writes (Formats/Replay/v2.upgraded); in version 2 a Fingerprint member is
		// unknown, and version 3 requires it.
		TEST_CASE("ReplayData: a version 2 replay migrates to version 3 with a null fingerprint" * doctest::skip(true))
		{
			const Result<std::string> v2 = Test::ReadTestDataText("Formats/Replay/v2.replay");
			const Result<std::string> upgraded = Test::ReadTestDataText("Formats/Replay/v2.upgraded.replay");
			REQUIRE(v2.has_value());
			REQUIRE(upgraded.has_value());
			ReplayLoadReport report;
			const auto migrated = ReplayFromText(*v2, report, true);
			REQUIRE_MESSAGE(migrated.has_value(), migrated.error().ToString());
			CHECK(report.FileVersion == 2);
			CHECK(report.Diagnostics.empty());
			CHECK(ReplayFormatVersion == ReplayFingerprintFormatVersion);
			CHECK(migrated->Version == ReplayFingerprintFormatVersion);
			CHECK_FALSE(migrated->Fingerprint.has_value());
			REQUIRE(migrated->Checkpoints.size() == 2);
			CHECK(migrated->FinalStateHash == "3e0c91a2b4d5f607");
			const auto text = ReplayToText(*migrated);
			REQUIRE(text.has_value());
			CHECK(*text == *upgraded);
			// The written form reads back unchanged, as version 3.
			const auto again = ReplayFromText(*upgraded, report, true);
			REQUIRE_MESSAGE(again.has_value(), again.error().ToString());
			CHECK(report.FileVersion == ReplayFingerprintFormatVersion);
			CHECK(ReplayToText(*again).value_or("") == *upgraded);

			// Version 2 has no Fingerprint: one there is an unknown member (a warning, an error when strict) and is ignored.
			const auto document = JsonReader::Parse(*v2);
			REQUIRE(document.has_value());
			Json withFingerprint = *document;
			withFingerprint["Fingerprint"] = nullptr;
			const auto lenient = ReplayFromJson(withFingerprint, report);
			REQUIRE(lenient.has_value());
			CHECK_FALSE(lenient->Fingerprint.has_value());
			REQUIRE(report.Diagnostics.size() == 1);
			CHECK(report.Diagnostics[0].JsonPointer == "/Fingerprint");
			CHECK_FALSE(ReplayFromJson(withFingerprint, report, true).has_value());
			// Version 3 requires it.
			Json missing = JsonReader::Parse(*upgraded).value_or(Json());
			missing.erase("Fingerprint");
			CHECK_FALSE(ReplayFromJson(missing, report).has_value());
		}

		// Decision 2: the fingerprint is written after Config, read back member for member, carried by the cooked replay
		// (FormatVersion 3) and validated: ids and handles valid and strictly increasing, component names unique, hashes
		// canonical.
		TEST_CASE("ReplayData: the start fingerprint round trips through the source and cooked replays and is validated" * doctest::skip(true))
		{
			auto d = Document();
			d.Version = ReplayFingerprintFormatVersion;
			ReplayFingerprint fingerprint;
			fingerprint.Entities = {
				{ .Entity = UUID(0x11), .Path = "/Camera", .Hash = "00000000000000a1", .Components = { { .Component = "Transform", .Hash = "00000000000000b1" }, { .Component = "Camera", .Hash = "00000000000000c1" } } },
				{ .Entity = UUID(0x22), .Path = "/Board", .Hash = "00000000000000a2", .Components = { { .Component = "Transform", .Hash = "00000000000000b2" } } },
			};
			fingerprint.Assets = { { .Handle = UUID(0x515), .Path = "Assets/Scripts/Board.luau", .Type = "Script", .Hash = "00000000000000d1" } };
			d.Fingerprint = fingerprint;
			REQUIRE(ValidateReplayDocument(d));
			const auto text = ReplayToText(d);
			REQUIRE(text.has_value());
			CHECK(text->find("\"Config\"") < text->find("\"Fingerprint\": {"));
			CHECK(text->find("\"Fingerprint\": {") < text->find("\"Events\": ["));
			ReplayLoadReport report;
			const auto read = ReplayFromText(*text, report, true);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			REQUIRE(read->Fingerprint.has_value());
			CHECK(*read->Fingerprint == fingerprint);

			const auto cooked = CookReplay(d, {}, ReplayFingerprintFormatVersion);
			REQUIRE_MESSAGE(cooked.has_value(), cooked.error().ToString());
			const auto artifact = ReadCookedArtifact(*cooked);
			REQUIRE(artifact.has_value());
			CHECK(artifact->Header.FormatVersion == ReplayFingerprintFormatVersion);
			const auto loaded = LoadCookedReplay(*cooked);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			REQUIRE((*loaded)->Fingerprint.has_value());
			CHECK(*(*loaded)->Fingerprint == fingerprint);
			for (size_t size = 0; size < cooked->size(); ++size)
				CHECK_FALSE(LoadCookedReplay(std::span<const std::byte>(*cooked).first(size)));

			const auto refused = [&d](const ReplayFingerprint& invalid)
			{
				ReplayDocument copy = d;
				copy.Fingerprint = invalid;
				CHECK_FALSE(ValidateReplayDocument(copy).has_value());
			};
			ReplayFingerprint unordered = fingerprint;
			std::swap(unordered.Entities[0], unordered.Entities[1]);
			refused(unordered);
			ReplayFingerprint invalidId = fingerprint;
			invalidId.Entities[0].Entity = UUID();
			refused(invalidId);
			ReplayFingerprint duplicateComponent = fingerprint;
			duplicateComponent.Entities[0].Components[1].Component = "Transform";
			refused(duplicateComponent);
			ReplayFingerprint badHash = fingerprint;
			badHash.Assets[0].Hash = "D1";
			refused(badHash);
		}
	}

}
