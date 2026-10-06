#include "TestsPCH.h"

#include "Engine/Core/Json/Json.h"

#include "Engine/Core/Json/JsonReader.h"

#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("Core")
	{
		TEST_CASE("Json: JsonTypeToString uses JSON type names")
		{
			CHECK(JsonTypeToString(JsonType::Null) == "null");
			CHECK(JsonTypeToString(JsonType::Bool) == "boolean");
			CHECK(JsonTypeToString(JsonType::Integer) == "number");
			CHECK(JsonTypeToString(JsonType::Float) == "number");
			CHECK(JsonTypeToString(JsonType::String) == "string");
			CHECK(JsonTypeToString(JsonType::Array) == "array");
			CHECK(JsonTypeToString(JsonType::Object) == "object");
		}

		TEST_CASE("Json: GetJsonType classifies every kind of value")
		{
			Result<Json> document = JsonReader::Parse(R"([null, true, 1, 1.5, "s", [], {}])");
			REQUIRE(document.has_value());
			const std::array<JsonType, 7> expected = {
				JsonType::Null,
				JsonType::Bool,
				JsonType::Integer,
				JsonType::Float,
				JsonType::String,
				JsonType::Array,
				JsonType::Object,
			};
			for (size_t index = 0; index < expected.size(); ++index)
				CHECK(GetJsonType((*document)[index]) == expected[index]);
		}

		TEST_CASE("Json: MaxJsonDepth is 512 nested containers")
		{
			CHECK(MaxJsonDepth == 512u);
		}
	}

}
