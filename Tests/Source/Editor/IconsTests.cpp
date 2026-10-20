#include "TestsPCH.h"
#include "Editor/Icons.h"

#include "Editor/PanelInteractionFixture.h"

#include <algorithm>
#include <limits>

namespace Engine {

	namespace {

		constexpr EditorIcon AllEditorIcons[] = { EditorIcon::Folder, EditorIcon::Scene, EditorIcon::Prefab, EditorIcon::Mesh, EditorIcon::Material,
			EditorIcon::Texture, EditorIcon::Environment, EditorIcon::Audio, EditorIcon::Script, EditorIcon::Font, EditorIcon::Replay,
			EditorIcon::SpriteSheet };

		struct DrawnIcon
		{
			int Vertices = 0;
			ImVec2 Minimum{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
			ImVec2 Maximum{ std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
		};

		// Draws one glyph into a fresh window's draw list and measures the vertices it added.
		DrawnIcon DrawIcon(Test::PanelInteractionUi& ui, EditorIcon icon, glm::vec2 minimum, float size)
		{
			DrawnIcon drawn;
			REQUIRE(ui.Frame([&drawn, icon, minimum, size]() -> Status
			{
				ImDrawList& list = *ImGui::GetWindowDrawList();
				const int first = list.VtxBuffer.Size;
				DrawEditorIcon(list, icon, minimum, size, IM_COL32(255, 255, 255, 255));
				drawn.Vertices = list.VtxBuffer.Size - first;
				for (int index = first; index < list.VtxBuffer.Size; ++index)
				{
					const ImVec2 position = list.VtxBuffer[index].pos;
					drawn.Minimum = ImVec2(std::min(drawn.Minimum.x, position.x), std::min(drawn.Minimum.y, position.y));
					drawn.Maximum = ImVec2(std::max(drawn.Maximum.x, position.x), std::max(drawn.Maximum.y, position.y));
				}
				return {};
			}));
			return drawn;
		}

	}

	TEST_SUITE("Editor")
	{
		TEST_CASE("Icons: every glyph draws lines inside its square")
		{
			Test::PanelInteractionUi ui;
			for (const EditorIcon icon : AllEditorIcons)
			{
				CAPTURE(static_cast<int>(icon));
				const DrawnIcon drawn = DrawIcon(ui, icon, { 100.0f, 200.0f }, 32.0f);
				CHECK(drawn.Vertices > 0);
				// Anti-aliased 1.5 px lines reach a little past their end points.
				CHECK(drawn.Minimum.x >= 100.0f - 2.0f);
				CHECK(drawn.Minimum.y >= 200.0f - 2.0f);
				CHECK(drawn.Maximum.x <= 132.0f + 2.0f);
				CHECK(drawn.Maximum.y <= 232.0f + 2.0f);
				// The glyph scales with its square.
				const DrawnIcon large = DrawIcon(ui, icon, { 100.0f, 200.0f }, 64.0f);
				CHECK(large.Maximum.x - large.Minimum.x > drawn.Maximum.x - drawn.Minimum.x);
			}
		}

		TEST_CASE("Icons: unusable sizes and positions draw nothing")
		{
			Test::PanelInteractionUi ui;
			CHECK(DrawIcon(ui, EditorIcon::Folder, { 0.0f, 0.0f }, 0.0f).Vertices == 0);
			CHECK(DrawIcon(ui, EditorIcon::Folder, { 0.0f, 0.0f }, -4.0f).Vertices == 0);
			CHECK(DrawIcon(ui, EditorIcon::Folder, { 0.0f, 0.0f }, std::numeric_limits<float>::quiet_NaN()).Vertices == 0);
			CHECK(DrawIcon(ui, EditorIcon::Folder, { std::numeric_limits<float>::infinity(), 0.0f }, 16.0f).Vertices == 0);
		}

		TEST_CASE("Icons: every asset type maps to its glyph")
		{
			CHECK(AssetTypeToEditorIcon(AssetType::None) == EditorIcon::Folder);
			CHECK(AssetTypeToEditorIcon(AssetType::Scene) == EditorIcon::Scene);
			CHECK(AssetTypeToEditorIcon(AssetType::Prefab) == EditorIcon::Prefab);
			CHECK(AssetTypeToEditorIcon(AssetType::Mesh) == EditorIcon::Mesh);
			CHECK(AssetTypeToEditorIcon(AssetType::Material) == EditorIcon::Material);
			CHECK(AssetTypeToEditorIcon(AssetType::Texture) == EditorIcon::Texture);
			CHECK(AssetTypeToEditorIcon(AssetType::Environment) == EditorIcon::Environment);
			CHECK(AssetTypeToEditorIcon(AssetType::AudioClip) == EditorIcon::Audio);
			CHECK(AssetTypeToEditorIcon(AssetType::Script) == EditorIcon::Script);
			CHECK(AssetTypeToEditorIcon(AssetType::Font) == EditorIcon::Font);
			CHECK(AssetTypeToEditorIcon(AssetType::Replay) == EditorIcon::Replay);
			CHECK(AssetTypeToEditorIcon(AssetType::SpriteSheet) == EditorIcon::SpriteSheet);
		}
	}

}
