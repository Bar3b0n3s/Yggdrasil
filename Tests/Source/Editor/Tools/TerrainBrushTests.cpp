#include "TestsPCH.h"
#include "Editor/Tools/TerrainBrush.h"

#include "Editor/PanelInteractionFixture.h"
#include "Editor/Viewport/EditorViewportHost.h"
#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/EditorContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The scene view's terrain brush (M26, C-TERR-EDIT; Docs/Decisions/0037-m26-contract.md decision 23): the surface under the
// cursor, and a stroke made in the view becoming the one terrain.edit of the points it picked.

namespace Engine {

	namespace {

		// A 9 x 9 ramp rising along local +X: sample (x, z) = x * 8000.
		HeightfieldData MakeRamp()
		{
			HeightfieldData ramp = MakeFlatHeightfield(9);
			for (uint32_t z = 0; z < 9; ++z)
			{
				for (uint32_t x = 0; x < 9; ++x)
					ramp.Heights[x + 9 * z] = static_cast<uint16_t>(x * 8000);
			}
			return ramp;
		}

		// The real brush drawn into one window, over a displayed image of the edit scene seen from above at an angle.
		class BrushHarness
		{
		public:
			explicit BrushHarness(std::string_view label)
				: m_Fixture(label)
			{
				Rectangle = EditorViewportRect{ .Min = glm::vec2(100.0f, 400.0f), .Size = glm::vec2(640.0f, 360.0f), .ImageSize = glm::uvec2(640, 360), .Focused = true, .Hovered = true };
				Image.Texture = static_cast<ImTextureID>(7);
				Image.Width = 640;
				Image.Height = 360;
			}

			// The image's camera, as the host copies it from the snapshot it renders.
			void RefreshCamera()
			{
				const ExplicitRenderCamera camera{ .Position = glm::vec3(32.0f, 40.0f, 90.0f), .Target = glm::vec3(32.0f, 0.0f, 32.0f) };
				const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(GetEditor().GetScene(),
					{ .Camera = RenderCameraSource::Explicit, .ExplicitCamera = camera, .Width = Image.Width, .Height = Image.Height });
				REQUIRE(snapshot.has_value());
				Image.Camera = snapshot->Camera;
			}

			[[nodiscard]] Status Frame(bool leftPressed = false, const std::function<void()>& before = {})
			{
				return m_Ui.Frame([this, leftPressed, &before]() -> Status
				{
					if (before)
						before();
					Brush.DrawControls(m_Fixture.GetContext(), 1000.0f);
					Last = Brush.Update(Rectangle, Image, { .ImageShown = true, .Captured = false, .LeftPressed = leftPressed });
					return {};
				});
			}

			void FrameOk(bool leftPressed = false, const std::function<void()>& before = {})
			{
				const Status status = Frame(leftPressed, before);
				REQUIRE_MESSAGE(status.has_value(), (status ? std::string() : status.error().ToString()));
			}

