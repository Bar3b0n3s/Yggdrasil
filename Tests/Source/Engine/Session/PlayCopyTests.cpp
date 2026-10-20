#include "TestsPCH.h"

#include "Engine/Session/PlaySession.h"

#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// The Play copy (Architecture §5.6; Docs/Decisions/0031-m20-contract.md decision 6): the faster serializer copy changes
// nothing it produces. A scene's play copy (PlaySession::CreateFromScene's ToJson, then its strict FromJson) is the scene's
// own document (a file's, when loading the file migrated nothing), loads into a runtime scene with the diagnostics the file
// loads with, and that scene re-saves to the document with the same state hash; a session created from the scene holds
// that runtime scene.

namespace Engine {

	namespace {

		// The diagnostics of `report`, one comparable line each.
		std::vector<std::string> DescribeDiagnostics(const LoadReport& report)
		{
			std::vector<std::string> lines;
			for (const LoadDiagnostic& diagnostic : report.Diagnostics)
			{
				lines.push_back(std::format("{} {} {} {} {}", diagnostic.Severity == DiagnosticSeverity::Error ? "error" : "warning", diagnostic.Code,
					diagnostic.JsonPointer, diagnostic.Entity, diagnostic.Message));
			}
			return lines;
		}

		std::string WriteMinified(const Json& document)
		{
			const Result<std::string> text = JsonWriter::Write(document, JsonStyle::Minified);
			REQUIRE_MESSAGE(text.has_value(), (text ? std::string() : text.error().ToString()));
			return *text;
		}

		// Loads the scene file `text` strictly into the fixture's edit scene, then checks its play copy: CreateFromScene's
		// document and its strict load into a runtime scene. Returns the file's load report.
		LoadReport CheckPlayCopy(Test::SceneTestFixture& fixture, std::string_view text)
		{
			LoadReport fileReport;
			const Status loaded = SceneSerializer::LoadFromString(fixture.GetScene(), text, LoadOptions{}, fileReport);
			REQUIRE_MESSAGE(loaded.has_value(), (loaded ? std::string() : loaded.error().ToString()));
			const Result<Json> file = JsonReader::Parse(text);
			REQUIRE(file.has_value());

			const Result<Json> document = SceneSerializer::ToJson(fixture.GetScene());
			REQUIRE_MESSAGE(document.has_value(), (document ? std::string() : document.error().ToString()));
			const std::string expected = WriteMinified(*document);
			if (!fileReport.Migrated)
				CHECK(expected == WriteMinified(*file));

			const Scope<Scene> copy = fixture.CreateEmptyScene(true);
			LoadReport copyReport;
			const Status copied = SceneSerializer::FromJson(*copy, *document, LoadOptions{}, copyReport);
			REQUIRE_MESSAGE(copied.has_value(), (copied ? std::string() : copied.error().ToString()));
			CHECK(DescribeDiagnostics(copyReport) == DescribeDiagnostics(fileReport));
			CHECK(copyReport.Repairs.empty());
			CHECK_FALSE(copyReport.Migrated);
			CHECK(copy->GetEntityCount() == fixture.GetScene().GetEntityCount());
			const Result<std::string> copyText = SceneSerializer::SaveToString(*copy, JsonStyle::Minified);
			REQUIRE(copyText.has_value());
			CHECK(*copyText == expected);
			CHECK(copy->ComputeStateHash() == fixture.GetScene().ComputeStateHash());
			return fileReport;
		}

		// A canonical scene of `count` entities, spelled here rather than written by the serializer: roots of nine children
		// each on a grid of quarter units, every third entity inactive, every fourth tagged, every other one rotated and
		// scaled, and every fifth with a material.
		std::string MakeEntitiesScene(const TypeRegistry& registry, uint32_t count)
		{
			constexpr uint64_t FirstId = 0x5300000000000001;
			std::string entities;
			for (uint32_t index = 0; index < count; ++index)
			{
				const std::string parent = index % 10 == 0 ? std::string("null") : std::format("\"{:016x}\"", FirstId + index / 10 * 10);
				const std::string_view rotation = index % 2 == 0 ? "[0, 0, 0, 1]" : "[0, 0.70710677, 0, 0.70710677]";
				const std::string_view scale = index % 2 == 0 ? "[1, 1, 1]" : "[2, 0.5, 2]";
				const std::string_view materials = index % 5 == 0 ? R"(["5400000000000001"])" : "[]";
				entities += std::format(R"({}{{ "ID": "{:016x}", "Name": "Entity{:04}", "Parent": {}, "Active": {}, "Tags": {}, "Components": {{ )"
										R"("Transform": {{ "Translation": [{}, 0, {}], "Rotation": {}, "Scale": {} }}, )"
										R"("MeshRenderer": {{ "Mesh": "0000000000000101", "Materials": {}, "CastShadows": true, "ReceiveShadows": true, "Visible": true }} }} }})",
					index == 0 ? "" : ", ", FirstId + index, index, parent, index % 3 == 1 ? "false" : "true", index % 4 == 0 ? R"(["Even"])" : "[]",
					static_cast<double>(index % 40) * 0.25, static_cast<double>(index / 40) * 0.25, rotation, scale, materials);
			}
			return std::format(R"({{ "Format": "Scene", "Version": 1, "Name": "Entities", "Seed": 7, )"
							   R"("ComponentVersions": {{ "Transform": {}, "MeshRenderer": {} }}, "Entities": [{}] }})",
				registry.FindComponent("Transform")->GetVersion(), registry.FindComponent("MeshRenderer")->GetVersion(), entities);
		}

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("PlaySession: the play copy of each FeatureTest scene keeps its document, load diagnostics and state hash")
		{
			const std::filesystem::path scenes = Test::GetRepositoryRoot() / "Projects" / "FeatureTest" / "Assets" / "Scenes";
			const Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(scenes, true);
			REQUIRE(entries.has_value());
			size_t checked = 0;
			for (const std::filesystem::path& path : *entries)
			{
				if (path.extension() != ".scene")
					continue;
				INFO(path.generic_string());
				const Result<std::string> text = FileSystem::ReadText(path);
				REQUIRE(text.has_value());
				Test::SceneTestFixture fixture;
				static_cast<void>(CheckPlayCopy(fixture, *text));
				++checked;
			}
			CHECK(checked > 0);
		}

		TEST_CASE("PlaySession: the play copy of a 1,000-entity scene keeps its document, load diagnostics and state hash")
		{
			Test::SceneTestFixture fixture;
			const std::string text = MakeEntitiesScene(fixture.GetRegistry(), 1000);
			const LoadReport report = CheckPlayCopy(fixture, text);
			CHECK(fixture.GetScene().GetEntityCount() == 1000);
			CHECK_FALSE(report.Migrated); // so the copy was compared with the document spelled above
			CHECK(report.Diagnostics.empty());

			PlaySessionSpecification specification;
			specification.Registry = &fixture.GetRegistry();
			specification.Seed = 42;
			const Result<Scope<PlaySession>> session = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(session.has_value(), (session ? std::string() : session.error().ToString()));
			const Scene& played = (*session)->GetScene();
			CHECK(played.GetEntityCount() == 1000);
			const Result<std::string> playedText = SceneSerializer::SaveToString(played, JsonStyle::Minified);
			REQUIRE(playedText.has_value());
			const Result<Json> spelled = JsonReader::Parse(text);
			REQUIRE(spelled.has_value());
			CHECK(*playedText == WriteMinified(*spelled));
			CHECK(played.ComputeStateHash() == fixture.GetScene().ComputeStateHash());
		}
	}

}
