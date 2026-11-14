#include "TestsPCH.h"
#include "Editor/Tools/TerrainHeightmapImport.h"

#include "Editor/PanelInteractionFixture.h"
#include "Editor/ScriptedFileDialog.h"
#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

// The terrain inspector's Import Heightmap action (M26, C-TERR; Docs/Decisions/0037-m26-contract.md decisions 20 and 31 (j)):
// where it imports, and the action through its dialog: terrain.importHeightmap, then the assignment.

namespace Engine {

	namespace {

		VfsPath ProjectPath(std::string_view relative)
		{
			return VfsPath::Create("project", relative).value_or(VfsPath());
		}

		void WriteProjectBytes(EditorContext& editor, std::string_view relative, std::span<const std::byte> bytes)
		{
			const VfsPath path = ProjectPath(relative);
			REQUIRE(editor.CreateProjectDirectory(path.GetParent()).has_value());
			REQUIRE(editor.WriteProjectFile(path, bytes).has_value());
		}

		UUID CreateTerrain(Test::AutomationFixture& setup, const Json& terrain)
		{
			const auto created = setup.Call("entity.create", Json{ { "name", "Hills" }, { "components", Json{ { "Terrain", terrain } } } });
			REQUIRE_MESSAGE(created.has_value(), (created ? std::string() : created.error().ToString()));
			const Result<UUID> id = JsonReader((*created)["entity"]["id"]).ReadUUID();
			REQUIRE(id.has_value());
			return *id;
		}

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("TerrainHeightmapImport: a terrain's own heightfield is replaced in place, and a terrain without one gets a free name to assign")
		{
			Test::AutomationFixture setup("TerrainHeightmapTarget");
			EditorContext& editor = setup.GetEditor();
			TerrainComponent bare;
			const Result<TerrainHeightmapImportTarget> fresh = FindTerrainHeightmapImportTarget(editor, bare, "D:/Downloads/Ridge.r16");
			REQUIRE(fresh.has_value());
			CHECK(fresh->Path == "Assets/Terrain/Ridge.heightfield");
			CHECK(fresh->Assign);

			// A file at the name is never replaced: the first free "-<n>" from 2.
			REQUIRE(setup.Call("asset.create", Json{ { "type", "Heightfield" }, { "path", "Assets/Terrain/Ridge.heightfield" } }).has_value());
			REQUIRE(setup.Call("asset.create", Json{ { "type", "Heightfield" }, { "path", "Assets/Terrain/Ridge-2.heightfield" } }).has_value());
			const Result<TerrainHeightmapImportTarget> taken = FindTerrainHeightmapImportTarget(editor, bare, "D:/Downloads/Ridge.r16");
			REQUIRE(taken.has_value());
			CHECK(taken->Path == "Assets/Terrain/Ridge-3.heightfield");
			CHECK(taken->Assign);

			// A terrain naming a heightfield of the project imports into it, keeping its handle.
			TerrainComponent named;
			const std::optional<AssetHandle> ridge = editor.GetAssets().Resolve("Assets/Terrain/Ridge.heightfield");
			REQUIRE(ridge.has_value());
			named.Heightfield = TypedAssetHandle<AssetType::Heightfield>(*ridge);
			const Result<TerrainHeightmapImportTarget> own = FindTerrainHeightmapImportTarget(editor, named, "D:/Downloads/Valley.png");
			REQUIRE(own.has_value());
			CHECK(own->Path == "Assets/Terrain/Ridge.heightfield");
			CHECK_FALSE(own->Assign);

			// A source without a file name names nothing to import.
			CHECK_FALSE(FindTerrainHeightmapImportTarget(editor, bare, "").has_value());
		}

