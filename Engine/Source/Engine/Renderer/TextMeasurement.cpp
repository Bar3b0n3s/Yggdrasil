#include "EnginePCH.h"
#include "Engine/Renderer/TextMeasurement.h"

#include "Engine/Asset/FontData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Utf8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

// MeasureText decodes the text once into the codepoints the chain draws and its line feeds, then fills each paragraph (the
// text between two line feeds) with lines. A line is laid out from pen x = 0, its glyphs pushed as it goes; when Word
// wrapping ends it early, the glyphs past its end are dropped and laid out again from the next line's start, so every
// glyph's position comes from one walk of its own line. Once the block's width is known, each line is shifted by its
// alignment. Pen positions and widths are in pixels: advances and kerning times Size, added in text order. Only
// additions, subtractions and multiplications of the fonts' floats and the options' values (and the alignment's 0.5) are
// used, so identical input gives identical output in every configuration.
//
// The wrapping rules beside TextMeasurement.h's:
//   - A break opportunity is a space (U+0020) that a font of the chain draws, preceded on its line by a codepoint that is
//     not a space; a line therefore never wraps before its first other codepoint, and a wrap never leaves a line empty.
//   - A codepoint other than a space whose advance would end beyond MaxWidth (its pen, plus its kerning, plus its advance)
//     ends the line before it once the line holds another codepoint that is not a space: at the line's last break
//     opportunity whose line, ended there, fits MaxWidth, else at its first break opportunity, else (a word wider than
//     MaxWidth) between the two glyphs. Spaces never end a line themselves, so trailing spaces may reach beyond MaxWidth.
//   - The line ends before the break's space and the next one starts after it: that one space belongs to neither line
//     (TextMeasuredLine's byte ranges), and further spaces of a run start the next line.
//   - Lines of one paragraph partition its bytes (except the break spaces): skipped codepoints, carriage returns included,
//     belong to the line around them. Kerning pairs two consecutive drawn codepoints of one font on one line; skipped
//     codepoints keep it, as TextLayout's do.
//   - Each byte that starts no well-formed UTF-8 sequence (Core/Utf8.h's GetUtf8SequenceLength) is one skipped codepoint.
//   - Byte offsets are 32-bit (TextMeasuredGlyph::ByteOffset), so a text is measured up to its first 2^32 - 1 bytes.
// TextDirection is read by W22.3's bidirectional algorithm; in W22.1 every block lays out left to right.

namespace Engine {

	namespace {

		// A codepoint of the text that a font of the chain draws, or a line feed (Glyph null).
		struct TextCodepoint
		{
			const FontGlyph* Glyph = nullptr;
			uint32_t Codepoint = 0;
			uint32_t Font = 0;
			uint32_t ByteOffset = 0;
		};

		// What the walk of one line needs besides the codepoints.
		struct LineLayout
		{
			std::span<const FontData* const> Fonts{};
			float Size = 0.0f;
			float Baseline = 0.0f; // the line's baseline, pixels from the block's top
			bool Wrap = false;
			float MaxWidth = 0.0f;
		};

		// A break opportunity of the line being walked: its space, the line's width before it, and the glyphs before it.
		struct LineBreak
		{
			size_t Space = 0;
			float Width = 0.0f;
			size_t GlyphCount = 0;
		};

		// Where a line of a paragraph ends: the first codepoint it leaves out, where the next line starts, its width, how many
		// glyphs of the measurement it ends after, and its byte range's end and the next line's start.
		struct LineEnd
		{
			size_t End = 0;
			size_t Resume = 0;
			float Width = 0.0f;
			size_t GlyphCount = 0;
			uint32_t ByteEnd = 0;
			uint32_t NextByteBegin = 0;
		};

	}

	namespace Utils {

		static constexpr uint32_t SpaceCodepoint = 0x20;
		static constexpr uint32_t LineFeedCodepoint = 0x0A;
		static constexpr uint32_t CarriageReturnCodepoint = 0x0D;
		static constexpr size_t MaxMeasuredBytes = std::numeric_limits<uint32_t>::max();

		// The codepoint of the well-formed sequence of `length` bytes (GetUtf8SequenceLength) at `text[offset]`.
		static uint32_t DecodeUtf8Sequence(std::string_view text, size_t offset, size_t length)
		{
			static constexpr std::array<uint32_t, 5> LeadMasks = { 0x00, 0x7F, 0x1F, 0x0F, 0x07 };
			uint32_t codepoint = static_cast<unsigned char>(text[offset]) & LeadMasks[length];
			for (size_t index = 1; index < length; ++index)
				codepoint = (codepoint << 6U) | (static_cast<unsigned char>(text[offset + index]) & 0x3FU);
			return codepoint;
		}

		// The kerning added to `first`'s advance when `second` follows it in `font` (0 when the font has no such pair).
		static float FindKerning(const FontData& font, uint32_t first, uint32_t second)
		{
			const auto found = std::ranges::lower_bound(font.Kerning, std::pair(first, second), {}, [](const FontKerningPair& pair)
			{
				return std::pair(pair.First, pair.Second);
			});
			return found != font.Kerning.end() && found->First == first && found->Second == second ? found->Advance : 0.0f;
		}

