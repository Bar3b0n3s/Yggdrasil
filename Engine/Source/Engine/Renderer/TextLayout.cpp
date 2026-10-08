#include "EnginePCH.h"
#include "Engine/Renderer/TextLayout.h"

#include "Engine/Asset/FontData.h"
#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

// LayoutText places each line at pen x = 0 first, remembering where its quads start and how wide it is, then shifts every
// line by its alignment once the block's width is known. Only additions, subtractions and multiplications by the font's
// floats and the alignment's 0, 0.5 and 1 are used, so identical input gives identical quads everywhere.

namespace Engine {

	namespace Utils {

		// One decoded UTF-8 sequence: its codepoint (nullopt for an invalid sequence) and its length in bytes (at least 1).
		struct Utf8Sequence
		{
			std::optional<uint32_t> Codepoint{};
			size_t Length = 1;
		};

		// Decodes the sequence starting at `text[offset]` by the well-formed byte sequences of the Unicode Standard (§3.9,
		// table 3-7), which exclude overlong forms, surrogates and values past U+10FFFF. An ill-formed sequence is its maximal
		// subpart (§3.9, "U+FFFD Substitution of Maximal Subparts"): the lead byte and the continuation bytes that were valid
		// so far, or the single byte when it cannot start a sequence; decoding resumes after it.
		static Utf8Sequence DecodeUtf8(std::string_view text, size_t offset)
		{
			const auto byteAt = [&text](size_t index)
			{
				return static_cast<uint32_t>(static_cast<unsigned char>(text[index]));
			};
			const uint32_t lead = byteAt(offset);
			if (lead < 0x80)
				return { .Codepoint = lead, .Length = 1 };

			size_t length = 0;
			uint32_t codepoint = 0;
			// The range of the second byte, which depends on the lead; later continuation bytes are 0x80 to 0xBF.
			uint32_t secondMin = 0x80;
			uint32_t secondMax = 0xBF;
			if (lead >= 0xC2 && lead <= 0xDF)
			{
				length = 2;
				codepoint = lead & 0x1FU;
			}
			else if (lead >= 0xE0 && lead <= 0xEF)
			{
				length = 3;
				codepoint = lead & 0x0FU;
				secondMin = lead == 0xE0 ? 0xA0 : 0x80; // no overlong forms
				secondMax = lead == 0xED ? 0x9F : 0xBF; // no surrogates
			}
			else if (lead >= 0xF0 && lead <= 0xF4)
			{
				length = 4;
				codepoint = lead & 0x07U;
				secondMin = lead == 0xF0 ? 0x90 : 0x80; // no overlong forms
				secondMax = lead == 0xF4 ? 0x8F : 0xBF; // nothing past U+10FFFF
			}
			else
			{
				return { .Codepoint = std::nullopt, .Length = 1 };
			}
			for (size_t index = 1; index < length; ++index)
			{
				if (offset + index >= text.size())
					return { .Codepoint = std::nullopt, .Length = index };
				const uint32_t continuation = byteAt(offset + index);
				const uint32_t minimum = index == 1 ? secondMin : 0x80U;
				const uint32_t maximum = index == 1 ? secondMax : 0xBFU;
				if (continuation < minimum || continuation > maximum)
					return { .Codepoint = std::nullopt, .Length = index };
				codepoint = (codepoint << 6U) | (continuation & 0x3FU);
			}
			return { .Codepoint = codepoint, .Length = length };
		}

		// The kerning added to `first`'s advance when `second` follows it (0 when the font has no such pair).
		static float FindKerning(const FontData& font, uint32_t first, uint32_t second)
		{
			const auto found = std::ranges::lower_bound(font.Kerning, std::pair(first, second), {}, [](const FontKerningPair& pair)
			{
				return std::pair(pair.First, pair.Second);
			});
			return found != font.Kerning.end() && found->First == first && found->Second == second ? found->Advance : 0.0f;
		}

		// The quads of one line and its advance width.
		struct LaidOutLine
		{
			size_t FirstQuad = 0;
			size_t QuadCount = 0;
			float Width = 0.0f;
		};

