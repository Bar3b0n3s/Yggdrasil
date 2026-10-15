#include "TestsPCH.h"

#include "Engine/Automation/Protocol/ResultOffload.h"

#include "Engine/Core/Json/JsonWriter.h"

#include <optional>
#include <string_view>

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("ResultOffload: file names are the server tag and a zero-padded sequence number")
		{
			const std::string tag = MakeOffloadServerTag(4242, 1791244800);
			CHECK(tag == "4242-1791244800");
			CHECK(MakeOffloadFileName(tag, 1) == "4242-1791244800-00000001.json");
			CHECK(MakeOffloadFileName(tag, 42) == "4242-1791244800-00000042.json");
			CHECK(MakeOffloadFileName(tag, 123456789) == "4242-1791244800-123456789.json");
			CHECK(OffloadDirectory == "Library/Automation/Out");
			CHECK(DefaultOffloadThresholdBytes == 48 * 1024);
		}

		TEST_CASE("ResultOffload: output files take the same name form with their own extension")
		{
			const std::string tag = MakeOffloadServerTag(4242, 1791244800);
			CHECK(MakeOffloadFileName(tag, 7, "png") == "4242-1791244800-00000007.png");
			CHECK(MakeOffloadFileName(tag, 8, "xml") == "4242-1791244800-00000008.xml");
			CHECK(MakeOffloadFileName(tag, 9) == MakeOffloadFileName(tag, 9, OffloadExtension));
		}

		// Docs/Decisions/0026-m16-contract.md, friction S1-03: pruning recognizes every file a server leaves in an offload
		// directory (results, screenshots, test reports, and the backups and temporaries of older or interrupted writes) and
		// nothing else.
		TEST_CASE("ResultOffload: a server's files of every extension and their leftovers parse back to its process id")
		{
			for (const std::string_view name : { "4242-1791244800-00000001.json", "4242-1791244800-00000002.png", "4242-1791244800-00000003.xml",
					 "4242-1791244800-00000004.json.bak", "4242-1791244800-00000005.json.tmp-0123456789abcdef", "4242-1-123456789.json" })
			{
				CAPTURE(name);
				CHECK(ParseOffloadFileProcessId(name) == std::optional<uint32_t>(4242));
			}
			for (const std::string_view name : { "Notes.txt", "4242.json", "4242-1791244800.json", "4242-x-00000001.json", "4242-1791244800-00000001",
					 "4242-1791244800-00000001.JSON", "4242-1791244800-00000001.", "-1791244800-00000001.json", "99999999999-1-1.json", "4242-1-1-1.json" })
			{
				CAPTURE(name);
				CHECK_FALSE(ParseOffloadFileProcessId(name).has_value());
			}
		}

		TEST_CASE("ResultOffload: two servers never produce the same file name")
		{
			// Two editors at once (different processes), and a restarted editor that got its predecessor's process id.
			const std::string first = MakeOffloadFileName(MakeOffloadServerTag(4242, 1791244800), 1);
			CHECK(first != MakeOffloadFileName(MakeOffloadServerTag(4243, 1791244800), 1));
			CHECK(first != MakeOffloadFileName(MakeOffloadServerTag(4242, 1791244801), 1));
		}

		TEST_CASE("ResultOffload: the summary describes members without their content and stays small")
		{
			Json result = Json::object();
			result["scene"] = Json::object();
			result["scene"]["Entities"] = Json::array();
			for (int index = 0; index < 2000; ++index)
				result["scene"]["Entities"].push_back(Json{ { "ID", std::format("{:016x}", index) } });
			result["name"] = std::string(10000, 'n');
			result["count"] = 2000;

			Json summary = MakeOffloadSummary(result);
			CHECK(summary["scene"]["type"] == Json("object"));
			CHECK(summary["scene"]["count"] == Json(1));
			CHECK(summary["name"]["type"] == Json("string"));
			CHECK(summary["name"]["length"] == Json(10000));
			CHECK(summary["count"] == Json(2000));
			const Result<std::string> text = JsonWriter::Write(summary, JsonStyle::Minified);
			REQUIRE(text.has_value());
			CHECK(text->size() < 4096);

			Json many = Json::object();
			for (int index = 0; index < 500; ++index)
				many[std::format("member{}", index)] = index;
			Json bounded = MakeOffloadSummary(many);
			CHECK(bounded.contains("omittedMembers"));
			const Result<std::string> boundedText = JsonWriter::Write(bounded, JsonStyle::Minified);
			REQUIRE(boundedText.has_value());
			CHECK(boundedText->size() < 4096);
		}

		// Friction S1-02: an offloaded test.run result still names its report files, its outcome counts and its failing cases.
		TEST_CASE("ResultOffload: a short string member appears verbatim")
		{
			Json result = Json::object();
			result["passed"] = false;
			result["counts"] = Json{ { "total", 20 }, { "passed", 19 }, { "failed", 1 } };
			result["failedCases"] = Json::array({ "Breakout/launch sends the ball up at a constant speed" });
			result["cases"] = Json::array();
			for (int index = 0; index < 200; ++index)
				result["cases"].push_back(Json{ { "suite", "Breakout" }, { "case", std::format("case {}", index) } });
			result["jsonPath"] = "D:/Games/Breakout/Library/Automation/Out/4242-1791244800-00000002.json";
			result["log"] = std::string(MaxOffloadSummaryVerbatimBytes, 'x');

			const Json summary = MakeOffloadSummary(result);
			CHECK(summary["passed"] == Json(false));
			CHECK(summary["counts"] == result["counts"]);
			CHECK(summary["failedCases"] == result["failedCases"]);
			CHECK(summary["jsonPath"] == result["jsonPath"]);
			// Over the limit, strings and containers are described by their size.
			CHECK(summary["log"] == Json{ { "type", "string" }, { "length", MaxOffloadSummaryVerbatimBytes } });
			CHECK(summary["cases"] == Json{ { "type", "array" }, { "count", 200 } });
		}

		// M16 review R26: a test.run result is offloaded when many failing rows carry long messages, which is when failedCases
		// (at most 20 "<suite>/<case>" names) is too long to repeat verbatim, and the summary kept only its count. A longer
		// array of strings now keeps its leading strings within MaxOffloadSummaryListBytes, and the summary stays small.
		TEST_CASE("ResultOffload: an offloaded test.run summary keeps the leading failing case names when they are too long to repeat verbatim")
		{
			Json result = Json::object();
			result["mode"] = "Editor";
			result["passed"] = false;
			result["counts"] = Json{ { "total", 200 }, { "passed", 180 }, { "failed", 20 } };
			result["failedCases"] = Json::array();
			for (int index = 0; index < 20; ++index)
				result["failedCases"].push_back(std::format("Breakout/the ball bounces off brick row {} at a constant speed", index));
			result["cases"] = Json::array();
			for (int index = 0; index < 200; ++index)
				result["cases"].push_back(Json{ { "suite", "Breakout" }, { "case", std::format("case {}", index) }, { "message", std::string(400, 'm') } });
			result["jsonPath"] = "D:/Games/Breakout/Library/Automation/Out/4242-1791244800-00000002.json";
			result["junitPath"] = "D:/Games/Breakout/Library/Automation/Out/4242-1791244800-00000003.xml";
			REQUIRE(result["failedCases"].dump().size() > MaxOffloadSummaryVerbatimBytes);

			const Json summary = MakeOffloadSummary(result);
			CHECK_FALSE(summary.contains("omittedMembers"));
			CHECK(summary["counts"] == result["counts"]);
			REQUIRE(summary["failedCases"].contains("first"));
			CHECK(summary["failedCases"]["type"] == Json("array"));
			CHECK(summary["failedCases"]["count"] == Json(20));
			CHECK(summary["failedCases"]["first"] == result["failedCases"]);
			CHECK(summary["cases"] == Json{ { "type", "array" }, { "count", 200 } });
			CHECK(summary["junitPath"] == result["junitPath"]);
			const Result<std::string> text = JsonWriter::Write(summary, JsonStyle::Minified);
			REQUIRE(text.has_value());
			CHECK(text->size() < 4096);

			// Names of the longest kind: only the leading ones that fit are kept, in order.
			Json longest = Json::object();
			longest["failedCases"] = Json::array();
			for (int index = 0; index < 20; ++index)
				longest["failedCases"].push_back(std::format("{:0>256}/{:0>256}", index, index));
			const Json bounded = MakeOffloadSummary(longest)["failedCases"];
			CHECK(bounded["count"] == Json(20));
			REQUIRE(bounded.contains("first"));
			REQUIRE_FALSE(bounded["first"].empty());
			CHECK(bounded["first"].size() < 20);
			CHECK(bounded["first"][0] == longest["failedCases"][0]);
			CHECK(bounded["first"].dump().size() <= MaxOffloadSummaryListBytes);

			// A list whose leading strings no longer fit the summary leaves room for the members after it.
			Json crowded = Json::object();
			for (const std::string_view name : { "a", "b", "c", "d" })
				crowded[std::string(name)] = longest["failedCases"];
			crowded["z"] = 7;
			const Json squeezed = MakeOffloadSummary(crowded);
			for (const std::string_view name : { "a", "b", "c" })
				CHECK(squeezed[std::string(name)].contains("first"));
			CHECK(squeezed["d"] == Json{ { "type", "array" }, { "count", 20 } });
			CHECK(squeezed["z"] == Json(7));
			CHECK_FALSE(squeezed.contains("omittedMembers"));
		}

		TEST_CASE("ResultOffload: the replacement names the path and is marked truncated")
		{
			Json replacement = MakeOffloadedResult("C:/Projects/Tetris/Library/Automation/Out/00000001.json", Json{ { "a", 1 } });
			CHECK(replacement["path"] == Json("C:/Projects/Tetris/Library/Automation/Out/00000001.json"));
			CHECK(replacement["truncated"] == Json(true));
			CHECK(replacement["summary"]["a"] == Json(1));
		}
	}

}