		// The codepoints of `text` the chain draws, each with the first font that has its glyph, and the line feeds; counts
		// the skipped ones (a codepoint no font has, an ill-formed byte) in `skipped`. Carriage returns are ignored.
		static std::vector<TextCodepoint> DecodeText(std::span<const FontData* const> fonts, std::string_view text, uint32_t& skipped)
		{
			std::vector<TextCodepoint> codepoints;
			codepoints.reserve(text.size());
			size_t offset = 0;
			while (offset < text.size())
			{
				const size_t length = GetUtf8SequenceLength(text, offset);
				const uint32_t byteOffset = static_cast<uint32_t>(offset);
				if (length == 0)
				{
					++skipped;
					++offset;
					continue;
				}
				const uint32_t codepoint = DecodeUtf8Sequence(text, offset, length);
				offset += length;
				if (codepoint == CarriageReturnCodepoint)
					continue;
				if (codepoint == LineFeedCodepoint)
				{
					codepoints.push_back({ .Glyph = nullptr, .Codepoint = codepoint, .Font = 0, .ByteOffset = byteOffset });
					continue;
				}
				std::optional<TextCodepoint> drawn;
				for (size_t font = 0; font < fonts.size() && !drawn.has_value(); ++font)
				{
					if (const FontGlyph* glyph = FindGlyph(*fonts[font], codepoint))
						drawn = TextCodepoint{ .Glyph = glyph, .Codepoint = codepoint, .Font = static_cast<uint32_t>(font), .ByteOffset = byteOffset };
				}
				if (drawn.has_value())
					codepoints.push_back(*drawn);
				else
					++skipped;
			}
			return codepoints;
		}

		// Lays out the line of the paragraph [start, paragraphEnd) of `codepoints` that starts at `start` (the file comment's
		// rules), appending its glyphs at x relative to the line's pen start; `paragraphByteEnd` is the paragraph's last byte
		// plus one. The returned GlyphCount is the size `glyphs` is cut to.
		static LineEnd WalkLine(std::span<const TextCodepoint> codepoints, size_t start, size_t paragraphEnd, uint32_t paragraphByteEnd,
			const LineLayout& layout, std::vector<TextMeasuredGlyph>& glyphs)
		{
			float pen = 0.0f;
			const TextCodepoint* previous = nullptr;
			bool hasContent = false;
			std::optional<LineBreak> lastBreak;
			for (size_t index = start; index < paragraphEnd; ++index)
			{
				const TextCodepoint& current = codepoints[index];
				const FontGlyph& glyph = *current.Glyph;
				const float kerning = previous != nullptr && previous->Font == current.Font
					? FindKerning(*layout.Fonts[current.Font], previous->Codepoint, current.Codepoint) * layout.Size
					: 0.0f;
				const float advance = glyph.Advance * layout.Size;
				const bool isSpace = current.Codepoint == SpaceCodepoint;
				if (layout.Wrap && hasContent)
				{
					if (isSpace)
					{
						if (pen <= layout.MaxWidth || !lastBreak.has_value())
							lastBreak = LineBreak{ .Space = index, .Width = pen, .GlyphCount = glyphs.size() };
					}
					else if (pen + kerning + advance > layout.MaxWidth)
					{
						if (lastBreak.has_value())
						{
							const uint32_t spaceOffset = codepoints[lastBreak->Space].ByteOffset;
							return { .End = lastBreak->Space, .Resume = lastBreak->Space + 1, .Width = lastBreak->Width, .GlyphCount = lastBreak->GlyphCount, .ByteEnd = spaceOffset, .NextByteBegin = spaceOffset + 1 };
						}
						return { .End = index, .Resume = index, .Width = pen, .GlyphCount = glyphs.size(), .ByteEnd = current.ByteOffset, .NextByteBegin = current.ByteOffset };
					}
				}
				pen += kerning;
				if (glyph.PlaneMin != glyph.PlaneMax)
				{
					// Plane coordinates are em units, y up from the baseline; the block's are pixels, y down from its top.
					glyphs.push_back({
						.Min = glm::vec2(pen + glyph.PlaneMin.x * layout.Size, layout.Baseline - glyph.PlaneMax.y * layout.Size),
						.Max = glm::vec2(pen + glyph.PlaneMax.x * layout.Size, layout.Baseline - glyph.PlaneMin.y * layout.Size),
						.AtlasMin = glyph.AtlasMin,
						.AtlasMax = glyph.AtlasMax,
						.Font = current.Font,
						.ByteOffset = current.ByteOffset,
					});
				}
				pen += advance;
				previous = &current;
				hasContent = hasContent || !isSpace;
			}
			return { .End = paragraphEnd, .Resume = paragraphEnd, .Width = pen, .GlyphCount = glyphs.size(), .ByteEnd = paragraphByteEnd, .NextByteBegin = paragraphByteEnd };
		}

