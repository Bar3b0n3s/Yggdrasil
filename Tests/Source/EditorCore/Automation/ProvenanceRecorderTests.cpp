#include "TestsPCH.h"

#include "EditorCore/Automation/ProvenanceRecorder.h"

#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static WriteAttribution MakeAttribution(std::string method, Json requestId, std::string client, std::optional<uint64_t> line)
	{
		WriteAttribution attribution;
		attribution.Method = std::move(method);
		attribution.RequestId = VariantValue(std::move(requestId));
		attribution.Client = std::move(client);
		attribution.TranscriptLine = line;
		return attribution;
	}

	static Scope<VirtualFileSystem> MakeProvenanceVfs()
	{
		Scope<VirtualFileSystem> vfs = CreateScope<VirtualFileSystem>();
		REQUIRE(vfs->Mount("project", CreateScope<MemoryMount>()).has_value());
		return vfs;
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ProvenanceRecorder: entries are sorted by path, one per path, and written canonically" * doctest::skip(true))
		{
			Scope<VirtualFileSystem> vfs = MakeProvenanceVfs();
			Result<ProvenanceRecorder> recorder = ProvenanceRecorder::Load(*vfs);
			REQUIRE(recorder.has_value());
			CHECK(recorder->GetEntries().empty());

			recorder->Record("Assets/Scripts/Game.luau", 0x0123456789abcdefull, MakeAttribution("script.write", Json(3), "engine-mcp", 9));
			recorder->Record("Tetris.eproj", 0x1111ull, MakeAttribution("project.create", Json(1), "engine-mcp", 2));
			recorder->Record("Assets/Scenes/Main.scene", 0x2222ull, MakeAttribution("scene.new", Json("r-5"), "engine-mcp", 5));
			recorder->Record("Assets/Scripts/Game.luau", 0x3333ull, MakeAttribution("script.write", Json(4), "engine-mcp", 11));
			REQUIRE(recorder->GetEntries().size() == 3);
			CHECK(recorder->GetEntries()[0].Path == "Assets/Scenes/Main.scene");
			CHECK(recorder->GetEntries()[1].Path == "Assets/Scripts/Game.luau");
			CHECK(recorder->GetEntries()[1].Hash == 0x3333ull);
			CHECK(recorder->GetEntries()[1].TranscriptLine == 11u);
			CHECK(recorder->GetEntries()[2].Path == "Tetris.eproj");

			REQUIRE(recorder->Save(*vfs).has_value());
			const Result<VfsPath> file = VfsPath::Parse("project://Automation/Provenance.json");
			REQUIRE(file.has_value());
			const Result<std::string> text = vfs->ReadText(*file);
			REQUIRE(text.has_value());
			CHECK(*text == ProvenanceRecorder::ToText(recorder->GetEntries()));
			CHECK(text->starts_with("{\n\t\"Format\": \"Provenance\",\n\t\"Version\": 1,"));
			CHECK(text->contains("\"XXH64\": \"0000000000003333\""));
			CHECK(text->contains("\"RequestId\": \"r-5\""));

			const Result<ProvenanceRecorder> reloaded = ProvenanceRecorder::Load(*vfs);
			REQUIRE(reloaded.has_value());
			REQUIRE(reloaded->GetEntries().size() == 3);
			const ProvenanceEntry* scene = reloaded->Find("Assets/Scenes/Main.scene");
			REQUIRE(scene != nullptr);
			CHECK(scene->RequestId.Get() == Json("r-5"));
			const ProvenanceEntry* project = reloaded->Find("Tetris.eproj");
			REQUIRE(project != nullptr);
			CHECK(project->Method == "project.create");
		}

		TEST_CASE("ProvenanceRecorder: ui and cli writes have null request ids and transcript lines" * doctest::skip(true))
		{
			std::vector<ProvenanceEntry> entries = { { .Path = "Assets/A.scene", .Hash = 1, .Method = "ui", .RequestId = {}, .Client = "ui", .TranscriptLine = std::nullopt } };
			const std::string text = ProvenanceRecorder::ToText(entries);
			CHECK(text.contains("\"RequestId\": null"));
			CHECK(text.contains("\"TranscriptLine\": null"));
			const Result<std::vector<ProvenanceEntry>> read = ProvenanceRecorder::FromText(text);
			REQUIRE(read.has_value());
			CHECK((*read)[0].RequestId.IsNull());
			CHECK_FALSE((*read)[0].TranscriptLine.has_value());
		}

		TEST_CASE("ProvenanceRecorder: only the .eproj at the root and files under Assets/ are recorded" * doctest::skip(true))
		{
			CHECK(ProvenanceRecorder::IsRecordedPath("Tetris.eproj"));
			CHECK(ProvenanceRecorder::IsRecordedPath("Assets/Scenes/Main.scene"));
			CHECK(ProvenanceRecorder::IsRecordedPath("Assets/Scenes/Main.scene.meta"));
			CHECK_FALSE(ProvenanceRecorder::IsRecordedPath("Library/Cache/x.bin"));
			CHECK_FALSE(ProvenanceRecorder::IsRecordedPath("Automation/Provenance.json"));
			CHECK_FALSE(ProvenanceRecorder::IsRecordedPath("Sub/Other.eproj"));
			CHECK_FALSE(ProvenanceRecorder::IsRecordedPath("AGENTS.md"));
			CHECK_FALSE(ProvenanceRecorder::IsRecordedPath("AssetsX/a.scene"));
		}

		TEST_CASE("ProvenanceRecorder: malformed files are located errors" * doctest::skip(true))
		{
			CHECK(ProvenanceRecorder::FromText("{").error().GetCode() == ErrorCode::Parse);
			CHECK(ProvenanceRecorder::FromText(R"({"Format":"Scene","Version":1,"Entries":[]})").error().GetCode() == ErrorCode::Validation);
			CHECK(ProvenanceRecorder::FromText(R"({"Format":"Provenance","Version":2,"Entries":[]})").error().GetCode() == ErrorCode::UnsupportedVersion);
			const Result<std::vector<ProvenanceEntry>> unsorted = ProvenanceRecorder::FromText(
				R"({"Format":"Provenance","Version":1,"Entries":[
				{"Path":"b","XXH64":"0000000000000001","Method":"ui","RequestId":null,"Client":"ui","TranscriptLine":null},
				{"Path":"a","XXH64":"0000000000000001","Method":"ui","RequestId":null,"Client":"ui","TranscriptLine":null}]})");
			REQUIRE_FALSE(unsorted.has_value());
			CHECK(unsorted.error().GetCode() == ErrorCode::Validation);
			const Result<std::vector<ProvenanceEntry>> badHash = ProvenanceRecorder::FromText(
				R"({"Format":"Provenance","Version":1,"Entries":[
				{"Path":"a","XXH64":"12","Method":"ui","RequestId":null,"Client":"ui","TranscriptLine":null}]})");
			REQUIRE_FALSE(badHash.has_value());
			CHECK(badHash.error().GetLocation().JsonPointer == "/Entries/0/XXH64");
		}
	}

}
