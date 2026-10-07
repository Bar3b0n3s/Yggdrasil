#include "TestsPCH.h"

#include "Engine/Automation/Protocol/JsonReference.h"

#include "Engine/Core/Json/JsonReader.h"

namespace Engine {

	static Json ParseReferenceJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("JsonReference: $ref values are replaced from earlier results")
		{
			const std::vector<Json> results = {
				ParseReferenceJson(R"({"asset":{"id":"a41f0c2290b1d3e4"}})"),
				ParseReferenceJson(R"({"entity":{"id":"5d1c9a7e33b04f12","name":"Game"},"entities":[{"id":"0000000000000001"}]})"),
			};
			Json params = ParseReferenceJson(R"({"parent":{"$ref":"1.entity.id"},
				"components":{"MeshRenderer":{"Materials":[{"$ref":"0.asset.id"}]}},
				"first":{"$ref":"1.entities.0.id"},"whole":{"$ref":"0"}})");
			REQUIRE(SubstituteReferences(params, results, 0).has_value());
			CHECK(params["parent"] == Json("5d1c9a7e33b04f12"));
			CHECK(params["components"]["MeshRenderer"]["Materials"][0] == Json("a41f0c2290b1d3e4"));
			CHECK(params["first"] == Json("0000000000000001"));
			CHECK(params["whole"] == results[0]);

			// Batch files number their lines from 1.
			Json line = ParseReferenceJson(R"({"entity":{"$ref":"2.entity.id"}})");
			REQUIRE(SubstituteReferences(line, results, 1).has_value());
			CHECK(line["entity"] == Json("5d1c9a7e33b04f12"));
		}

		TEST_CASE("JsonReference: forward, self and dangling references are located errors")
		{
			const std::vector<Json> results = { ParseReferenceJson(R"({"entity":{"id":"5d1c9a7e33b04f12"}})") };
			const std::array<std::string_view, 5> bad = {
				R"({"a":{"$ref":"1.entity.id"}})",
				R"({"a":{"$ref":"0.entity.name"}})",
				R"({"a":{"$ref":"x.entity"}})",
				R"({"a":{"$ref":"01.entity"}})",
				R"({"a":[0,{"$ref":"0.entities.0"}]})",
			};
			for (const std::string_view text : bad)
			{
				INFO(std::string(text));
				Json params = ParseReferenceJson(text);
				const Json before = params;
				const Status status = SubstituteReferences(params, results, 0);
				REQUIRE_FALSE(status.has_value());
				CHECK(status.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(status.error().GetLocation().JsonPointer.has_value());
				CHECK(params == before);
			}
		}

		TEST_CASE("JsonReference: objects with other members next to $ref are data")
		{
			const std::vector<Json> results = { Json::object() };
			Json params = ParseReferenceJson(R"({"a":{"$ref":"0","note":"literal"},"b":{"$ref":5}})");
			const Json before = params;
			REQUIRE(SubstituteReferences(params, results, 0).has_value());
			CHECK(params == before);
		}
	}

}
