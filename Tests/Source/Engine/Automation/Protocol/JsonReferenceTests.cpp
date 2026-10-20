#include "TestsPCH.h"

#include "Engine/Automation/Protocol/JsonReference.h"

#include "Engine/Core/Json/JsonReader.h"

#include <nlohmann/json.hpp>

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

		TEST_CASE("JsonReference: a missing member names the members the result has")
		{
			// prefab.create's result names its instance "instance", prefab.instantiate's "entity": a batch that confuses them
			// learns which members the result has instead.
			const std::vector<Json> results = { ParseReferenceJson(R"({"prefab":{"id":"a41f0c2290b1d3e4"},"undoIndex":0,"instance":{"id":""}})"),
				ParseReferenceJson(R"({"undoIndex":0,"prefab":{"id":"a41f0c2290b1d3e4"},"entity":{"id":"5d1c9a7e33b04f12"}})"), Json::object() };
			Json params = ParseReferenceJson(R"({"entity":{"$ref":"1.instance.id"}})");
			const Status status = SubstituteReferences(params, results, 0);
			REQUIRE_FALSE(status.has_value());
			const Error& error = status.error();
			CHECK(error.GetCode() == ErrorCode::InvalidArgument);
			CHECK(error.GetMessageText() == "$ref '1.instance.id': no member 'instance' in the result of op 1");
			CHECK(error.GetHint() == "the result of op 1 has 'entity', 'prefab', 'undoIndex'");
			REQUIRE(error.GetIssues().size() == 1);
			CHECK(error.GetIssues()[0].JsonPointer == "/entity");
			CHECK(error.GetIssues()[0].Hint == error.GetHint());
			CHECK(error.GetIssues()[0].Suggestions.empty());

			// A close name is suggested first, in the hint and as the issue's suggestion.
			Json misspelt = ParseReferenceJson(R"({"entity":{"$ref":"1.entityy.id"}})");
			const Status close = SubstituteReferences(misspelt, results, 0);
			REQUIRE_FALSE(close.has_value());
			CHECK(close.error().GetHint() == "did you mean 'entity'? the result of op 1 has 'entity', 'prefab', 'undoIndex'");
			REQUIRE(close.error().GetIssues().size() == 1);
			CHECK(close.error().GetIssues()[0].Suggestions == std::vector<std::string>{ "entity" });
			// M16 review R27: an object inside the result lists its own members under its own name, not as the result's.
			Json nested = ParseReferenceJson(R"({"asset":{"$ref":"0.prefab.ids"}})");
			const Status inner = SubstituteReferences(nested, results, 0);
			REQUIRE_FALSE(inner.has_value());
			CHECK(inner.error().GetMessageText() == "$ref '0.prefab.ids': no member 'ids' in '0.prefab' (in the result of op 0)");
			CHECK(inner.error().GetHint() == "did you mean 'id'? '0.prefab' (in the result of op 0) has 'id'");

			// Batch files number their lines from 1; an empty result says so.
			Json line = ParseReferenceJson(R"({"entity":{"$ref":"3.entity.id"}})");
			const Status empty = SubstituteReferences(line, results, 1);
			REQUIRE_FALSE(empty.has_value());
			CHECK(empty.error().GetHint() == "the result of line 3 is an empty object");

			// M16 review R27: the listed members are bounded (error responses are never offloaded): the first 32, sorted, then
			// how many more.
			Json wide = Json::object();
			for (int member = 0; member < 40; ++member)
				wide["inner"][std::format("m{:02}", member)] = member;
			const std::vector<Json> wideResults = { wide };
			Json missing = ParseReferenceJson(R"({"value":{"$ref":"0.inner.zzz"}})");
			const Status bounded = SubstituteReferences(missing, wideResults, 0);
			REQUIRE_FALSE(bounded.has_value());
			const std::string hint = bounded.error().GetHint();
			CHECK(hint.starts_with("'0.inner' (in the result of op 0) has 'm00', 'm01', "));
			CHECK(hint.contains("'m31'"));
			CHECK_FALSE(hint.contains("'m32'"));
			CHECK(hint.ends_with("'m31' and 8 more"));
		}

		TEST_CASE("JsonReference: objects with other members next to $ref are data")
		{
			const std::vector<Json> results = { Json::object() };
			Json params = ParseReferenceJson(R"({"a":{"$ref":"0","note":"literal"},"b":{"$ref":5}})");
			const Json before = params;
			REQUIRE(SubstituteReferences(params, results, 0).has_value());
			CHECK(params == before);
		}

		// Docs/Decisions/0030-m19-contract.md decision 8 (A-NREF): "<name>.<path>" stands for a value of the result of the
		// earlier op with that name; indices keep working beside names; a name no earlier op has (a later op's included) is a
		// located InvalidArgument that leaves the params unchanged and names the reference.
		TEST_CASE("JsonReference: a named reference stands for the result of the earlier op with that name")
		{
			const std::vector<Json> results = {
				ParseReferenceJson(R"({"asset":{"id":"a41f0c2290b1d3e4"}})"),
				ParseReferenceJson(R"({"entity":{"id":"5d1c9a7e33b04f12","name":"Board"}})"),
			};
			// Every op's name: op 2 (named "later") is the op whose params these are, op 3 runs after it.
			const std::vector<std::string> names = { "", "board", "later", "last" };
			Json params = ParseReferenceJson(R"({"parent":{"$ref":"board.entity.id"},"material":{"$ref":"0.asset.id"},"whole":{"$ref":"board"}})");
			const Status substituted = SubstituteReferences(params, results, 0, names);
			REQUIRE_MESSAGE(substituted.has_value(), substituted.error().ToString());
			CHECK(params["parent"] == Json("5d1c9a7e33b04f12"));
			CHECK(params["material"] == Json("a41f0c2290b1d3e4"));
			CHECK(params["whole"] == results[1]);

			// No op named so, this op itself, a later op, and a path the named result lacks.
			for (const std::string_view text : { R"({"a":{"$ref":"cell.entity.id"}})", R"({"a":{"$ref":"later.entity.id"}})", R"({"a":{"$ref":"last.entity.id"}})",
					 R"({"a":{"$ref":"board.entities.id"}})" })
			{
				CAPTURE(text);
				Json bad = ParseReferenceJson(text);
				const Json before = bad;
				const Status status = SubstituteReferences(bad, results, 0, names);
				REQUIRE_FALSE(status.has_value());
				CHECK(status.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(status.error().GetLocation().JsonPointer == std::optional<std::string>("/a"));
				CHECK(bad == before);
			}
			// The missing name is named, and the earlier names are offered.
			Json misspelt = ParseReferenceJson(R"({"a":{"$ref":"bord.entity.id"}})");
			const Status close = SubstituteReferences(misspelt, results, 0, names);
			REQUIRE_FALSE(close.has_value());
			CHECK(close.error().GetMessageText().find("'bord'") != std::string::npos);
			CHECK(close.error().GetHint().find("board") != std::string::npos);
		}

		// Docs/Decisions/0030-m19-contract.md decision 8: with names, the part before the first '.' is an index or a name, and
		// anything else is malformed; a path error of a reference by name names the op by that name; and an op whose earlier ops
		// have no names is told to name one or use its index.
		TEST_CASE("JsonReference: a named reference that is neither an index nor a name is malformed, and its path errors name the op")
		{
			const std::vector<Json> results = { ParseReferenceJson(R"({"entity":{"id":"5d1c9a7e33b04f12"}})") };
			const std::vector<std::string> names = { "board", "" };
			for (const std::string_view text : { R"({"a":{"$ref":"2b.entity"}})", R"({"a":{"$ref":"-1.entity"}})", R"({"a":{"$ref":"a-b.entity"}})",
					 R"({"a":{"$ref":".entity"}})" })
			{
				CAPTURE(text);
				Json bad = ParseReferenceJson(text);
				const Json before = bad;
				const Status status = SubstituteReferences(bad, results, 0, names);
				REQUIRE_FALSE(status.has_value());
				CHECK(status.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(status.error().GetLocation().JsonPointer == std::optional<std::string>("/a"));
				CHECK(bad == before);
			}

			Json missing = ParseReferenceJson(R"({"a":{"$ref":"board.entities.id"}})");
			const Status path = SubstituteReferences(missing, results, 0, names);
			REQUIRE_FALSE(path.has_value());
			CHECK(path.error().GetMessageText().contains("op 'board'"));
			CHECK(path.error().GetHint().contains("'entity'"));

			// Only unnamed earlier ops: the hint says how to reference them.
			const std::vector<std::string> unnamed = { "", "" };
			Json absent = ParseReferenceJson(R"({"a":{"$ref":"board.entity.id"}})");
			const Status none = SubstituteReferences(absent, results, 0, unnamed);
			REQUIRE_FALSE(none.has_value());
			CHECK(none.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(none.error().GetHint().contains("index"));
		}
	}

}
