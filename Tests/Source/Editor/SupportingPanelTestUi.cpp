#include "TestsPCH.h"
#include "Editor/SupportingPanelTestUi.h"

namespace Engine {

	namespace Test {

		std::vector<ImFontGlyph> SupportingPanelTestUi::CaptureGlyphs(std::string_view text)
		{
			ImFontBaked* font = ImGui::GetFontBaked();
			if (!font)
				return {};
			std::vector<ImFontGlyph> glyphs;
			for (const char character : text)
			{
				const auto codepoint = static_cast<ImWchar>(static_cast<unsigned char>(character));
				if (!font->IsGlyphLoaded(codepoint))
					return {};
				const ImFontGlyph* glyph = font->FindGlyphNoFallback(codepoint);
				if (!glyph)
					return {};
				if (glyph->Visible)
					glyphs.push_back(*glyph);
			}
			return glyphs;
		}

		// Match copied glyphs against actual rendered quads. No current font or window is needed after Render().
		std::optional<ImVec2> SupportingPanelTestUi::FindRenderedText(std::span<const ImFontGlyph> glyphs)
		{
			const ImDrawData* data = ImGui::GetDrawData();
			if (!data || glyphs.empty())
				return std::nullopt;
			for (int listIndex = data->CmdListsCount - 1; listIndex >= 0; --listIndex)
			{
				const ImDrawList& list = *data->CmdLists[listIndex];
				const size_t vertexCount = static_cast<size_t>(list.VtxBuffer.Size);
				for (size_t first = 0; first + glyphs.size() * 4 <= vertexCount; ++first)
				{
					bool match = true;
					for (size_t index = 0; index < glyphs.size(); ++index)
					{
						const ImDrawVert& top = list.VtxBuffer[static_cast<int>(first + index * 4)];
						const ImDrawVert& bottom = list.VtxBuffer[static_cast<int>(first + index * 4 + 2)];
						const ImFontGlyph& glyph = glyphs[index];
						// SmallButton clips glyphs to its tight text bounds, adjusting their UV rectangle.
						// The visible subrectangle must still belong to this exact glyph in the atlas.
						if (top.uv.x < glyph.U0 || top.uv.y < glyph.V0 || bottom.uv.x > glyph.U1 || bottom.uv.y > glyph.V1 || top.uv.x >= bottom.uv.x || top.uv.y >= bottom.uv.y)
						{
							match = false;
							break;
						}
					}
					if (match)
					{
						const ImVec2 top = list.VtxBuffer[static_cast<int>(first)].pos;
						const ImVec2 bottom = list.VtxBuffer[static_cast<int>(first + glyphs.size() * 4 - 2)].pos;
						const ImVec2 center((top.x + bottom.x) * 0.5f, (top.y + bottom.y) * 0.5f);
						if (center.x < 0.0f || center.y < 0.0f || center.x >= data->DisplaySize.x || center.y >= data->DisplaySize.y)
							continue;
						// The log fixture also submits clipped content. Accept only geometry inside its draw command's clip.
						for (const ImDrawCmd& command : list.CmdBuffer)
						{
							if (top.x < command.ClipRect.x || top.y < command.ClipRect.y || bottom.x > command.ClipRect.z || bottom.y > command.ClipRect.w)
								continue;
							for (unsigned int element = 0; element < command.ElemCount; ++element)
							{
								const auto vertex = static_cast<size_t>(list.IdxBuffer[static_cast<int>(command.IdxOffset + element)]) + command.VtxOffset;
								if (vertex == first)
									return center;
							}
						}
					}
				}
			}
			return std::nullopt;
		}

	}

}
