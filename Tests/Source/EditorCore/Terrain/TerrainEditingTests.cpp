#include "TestsPCH.h"

#include "EditorCore/Terrain/TerrainEditing.h"

#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Terrain sculpting and painting (M26, C26.1, C-TERR-EDIT; Docs/Decisions/0037-m26-contract.md decision 23, stream T-E): the
// brush's pure rules, and the edit's round trip: a stroke through EditTerrain, through terrain.edit as the editor's terrain
// brush queues it (EditorActions, the user's origin) and through terrain.edit from an agent writes the same bytes, and one
// undo restores them.

namespace Engine {

	namespace {

		// A 9 x 9 flat terrain over 8 x 8 metres, 65535 samples = 8 metres (a sample per 8 / 65535 m).
		constexpr TerrainExtent Extent{ .Size = glm::vec2(8.0f), .HeightScale = 8.0f };

		TerrainStroke MakeStroke(TerrainTool tool)
		{
			TerrainStroke stroke;
			stroke.Tool = tool;
			stroke.Points = { glm::vec2(4.0f, 4.0f) };
			stroke.Radius = 2.0f;
			stroke.Strength = 1.0f;
			return stroke;
		}

		constexpr std::string_view GroundPath = "Assets/Terrain/Ground.heightfield";

		Json CallOrFail(Test::AutomationFixture& setup, std::string_view method, const Json& params)
		{
			Result<Json> result = setup.Call(method, params);
			INFO(std::string(method));
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return std::move(*result);
		}

		// The project file `relative`'s bytes, or nullopt when it does not exist.
		std::optional<Buffer> ReadProjectBytes(EditorContext& editor, std::string_view relative)
		{
			const VfsPath path = VfsPath::Create("project", relative).value_or(VfsPath());
			Result<Buffer> bytes = editor.GetVfs().ReadFile(path);
			if (!bytes.has_value())
				return std::nullopt;
			return std::move(*bytes);
		}

		// A project with Assets/Terrain/Ground.heightfield (129 x 129, flat) and the entity "Terrain" holding it over 32 x 32
		// metres at a translated, scaled transform whose inverse is exact (the method takes world points back to local ones
		// bit for bit). Returns the entity's id.
		UUID CreateTerrain(Test::AutomationFixture& setup, std::string_view name = "Terrain")
		{
			if (!ReadProjectBytes(setup.GetEditor(), GroundPath).has_value())
				CallOrFail(setup, "asset.create", Json{ { "type", "Heightfield" }, { "path", std::string(GroundPath) } });
			const Json components{ { "Transform", Json{ { "Translation", Json::array({ 10, 2, -6 }) }, { "Scale", Json::array({ 2, 1, 2 }) } } },
				{ "Terrain", Json{ { "Heightfield", std::string(GroundPath) }, { "Size", Json::array({ 32, 32 }) }, { "HeightScale", 8 } } } };
			const Json created = CallOrFail(setup, "entity.create", Json{ { "name", std::string(name) }, { "components", components } });
			const Result<UUID> id = JsonReader(created["entity"]["id"]).ReadUUID();
			REQUIRE(id.has_value());
			return *id;
		}

		// terrain.edit's params for `stroke` on `entity`, its local points taken to world space by CreateTerrain's transform.
		Json MakeEditParams(UUID entity, const TerrainStroke& stroke, std::string_view tool)
		{
			Json points = Json::array();
			for (const glm::vec2 point : stroke.Points)
				points.push_back(Json::array({ 10.0f + 2.0f * point.x, 2.5f, -6.0f + 2.0f * point.y }));
			return Json{ { "entity", entity.ToString() }, { "tool", std::string(tool) }, { "points", std::move(points) }, { "radius", stroke.Radius },
				{ "strength", stroke.Strength }, { "layer", stroke.Layer } };
		}

