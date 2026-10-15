#include "TestsPCH.h"

#include "EditorCore/Automation/TranscriptLog.h"

#include "Engine/Core/Buffer.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Support/ExpectLog.h"

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
		TEST_CASE("TranscriptLog: request lines are numbered from 1 and responses reference them")
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

		TEST_CASE("TranscriptLog: lines carry the method, params and the response's error")
		{
			Scope<VirtualFileSystem> vfs = MakeTranscriptVfs();
			Json params = Json::object();
			params["dryRun"] = true;
			const Result<uint64_t> request = TranscriptLog::AppendRequest(*vfs, "cli", Json("u-1"), "project.upgrade", params);
			REQUIRE(request.has_value());
			const Result<Json> error = JsonReader::Parse(R"({"code":-32003,"message":"Validation","data":{"errorCode":"Validation","detail":"bad scene","issues":[]}})");
			REQUIRE(error.has_value());
			REQUIRE(TranscriptLog::AppendResponse(*vfs, "cli", Json("u-1"), *request, "failed", *error).has_value());

			const Result<VfsPath> file = VfsPath::Parse("project://Automation/BuildLog.jsonl");
			REQUIRE(file.has_value());
			const Result<std::string> text = vfs->ReadText(*file);
			REQUIRE(text.has_value());
			const size_t firstEnd = text->find('\n');
			REQUIRE(firstEnd != std::string::npos);
			const Result<Json> requestLine = JsonReader::Parse(text->substr(0, firstEnd));
			REQUIRE(requestLine.has_value());
			CHECK((*requestLine)["type"] == Json("request"));
			CHECK((*requestLine)["id"] == Json("u-1"));
			CHECK((*requestLine)["method"] == Json("project.upgrade"));
			CHECK((*requestLine)["params"] == params);
			const std::string time = JsonReader((*requestLine)["time"]).ReadString().value_or(std::string());
			CHECK(time.size() == std::string_view("2026-10-06T17:48:24.512Z").size());
			CHECK(time.ends_with("Z"));
			CHECK(time[10] == 'T');

			const Result<Json> responseLine = JsonReader::Parse(text->substr(firstEnd + 1, text->size() - firstEnd - 2));
			REQUIRE(responseLine.has_value());
			CHECK((*responseLine)["ok"] == Json(false));
			CHECK((*responseLine)["summary"] == Json("failed"));
			CHECK((*responseLine)["error"]["code"] == Json(-32003));
			CHECK((*responseLine)["error"]["errorCode"] == Json("Validation"));
			CHECK((*responseLine)["error"]["detail"] == Json("bad scene"));
		}

		// Docs/Decisions/0026-m16-contract.md decision 13: a writer that died mid-line (a killed bridge) leaves a torn last line.
		// It is completed with "\n" and keeps its number, as the bridge completes it (transcript.py), so every line reference
		// keeps its meaning and nobody edits the append-only file by hand.
		TEST_CASE("TranscriptLog: a torn last line is completed and the next line numbered after it")
		{
			Scope<VirtualFileSystem> vfs = MakeTranscriptVfs();
			const Result<VfsPath> file = VfsPath::Parse("project://Automation/BuildLog.jsonl");
			REQUIRE(file.has_value());
			const Result<VfsPath> directory = VfsPath::Parse("project://Automation");
			REQUIRE(directory.has_value());
			REQUIRE(vfs->CreateDirectories(*directory).has_value());
			const std::string complete = R"({"type": "request", "id": 1})";
			const std::string torn = R"({"type": "request")";
			const std::string before = complete + "\n" + torn;
			REQUIRE(vfs->WriteFileAtomic(*file, std::as_bytes(std::span(before.data(), before.size()))).has_value());

			Result<uint64_t> appended = MakeError(ErrorCode::Unsupported, "not appended");
			{
				Test::ExpectLog completed(LogLevel::Warn, "torn line");
				appended = TranscriptLog::AppendRequest(*vfs, "cli", Json(1), "project.upgrade", Json::object());
			}
			REQUIRE_MESSAGE(appended.has_value(), appended.error().ToString());
			CHECK(*appended == 3);

			// The torn line is kept byte for byte, completed, and the new line follows it.
			const Result<std::string> text = vfs->ReadText(*file);
			REQUIRE(text.has_value());
			CHECK(text->starts_with(before + "\n"));
			CHECK(std::count(text->begin(), text->end(), '\n') == 3);
			const Result<Json> line = JsonReader::Parse(std::string_view(*text).substr(before.size() + 1, text->size() - before.size() - 2));
			REQUIRE_MESSAGE(line.has_value(), line.error().ToString());
			CHECK((*line)["method"] == Json("project.upgrade"));

			// A response to it references its line, and the file stays complete.
			REQUIRE(TranscriptLog::AppendResponse(*vfs, "cli", Json(1), *appended, "ok", Json()).has_value());
			const Result<std::string> after = vfs->ReadText(*file);
			REQUIRE(after.has_value());
			CHECK(std::count(after->begin(), after->end(), '\n') == 4);
			CHECK(after->ends_with("\n"));
		}

		// The bridge writes raw UTF-8 (ensure_ascii=False) and completes a torn line byte-wise, so a writer killed inside a
		// multi-byte character leaves bytes that are not valid UTF-8 in the append-only file for good. The editor reads and
		// counts them as bytes, as the bridge does, and keeps appending.
		TEST_CASE("TranscriptLog: a line torn inside a multi-byte UTF-8 character is completed and appending goes on")
		{
			Scope<VirtualFileSystem> vfs = MakeTranscriptVfs();
			const Result<VfsPath> file = VfsPath::Parse("project://Automation/BuildLog.jsonl");
			REQUIRE(file.has_value());
			const Result<VfsPath> directory = VfsPath::Parse("project://Automation");
			REQUIRE(directory.has_value());
			REQUIRE(vfs->CreateDirectories(*directory).has_value());
			// "\xE2\x80" is U+2014 (EM DASH, E2 80 94) cut after its second byte.
			const std::string complete = "{\"type\": \"request\", \"params\": {\"source\": \"-- \xE2\x80\x94 ok\"}}";
			const std::string torn = "{\"type\": \"request\", \"params\": {\"source\": \"-- \xE2\x80";
			const std::string before = complete + "\n" + torn;
			REQUIRE(vfs->WriteFileAtomic(*file, AsBytes(before)).has_value());

			Result<uint64_t> appended = MakeError(ErrorCode::Unsupported, "not appended");
			{
				Test::ExpectLog completed(LogLevel::Warn, "torn line");
				appended = TranscriptLog::AppendRequest(*vfs, "cli", Json(1), "project.refreshAssets", Json::object());
			}
			REQUIRE_MESSAGE(appended.has_value(), appended.error().ToString());
			CHECK(*appended == 3);
			REQUIRE(TranscriptLog::AppendResponse(*vfs, "cli", Json(1), *appended, "ok", Json()).has_value());

			// Every byte before the tear is kept, the torn line is completed, and the two new lines follow it.
			const Result<Buffer> bytes = vfs->ReadFile(*file);
			REQUIRE(bytes.has_value());
			const std::string_view text = AsStringView(*bytes);
			CHECK(text.starts_with(before + "\n"));
			CHECK(std::ranges::count(text, '\n') == 4);
			CHECK(text.ends_with("\n"));
			const std::string_view appendedLines = text.substr(before.size() + 1);
			const Result<Json> request = JsonReader::Parse(appendedLines.substr(0, appendedLines.find('\n')));
			REQUIRE_MESSAGE(request.has_value(), request.error().ToString());
			CHECK((*request)["method"] == Json("project.refreshAssets"));
		}
	}

}