		static float GetAlignmentFactor(RenderTextAlignment alignment)
		{
			switch (alignment)
			{
				case RenderTextAlignment::Left:   return 0.0f;
				case RenderTextAlignment::Center: return 0.5f;
				case RenderTextAlignment::Right:  return 1.0f;
			}
			return 0.0f;
		}

	}

	TextLayoutResult LayoutText(const FontData& font, std::string_view text, RenderTextAlignment alignment)
	{
		TextLayoutResult result;
		if (text.empty())
			return result;

		const float lineHeight = font.Ascent - font.Descent + font.LineGap;
		std::vector<Utils::LaidOutLine> lines(1);
		float pen = 0.0f;
		float baseline = font.Ascent;
		const FontGlyph* previous = nullptr; // the glyph kerning pairs with; skipped codepoints keep it
		size_t offset = 0;
		while (offset < text.size())
		{
			const Utils::Utf8Sequence sequence = Utils::DecodeUtf8(text, offset);
			offset += sequence.Length;
			if (!sequence.Codepoint.has_value())
			{
				++result.SkippedCodepoints;
				continue;
			}
			const uint32_t codepoint = *sequence.Codepoint;
			if (codepoint == '\r')
				continue;
			if (codepoint == '\n')
			{
				lines.back().Width = pen;
				lines.push_back({ .FirstQuad = result.Quads.size(), .QuadCount = 0, .Width = 0.0f });
				pen = 0.0f;
				baseline += lineHeight;
				previous = nullptr;
				continue;
			}
			const FontGlyph* glyph = FindGlyph(font, codepoint);
			if (glyph == nullptr)
			{
				++result.SkippedCodepoints;
				continue;
			}
			if (previous != nullptr)
				pen += Utils::FindKerning(font, previous->Codepoint, glyph->Codepoint);
			if (glyph->PlaneMin != glyph->PlaneMax)
			{
				// Plane coordinates are y up from the baseline; the block's are y down from its top.
				result.Quads.push_back({
					.PositionMin = glm::vec2(pen + glyph->PlaneMin.x, baseline - glyph->PlaneMax.y),
					.PositionMax = glm::vec2(pen + glyph->PlaneMax.x, baseline - glyph->PlaneMin.y),
					.AtlasMin = glyph->AtlasMin,
					.AtlasMax = glyph->AtlasMax,
				});
				++lines.back().QuadCount;
			}
			pen += glyph->Advance;
			previous = glyph;
		}
		lines.back().Width = pen;

		result.LineCount = static_cast<uint32_t>(lines.size());
		float blockWidth = 0.0f;
		for (const Utils::LaidOutLine& line : lines)
			blockWidth = std::max(blockWidth, line.Width);
		result.Size = glm::vec2(blockWidth, static_cast<float>(lines.size()) * lineHeight - font.LineGap);

		const float factor = Utils::GetAlignmentFactor(alignment);
		for (const Utils::LaidOutLine& line : lines)
		{
			const float shift = (blockWidth - line.Width) * factor;
			if (shift == 0.0f)
				continue;
			for (size_t index = line.FirstQuad; index < line.FirstQuad + line.QuadCount; ++index)
			{
				result.Quads[index].PositionMin.x += shift;
				result.Quads[index].PositionMax.x += shift;
			}
		}
		return result;
	}

	glm::vec2 PlaceScreenText(const TextItem& item, const glm::vec2& blockSize, uint32_t viewportWidth, uint32_t viewportHeight)
	{
		ENGINE_CORE_ASSERT(viewportWidth >= 1 && viewportHeight >= 1, "PlaceScreenText needs a viewport of at least 1x1 pixels, got {}x{}",
			viewportWidth, viewportHeight);
		const glm::vec2 viewport(static_cast<float>(viewportWidth), static_cast<float>(viewportHeight));
		const float scale = viewport.y / TextReferenceHeight;
		return item.Anchor * viewport + item.Offset * scale - item.Pivot * blockSize;
	}

}