		Result<Json> CompleteAction(EditorActions& actions, std::string_view method, const Json& params)
		{
			ENGINE_TRY_ASSIGN(const uint64_t ticket, actions.Submit(method, params));
			actions.Pump();
			ENGINE_TRY_ASSIGN(std::optional<Result<Json>> completion, actions.TakeResult(ticket));
			REQUIRE(completion.has_value());
			return std::move(*completion);
		}

		const TerrainComponent& GetTerrain(EditorContext& editor, UUID entity)
		{
			const Entity found = editor.GetScene().FindEntityByID(entity);
			REQUIRE(found.IsValid());
			return found.GetComponent<TerrainComponent>();
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("TerrainEditing: a Raise dab lifts the samples under the brush by its falloff and leaves the rest")
		{
			HeightfieldData heightfield = MakeFlatHeightfield(9);
			ApplyTerrainSculpt(heightfield, Extent, MakeStroke(TerrainTool::Raise));
			// The centre sample (4, 4) rises by Strength * w(0) = 1 metre: 65535 / 8 samples, rounded.
			CHECK(heightfield.Heights[4 + 9 * 4] == 8192);
			// One metre off the centre, w = (1 - (1 / 2)^2)^2 = 0.5625: 0.5625 metres.
			CHECK(heightfield.Heights[5 + 9 * 4] == 4608);
			// Beyond the radius nothing changes.
			CHECK(heightfield.Heights[7 + 9 * 4] == 0);
			CHECK(heightfield.Heights[0] == 0);
			// Lower undoes Raise exactly, and a second application of the same stroke gives the same bytes.
			HeightfieldData again = heightfield;
			ApplyTerrainSculpt(heightfield, Extent, MakeStroke(TerrainTool::Lower));
			CHECK(heightfield.Heights == MakeFlatHeightfield(9).Heights);
			ApplyTerrainSculpt(again, Extent, MakeStroke(TerrainTool::Lower));
			CHECK(again.Heights == heightfield.Heights);
		}

		TEST_CASE("TerrainEditing: Flatten and Smooth move samples towards their targets, never past them")
		{
			HeightfieldData heightfield = MakeFlatHeightfield(9);
			TerrainStroke flatten = MakeStroke(TerrainTool::Flatten);
			flatten.Height = 2.0f;
			ApplyTerrainSculpt(heightfield, Extent, flatten);
			CHECK(heightfield.Heights[4 + 9 * 4] == 16384);
			CHECK(heightfield.Heights[5 + 9 * 4] < 16384);
			ApplyTerrainSculpt(heightfield, Extent, MakeStroke(TerrainTool::Smooth));
			CHECK(heightfield.Heights[4 + 9 * 4] < 16384);
			CHECK(heightfield.Heights[4 + 9 * 4] > 0);
		}

		TEST_CASE("TerrainEditing: a Paint dab raises its layer's weight and keeps every texel's weights summing to 255")
		{
			TerrainSplatImage splat;
			splat.Width = 8;
			splat.Height = 8;
			for (uint32_t texel = 0; texel < 64; ++texel)
				splat.Texels.insert(splat.Texels.end(), { 255, 0, 0, 0 });
			TerrainStroke paint = MakeStroke(TerrainTool::Paint);
			paint.Layer = 2;
			paint.Strength = 0.5f;
			ApplyTerrainPaint(splat, Extent, paint);
			// Texel (3, 3) covers (3.5, 3.5): inside the brush, so blue rose and red fell.
			const size_t centre = (3 + 8 * 3) * 4;
			CHECK(splat.Texels[centre + 2] > 0);
			CHECK(splat.Texels[centre + 0] < 255);
			for (size_t texel = 0; texel < 64; ++texel)
			{
				CAPTURE(texel);
				const uint32_t sum = uint32_t{ splat.Texels[texel * 4] } + splat.Texels[texel * 4 + 1] + splat.Texels[texel * 4 + 2] + splat.Texels[texel * 4 + 3];
				CHECK(sum == 255);
			}
			// A corner texel lies outside the brush.
			CHECK(splat.Texels[0] == 255);
		}

		TEST_CASE("TerrainEditing: Smooth means each sample's neighbours as the previous dab left them, and Paint rescales the other weights by their shares")
		{
			// A single raised sample: a Smooth dab at full strength over it gives the centre the mean of its 3 x 3 neighbourhood
			// as the dab found it, (9 * 0 + 9000) / 9 = 1000, and each direct neighbour inside the brush its own mean,
			// 9000 / 9 * w, never the centre's new value.
			HeightfieldData spike = MakeFlatHeightfield(9);
			spike.Heights[4 + 9 * 4] = 9000;
			TerrainStroke smooth = MakeStroke(TerrainTool::Smooth);
			smooth.Radius = 1.5f;
			ApplyTerrainSculpt(spike, Extent, smooth);
			CHECK(spike.Heights[4 + 9 * 4] == 1000);
			// (5, 4) is 1 m off the centre: w = (1 - 1 / 2.25)^2, so it moves w * 1000 towards its mean 1000.
			CHECK(spike.Heights[5 + 9 * 4] == 309);
			CHECK(spike.Heights[6 + 9 * 4] == 0);

			// Paint keeps the others' proportions: (100, 50, 105, 0) painting layer 3 at full strength and weight 1 takes the
			// whole sum; at half it takes 128 (0 + 0.5 * 255, ties to even), the rest 127 split 100 : 50 : 105 by largest
			// remainders: 49.8, 24.9 and 52.3 become 50, 25 and 52.
			TerrainSplatImage splat;
			splat.Width = 1;
			splat.Height = 1;
			splat.Texels = { 100, 50, 105, 0 };
			TerrainStroke paint = MakeStroke(TerrainTool::Paint);
			paint.Points = { glm::vec2(4.0f, 4.0f) };
			paint.Layer = 3;
			paint.Strength = 0.5f;
			ApplyTerrainPaint(splat, Extent, paint);
			CHECK(splat.Texels == std::vector<uint8_t>{ 50, 25, 52, 128 });
			// A texel whose other weights are all 0 gives the painted layer the whole sum.
			splat.Texels = { 0, 0, 0, 0 };
			paint.Layer = 1;
			paint.Strength = 0.1f;
			ApplyTerrainPaint(splat, Extent, paint);
			CHECK(splat.Texels == std::vector<uint8_t>{ 0, 255, 0, 0 });
		}

		TEST_CASE("TerrainEditing: a stroke through terrain.edit writes the bytes the editor's tool writes, and undo restores them")
		{
			Test::AutomationFixture setup("TerrainEditRoundTrip");
			EditorContext& editor = setup.GetEditor();
			const UUID terrain = CreateTerrain(setup);
			const std::optional<Buffer> original = ReadProjectBytes(editor, GroundPath);
			REQUIRE(original.has_value());
			EditorActions actions(editor, setup.GetClient().GetServer());

			TerrainStroke sculpt;
			sculpt.Tool = TerrainTool::Raise;
			sculpt.Points = { glm::vec2(8.0f, 8.0f), glm::vec2(9.0f, 8.5f), glm::vec2(10.0f, 9.0f) };
			sculpt.Radius = 3.0f;
			sculpt.Strength = 0.75f;

			// EditTerrain, the one implementation, with the stroke's local points: one "Sculpt Terrain" step.
			const size_t history = editor.GetHistory().GetUndoCount();
			const Result<TerrainEditResult> direct = EditTerrain(editor, editor.GetActiveSceneId(), terrain, sculpt);
			REQUIRE_MESSAGE(direct.has_value(), direct.error().ToString());
			CHECK(direct->Path == GroundPath);
			CHECK_FALSE(direct->CreatedSplatMap);
			CHECK(direct->Changed > 0);
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);
			CHECK(editor.GetHistory().GetEntries(1).front().Label == "Sculpt Terrain");
			const std::optional<Buffer> sculpted = ReadProjectBytes(editor, GroundPath);
			REQUIRE(sculpted.has_value());
			CHECK(*sculpted != *original);
			// The reimport published the new samples at once.
			EditorAssetManager& assets = editor.GetAssets();
			const AssetHandle ground = GetTerrain(editor, terrain).Heightfield.GetHandle();
			const Result<AssetRef<Asset>> loaded = assets.Load(ground);
			REQUIRE(loaded.has_value());
			const Result<HeightfieldData> expected = ReadHeightfieldSource(*sculpted);
			REQUIRE(expected.has_value());
			CHECK(AssetCast<HeightfieldData>(*loaded)->Heights == expected->Heights);
			CallOrFail(setup, "edit.undo", Json::object());
			CHECK(ReadProjectBytes(editor, GroundPath) == original);

			// The editor's terrain brush queues terrain.edit with the world points it picked (the user's origin), and an agent
			// calls it: both write EditTerrain's bytes, each one step that one undo takes back.
			const Json params = MakeEditParams(terrain, sculpt, "Raise");
			const Result<Json> fromEditor = CompleteAction(actions, "terrain.edit", params);
			REQUIRE_MESSAGE(fromEditor.has_value(), fromEditor.error().ToString());
			CHECK((*fromEditor)["changed"] == Json(direct->Changed));
			CHECK(ReadProjectBytes(editor, GroundPath) == sculpted);
			CHECK(editor.GetHistory().GetEntries(1).front().Origin == CommandOrigin::User);
			CallOrFail(setup, "edit.undo", Json::object());
			CHECK(ReadProjectBytes(editor, GroundPath) == original);
			const Json fromAgent = CallOrFail(setup, "terrain.edit", params);
			CHECK(fromAgent["path"] == Json(std::string(GroundPath)));
			CHECK(fromAgent["entity"]["id"] == Json(terrain.ToString()));
			CHECK(ReadProjectBytes(editor, GroundPath) == sculpted);
			CHECK(editor.GetHistory().GetEntries(1).front().Origin == CommandOrigin::Agent);
			CallOrFail(setup, "edit.undo", Json::object());
			CHECK(ReadProjectBytes(editor, GroundPath) == original);

			// A first paint makes the splat map beside the heightfield, named after the entity, and assigns it in the same step;
			// the editor's stroke and the agent's write the same bytes, and undo removes the map and clears the assignment.
			TerrainStroke paint;
			paint.Tool = TerrainTool::Paint;
			paint.Points = { glm::vec2(16.0f, 16.0f), glm::vec2(18.0f, 16.0f) };
			paint.Radius = 6.0f;
			paint.Strength = 0.5f;
			paint.Layer = 1;
			const std::string splatPath = std::format("Assets/Terrain/Ground.{}.splat.png", terrain.ToString());
			const Result<TerrainEditResult> painted = EditTerrain(editor, editor.GetActiveSceneId(), terrain, paint);
			REQUIRE_MESSAGE(painted.has_value(), painted.error().ToString());
			CHECK(painted->Path == splatPath);
			CHECK(painted->CreatedSplatMap);
			CHECK(editor.GetHistory().GetEntries(1).front().Label == "Paint Terrain");
			const std::optional<Buffer> splat = ReadProjectBytes(editor, splatPath);
			REQUIRE(splat.has_value());
			const Result<Image> image = DecodePng(*splat);
			REQUIRE(image.has_value());
			CHECK(image->Width == 128);
			CHECK(image->Height == 128);
			CHECK(GetTerrain(editor, terrain).SplatMap.GetHandle() == assets.Resolve(splatPath));
			CallOrFail(setup, "edit.undo", Json::object());
			CHECK_FALSE(ReadProjectBytes(editor, splatPath).has_value());
			CHECK_FALSE(ReadProjectBytes(editor, splatPath + ".meta").has_value());
			CHECK_FALSE(GetTerrain(editor, terrain).SplatMap.GetHandle().IsValid());
			const Result<Json> paintedInEditor = CompleteAction(actions, "terrain.edit", MakeEditParams(terrain, paint, "Paint"));
			REQUIRE_MESSAGE(paintedInEditor.has_value(), paintedInEditor.error().ToString());
			CHECK((*paintedInEditor)["createdSplatMap"] == Json(true));
			CHECK(ReadProjectBytes(editor, splatPath) == splat);
			CallOrFail(setup, "edit.undo", Json::object());
			const Json paintedByAgent = CallOrFail(setup, "terrain.edit", MakeEditParams(terrain, paint, "Paint"));
			CHECK(paintedByAgent["path"] == Json(splatPath));
			CHECK(ReadProjectBytes(editor, splatPath) == splat);

			// A second paint rewrites the existing map in place, one step, which undo restores to the first paint's bytes.
			const Json again = CallOrFail(setup, "terrain.edit", MakeEditParams(terrain, paint, "Paint"));
			CHECK(again["createdSplatMap"] == Json(false));
			CHECK(JsonReader(again["changed"]).ReadUInt32().value_or(0) > 0);
			CHECK(ReadProjectBytes(editor, splatPath) != splat);
			CallOrFail(setup, "edit.undo", Json::object());
			CHECK(ReadProjectBytes(editor, splatPath) == splat);
		}