		TEST_CASE("TerrainHeightmapImport: the chosen heightmap is imported with terrain.importHeightmap and assigned to the terrain")
		{
			Test::PanelInteractionFixture fixture("TerrainHeightmapAction");
			EditorPanelContext& context = fixture.GetContext();
			EditorContext& editor = context.Editor;
			Test::AutomationFixture& setup = fixture.GetAutomation();
			// A flat 17 x 17 RAW heightmap in the project, chosen through the (scripted) native dialog.
			WriteProjectBytes(editor, "Heightmaps/Hills.raw", Buffer(static_cast<size_t>(17) * 17 * 2, std::byte{ 0 }));
			const std::filesystem::path native = editor.GetProject().GetRoot() / "Heightmaps" / "Hills.raw";
			const auto script = CreateRef<Test::ScriptedFileDialogScript>();
			script->Outcome = FileDialogOutcome{ .State = FileDialogState::Accepted, .Path = native };
			context.FileDialogs = Test::MakeScriptedFileDialogBackend(script);
			const UUID hills = CreateTerrain(setup, Json::object());

			TerrainHeightmapImport action;
			Test::PanelInteractionUi ui;
			std::optional<ImVec2> button;
			const auto draw = [&action, &context, &button, hills]() -> Status
			{
				const Status status = action.Draw(context, hills);
				if (!button.has_value())
					button = Test::PanelInteractionUi::LastItemCenter();
				return status;
			};
			REQUIRE(ui.Frame(draw).has_value());
			REQUIRE(button.has_value());
			const size_t history = editor.GetHistory().GetUndoCount();
			REQUIRE(ui.Click(draw, *button).has_value());
			REQUIRE(script->Requests.size() == 1);
			CHECK(script->Requests.front().Extensions == std::vector<std::string>{ ".png", ".raw", ".r16" });
			// The dialog answers; the import, then the assignment, run at the next safe points.
			for (int frame = 0; frame < 6; ++frame)
			{
				const Status status = ui.Frame(draw);
				REQUIRE_MESSAGE(status.has_value(), (status ? std::string() : status.error().ToString()));
				context.Actions.Pump();
			}
			const std::optional<AssetHandle> imported = editor.GetAssets().Resolve("Assets/Terrain/Hills.heightfield");
			REQUIRE(imported.has_value());
			CHECK(editor.GetScene().FindEntityByID(hills).GetComponent<TerrainComponent>().Heightfield.GetHandle() == *imported);
			// Two undo steps: the import and the assignment.
			CHECK(editor.GetHistory().GetUndoCount() == history + 2);

			// Importing again replaces the terrain's own heightfield in place: its file holds the new heightmap's samples, one
			// undo step (the import), no new file and no new assignment. The heightmap now rises along +X, so the import is
			// observable (the same bytes again would write nothing).
			HeightfieldData ramp = MakeFlatHeightfield(17);
			Buffer raw;
			for (uint32_t z = 0; z < 17; ++z)
			{
				for (uint32_t x = 0; x < 17; ++x)
				{
					const uint32_t sample = x * 4000;
					ramp.Heights[x + 17 * z] = static_cast<uint16_t>(sample);
					raw.push_back(static_cast<std::byte>(sample & 0xFFU));
					raw.push_back(static_cast<std::byte>(sample >> 8U));
				}
			}
			WriteProjectBytes(editor, "Heightmaps/Hills.raw", raw);
			script->Requests.clear();
			REQUIRE(ui.Click(draw, *button).has_value());
			REQUIRE(script->Requests.size() == 1);
			for (int frame = 0; frame < 6; ++frame)
			{
				REQUIRE(ui.Frame(draw).has_value());
				context.Actions.Pump();
			}
			CHECK(editor.GetVfs().ReadFile(ProjectPath("Assets/Terrain/Hills.heightfield")).value_or(Buffer()) == WriteHeightfieldSource(ramp));
			CHECK_FALSE(editor.GetVfs().Exists(ProjectPath("Assets/Terrain/Hills-2.heightfield")));
			CHECK(editor.GetAssets().Resolve("Assets/Terrain/Hills.heightfield") == imported);
			CHECK(editor.GetScene().FindEntityByID(hills).GetComponent<TerrainComponent>().Heightfield.GetHandle() == *imported);
			CHECK(editor.GetHistory().GetUndoCount() == history + 3);
		}
	}

}
