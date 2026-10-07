#include "TestsPCH.h"

#include "Engine/Automation/Protocol/ResultOffload.h"

#include "Engine/Core/Json/JsonWriter.h"

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

		TEST_CASE("ResultOffload: the replacement names the path and is marked truncated")
		{
			Json replacement = MakeOffloadedResult("C:/Projects/Tetris/Library/Automation/Out/00000001.json", Json{ { "a", 1 } });
			CHECK(replacement["path"] == Json("C:/Projects/Tetris/Library/Automation/Out/00000001.json"));
			CHECK(replacement["truncated"] == Json(true));
			CHECK(replacement["summary"]["a"] == Json(1));
		}
	}

}
