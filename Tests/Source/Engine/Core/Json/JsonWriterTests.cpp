#include "TestsPCH.h"

#include "Engine/Core/Json/JsonWriter.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/DeathTest.h"

#include <nlohmann/json.hpp>

#include <limits>
#include <map>

namespace Engine {

	// An array nested `depth` levels deep: [[[...]]].
	static Json MakeNestedArray(size_t depth)
	{
		Json value = Json::array();
		for (size_t level = 1; level < depth; ++level)
		{
			Json outer = Json::array();
			outer.push_back(std::move(value));
			value = std::move(outer);
		}
		return value;
	}

	ENGINE_DEATH_TEST("Core/JsonWriterTooDeep")
	{
		const Result<std::string> text = JsonWriter::Write(MakeNestedArray(MaxJsonDepth + 1));
		static_cast<void>(text);
	}

	// A document in canonical form: written by JsonWriter, it must come back byte for byte. It covers every rule of the
	// pretty layout: nested objects, inline scalar arrays, expanded arrays (of objects, of arrays, and mixed), empty
	// containers, integers over the full 64-bit range, shortest floats, escapes and raw UTF-8.
	static constexpr std::string_view CanonicalDocument =
		"{\n"
		"\t\"Format\": \"Canonical\",\n"
		"\t\"Version\": 1,\n"
		"\t\"Name\": \"Caf\xc3\xa9 \\\"quoted\\\" line\\nbreak\\ttab \\\\ slash /\",\n"
		"\t\"Count\": -42,\n"
		"\t\"Big\": 18446744073709551615,\n"
		"\t\"Smallest\": -9223372036854775808,\n"
		"\t\"Ratio\": 0.1,\n"
		"\t\"Tiny\": 1e-07,\n"
		"\t\"Huge\": 3.4028235e+38,\n"
		"\t\"Enabled\": true,\n"
		"\t\"Parent\": null,\n"
		"\t\"Translation\": [0, 2.5, -1],\n"
		"\t\"Tags\": [\"Player\", \"Ball\"],\n"
		"\t\"Empty\": {},\n"
		"\t\"None\": [],\n"
		"\t\"Entities\": [\n"
		"\t\t{\n"
		"\t\t\t\"ID\": \"5d1c9a7e33b04f12\",\n"
		"\t\t\t\"Components\": {\n"
		"\t\t\t\t\"Transform\": {\n"
		"\t\t\t\t\t\"Scale\": [1, 1, 1]\n"
		"\t\t\t\t}\n"
		"\t\t\t}\n"
		"\t\t},\n"
		"\t\t[]\n"
		"\t],\n"
		"\t\"Mixed\": [\n"
		"\t\t1,\n"
		"\t\t{\n"
		"\t\t\t\"A\": 1\n"
		"\t\t}\n"
		"\t],\n"
		"\t\"Control\": \"\\u0001\"\n"
		"}\n";

