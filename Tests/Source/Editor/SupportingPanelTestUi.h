#pragma once

#include "Editor/Panels/UtilityPanelFixture.h"

#include <imgui.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Engine {

	namespace Test {

		// Supporting-panel tests locate text in public ImGui draw data, then send ordinary mouse input.
		// No copied layout offsets, private widget state, or synthetic command results.
		class SupportingPanelTestUi
		{
		public:
			SupportingPanelTestUi() = delete;
			// `occurrence` picks a later visible match of the same text (0, the first): the topmost window's first, in its
			// drawing order (a window's title before its contents), then the windows below.
			template<typename Panel>
			[[nodiscard]] static std::optional<ImVec2> FindText(UtilityPanelFixture& fixture, Panel& panel, std::string_view text, size_t occurrence = 0);

			template<typename Panel>
			static void ClickText(UtilityPanelFixture& fixture, Panel& panel, std::string_view text, size_t occurrence = 0);

			// Call after drawing the text, inside its active window/frame with the same font and scale,
			// before End()/Render(). Copies glyph values; no live font pointer is retained.
			[[nodiscard]] static std::vector<ImFontGlyph> CaptureGlyphs(std::string_view text);

			// Call after Render() for that same frame and before NewFrame(), using the captured glyphs.
			// Returns the visible text's measured center from draw geometry (its `occurrence`-th visible match, FindText's
			// order), or no match.
			[[nodiscard]] static std::optional<ImVec2> FindRenderedText(std::span<const ImFontGlyph> glyphs, size_t occurrence = 0);
		};

		template<typename Panel>
		std::optional<ImVec2> SupportingPanelTestUi::FindText(UtilityPanelFixture& fixture, Panel& panel, std::string_view text, size_t occurrence)
		{
			struct CapturingPanel
			{
				Panel& Instance;
				std::string_view Text{};
				std::vector<ImFontGlyph> Glyphs{};

				[[nodiscard]] Status Draw(EditorPanelContext& context)
				{
					const Status result = Instance.Draw(context);
					// Copy the active window's glyphs before End()/Render() clears its baked font and scale.
					Glyphs = SupportingPanelTestUi::CaptureGlyphs(Text);
					return result;
				}
			};

			CapturingPanel captured{ panel, text, {} };
			fixture.Draw(captured);
			return FindRenderedText(captured.Glyphs, occurrence);
		}

		template<typename Panel>
		void SupportingPanelTestUi::ClickText(UtilityPanelFixture& fixture, Panel& panel, std::string_view text, size_t occurrence)
		{
			const auto position = FindText(fixture, panel, text, occurrence);
			REQUIRE_MESSAGE(position.has_value(), "Visible click target: ", text);
			fixture.ClickAt(panel, position->x, position->y);
		}

	}

}