			// Picks the tool picker's entry `steps` below "Off" with the keyboard: Space opens it, Down moves, Space takes it.
			void PickTool(int steps)
			{
				FrameOk(false, []()
				{
					ImGuiWindow* window = ImGui::GetCurrentWindow();
					ImGui::SetWindowFocus();
					ImGui::SetNavID(window->GetID("##TerrainBrushTool"), ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
					ImGui::SetNavCursorVisible(true);
				});
				std::vector<ImGuiKey> keys{ ImGuiKey_Space };
				for (int step = 0; step < steps; ++step)
					keys.push_back(ImGuiKey_DownArrow);
				keys.push_back(ImGuiKey_Space);
				for (const ImGuiKey key : keys)
				{
					ImGui::GetIO().AddKeyEvent(key, true);
					FrameOk();
					ImGui::GetIO().AddKeyEvent(key, false);
					FrameOk();
				}
			}

			void MoveTo(glm::vec2 position)
			{
				ImGui::GetIO().AddMousePosEvent(position.x, position.y);
				FrameOk();
			}

			// The surface point the brush must find under the screen point `position`.
			[[nodiscard]] glm::vec3 SurfaceAt(glm::vec2 position, UUID entity)
			{
				const std::optional<EditorViewportPixel> pixel = ToViewportPixel(Rectangle, position);
				REQUIRE(pixel.has_value());
				const RenderOutputMapping mapping = ComputeRenderOutputMapping(Image.Width, Image.Height, RenderOutputSettings{});
				const auto ray = ComputeOutputPixelRayInterval(Image.Camera, mapping, pixel->X, pixel->Y);
				REQUIRE(ray.has_value());
				REQUIRE(ray->has_value());
				const ConstEntity found = GetEditor().GetScene().FindEntityByID(entity);
				const TerrainComponent& terrain = found.GetComponent<TerrainComponent>();
				const Result<AssetRef<Asset>> loaded = GetEditor().GetAssets().Load(terrain.Heightfield.GetHandle());
				REQUIRE(loaded.has_value());
				const std::optional<glm::vec3> point = IntersectTerrainSurface((*ray)->Ray, TransformSystem::ComputeWorldMatrix(found), *AssetCast<HeightfieldData>(*loaded),
					{ .Size = terrain.Size, .HeightScale = terrain.HeightScale });
				REQUIRE(point.has_value());
				return *point;
			}

			[[nodiscard]] EditorContext& GetEditor() { return m_Fixture.GetContext().Editor; }
			[[nodiscard]] EditorPanelContext& GetContext() { return m_Fixture.GetContext(); }
			[[nodiscard]] Test::AutomationFixture& GetAutomation() { return m_Fixture.GetAutomation(); }

			TerrainBrush Brush{};
			EditorViewportImage Image{};
			EditorViewportRect Rectangle{};
			Result<std::optional<Json>> Last = std::optional<Json>();
		private:
			Test::PanelInteractionFixture m_Fixture;
			Test::PanelInteractionUi m_Ui{ false };
		};

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("TerrainBrush: a ray meets the terrain's surface where its triangles are, in world space, and misses past its edges")
		{
			const HeightfieldData ramp = MakeRamp();
			const TerrainExtent extent{ .Size = glm::vec2(8.0f), .HeightScale = 8.0f };
			const glm::mat4 world = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 1.0f, 5.0f)), glm::vec3(2.0f, 1.0f, 2.0f));
			const auto surface = [&ramp, &extent](float x, float z)
			{
				const std::optional<float> height = SampleTerrainHeight(ramp, extent, glm::vec2(x, z));
				REQUIRE(height.has_value());
				return glm::vec3(5.0f + 2.0f * x, 1.0f + *height, 5.0f + 2.0f * z);
			};