	// The minified text of a document whose only value is `value`.
	static std::string WriteFloatAlone(float value)
	{
		JsonWriter writer(JsonStyle::Minified);
		writer.WriteFloat(value);
		Result<std::string> text = writer.Finish();
		REQUIRE(text.has_value());
		return std::move(*text);
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("JsonWriter: floats use the shortest round-trip form" * doctest::skip(true))
		{
			CHECK(WriteFloatAlone(0.1f) == "0.1");
			CHECK(WriteFloatAlone(0.3f) == "0.3");
			CHECK(WriteFloatAlone(-9.81f) == "-9.81");
			CHECK(WriteFloatAlone(123.456f) == "123.456");
			CHECK(WriteFloatAlone(1.0f) == "1");
			CHECK(WriteFloatAlone(100.0f) == "100");
			CHECK(WriteFloatAlone(16777216.0f) == "16777216");
			CHECK(WriteFloatAlone(0.0f) == "0");
			CHECK(WriteFloatAlone(-0.0f) == "0");
			CHECK(WriteFloatAlone(1e8f) == "1e+08");
			CHECK(WriteFloatAlone(1e10f) == "1e+10");
			CHECK(WriteFloatAlone(0.001f) == "0.001"); // a tie in length resolves to fixed notation
			CHECK(WriteFloatAlone(0.0001f) == "1e-04");
			CHECK(WriteFloatAlone(1e-7f) == "1e-07");
			CHECK(WriteFloatAlone(std::numeric_limits<float>::max()) == "3.4028235e+38");
			CHECK(WriteFloatAlone(std::numeric_limits<float>::denorm_min()) == "1e-45");
		}

		TEST_CASE("JsonWriter: load then save is byte-identical" * doctest::skip(true))
		{
			Result<Json> document = JsonReader::Parse(CanonicalDocument);
			REQUIRE(document.has_value());

			const Result<std::string> written = JsonWriter::Write(*document);
			REQUIRE(written.has_value());
			CHECK(*written == CanonicalDocument);

			Result<Json> reloaded = JsonReader::Parse(*written);
			REQUIRE(reloaded.has_value());
			const Result<std::string> rewritten = JsonWriter::Write(*reloaded);
			REQUIRE(rewritten.has_value());
			CHECK(*rewritten == *written);
		}

		TEST_CASE("JsonWriter: the pretty layout follows the canonical rules" * doctest::skip(true))
		{
			JsonWriter writer;
			writer.BeginObject();
			writer.WriteKey("Format");
			writer.WriteString("Scene");
			writer.WriteKey("Version");
			writer.WriteInt(1);
			writer.WriteKey("Seed");
			writer.WriteUInt(42);
			writer.WriteKey("Gravity");
			writer.BeginArray();
			writer.WriteFloat(0.0f);
			writer.WriteFloat(-9.81f);
			writer.WriteFloat(0.0f);
			writer.EndArray();
			writer.WriteKey("ID");
			writer.WriteUUID(UUID(0x5d1c9a7e33b04f12));
			writer.WriteKey("Entities");
			writer.BeginArray();
			writer.BeginObject();
			writer.WriteKey("Name");
			writer.WriteString("Ball");
			writer.WriteKey("Parent");
			writer.WriteNull();
			writer.WriteKey("Active");
			writer.WriteBool(true);
			writer.EndObject();
			writer.EndArray();
			writer.EndObject();

			const Result<std::string> text = writer.Finish();
			REQUIRE(text.has_value());
			CHECK(*text
				== "{\n"
				   "\t\"Format\": \"Scene\",\n"
				   "\t\"Version\": 1,\n"
				   "\t\"Seed\": 42,\n"
				   "\t\"Gravity\": [0, -9.81, 0],\n"
				   "\t\"ID\": \"5d1c9a7e33b04f12\",\n"
				   "\t\"Entities\": [\n"
				   "\t\t{\n"
				   "\t\t\t\"Name\": \"Ball\",\n"
				   "\t\t\t\"Parent\": null,\n"
				   "\t\t\t\"Active\": true\n"
				   "\t\t}\n"
				   "\t]\n"
				   "}\n");
		}

		TEST_CASE("JsonWriter: the minified layout has no whitespace and no final newline" * doctest::skip(true))
		{
			Result<Json> document = JsonReader::Parse(CanonicalDocument);
			REQUIRE(document.has_value());
			const Result<std::string> minified = JsonWriter::Write(*document, JsonStyle::Minified);
			REQUIRE(minified.has_value());
			CHECK(minified->starts_with("{\"Format\":\"Canonical\",\"Version\":1,"));
			CHECK(minified->contains("\"Translation\":[0,2.5,-1]"));
			CHECK(minified->ends_with("\"Control\":\"\\u0001\"}"));
			CHECK_FALSE(minified->contains('\n'));
			CHECK_FALSE(minified->contains('\t'));
		}

		TEST_CASE("JsonWriter: strings escape quotes, backslashes and control characters" * doctest::skip(true))
		{
			JsonWriter writer(JsonStyle::Minified);
			writer.WriteString("\"\\\b\f\n\r\t\x01\x1f/\x7f\xc3\xa9");
			const Result<std::string> text = writer.Finish();
			REQUIRE(text.has_value());
			CHECK(*text == "\"\\\"\\\\\\b\\f\\n\\r\\t\\u0001\\u001f/\x7f\xc3\xa9\"");
		}

		TEST_CASE("JsonWriter: Map fields are written in byte-wise key order" * doctest::skip(true))
		{
			const std::map<std::string, int> actions = { { "b", 2 }, { "a", 1 }, { "B", 3 } };
			JsonWriter writer(JsonStyle::Minified);
			writer.WriteMap(actions, [](JsonWriter& target, int value)
			{
				target.WriteInt(value);
			});
			const Result<std::string> text = writer.Finish();
			REQUIRE(text.has_value());
			CHECK(*text == R"({"B":3,"a":1,"b":2})");
		}

		TEST_CASE("JsonWriter: integers keep full 64-bit precision" * doctest::skip(true))
		{
			JsonWriter writer(JsonStyle::Minified);
			writer.BeginArray();
			writer.WriteInt(std::numeric_limits<int64_t>::min());
			writer.WriteInt(std::numeric_limits<int64_t>::max());
			writer.WriteUInt(std::numeric_limits<uint64_t>::max());
			writer.EndArray();
			const Result<std::string> text = writer.Finish();
			REQUIRE(text.has_value());
			CHECK(*text == "[-9223372036854775808,9223372036854775807,18446744073709551615]");
		}

		TEST_CASE("JsonWriter: non-finite floats, invalid UTF-8 and duplicate keys fail Finish with a pointer" * doctest::skip(true))
		{
			SUBCASE("NaN")
			{
				JsonWriter writer;
				writer.BeginObject();
				writer.WriteKey("Values");
				writer.BeginArray();
				writer.WriteFloat(1.0f);
				writer.WriteFloat(std::numeric_limits<float>::quiet_NaN());
				writer.EndArray();
				writer.EndObject();
				const Result<std::string> text = writer.Finish();
				REQUIRE_FALSE(text.has_value());
				CHECK(text.error().GetCode() == ErrorCode::Validation);
				CHECK(text.error().GetLocation().JsonPointer == "/Values/1");
			}

			SUBCASE("infinity")
			{
				JsonWriter writer(JsonStyle::Minified);
				writer.WriteFloat(-std::numeric_limits<float>::infinity());
				CHECK_FALSE(writer.Finish().has_value());
			}

			SUBCASE("invalid UTF-8")
			{
				JsonWriter writer;
				writer.BeginObject();
				writer.WriteKey("Name");
				writer.WriteString("\xc3\x28");
				writer.EndObject();
				const Result<std::string> text = writer.Finish();
				REQUIRE_FALSE(text.has_value());
				CHECK(text.error().GetLocation().JsonPointer == "/Name");
			}

			SUBCASE("duplicate key")
			{
				JsonWriter writer;
				writer.BeginObject();
				writer.WriteKey("A");
				writer.WriteInt(1);
				writer.WriteKey("A");
				writer.WriteInt(2);
				writer.EndObject();
				const Result<std::string> text = writer.Finish();
				REQUIRE_FALSE(text.has_value());
				CHECK(text.error().GetLocation().JsonPointer == "/A");
			}
		}

		TEST_CASE("JsonWriter: WriteJson writes float numbers as float and integers exactly" * doctest::skip(true))
		{
			Result<Json> document = JsonReader::Parse(
				R"({"Double": 0.1000000000000000055511151231257827, "Integer": 9007199254740993})");
			REQUIRE(document.has_value());
			const Result<std::string> text = JsonWriter::Write(*document, JsonStyle::Minified);
			REQUIRE(text.has_value());
			CHECK(*text == R"({"Double":0.1,"Integer":9007199254740993})");
		}

		TEST_CASE("JsonWriter: WriteJson writes trees up to MaxJsonDepth and asserts beyond" * doctest::skip(true))
		{
			const Result<std::string> deepest = JsonWriter::Write(MakeNestedArray(MaxJsonDepth), JsonStyle::Minified);
			REQUIRE(deepest.has_value());
			CHECK(*deepest == std::string(MaxJsonDepth, '[') + std::string(MaxJsonDepth, ']'));

			Test::CheckDeath("Core/JsonWriterTooDeep", "Assertion failed");
		}
	}

}