		TEST_CASE("TerrainEditing: a first paint never overwrites a file at its splat map's name, and two terrains on one heightfield paint maps of their own")
		{
			Test::AutomationFixture setup("TerrainEditSplatNames");
			EditorContext& editor = setup.GetEditor();
			const UUID first = CreateTerrain(setup, "First");
			const UUID second = CreateTerrain(setup, "Second");
			// A file already at the first terrain's name (a scene copied with its ids painted it): the paint takes "-2".
			const std::string taken = std::format("Assets/Terrain/Ground.{}.splat.png", first.ToString());
			const Result<Image> pixel = CreateImage(1, 1, nvrhi::Format::RGBA8_UNORM);
			REQUIRE(pixel.has_value());
			const Result<Buffer> placeholder = EncodePng(*pixel);
			REQUIRE(placeholder.has_value());
			REQUIRE(editor.WriteProjectFile(VfsPath::Create("project", taken).value(), *placeholder).has_value());
			TerrainStroke paint;
			paint.Tool = TerrainTool::Paint;
			paint.Points = { glm::vec2(4.0f, 4.0f) };
			paint.Radius = 3.0f;
			paint.Layer = 2;
			const Result<TerrainEditResult> firstPainted = EditTerrain(editor, editor.GetActiveSceneId(), first, paint);
			REQUIRE_MESSAGE(firstPainted.has_value(), firstPainted.error().ToString());
			CHECK(firstPainted->Path == std::format("Assets/Terrain/Ground.{}-2.splat.png", first.ToString()));
			CHECK(ReadProjectBytes(editor, taken) == *placeholder);
			CHECK(firstPainted->Changed > 0);
			const Result<TerrainEditResult> secondPainted = EditTerrain(editor, editor.GetActiveSceneId(), second, paint);
			REQUIRE_MESSAGE(secondPainted.has_value(), secondPainted.error().ToString());
			CHECK(secondPainted->Path == std::format("Assets/Terrain/Ground.{}.splat.png", second.ToString()));
			CHECK(GetTerrain(editor, first).SplatMap.GetHandle() != GetTerrain(editor, second).SplatMap.GetHandle());
			// The new maps are data textures: Usage Linear and Compression None.
			const Json settings = CallOrFail(setup, "asset.getImportSettings", Json{ { "asset", secondPainted->Path } })["settings"];
			CHECK(settings["Usage"] == Json("Linear"));
			CHECK(settings["Compression"] == Json("None"));
		}
	}

}