		// The x of a line's pen start in a block `blockWidth` wide.
		static float AlignLine(RenderTextAlignment alignment, float blockWidth, float lineWidth)
		{
			switch (alignment)
			{
				case RenderTextAlignment::Left:   return 0.0f;
				case RenderTextAlignment::Center: return (blockWidth - lineWidth) * 0.5f;
				case RenderTextAlignment::Right:  return blockWidth - lineWidth;
			}
			ENGINE_CORE_ASSERT(false, "Unknown RenderTextAlignment {}", std::to_underlying(alignment));
			return 0.0f;
		}

	}

	TextMeasurement MeasureText(std::span<const FontData* const> fonts, std::string_view text, const TextMeasureOptions& options)
	{
		ENGINE_CORE_ASSERT(!fonts.empty() && std::ranges::none_of(fonts, [](const FontData* font)
		{
			return font == nullptr;
		}),
			"MeasureText needs a font chain of at least one font and no null font");
		ENGINE_CORE_ASSERT(std::isfinite(options.Size) && options.Size > 0.0f, "MeasureText needs a finite Size > 0, got {}", options.Size);
		ENGINE_CORE_ASSERT(std::isfinite(options.LineSpacing) && options.LineSpacing > 0.0f, "MeasureText needs a finite LineSpacing > 0, got {}",
			options.LineSpacing);

		const bool bounded = std::isfinite(options.MaxWidth) && options.MaxWidth > 0.0f;
		const std::string_view measured = text.substr(0, std::min(text.size(), Utils::MaxMeasuredBytes));
		TextMeasurement result;
		if (measured.empty())
		{
			result.Size.x = bounded ? options.MaxWidth : 0.0f;
			return result;
		}

		const FontData& first = *fonts.front();
		const float pitch = (first.Ascent - first.Descent + first.LineGap) * options.Size * options.LineSpacing;
		const float ascent = first.Ascent * options.Size;
		const std::vector<TextCodepoint> codepoints = Utils::DecodeText(fonts, measured, result.SkippedCodepoints);
		LineLayout layout{
			.Fonts = fonts,
			.Size = options.Size,
			.Baseline = 0.0f,
			.Wrap = options.Wrap == TextWrap::Word && bounded,
			.MaxWidth = options.MaxWidth,
		};

		size_t paragraphStart = 0;
		uint32_t paragraphByteBegin = 0;
		while (true)
		{
			const auto lineFeed = std::ranges::find_if(codepoints.begin() + static_cast<std::ptrdiff_t>(paragraphStart), codepoints.end(), [](const TextCodepoint& codepoint)
			{
				return codepoint.Glyph == nullptr;
			});
			const size_t paragraphEnd = static_cast<size_t>(lineFeed - codepoints.begin());
			const uint32_t paragraphByteEnd = lineFeed != codepoints.end() ? lineFeed->ByteOffset : static_cast<uint32_t>(measured.size());

			size_t start = paragraphStart;
			uint32_t byteBegin = paragraphByteBegin;
			while (true)
			{
				const float top = static_cast<float>(result.Lines.size()) * pitch;
				layout.Baseline = top + ascent;
				const size_t firstGlyph = result.Glyphs.size();
				const LineEnd end = Utils::WalkLine(codepoints, start, paragraphEnd, paragraphByteEnd, layout, result.Glyphs);
				result.Glyphs.resize(end.GlyphCount);
				result.Lines.push_back({
					.FirstGlyph = static_cast<uint32_t>(firstGlyph),
					.GlyphCount = static_cast<uint32_t>(end.GlyphCount - firstGlyph),
					.ByteBegin = byteBegin,
					.ByteEnd = end.ByteEnd,
					.Left = 0.0f,
					.Width = end.Width,
					.Top = top,
					.Baseline = layout.Baseline,
				});
				if (end.End == paragraphEnd)
					break;
				start = end.Resume;
				byteBegin = end.NextByteBegin;
			}

			if (lineFeed == codepoints.end())
				break;
			paragraphStart = paragraphEnd + 1;
			paragraphByteBegin = lineFeed->ByteOffset + 1;
		}

		float blockWidth = 0.0f;
		for (const TextMeasuredLine& line : result.Lines)
			blockWidth = std::max(blockWidth, line.Width);
		if (bounded)
			blockWidth = options.MaxWidth;
		result.Size = glm::vec2(blockWidth, static_cast<float>(result.Lines.size() - 1) * pitch + (first.Ascent - first.Descent) * options.Size);

		for (TextMeasuredLine& line : result.Lines)
		{
			line.Left = Utils::AlignLine(options.Alignment, blockWidth, line.Width);
			for (size_t index = line.FirstGlyph; index < line.FirstGlyph + line.GlyphCount; ++index)
			{
				result.Glyphs[index].Min.x += line.Left;
				result.Glyphs[index].Max.x += line.Left;
			}
		}
		return result;
	}

	glm::vec2 MeasureTextSize(std::span<const FontData* const> fonts, std::string_view text, const TextMeasureOptions& options)
	{
		return MeasureText(fonts, text, options).Size;
	}

	// A stub of the M22 contract (C22.3, Docs/Decisions/0033-m22-contract.md decision 48) that stream S implements.
	TextCaret FindTextCaret(const TextMeasurement& /*measurement*/, uint32_t /*byteOffset*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
