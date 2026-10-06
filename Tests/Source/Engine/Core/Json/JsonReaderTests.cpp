#include "TestsPCH.h"

#include "Engine/Core/Json/JsonReader.h"

#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/Utf8.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static Json ParseOrFail(std::string_view text)
	{
		Result<Json> parsed = JsonReader::Parse(text);
		REQUIRE_MESSAGE(parsed.has_value(), "fixture JSON failed to parse");
		return std::move(*parsed);
	}

	static constexpr std::string_view SceneFixture = R"({
	"Format": "Scene",
	"Version": 1,
	"Name": "Level1",
	"Entities": [
		{
			"ID": "5d1c9a7e33b04f12",
			"Components": {
				"RigidBody": { "Mass": "heavy", "Friction": 0.8 }
			}
		}
	]
})";

	// The seed document of the mutation test: a scene-like file with every kind of value, string escapes (a \u surrogate
	// pair and an escaped NUL among them), the extreme 64-bit integers, exponents and nested arrays and objects.
	static constexpr std::string_view MutationFixture = R"({
	"Format": "Scene",
	"Version": 1,
	"Name": "Level \"One\" \\ \/ \b\f\n\r\t \u00e9 \ud83d\ude00 \u0000 caf\u00e9",
	"Seed": 18446744073709551615,
	"Offset": -9223372036854775808,
	"Gravity": [0, -9.81, 0.5e-3, 1E+2, -0.0],
	"Flags": [true, false, null],
	"Entities": [
		{
			"ID": "5d1c9a7e33b04f12",
			"Children": [[1, [2, [3, []]]], {}],
			"Components": {
				"Transform": { "Position": [1.5, 2, -3], "Scale": 1e38 },
				"Tag": { "Value": "Ball" }
			}
		},
		{ "ID": "00000000000003ff", "Tags": ["a", "b\u0001c", ""] }
	]
})";

	// One random edit of `text`: bit flips, a truncation, inserted or deleted bytes, or injected structural characters.
	static void MutateJson(std::string& text, Random& random)
	{
		constexpr std::string_view Structural = "[]{}\"\\,:";
		if (text.empty())
			return;
		const auto randomOffset = [&random](size_t size)
		{
			return static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(size) - 1));
		};
		switch (random.RangeInt(0, 4))
		{
			case 0: // flip random bits
			{
				const int64_t flips = random.RangeInt(1, 4);
				for (int64_t flip = 0; flip < flips; ++flip)
					text[randomOffset(text.size())] ^= static_cast<char>(1u << random.RangeInt(0, 7));
				break;
			}
			case 1: // truncate
				text.resize(randomOffset(text.size()));
				break;
			case 2: // insert random bytes
			{
				const size_t offset = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(text.size())));
				const int64_t count = random.RangeInt(1, 8);
				for (int64_t index = 0; index < count; ++index)
					text.insert(text.begin() + static_cast<std::ptrdiff_t>(offset), static_cast<char>(random.NextU32() & 0xffu));
				break;
			}
			case 3: // delete a run of bytes
			{
				const size_t offset = randomOffset(text.size());
				const auto count = static_cast<size_t>(random.RangeInt(1, 16));
				text.erase(offset, std::min(count, text.size() - offset));
				break;
			}
			case 4: // inject structural characters
			{
				const int64_t count = random.RangeInt(1, 3);
				for (int64_t index = 0; index < count; ++index)
				{
					const size_t offset = static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(text.size())));
					const char character = Structural[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(Structural.size()) - 1))];
					text.insert(text.begin() + static_cast<std::ptrdiff_t>(offset), character);
				}
				break;
			}
		}
	}

	// A failed read of a parsed tree is a Validation error (located data problem), never anything else.
	template<typename T>
	static void RequireValueOrValidation(const Result<T>& result)
	{
		if (!result.has_value())
			REQUIRE(result.error().GetCode() == ErrorCode::Validation);
	}

	// Reads `reader` with the typed reads that fit its type, and every member and element below it. Returns the number of
	// values visited.
	static size_t WalkParsedTree(const JsonReader& reader)
	{
		size_t visited = 1;
		switch (reader.GetType())
		{
			case JsonType::Null:
			{
				REQUIRE(reader.IsNull());
				const Result<bool> notBool = reader.ReadBool();
				REQUIRE_FALSE(notBool.has_value());
				REQUIRE(notBool.error().GetCode() == ErrorCode::Validation);
				break;
			}
			case JsonType::Bool:
				REQUIRE(reader.ReadBool().has_value());
				break;
			case JsonType::Integer:
				REQUIRE((reader.ReadInt64().has_value() || reader.ReadUInt64().has_value()));
				RequireValueOrValidation(reader.ReadInt32());
				RequireValueOrValidation(reader.ReadUInt32());
				RequireValueOrValidation(reader.ReadFloat());
				RequireValueOrValidation(reader.ReadDouble());
				break;
			case JsonType::Float:
				RequireValueOrValidation(reader.ReadFloat());
				RequireValueOrValidation(reader.ReadDouble());
				RequireValueOrValidation(reader.ReadInt64());
				break;
			case JsonType::String:
				REQUIRE(reader.ReadString().has_value());
				RequireValueOrValidation(reader.ReadUUID());
				break;
			case JsonType::Array:
			{
				const Result<size_t> size = reader.GetArraySize();
				REQUIRE(size.has_value());
				for (size_t index = 0; index < *size; ++index)
				{
					const Result<JsonReader> element = reader.GetElement(index);
					REQUIRE(element.has_value());
					visited += WalkParsedTree(*element);
				}
				RequireValueOrValidation(reader.GetElement(*size));
				break;
			}
			case JsonType::Object:
			{
				const Result<std::vector<std::string>> names = reader.GetMemberNames();
				REQUIRE(names.has_value());
				for (const std::string& name : *names)
				{
					const Result<JsonReader> member = reader.GetMember(name);
					REQUIRE(member.has_value());
					visited += WalkParsedTree(*member);
				}
				const Result<uint32_t> header = reader.ReadFormatHeader("Scene", 0, 1);
				if (!header.has_value())
				{
					const ErrorCode code = header.error().GetCode();
					REQUIRE((code == ErrorCode::Validation || code == ErrorCode::UnsupportedVersion));
				}
				break;
			}
		}
		return visited;
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("JsonReader: 10,000 seeded mutations never crash and always return Result")
		{
			// Architecture §15.2: JsonReader is the boundary for hostile JSON from files and automation requests.
			const std::string bom = "\xef\xbb\xbf";
			const std::array<std::string, 2> seeds = { std::string(MutationFixture), bom + std::string(MutationFixture) };
			for (const std::string& seed : seeds)
				REQUIRE(JsonReader::Parse(seed).has_value());

			Random random(0x15a7);
			int parseFailures = 0;
			int parsed = 0;
			for (int iteration = 0; iteration < 10000; ++iteration)
			{
				std::string mutated = seeds[static_cast<size_t>(iteration % 2)];
				const int64_t edits = random.RangeInt(1, 3);
				for (int64_t edit = 0; edit < edits; ++edit)
					MutateJson(mutated, random);

				// The oracle: Parse returns (a crash or an assert would end the run) a tree or a Parse error. A tree can be
				// read without anything but Validation errors, and the canonical writer either rejects it with Validation
				// (a number outside the float range) or writes text that parses again and writes back byte-identically.
				const Result<Json> document = JsonReader::Parse(mutated);
				if (!document.has_value())
				{
					++parseFailures;
					REQUIRE(document.error().GetCode() == ErrorCode::Parse);
					continue;
				}

				++parsed;
				REQUIRE(WalkParsedTree(JsonReader(*document)) > 0);
				const Result<std::string> written = JsonWriter::Write(*document);
				if (!written.has_value())
				{
					REQUIRE(written.error().GetCode() == ErrorCode::Validation);
					continue;
				}
				const Result<Json> reparsed = JsonReader::Parse(*written);
				REQUIRE(reparsed.has_value());
				const Result<std::string> rewritten = JsonWriter::Write(*reparsed);
				REQUIRE(rewritten.has_value());
				REQUIRE(*rewritten == *written);
			}
			CHECK(parseFailures > 0);
			CHECK(parsed > 0);
		}

		TEST_CASE("JsonReader: errors carry JSON pointers")
		{
			const Json document = ParseOrFail(SceneFixture);
			const JsonReader root(document);

			Result<JsonReader> entities = root.GetMember("Entities");
			REQUIRE(entities.has_value());
			Result<JsonReader> entity = entities->GetElement(0);
			REQUIRE(entity.has_value());
			CHECK(entity->GetPointer() == "/Entities/0");
			Result<JsonReader> components = entity->GetMember("Components");
			REQUIRE(components.has_value());
			Result<JsonReader> rigidBody = components->GetMember("RigidBody");
			REQUIRE(rigidBody.has_value());

			const Result<float> mass = rigidBody->ReadMember<float>("Mass");
			REQUIRE_FALSE(mass.has_value());
			CHECK(mass.error().GetCode() == ErrorCode::Validation);
			CHECK(mass.error().GetMessageText() == "expected number, got string");
			CHECK(mass.error().GetLocation().JsonPointer == "/Entities/0/Components/RigidBody/Mass");

			const Result<float> friction = rigidBody->ReadMember<float>("Friction");
			REQUIRE(friction.has_value());
			CHECK(*friction == 0.8f);

			const Result<JsonReader> missing = rigidBody->GetMember("Layer");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetMessageText() == "missing required field 'Layer'");
			CHECK(missing.error().GetLocation().JsonPointer == "/Entities/0/Components/RigidBody");

			const Result<JsonReader> outOfRange = entities->GetElement(1);
			REQUIRE_FALSE(outOfRange.has_value());
			CHECK(outOfRange.error().GetLocation().JsonPointer == "/Entities");
		}

		TEST_CASE("JsonReader: errors about the document root carry the empty pointer")
		{
			const Json document = ParseOrFail(R"({"Version": 1})");
			const JsonReader root(document);

			const Result<JsonReader> missing = root.GetMember("Format");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetMessageText() == "missing required field 'Format'");
			REQUIRE(missing.error().GetLocation().JsonPointer.has_value());
			CHECK(*missing.error().GetLocation().JsonPointer == "");

			const Json array = ParseOrFail("[1, 2]");
			const Status notAnObject = JsonReader(array).ExpectType(JsonType::Object);
			REQUIRE_FALSE(notAnObject.has_value());
			CHECK(notAnObject.error().GetLocation().JsonPointer == std::optional<std::string>(""));
		}

		TEST_CASE("JsonReader: Parse reports the line and column of a syntax error")
		{
			const Result<Json> parsed = JsonReader::Parse("{\n\t\"A\": 1,\n\t\"B\": tru\n}");
			REQUIRE_FALSE(parsed.has_value());
			CHECK(parsed.error().GetCode() == ErrorCode::Parse);
			CHECK(parsed.error().GetLocation().Line == 3);
			CHECK(parsed.error().GetLocation().Column >= 7);
		}

		TEST_CASE("JsonReader: Parse rejects duplicate keys, comments, trailing commas and NaN")
		{
			const Result<Json> duplicate = JsonReader::Parse(R"({"A": 1, "B": {"C": 1, "C": 2}})");
			REQUIRE_FALSE(duplicate.has_value());
			CHECK(duplicate.error().GetCode() == ErrorCode::Parse);
			CHECK(duplicate.error().GetLocation().JsonPointer == "/B/C");

			CHECK_FALSE(JsonReader::Parse("{\"A\": 1 // comment\n}").has_value());
			CHECK_FALSE(JsonReader::Parse(R"({"A": [1, 2,]})").has_value());
			CHECK_FALSE(JsonReader::Parse(R"({"A": NaN})").has_value());
			CHECK_FALSE(JsonReader::Parse(R"({"A": Infinity})").has_value());
			CHECK_FALSE(JsonReader::Parse(R"({"A": 1} {"B": 2})").has_value());
			CHECK_FALSE(JsonReader::Parse("").has_value());
			CHECK(JsonReader::Parse(" \n{\"A\": 1}\n ").has_value());
		}

		TEST_CASE("JsonReader: Parse rejects nesting deeper than MaxJsonDepth without recursing")
		{
			const std::string deepest = std::string(MaxJsonDepth, '[') + std::string(MaxJsonDepth, ']');
			CHECK(JsonReader::Parse(deepest).has_value());

			const std::string tooDeep = std::string(MaxJsonDepth + 1, '[') + std::string(MaxJsonDepth + 1, ']');
			const Result<Json> rejected = JsonReader::Parse(tooDeep);
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::Parse);
			CHECK(rejected.error().GetLocation().Line == 1);
			CHECK(rejected.error().GetLocation().Column == MaxJsonDepth + 1);

			std::string objects;
			for (size_t level = 0; level <= MaxJsonDepth; ++level)
				objects += "{\"A\": ";
			objects += "1" + std::string(MaxJsonDepth + 1, '}');
			CHECK_FALSE(JsonReader::Parse(objects).has_value());

			// A megabyte of '[' (a corrupted file or an agent's mistake) is an error, never a stack overflow.
			const Result<Json> huge = JsonReader::Parse(std::string(1000000, '['));
			REQUIRE_FALSE(huge.has_value());
			CHECK(huge.error().GetCode() == ErrorCode::Parse);
		}

		TEST_CASE("JsonReader: Parse locates duplicate keys after unescaping them")
		{
			const Result<Json> duplicate = JsonReader::Parse("{\n\t\"Entities\": [\n\t\t{\"a\": 1,\n\t\t \"\\u0061\": 2}\n\t]\n}");
			REQUIRE_FALSE(duplicate.has_value());
			CHECK(duplicate.error().GetCode() == ErrorCode::Parse);
			CHECK(duplicate.error().GetMessageText() == "duplicate key 'a'");
			CHECK(duplicate.error().GetLocation().JsonPointer == "/Entities/0/a");
			CHECK(duplicate.error().GetLocation().Line == 4);
			CHECK(duplicate.error().GetLocation().Column == 4);

			const Result<Json> escapedPointer = JsonReader::Parse(R"({"a/b": {"~": 1, "~": 2}})");
			REQUIRE_FALSE(escapedPointer.has_value());
			CHECK(escapedPointer.error().GetLocation().JsonPointer == "/a~1b/~0");

			CHECK(JsonReader::Parse(R"([{"A": 1}, {"A": 2}])").has_value());
		}

		TEST_CASE("JsonReader: Parse rejects integer literals outside the 64-bit range")
		{
			const Result<Json> tooLarge = JsonReader::Parse(R"({"List": [1, 18446744073709551616]})");
			REQUIRE_FALSE(tooLarge.has_value());
			CHECK(tooLarge.error().GetCode() == ErrorCode::Parse);
			CHECK(tooLarge.error().GetLocation().JsonPointer == "/List/1");
			CHECK(tooLarge.error().GetLocation().Column == 14);

			CHECK_FALSE(JsonReader::Parse("-9223372036854775809").has_value());
			CHECK(JsonReader::Parse("-9223372036854775808").has_value());
			CHECK(JsonReader::Parse("18446744073709551615").has_value());
			const Result<Json> exponent = JsonReader::Parse("1e300");
			REQUIRE(exponent.has_value());
			CHECK(GetJsonType(*exponent) == JsonType::Float);
		}

		TEST_CASE("JsonReader: Parse locates an unexpected end and keeps messages valid UTF-8")
		{
			const Result<Json> truncated = JsonReader::Parse("{\"A\": [1, 2");
			REQUIRE_FALSE(truncated.has_value());
			CHECK(truncated.error().GetCode() == ErrorCode::Parse);
			CHECK(truncated.error().GetLocation().Line == 1);
			CHECK(truncated.error().GetLocation().Column == 12);
			CHECK(truncated.error().GetMessageText().contains("end"));

			const Result<Json> empty = JsonReader::Parse("");
			REQUIRE_FALSE(empty.has_value());
			CHECK(empty.error().GetLocation().Line == 1);
			CHECK(empty.error().GetLocation().Column == 1);

			const Result<Json> garbage = JsonReader::Parse("{\"A\": \xc3\xa9}");
			REQUIRE_FALSE(garbage.has_value());
			CHECK(IsValidUtf8(garbage.error().GetMessageText()));
			CHECK(garbage.error().GetLocation().Column == 7);

			const Result<Json> deepAfterError = JsonReader::Parse("[x" + std::string(MaxJsonDepth + 1, '['));
			REQUIRE_FALSE(deepAfterError.has_value());
			CHECK(deepAfterError.error().GetLocation().Column == 2);
		}

		TEST_CASE("JsonReader: Parse accepts a leading byte order mark")
		{
			const Result<Json> parsed = JsonReader::Parse("\xef\xbb\xbf{\"A\": 1}");
			REQUIRE(parsed.has_value());
			CHECK(JsonReader(*parsed).ReadMember<int32_t>("A") == 1);
		}

		TEST_CASE("JsonReader: Parse rejects invalid UTF-8")
		{
			const Result<Json> parsed = JsonReader::Parse("{\"Name\": \"\xff\xfe\"}");
			REQUIRE_FALSE(parsed.has_value());
			CHECK(parsed.error().GetCode() == ErrorCode::Parse);
		}

		TEST_CASE("JsonReader: integer reads reject fractions, exponents and out-of-range values")
		{
			const Json document = ParseOrFail(
				R"({"Small": 7, "Negative": -1, "Fraction": 1.5, "Exponent": 1e3, "Large": 4294967296, "Max": 18446744073709551615})");
			const JsonReader root(document);

			CHECK(root.ReadMember<int32_t>("Small") == 7);
			CHECK(root.ReadMember<uint32_t>("Small") == 7u);
			CHECK(root.ReadMember<int64_t>("Negative") == -1);
			CHECK_FALSE(root.ReadMember<uint32_t>("Negative").has_value());
			CHECK_FALSE(root.ReadMember<int32_t>("Fraction").has_value());
			CHECK_FALSE(root.ReadMember<int64_t>("Exponent").has_value());
			CHECK_FALSE(root.ReadMember<uint32_t>("Large").has_value());
			CHECK(root.ReadMember<int64_t>("Large") == 4294967296ll);
			CHECK(root.ReadMember<uint64_t>("Max") == 18446744073709551615ull);
			CHECK_FALSE(root.ReadMember<int64_t>("Max").has_value());

			const Result<int32_t> fraction = root.ReadMember<int32_t>("Fraction");
			REQUIRE_FALSE(fraction.has_value());
			CHECK(fraction.error().GetCode() == ErrorCode::Validation);
			CHECK(fraction.error().GetLocation().JsonPointer == "/Fraction");
		}

		TEST_CASE("JsonReader: float reads accept integers and reject values outside the float range")
		{
			const Json document = ParseOrFail(R"({"Integer": 3, "Value": 0.25, "TooLarge": 1e39, "Double": 1e39})");
			const JsonReader root(document);

			CHECK(root.ReadMember<float>("Integer") == 3.0f);
			CHECK(root.ReadMember<float>("Value") == 0.25f);
			CHECK_FALSE(root.ReadMember<float>("TooLarge").has_value());
			CHECK(root.ReadMember<double>("Double") == 1e39);
		}

		TEST_CASE("JsonReader: type errors name the expected and the actual type")
		{
			const Json document = ParseOrFail(R"({"Flag": "yes", "Items": {}, "Name": 5})");
			const JsonReader root(document);

			const Result<bool> flag = root.ReadMember<bool>("Flag");
			REQUIRE_FALSE(flag.has_value());
			CHECK(flag.error().GetMessageText() == "expected boolean, got string");

			const Result<size_t> items = root.GetMember("Items")->GetArraySize();
			REQUIRE_FALSE(items.has_value());
			CHECK(items.error().GetMessageText() == "expected array, got object");
			CHECK(items.error().GetLocation().JsonPointer == "/Items");

			const Result<std::string> name = root.ReadMember<std::string>("Name");
			REQUIRE_FALSE(name.has_value());
			CHECK(name.error().GetMessageText() == "expected string, got number");

			CHECK(root.ExpectType(JsonType::Object).has_value());
			CHECK_FALSE(root.ExpectType(JsonType::Array).has_value());
		}

		TEST_CASE("JsonReader: ReadUUID accepts sixteen hex digits only")
		{
			const Json document = ParseOrFail(
				R"({"Valid": "5D1C9A7E33B04F12", "Zero": "0000000000000000", "Short": "5d1c", "Number": 12})");
			const JsonReader root(document);

			CHECK(root.ReadMember<UUID>("Valid") == UUID(0x5d1c9a7e33b04f12));
			const Result<UUID> zero = root.ReadMember<UUID>("Zero");
			REQUIRE(zero.has_value());
			CHECK_FALSE(zero->IsValid());
			CHECK_FALSE(root.ReadMember<UUID>("Short").has_value());
			CHECK_FALSE(root.ReadMember<UUID>("Number").has_value());
		}

		TEST_CASE("JsonReader: members keep document order and unknown members are listed")
		{
			const Json document = ParseOrFail(R"({"Zeta": 1, "Alpha": 2, "Mass": 3, "Mystery": 4})");
			const JsonReader root(document);

			const Result<std::vector<std::string>> names = root.GetMemberNames();
			REQUIRE(names.has_value());
			CHECK(*names == std::vector<std::string>{ "Zeta", "Alpha", "Mass", "Mystery" });

			const std::array<std::string_view, 2> known = { "Alpha", "Mass" };
			const Result<std::vector<std::string>> unknown = root.FindUnknownMembers(known);
			REQUIRE(unknown.has_value());
			CHECK(*unknown == std::vector<std::string>{ "Zeta", "Mystery" });

			CHECK(root.HasMember("Mass"));
			CHECK_FALSE(root.HasMember("mass"));
			CHECK(root.FindMember("Alpha").has_value());
			CHECK_FALSE(root.FindMember("Beta").has_value());
		}

		TEST_CASE("JsonReader: ReadFormatHeader checks the format and the version range")
		{
			const Json current = ParseOrFail(R"({"Format": "Scene", "Version": 1})");
			CHECK(JsonReader(current).ReadFormatHeader("Scene", 1, 2) == 1u);

			const Result<uint32_t> wrongFormat = JsonReader(current).ReadFormatHeader("Prefab", 1, 1);
			REQUIRE_FALSE(wrongFormat.has_value());
			CHECK(wrongFormat.error().GetCode() == ErrorCode::Validation);
			CHECK(wrongFormat.error().GetLocation().JsonPointer == "/Format");

			const Json newer = ParseOrFail(R"({"Format": "Scene", "Version": 3})");
			const Result<uint32_t> tooNew = JsonReader(newer).ReadFormatHeader("Scene", 1, 2);
			REQUIRE_FALSE(tooNew.has_value());
			CHECK(tooNew.error().GetCode() == ErrorCode::UnsupportedVersion);
			CHECK(tooNew.error().GetMessageText().contains("version 3 is newer"));
			CHECK(tooNew.error().GetMessageText().contains("(version 2)"));

			SUBCASE("minimum version 1 rejects a version 0 file")
			{
				const Json zero = ParseOrFail(R"({"Format": "Scene", "Version": 0})");
				const Result<uint32_t> tooOld = JsonReader(zero).ReadFormatHeader("Scene", 1, 2);
				REQUIRE_FALSE(tooOld.has_value());
				CHECK(tooOld.error().GetCode() == ErrorCode::Validation);
				CHECK(tooOld.error().GetLocation().JsonPointer == "/Version");
			}

			SUBCASE("minimum version 0 reads a version 0 fixture for migration")
			{
				const Json zero = ParseOrFail(R"({"Format": "Scene", "Version": 0})");
				CHECK(JsonReader(zero).ReadFormatHeader("Scene", 0, 1) == 0u);
			}

			SUBCASE("a version that is not an unsigned integer is Validation")
			{
				using namespace std::string_view_literals;
				const std::array malformed = {
					R"({"Format": "Scene", "Version": -1})"sv,
					R"({"Format": "Scene", "Version": 1.5})"sv,
					R"({"Format": "Scene", "Version": "1"})"sv,
					R"({"Format": "Scene"})"sv,
				};
				for (const std::string_view text : malformed)
				{
					const Json document = ParseOrFail(text);
					const Result<uint32_t> version = JsonReader(document).ReadFormatHeader("Scene", 0, 2);
					INFO("document: ", std::string(text));
					REQUIRE_FALSE(version.has_value());
					CHECK(version.error().GetCode() == ErrorCode::Validation);
				}
			}
		}

		TEST_CASE("JsonReader: AppendPointer escapes tilde and slash")
		{
			CHECK(JsonReader::AppendPointer("", "Entities") == "/Entities");
			CHECK(JsonReader::AppendPointer("/Entities", 12) == "/Entities/12");
			CHECK(JsonReader::AppendPointer("/Map", "a/b~c") == "/Map/a~1b~0c");
			CHECK(JsonReader::AppendPointer("", "") == "/");
		}

		TEST_CASE("JsonReader: MakeLocatedError points at the reader's value")
		{
			const Json document = ParseOrFail(R"({"Simulation": {"FixedHz": 0}})");
			const Result<JsonReader> hz = JsonReader(document).GetMember("Simulation")->GetMember("FixedHz");
			REQUIRE(hz.has_value());
			const Error error = hz->MakeLocatedError(ErrorCode::Validation, "must be > 0");
			CHECK(error.GetLocation().JsonPointer == "/Simulation/FixedHz");
			CHECK(error.GetMessageText() == "must be > 0");
		}
	}

}
