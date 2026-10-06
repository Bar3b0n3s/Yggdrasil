#include "TestsPCH.h"

#include "EditorCore/Automation/TranscriptLog.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static Scope<VirtualFileSystem> MakeTranscriptVfs()
	{
		Scope<VirtualFileSystem> vfs = CreateScope<VirtualFileSystem>();
		REQUIRE(vfs->Mount("project", CreateScope<MemoryMount>()).has_value());
		return vfs;
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("TranscriptLog: request lines are numbered from 1 and responses reference them" * doctest::skip(true))
		{
			Scope<VirtualFileSystem> vfs = MakeTranscriptVfs();
			const Result<uint64_t> first = TranscriptLog::AppendRequest(*vfs, "cli", Json(), "project.upgrade", Json::object());
			REQUIRE(first.has_value());
			CHECK(*first == 1);
			REQUIRE(TranscriptLog::AppendResponse(*vfs, "cli", Json(), *first, "2 files changed", Json()).has_value());
			const Result<uint64_t> third = TranscriptLog::AppendRequest(*vfs, "cli", Json(7), "project.upgrade", Json::object());
			REQUIRE(third.has_value());
			CHECK(*third == 3);

			const Result<VfsPath> file = VfsPath::Parse("project://Automation/BuildLog.jsonl");
			REQUIRE(file.has_value());
			const Result<std::string> text = vfs->ReadText(*file);
			REQUIRE(text.has_value());
			CHECK(std::count(text->begin(), text->end(), '\n') == 3);
			const size_t firstEnd = text->find('\n');
			REQUIRE(firstEnd != std::string::npos);
			const size_t secondEnd = text->find('\n', firstEnd + 1);
			REQUIRE(secondEnd != std::string::npos);
			const Result<Json> response = JsonReader::Parse(text->substr(firstEnd + 1, secondEnd - firstEnd - 1));
			REQUIRE(response.has_value());
			const Json& line = *response;
			CHECK(line["type"] == Json("response"));
			CHECK(line["requestLine"] == Json(1));
			CHECK(line["ok"] == Json(true));
			CHECK(line["client"] == Json("cli"));
		}

		TEST_CASE("TranscriptLog: a file that does not end with a newline is rejected" * doctest::skip(true))
		{
			Scope<VirtualFileSystem> vfs = MakeTranscriptVfs();
			const Result<VfsPath> file = VfsPath::Parse("project://Automation/BuildLog.jsonl");
			REQUIRE(file.has_value());
			const Result<VfsPath> directory = VfsPath::Parse("project://Automation");
			REQUIRE(directory.has_value());
			REQUIRE(vfs->CreateDirectories(*directory).has_value());
			const std::string torn = R"({"type": "request")";
			REQUIRE(vfs->WriteFileAtomic(*file, std::as_bytes(std::span(torn.data(), torn.size()))).has_value());
			const Result<uint64_t> appended = TranscriptLog::AppendRequest(*vfs, "cli", Json(), "project.upgrade", Json::object());
			REQUIRE_FALSE(appended.has_value());
			CHECK(appended.error().GetCode() == ErrorCode::Validation);
		}
	}

}
