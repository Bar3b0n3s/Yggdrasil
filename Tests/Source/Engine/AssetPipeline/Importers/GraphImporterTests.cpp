#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/GraphImporter.h"

#include "EditorCore/Scripting/ScriptTypeChecker.h"
#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"
#include "Engine/AssetPipeline/Importers/ScriptImporter.h"
#include "Engine/Core/Hash.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/Sandbox.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/AssetTestFixture.h"
#include "Support/SceneTestFixture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// GraphImporter (M24, C24.1, C-VS; Docs/Decisions/0035-m24-contract.md decision 9, stream G-B), over stream G-A's document.

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

		// A diagnostics provider that reports one type finding at the line of a marker in the text it checks, and records what
		// it was asked.
		class MarkerProvider final : public IScriptDiagnosticsProvider
		{
		public:
			explicit MarkerProvider(std::string marker)
				: m_Marker(std::move(marker))
			{
			}

			[[nodiscard]] uint64_t GetEnvironmentHash() const override { return 0x5eedull; }

			[[nodiscard]] std::vector<ScriptDiagnostic> CheckScript(const ScriptCheckRequest& request) override
			{
				Path = request.Path.ToString();
				Source = std::string(request.Source);
				const size_t at = Source.find(m_Marker);
				if (at == std::string::npos)
					return {};
				const uint32_t line = static_cast<uint32_t>(std::count(Source.begin(), Source.begin() + static_cast<std::ptrdiff_t>(at), '\n')) + 1;
				return { ScriptDiagnostic{ .Severity = DiagnosticSeverity::Error, .Code = "SCRIPT_TYPE_ERROR", .File = std::string(request.Path.GetPath()), .Line = line, .Column = 1, .Message = "a finding" } };
			}

			std::string Path{};
			std::string Source{};
		private:
			std::string m_Marker{};
		};

		Result<ImportResult> ImportGraph(Test::AssetTestFixture& fixture, std::string_view text, IScriptDiagnosticsProvider* provider, std::optional<ScriptImportCheck>& check)
		{
			fixture.WriteProjectText("Assets/Scripts/Jingle.graph", text);
			const Buffer bytes = fixture.ReadProjectFile("Assets/Scripts/Jingle.graph");
			ImportContext context({ .Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath("Assets/Scripts/Jingle.graph"),
				.SourceBytes = bytes,
				.Registry = &fixture.GetRegistry(),
				.ScriptDiagnostics = provider });
			Result<ImportResult> imported = GraphImporter().Import(context, GraphMetadata());
			check = context.GetScriptCheck();
			return imported;
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

		TEST_CASE("GraphImporter: a graph imports as a Script behaviour named after it, whose dependencies hold its asset literals and not its literal loads")
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

		TEST_CASE("GraphImporter: a graph that does not validate fails its import with a GRAPH_INVALID finding per issue and no artifact")
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

		TEST_CASE("GraphImporter: the script check holds the checker's findings of the text, each naming its node, and a document that does not read is a GRAPH_INVALID finding")
		{
			Test::AssetTestFixture fixture;
			MarkerProvider provider("-- 2: Call.Audio.PlayOneShot");
			std::optional<ScriptImportCheck> check;
			const Result<ImportResult> imported = ImportGraph(fixture, JingleGraph, &provider, check);
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			// The checker sees the transpiled text under the .graph path (decision 9).
			CHECK(provider.Path == "project://Assets/Scripts/Jingle.graph");
			CHECK(provider.Source.starts_with("--!strict\n"));
			REQUIRE(check.has_value());
			CHECK(check->Performed);
			CHECK(check->EnvironmentHash == 0x5eedull);
			REQUIRE(check->Diagnostics.size() == 1);
			CHECK(check->Diagnostics.front().Code == "SCRIPT_TYPE_ERROR");
			CHECK(check->Diagnostics.front().Message.starts_with("node 2 (Call.Audio.PlayOneShot): "));

			const Result<ImportResult> unreadable = ImportGraph(fixture, "{ \"Format\": \"Graph\",", &provider, check);
			REQUIRE_FALSE(unreadable.has_value());
			CHECK(unreadable.error().GetCode() == ErrorCode::Parse);
			REQUIRE(check.has_value());
			REQUIRE(check->Diagnostics.size() == 1);
			CHECK(check->Diagnostics.front().Code == GraphInvalidCode);
			CHECK(check->Diagnostics.front().Line == 0);
		}

		// Docs/Decisions/0041-script-check-performance.md decision 3, redone for graphs at M24's landing: a graph's import built a
		// script API registry of its own for its node library and let its load-time VM build another. It borrows the context's
		// shared pure API for both, as a .luau import's VM does, on any import thread, and its check by the same checker records
		// the environment hash a .luau import records.
		TEST_CASE("GraphImporter: a graph import borrows the context's pure script API and records the environment hash a .luau import records")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Scripts/Jingle.graph", JingleGraph);
			fixture.WriteProjectText("Assets/Scripts/Tune.luau", "--!strict\nlocal Tune = { Fields = { Volume = Field.Number(1) } }\nreturn Script.Define('Tune', Tune)\n");
			const std::vector<ImportAssetLookupEntry> assets = {
				{ .SourcePath = fixture.ProjectPath("Assets/Audio/Beep.sfx"), .Handle = BeepHandle, .Kind = AssetMetaKind::Asset, .Type = AssetType::AudioClip },
			};
			struct Imported
			{
				std::optional<ImportResult> Result{};
				std::optional<ScriptImportCheck> Check{};
			};
			// Runs on worker threads too, so it makes no doctest assertion: its sources are read beforehand.
			const auto import = [&fixture, &assets](const IAssetImporter& importer, const VfsPath& path, const Buffer& source, const AssetMetadata& metadata,
									IScriptDiagnosticsProvider* provider, ScriptApiRegistry* api)
			{
				ImportContext context({ .Vfs = &fixture.GetVfs(), .SourcePath = path, .SourceBytes = source, .Registry = &fixture.GetRegistry(), .Assets = assets, .ScriptDiagnostics = provider, .LoadTimeScriptApi = api });
				Result<ImportResult> result = importer.Import(context, metadata);
				Imported imported{ .Check = context.GetScriptCheck() };
				if (result)
					imported.Result = std::move(*result);
				return imported;
			};
			const VfsPath graphPath = fixture.ProjectPath("Assets/Scripts/Jingle.graph");
			const Buffer graph = fixture.ReadProjectFile("Assets/Scripts/Jingle.graph");
			const VfsPath tunePath = fixture.ProjectPath("Assets/Scripts/Tune.luau");
			const Buffer tune = fixture.ReadProjectFile("Assets/Scripts/Tune.luau");
			const AssetMetadata tuneMetadata{ .Handle = AssetHandle(0x6a7a000000000002ull), .Type = AssetType::Script, .Importer = std::string(ScriptImporter::Id), .ImporterVersion = ScriptImporter::Version };
			const GraphImporter graphImporter;
			auto pure = PureScriptApi::Create();
			REQUIRE(pure);

			// Two imports borrow the one API at the same time, each on its own thread, as import jobs do, and cook what an import
			// with an API of its own cooks.
			const Imported own = import(graphImporter, graphPath, graph, GraphMetadata(), nullptr, nullptr);
			REQUIRE(own.Result.has_value());
			std::array<Imported, 2> shared;
			{
				std::jthread first([&shared, &import, &graphImporter, &graphPath, &graph, &pure]()
				{
					shared[0] = import(graphImporter, graphPath, graph, GraphMetadata(), nullptr, &(*pure)->GetApi());
				});
				std::jthread second([&shared, &import, &graphImporter, &graphPath, &graph, &pure]()
				{
					shared[1] = import(graphImporter, graphPath, graph, GraphMetadata(), nullptr, &(*pure)->GetApi());
				});
			}
			for (const Imported& borrowed : shared)
			{
				REQUIRE(borrowed.Result.has_value());
				REQUIRE(borrowed.Result->Artifacts.size() == 1);
				CHECK(borrowed.Result->Artifacts[0].Handle == own.Result->Artifacts[0].Handle);
				CHECK(borrowed.Result->Artifacts[0].Cooked == own.Result->Artifacts[0].Cooked);
				CHECK(borrowed.Result->Dependencies == own.Result->Dependencies);
				REQUIRE(borrowed.Check.has_value());
				REQUIRE(own.Check.has_value());
				CHECK(borrowed.Check->SourceHash == own.Check->SourceHash);
				CHECK(borrowed.Check->Diagnostics == own.Check->Diagnostics);
			}

			// One checker checks a .luau import and the graph's text, both borrowing the API: the two checks record the checker's
			// environment hash, and the graph's check takes the frontend the checker prepared, as the .luau one does.
			Scope<TypeRegistry> types = Test::CreateBuiltinRegistry();
			ScriptApiRegistry declarations;
			REQUIRE(RegisterBindings(declarations, *types));
			Result<ScriptTypeCheckerConfiguration> configuration = ScriptTypeChecker::CaptureConfiguration(fixture.GetVfs());
			REQUIRE(configuration);
			Result<Scope<ScriptTypeChecker>> checker = ScriptTypeChecker::Create(declarations, std::move(*configuration));
			REQUIRE(checker);
			const Imported script = import(ScriptImporter(), tunePath, tune, tuneMetadata, checker->get(), &(*pure)->GetApi());
			const Imported checked = import(graphImporter, graphPath, graph, GraphMetadata(), checker->get(), &(*pure)->GetApi());
			REQUIRE(script.Result.has_value());
			REQUIRE(checked.Result.has_value());
			REQUIRE(script.Check.has_value());
			REQUIRE(checked.Check.has_value());
			CHECK(script.Check->Performed);
			CHECK(checked.Check->Performed);
			CHECK(script.Check->EnvironmentHash == (*checker)->GetEnvironmentHash());
			CHECK(checked.Check->EnvironmentHash == script.Check->EnvironmentHash);
			CHECK(checked.Check->Diagnostics.empty());
			CHECK(checked.Result->Artifacts[0].Cooked == own.Result->Artifacts[0].Cooked);
			CHECK((*checker)->GetStatistics().Checks == 2);
			CHECK((*checker)->GetStatistics().DefinitionLoads == 1);

			// The import builds its library from, and extracts its text with, the API it borrows: one without the bindings has no
			// node of the graph's kinds, so the graph does not validate.
			TypeRegistry bareTypes;
			bareTypes.Freeze();
			ScriptApiRegistry bare;
			REQUIRE(bare.Freeze(bareTypes));
			const Imported unbound = import(graphImporter, graphPath, graph, GraphMetadata(), nullptr, &bare);
			CHECK_FALSE(unbound.Result.has_value());
			REQUIRE(unbound.Check.has_value());
			CHECK(std::ranges::any_of(unbound.Check->Diagnostics, [](const ScriptDiagnostic& diagnostic)
			{
				return diagnostic.Code == GraphInvalidCode;
			}));
		}

		// Docs/Decisions/0035-m24-contract.md risk 1: a graph whose text exceeds a limit of Luau's (here a function's 200 locals,
		// one statement reading a long chain of values) fails its import at the line of the node that exceeds it.
		TEST_CASE("GraphImporter: a graph whose text exceeds Luau's limits fails its import at the line of a node, which its compile finding names")
		{
			std::string nodes = R"({"Id": 1, "Kind": "Event.OnStart", "Position": [0, 0], "Values": {}}, {"Id": 2, "Kind": "Call.Log.Info", "Position": [0, 0], "Values": {}})";
			std::string links = R"({"From": {"Node": 1, "Pin": "Then"}, "To": {"Node": 2, "Pin": "In"}})";
			constexpr uint32_t Last = 400;
			for (uint32_t id = 3; id < Last; ++id)
			{
				nodes += std::format(R"(, {{"Id": {}, "Kind": "Number.Add", "Position": [0, 0], "Values": {{"B": 1}}}})", id);
				links += std::format(R"(, {{"From": {{"Node": {}, "Pin": "Result"}}, "To": {{"Node": {}, "Pin": "{}"}}}})", id, id == 3 ? 2 : id - 1, id == 3 ? "Values" : "A");
			}
			nodes += std::format(R"(, {{"Id": {}, "Kind": "Self.Get.Base", "Position": [0, 0], "Values": {{}}}})", Last);
			links += std::format(R"(, {{"From": {{"Node": {}, "Pin": "Value"}}, "To": {{"Node": {}, "Pin": "A"}}}})", Last, Last - 1);
			const std::string graph = std::format(R"({{"Format": "Graph", "Version": 1, "Name": "Jingle", "Fields": [{{"Name": "Base", "Kind": "Number", "Default": 0, "Element": "Number", "AssetType": "", "EnumValues": [], "Options": {{}}}}], "Variables": [], "Nodes": [{}], "Links": [{}]}})",
				nodes, links);
			Test::AssetTestFixture fixture;
			std::optional<ScriptImportCheck> check;
			const Result<ImportResult> imported = ImportGraph(fixture, graph, nullptr, check);
			REQUIRE_FALSE(imported.has_value());
			CHECK(imported.error().GetCode() == ErrorCode::CompileFailed);
			CHECK(imported.error().GetLocation().Line != 0);
			REQUIRE(check.has_value());
			REQUIRE(check->Diagnostics.size() == 1);
			CHECK(check->Diagnostics.front().Code == "SCRIPT_COMPILE_ERROR");
			CHECK(check->Diagnostics.front().Line == imported.error().GetLocation().Line);
			CHECK(check->Diagnostics.front().Message.starts_with("node "));
		}
	}

}
