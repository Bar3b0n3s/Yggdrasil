#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/GraphImporter.h"

#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/Core/Hash.h"
#include "Support/AssetTestFixture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// GraphImporter (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decision 9, stream G-B). The identity case is live (the
// contract implements it); the import cases are skipped until stream G-B implements the importer over stream G-A's document.

namespace Engine {

	namespace {

		constexpr AssetHandle GraphHandle = AssetHandle(0x6a7a000000000001ull);
		constexpr AssetHandle BeepHandle = AssetHandle(0x00000000005feedull);
		constexpr AssetHandle ChimeHandle = AssetHandle(0x00000000000a0d10ull);

		// Plays the Chime (a literal handle) at start, then what Assets.Load gives for a literal path (Beep) when that is not nil:
		// Assets.Load's Result is optional (AssetRef?), so it reaches the one-shot's required clip through Flow.IfNotNil
		// (Docs/Decisions/0035-m24-contract.md decision 6).
		constexpr std::string_view JingleGraph = R"({"Format": "Graph", "Version": 1, "Name": "Jingle", "Fields": [], "Variables": [],
			"Nodes": [
				{"Id": 1, "Kind": "Event.OnStart", "Position": [0, 0], "Values": {}},
				{"Id": 2, "Kind": "Call.Audio.PlayOneShot", "Position": [240, 0], "Values": {"clip": "00000000000a0d10"}},
				{"Id": 3, "Kind": "Call.Assets.Load", "Position": [240, 160], "Values": {"reference": "Assets/Audio/Beep.sfx"}},
				{"Id": 4, "Kind": "Call.Audio.PlayOneShot", "Position": [720, 0], "Values": {}},
				{"Id": 5, "Kind": "Flow.IfNotNil", "Position": [480, 0], "Values": {}}],
			"Links": [
				{"From": {"Node": 1, "Pin": "Then"}, "To": {"Node": 2, "Pin": "In"}},
				{"From": {"Node": 2, "Pin": "Then"}, "To": {"Node": 5, "Pin": "In"}},
				{"From": {"Node": 3, "Pin": "Result"}, "To": {"Node": 5, "Pin": "Value"}},
				{"From": {"Node": 5, "Pin": "Result"}, "To": {"Node": 4, "Pin": "clip"}},
				{"From": {"Node": 5, "Pin": "True"}, "To": {"Node": 4, "Pin": "In"}}]})";

		AssetMetadata GraphMetadata()
		{
			return { .Handle = GraphHandle, .Type = AssetType::Script, .Importer = std::string(GraphImporter::Id), .ImporterVersion = GraphImporter::Version };
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("GraphImporter: a graph is a Script asset of the Graph importer, which claims .graph")
		{
			const GraphImporter importer;
			CHECK(importer.GetId() == "Graph");
			CHECK(importer.GetVersion() == 1);
			CHECK(importer.GetMainType() == AssetType::Script);
			CHECK(std::ranges::equal(importer.GetExtensions(), std::array<std::string_view, 1>{ ".graph" }));
			CHECK(importer.GetSettingsTypeName().empty());
			CHECK_FALSE(importer.RequiresMainThread());
		}

		TEST_CASE("GraphImporter: a graph imports as a Script behaviour named after it, whose dependencies hold its asset literals and not its literal loads"
			* doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Scripts/Jingle.graph", JingleGraph);
			const Buffer bytes = fixture.ReadProjectFile("Assets/Scripts/Jingle.graph");
			const std::vector<ImportAssetLookupEntry> assets = {
				{ .SourcePath = fixture.ProjectPath("Assets/Audio/Beep.sfx"), .Handle = BeepHandle, .Kind = AssetMetaKind::Asset, .Type = AssetType::AudioClip },
			};
			ImportContext context({ .Vfs = &fixture.GetVfs(), .SourcePath = fixture.ProjectPath("Assets/Scripts/Jingle.graph"), .SourceBytes = bytes, .Registry = &fixture.GetRegistry(), .Assets = assets });
			const Result<ImportResult> imported = GraphImporter().Import(context, GraphMetadata());
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Artifacts[0].Type == AssetType::Script);
			CHECK(imported->Artifacts[0].Handle == GraphHandle);
			// The literal handle, as a component's reference is listed (whether or not the asset exists); the asset the literal
			// load names is the export's scan of the transpiled text, as a Luau script's (decision 24), never a dependency.
			CHECK(imported->Dependencies == std::vector<AssetHandle>{ ChimeHandle });

			const Result<AssetRef<ScriptData>> script = LoadCookedScript(imported->Artifacts[0].Cooked);
			REQUIRE_MESSAGE(script.has_value(), script.error().ToString());
			CHECK((*script)->Kind == ScriptKind::Behaviour);
			CHECK((*script)->Name == "Jingle");
			CHECK((*script)->Fields.empty());
			CHECK((*script)->Requires.empty());
			// Decision 8: the source map describes the transpiled text under the .graph path.
			CHECK((*script)->SourceMap.Path == "Assets/Scripts/Jingle.graph");
			CHECK((*script)->SourceMap.ChunkName == "@Assets/Scripts/Jingle.graph");

			const std::optional<ScriptImportCheck> check = context.GetScriptCheck();
			REQUIRE(check.has_value());
			CHECK_FALSE(check->Performed); // no diagnostics provider
			CHECK(check->SourceHash == XXH64(bytes));
			CHECK(check->Diagnostics.empty());

			// A pure function of its inputs: a second import gives the same artifact.
			ImportContext again({ .Vfs = &fixture.GetVfs(), .SourcePath = fixture.ProjectPath("Assets/Scripts/Jingle.graph"), .SourceBytes = bytes, .Registry = &fixture.GetRegistry(), .Assets = assets });
			const Result<ImportResult> second = GraphImporter().Import(again, GraphMetadata());
			REQUIRE(second.has_value());
			CHECK(second->Artifacts[0].Cooked == imported->Artifacts[0].Cooked);
		}

		TEST_CASE("GraphImporter: a graph that does not validate fails its import with a GRAPH_INVALID finding per issue and no artifact"
			* doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			std::string broken(JingleGraph);
			broken.replace(broken.find("Call.Assets.Load"), std::string_view("Call.Assets.Load").size(), "Call.Assets.Lode");
			fixture.WriteProjectText("Assets/Scripts/Jingle.graph", broken);
			const Buffer bytes = fixture.ReadProjectFile("Assets/Scripts/Jingle.graph");
			ImportContext context({ .Vfs = &fixture.GetVfs(), .SourcePath = fixture.ProjectPath("Assets/Scripts/Jingle.graph"), .SourceBytes = bytes, .Registry = &fixture.GetRegistry() });
			const Result<ImportResult> imported = GraphImporter().Import(context, GraphMetadata());
			REQUIRE_FALSE(imported.has_value());
			CHECK(imported.error().GetCode() == ErrorCode::Validation);
			REQUIRE_FALSE(imported.error().GetIssues().empty());
			CHECK(imported.error().GetIssues().front().JsonPointer == "/Nodes/2/Kind");
			// The check is published before the failure, with the issue as a GRAPH_INVALID finding.
			const std::optional<ScriptImportCheck> check = context.GetScriptCheck();
			REQUIRE(check.has_value());
			CHECK(check->SourceHash == XXH64(bytes));
			CHECK(std::ranges::any_of(check->Diagnostics, [](const ScriptDiagnostic& diagnostic)
			{
				return diagnostic.Code == GraphInvalidCode && diagnostic.Severity == DiagnosticSeverity::Error && diagnostic.Line == 0;
			}));
		}
	}

}