			// Straight down onto local (3.5, 2): the triangle's height there, in world space.
			const std::optional<glm::vec3> below = IntersectTerrainSurface({ .Origin = glm::vec3(12.0f, 100.0f, 9.0f), .Direction = glm::vec3(0.0f, -1.0f, 0.0f) }, world, ramp, extent);
			REQUIRE(below.has_value());
			CHECK(glm::length(*below - surface(3.5f, 2.0f)) < 1e-3f);
			// At an angle from the low side, towards local (4, 4): the first crossing is that point.
			const glm::vec3 target = surface(4.0f, 4.0f);
			const glm::vec3 origin(-20.0f, 50.0f, 13.0f);
			const std::optional<glm::vec3> oblique = IntersectTerrainSurface({ .Origin = origin, .Direction = glm::normalize(target - origin) }, world, ramp, extent);
			REQUIRE(oblique.has_value());
			CHECK(glm::length(*oblique - target) < 1e-2f);
			// A ray past the terrain's edge, one pointing away and one starting below the surface meet nothing.
			CHECK_FALSE(IntersectTerrainSurface({ .Origin = glm::vec3(40.0f, 100.0f, 9.0f), .Direction = glm::vec3(0.0f, -1.0f, 0.0f) }, world, ramp, extent).has_value());
			CHECK_FALSE(IntersectTerrainSurface({ .Origin = glm::vec3(12.0f, 100.0f, 9.0f), .Direction = glm::vec3(0.0f, 1.0f, 0.0f) }, world, ramp, extent).has_value());
			CHECK_FALSE(IntersectTerrainSurface({ .Origin = glm::vec3(12.0f, 0.5f, 9.0f), .Direction = glm::vec3(0.0f, -1.0f, 0.0f) }, world, ramp, extent).has_value());
			// A matrix without an inverse places no surface.
			CHECK_FALSE(IntersectTerrainSurface({ .Origin = glm::vec3(12.0f, 100.0f, 9.0f), .Direction = glm::vec3(0.0f, -1.0f, 0.0f) }, glm::mat4(0.0f), ramp, extent).has_value());
		}

		TEST_CASE("TerrainBrush: a left stroke in the scene view is one terrain.edit of the points it picked on the surface, one undo step")
		{
			BrushHarness test("TerrainBrushStroke");
			EditorContext& editor = test.GetEditor();
			Test::AutomationFixture& automation = test.GetAutomation();
			REQUIRE(automation.Call("asset.create", Json{ { "type", "Heightfield" }, { "path", "Assets/Terrain/Field.heightfield" } }).has_value());
			const auto created = automation.Call("entity.create",
				Json{ { "name", "Field" }, { "components", Json{ { "Terrain", Json{ { "Heightfield", "Assets/Terrain/Field.heightfield" } } } } } });
			REQUIRE_MESSAGE(created.has_value(), (created ? std::string() : created.error().ToString()));
			const auto field = JsonReader((*created)["entity"]["id"]).ReadUUID();
			REQUIRE(field.has_value());
			test.RefreshCamera();

			// Without a selected terrain there are no controls, and the brush takes nothing.
			test.FrameOk();
			CHECK_FALSE(test.Brush.IsArmed());
			REQUIRE(editor.SetSelection({ *field }, SceneTarget::Edit).has_value());
			test.FrameOk();
			CHECK_FALSE(test.Brush.IsArmed());
			// The picker's first tool, Raise, arms it.
			test.PickTool(1);
			REQUIRE(test.Brush.IsArmed());

			const glm::vec2 start = test.Rectangle.Min + glm::vec2(320.0f, 180.0f);
			const glm::vec2 end = start + glm::vec2(120.0f, 20.0f);
			const std::vector<glm::vec3> expected{ test.SurfaceAt(start, *field), test.SurfaceAt(end, *field) };
			test.MoveTo(start);
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk(true);
			REQUIRE(test.Last.has_value());
			CHECK_FALSE(test.Last->has_value());
			CHECK(test.Brush.IsStroking());
			test.MoveTo(end);
			REQUIRE(test.Last.has_value());
			CHECK_FALSE(test.Last->has_value());
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			REQUIRE(test.Last.has_value());
			REQUIRE(test.Last->has_value());
			CHECK_FALSE(test.Brush.IsStroking());
			const Json params = **test.Last;
			CHECK(params == MakeTerrainEditParams(*field, TerrainBrushSettings{}, expected));

			// Queued as the scene view queues it, the stroke is one terrain.edit, one undo step of the user's.
			const VfsPath source = VfsPath::Create("project", "Assets/Terrain/Field.heightfield").value();
			const Result<Buffer> before = editor.GetVfs().ReadFile(source);
			REQUIRE(before.has_value());
			const size_t history = editor.GetHistory().GetUndoCount();
			const Result<uint64_t> ticket = test.GetContext().Actions.Submit("terrain.edit", params);
			REQUIRE(ticket.has_value());
			test.GetContext().Actions.Pump();
			const auto completion = test.GetContext().Actions.TakeResult(*ticket);
			REQUIRE(completion.has_value());
			REQUIRE(completion->has_value());
			REQUIRE_MESSAGE((*completion)->has_value(), (**completion).error().ToString());
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);
			CHECK(editor.GetHistory().GetEntries(1).front().Origin == CommandOrigin::User);
			CHECK(editor.GetVfs().ReadFile(source).value_or(Buffer()) != *before);

			// A selection change during a stroke cancels it with a Conflict, and nothing is queued.
			test.MoveTo(start);
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			test.FrameOk(true);
			REQUIRE(test.Brush.IsStroking());
			REQUIRE(editor.SetSelection({}, SceneTarget::Edit).has_value());
			test.FrameOk();
			REQUIRE_FALSE(test.Last.has_value());
			CHECK(test.Last.error().GetCode() == ErrorCode::Conflict);
			CHECK_FALSE(test.Brush.IsStroking());
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);
		}

		// The class comment's cancellations and limit (W26.2's review): play starting, the terrain's heightfield changing or
		// the scene changing during a stroke cancels it with a Conflict, queuing nothing, and a stroke that crosses
		// MaxTerrainBrushPoints points is refused there with InvalidArgument.
		TEST_CASE("TerrainBrush: play starting, a terrain or scene change cancel a stroke, and a stroke past its point limit is refused")
		{
			BrushHarness test("TerrainBrushCancel");
			EditorContext& editor = test.GetEditor();
			Test::AutomationFixture& automation = test.GetAutomation();
			REQUIRE(automation.Call("asset.create", Json{ { "type", "Heightfield" }, { "path", "Assets/Terrain/Field.heightfield" } }).has_value());
			REQUIRE(automation.Call("asset.create", Json{ { "type", "Heightfield" }, { "path", "Assets/Terrain/Other.heightfield" } }).has_value());
			const auto created = automation.Call("entity.create",
				Json{ { "name", "Field" }, { "components", Json{ { "Terrain", Json{ { "Heightfield", "Assets/Terrain/Field.heightfield" } } } } } });
			REQUIRE_MESSAGE(created.has_value(), (created ? std::string() : created.error().ToString()));
			const auto field = JsonReader((*created)["entity"]["id"]).ReadUUID();
			REQUIRE(field.has_value());
			test.RefreshCamera();
			const size_t history = editor.GetHistory().GetUndoCount();
			const glm::vec2 start = test.Rectangle.Min + glm::vec2(320.0f, 180.0f);
			const glm::vec2 end = start + glm::vec2(120.0f, 20.0f);
			// Selects the terrain and arms Raise (a new target turns the tool off), then presses over the surface.
			const auto beginStroke = [&test, &editor, &field, start]()
			{
				REQUIRE(editor.SetSelection({ *field }, SceneTarget::Edit).has_value());
				test.FrameOk();
				if (!test.Brush.IsArmed())
					test.PickTool(1);
				REQUIRE(test.Brush.IsArmed());
				test.MoveTo(start);
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
				test.FrameOk(true);
				REQUIRE(test.Last.has_value());
				REQUIRE(test.Brush.IsStroking());
			};
			// The frame after `change` cancels the stroke with a Conflict; the release then queues nothing.
			const auto expectCancelled = [&test](const std::function<void()>& change)
			{
				change();
				test.FrameOk();
				REQUIRE_FALSE(test.Last.has_value());
				CHECK(test.Last.error().GetCode() == ErrorCode::Conflict);
				CHECK_FALSE(test.Brush.IsStroking());
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
				test.FrameOk();
				REQUIRE(test.Last.has_value());
				CHECK_FALSE(test.Last->has_value());
			};

			beginStroke();
			expectCancelled([&automation]()
			{
				REQUIRE(automation.Call("play.start", Json{ { "lockstep", true }, { "mode", "simulate" } }).has_value());
			});
			REQUIRE(automation.Call("play.stop", Json::object()).has_value());
			CHECK(editor.GetHistory().GetUndoCount() == history);

			beginStroke();
			expectCancelled([&automation, &field]()
			{
				REQUIRE(automation.Call("entity.update", Json{ { "entity", field->ToString() }, { "components", Json{ { "Terrain", Json{ { "Heightfield", "Assets/Terrain/Other.heightfield" } } } } } }).has_value());
			});
			// Only the assignment's own step.
			CHECK(editor.GetHistory().GetUndoCount() == history + 1);
			REQUIRE(automation.Call("edit.undo", Json::object()).has_value());

			// A stroke whose cursor moves farther than a quarter of the radius every frame adds a point each frame: the
			// press's, then one a frame, until the one past MaxTerrainBrushPoints is refused, leaving no stroke.
			beginStroke();
			size_t frames = 0;
			while (test.Last.has_value() && frames < MaxTerrainBrushPoints + 1)
			{
				test.MoveTo(frames % 2 == 0 ? end : start);
				++frames;
			}
			REQUIRE_FALSE(test.Last.has_value());
			CHECK(test.Last.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(frames == MaxTerrainBrushPoints);
			CHECK_FALSE(test.Brush.IsStroking());
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			test.FrameOk();
			REQUIRE(test.Last.has_value());
			CHECK_FALSE(test.Last->has_value());
			CHECK(editor.GetHistory().GetUndoCount() == history);

			// The scene replaced: the new scene's history holds nothing of the stroke.
			beginStroke();
			expectCancelled([&automation]()
			{
				REQUIRE(automation.Call("scene.new", Json{ { "path", "Assets/Scenes/Other.scene" }, { "discardChanges", true } }).has_value());
			});
			CHECK(editor.GetHistory().GetUndoCount() == 0);
		}
	}

}
