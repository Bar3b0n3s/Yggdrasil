#include "EnginePCH.h"
#include "Engine/Renderer/TextMeasurement.h"

#include "Engine/Asset/FontData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Renderer/Private/TextShaping.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// MeasureText decodes the text into paragraphs (the text between two line feeds), each a list of the codepoints the chain
// draws, then per paragraph: runs the bidirectional algorithm and the script locator on those codepoints
// (Renderer/Private/TextShaping.h), itemizes them into runs of one embedding level, one script and one font, and turns
// each run into clusters in logical order (one per codepoint without shaping data; HarfBuzz's monotone character clusters
// with it); breaks the clusters into lines in logical order; then lays each line out in visual order (the bidirectional
// algorithm's runs of the line, each read left to right, a right-to-left run's clusters in reverse) and, once the block's
// width is known, shifts each line by its alignment. Pen positions and widths are in pixels: a cluster without shaping
// data advances by its glyph's advance times Size, after the kerning with the codepoint before it; a shaped cluster's
// position is its segment's origin plus the font units from the segment's start converted once (units / UnitsPerEm *
// Size), a segment being the clusters of one shaped run that a line holds side by side. Only additions, subtractions,
// multiplications and divisions of the fonts' floats, the options' values and integers (and the alignment's 0.5) are
// used, so identical input gives identical output in every configuration.
//
// The rules beside TextMeasurement.h's:
//   - A break opportunity is a space cluster (U+0020 alone) preceded on its line by a cluster that is not a space; a
//     line therefore never wraps before its first other cluster, and a wrap never leaves a line empty.
//   - A cluster other than a space whose advance would end beyond MaxWidth (its pen, plus its kerning, plus its advance)
//     ends the line before it once the line holds another cluster that is not a space: at the line's last break
//     opportunity whose line, ended there, fits MaxWidth, else at its first break opportunity, else (a word wider than
//     MaxWidth) between the two clusters. Spaces never end a line themselves, so trailing spaces may reach beyond MaxWidth.
//   - The line ends before the break's space and the next one starts after it: that one space belongs to neither line
//     (TextMeasuredLine's byte ranges), and further spaces of a run start the next line.
//   - Lines of one paragraph partition its bytes (except the break spaces): skipped codepoints, carriage returns included,
//     belong to the line around them. Kerning pairs two consecutive clusters without shaping data of one font and one
//     embedding level on one line (two consecutive drawn codepoints: skipped codepoints keep it, as TextLayout's do); a
//     right-to-left run adds it after the visually left one, between the two.
//   - Each byte that starts no well-formed UTF-8 sequence (Core/Utf8.h's GetUtf8SequenceLength) is one skipped codepoint.
//   - Byte offsets are 32-bit (TextMeasuredGlyph::ByteOffset), so a text is measured up to its first 2^32 - 1 bytes.
//   - Lines are measured in logical order with the same additions their visual layout makes, so a line whose clusters
//     share one level (all left-to-right text, W22.1's) measures the width its layout reaches; the layout's width is the
//     line's.
//   - A cluster's extent on its line runs from its own origin (the pen where its glyphs start, after its kerning) to the
//     origin of the cluster after it in visual order, or to the line's end, so a kerning pair narrows the earlier cluster.
//   - Codepoints no font has (skipped) take no part in the bidirectional algorithm or shaping: the drawn codepoints are
//     read as one text, which is what lets kerning and joining cross a skipped one.

namespace Engine {

	namespace {

		// A codepoint of a paragraph that a font of the chain draws, in text order.
		struct DrawnCodepoint
		{
			uint32_t Codepoint = 0;
			uint32_t ByteBegin = 0;
			uint32_t ByteEnd = 0; // ByteBegin plus its sequence's length
			uint32_t Font = 0;
			const FontGlyph* Glyph = nullptr; // its glyph in Font (FindGlyph)
		};

		// The text between two line feeds: its drawn codepoints and its bytes (without the line feed).
		struct TextParagraph
		{
			std::vector<DrawnCodepoint> Codepoints{};
			uint32_t ByteBegin = 0;
			uint32_t ByteEnd = 0;
		};

		// One glyph of a shaped cluster: its indexed glyph (null when the font did not bake it), its pen from the cluster's
		// left edge and its offset (y up), in font units.
		struct ClusterGlyph
		{
			const FontGlyph* Glyph = nullptr;
			int64_t Pen = 0;
			int32_t XOffset = 0;
			int32_t YOffset = 0;
		};

		// One cluster of a paragraph, in logical order: its drawn codepoints [First, End), its bytes, its font and resolved
		// embedding level, and either its one codepoint and glyph (without shaping data) or its shaped glyphs
		// (ClusterGlyphs[FirstGlyph, FirstGlyph + GlyphCount)) and advance in font units, with the shaped run it belongs to.
		struct TextCluster
		{
			uint32_t First = 0;
			uint32_t End = 0;
			uint32_t ByteBegin = 0;
			uint32_t ByteEnd = 0;
			uint32_t Font = 0;
			uint8_t Level = 0;
			bool Space = false;
			bool Shaped = false;
			uint32_t Codepoint = 0;
			const FontGlyph* Glyph = nullptr;
			uint32_t Run = 0;
			int64_t AdvanceUnits = 0;
			uint32_t FirstGlyph = 0;
			uint32_t GlyphCount = 0;
		};

		// A paragraph's clusters and the glyphs of its shaped ones.
		struct ParagraphClusters
		{
			std::vector<TextCluster> Clusters{};
			std::vector<ClusterGlyph> Glyphs{};
		};

		// The clusters of a line of a paragraph and its byte range; the next line starts at Resume.
		struct LineBreak
		{
			size_t End = 0;
			size_t Resume = 0;
			uint32_t ByteEnd = 0;
			uint32_t NextByteBegin = 0;
		};

		// The pen after the clusters of one shaped run that a line holds side by side: its origin in pixels and the font
		// units from it.
		struct ShapedSegment
		{
			uint32_t Run = 0;
			float Origin = 0.0f;
			int64_t Units = 0;
		};

		// What laying out one line needs besides its clusters.
		struct LineLayout
		{
			std::span<const FontData* const> Fonts{};
			float Size = 0.0f;
			float Baseline = 0.0f; // the line's baseline, pixels from the block's top
		};

	}

	namespace Utils {

		static constexpr uint32_t SpaceCodepoint = 0x20;
		static constexpr uint32_t LineFeedCodepoint = 0x0A;
		static constexpr uint32_t CarriageReturnCodepoint = 0x0D;
		static constexpr size_t MaxMeasuredBytes = std::numeric_limits<uint32_t>::max();
		// The script a run without a script run gets (TextShaping.h never leaves one).
		static constexpr uint32_t UnknownScriptTag = 0x5A7A7A7A; // 'Zzzz'

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

		// Font units of `font` in pixels at `size`: the one conversion of a shaped position.
		static float UnitsToPixels(int64_t units, const FontData& font, float size)
		{
			return static_cast<float>(units) / static_cast<float>(font.UnitsPerEm) * size;
		}

		// The font of the chain that draws `codepoint`: the font of the codepoint before it for a mark or joiner when that
		// font has it, else the first font whose baked glyphs hold it. Nullopt when none does.
		static std::optional<std::pair<uint32_t, const FontGlyph*>> FindDrawingFont(std::span<const FontData* const> fonts, uint32_t codepoint,
			std::optional<uint32_t> previousFont)
		{
			if (previousFont.has_value() && IsTextMarkOrJoiner(codepoint))
			{
				if (const FontGlyph* glyph = FindGlyph(*fonts[*previousFont], codepoint))
					return std::pair(*previousFont, glyph);
			}
			for (size_t font = 0; font < fonts.size(); ++font)
			{
				if (const FontGlyph* glyph = FindGlyph(*fonts[font], codepoint))
					return std::pair(static_cast<uint32_t>(font), glyph);
			}
			return std::nullopt;
		}

		// The paragraphs of `text`, each with the codepoints the chain draws; counts the skipped ones (a codepoint no font
		// has, an ill-formed byte) in `skipped`. Carriage returns are ignored.
		static std::vector<TextParagraph> DecodeText(std::span<const FontData* const> fonts, std::string_view text, uint32_t& skipped)
		{
			std::vector<TextParagraph> paragraphs(1);
			std::optional<uint32_t> previousFont;
			size_t offset = 0;
			while (offset < text.size())
			{
				const size_t length = GetUtf8SequenceLength(text, offset);
				const uint32_t byteOffset = static_cast<uint32_t>(offset);
				if (length == 0)
				{
					++skipped;
					++offset;
					previousFont.reset();
					continue;
				}
				const uint32_t codepoint = DecodeUtf8Sequence(text, offset, length);
				offset += length;
				if (codepoint == CarriageReturnCodepoint)
					continue;
				if (codepoint == LineFeedCodepoint)
				{
					paragraphs.back().ByteEnd = byteOffset;
					paragraphs.push_back({ .Codepoints = {}, .ByteBegin = byteOffset + 1, .ByteEnd = 0 });
					previousFont.reset();
					continue;
				}
				const auto drawn = FindDrawingFont(fonts, codepoint, previousFont);
				if (!drawn.has_value())
				{
					++skipped;
					previousFont.reset();
					continue;
				}
				previousFont = drawn->first;
				paragraphs.back().Codepoints.push_back({
					.Codepoint = codepoint,
					.ByteBegin = byteOffset,
					.ByteEnd = static_cast<uint32_t>(offset),
					.Font = drawn->first,
					.Glyph = drawn->second,
				});
			}
			paragraphs.back().ByteEnd = static_cast<uint32_t>(text.size());
			return paragraphs;
		}

		// A cluster of the drawn codepoints [first, end) of `paragraph` at `level`, without its shaping fields.
		static TextCluster MakeCluster(const TextParagraph& paragraph, uint32_t first, uint32_t end, uint8_t level)
		{
			const DrawnCodepoint& start = paragraph.Codepoints[first];
			return TextCluster{
				.First = first,
				.End = end,
				.ByteBegin = start.ByteBegin,
				.ByteEnd = paragraph.Codepoints[end - 1].ByteEnd,
				.Font = start.Font,
				.Level = level,
				.Space = end - first == 1 && start.Codepoint == SpaceCodepoint,
				.Codepoint = start.Codepoint,
				.Glyph = start.Glyph,
			};
		}

		// Appends the clusters of the shaped glyphs `glyphs` of the run [first, end) (Run `run`) to `out`, in logical order;
		// false (appending nothing) when they do not describe the run's codepoints as monotone clusters.
		static bool AppendShapedClusters(const TextParagraph& paragraph, const FontData& font, std::span<const TextShapedGlyph> glyphs, uint32_t first,
			uint32_t end, uint8_t level, uint32_t run, ParagraphClusters& out, uint32_t& skipped)
		{
			// The glyphs come in visual order: group each cluster's, then order the groups by their first codepoint.
			struct Group
			{
				uint32_t Start = 0;
				size_t FirstGlyph = 0;
				size_t GlyphCount = 0;
			};
			std::vector<Group> groups;
			for (size_t index = 0; index < glyphs.size(); ++index)
			{
				const uint32_t cluster = glyphs[index].Cluster;
				if (cluster < first || cluster >= end)
					return false;
				if (!groups.empty() && groups.back().Start == cluster)
					++groups.back().GlyphCount;
				else
					groups.push_back({ .Start = cluster, .FirstGlyph = index, .GlyphCount = 1 });
			}
			std::ranges::sort(groups, {}, &Group::Start);
			for (size_t index = 1; index < groups.size(); ++index)
			{
				if (groups[index - 1].Start == groups[index].Start)
					return false;
			}
			if (groups.empty())
				return false;

			for (size_t index = 0; index < groups.size(); ++index)
			{
				const Group& group = groups[index];
				// The run's first cluster takes any codepoint before its first shaped one.
				const uint32_t clusterFirst = index == 0 ? first : group.Start;
				const uint32_t clusterEnd = index + 1 < groups.size() ? groups[index + 1].Start : end;
				TextCluster cluster = MakeCluster(paragraph, clusterFirst, clusterEnd, level);
				cluster.Shaped = true;
				cluster.Run = run;
				cluster.FirstGlyph = static_cast<uint32_t>(out.Glyphs.size());
				cluster.GlyphCount = static_cast<uint32_t>(group.GlyphCount);
				int64_t pen = 0;
				for (size_t glyph = group.FirstGlyph; glyph < group.FirstGlyph + group.GlyphCount; ++glyph)
				{
					const TextShapedGlyph& shaped = glyphs[glyph];
					const FontGlyph* indexed = FindGlyphByIndex(font, shaped.GlyphIndex);
					if (indexed == nullptr)
						++skipped;
					out.Glyphs.push_back({ .Glyph = indexed, .Pen = pen, .XOffset = shaped.XOffset, .YOffset = shaped.YOffset });
					pen += shaped.XAdvance;
				}
				cluster.AdvanceUnits = pen;
				out.Clusters.push_back(cluster);
			}
			return true;
		}

		// The clusters of `paragraph` in logical order: its drawn codepoints itemized into runs of one embedding level, one
		// script and one font, each shaped when its font has shaping data, else one cluster per codepoint.
		static ParagraphClusters BuildClusters(std::span<const FontData* const> fonts, const TextShaper& shaper, const TextParagraph& paragraph,
			std::span<const uint32_t> codepoints, const TextBidiParagraph& bidi, uint32_t& skipped)
		{
			ParagraphClusters result;
			result.Clusters.reserve(codepoints.size());
			std::vector<uint32_t> scripts(codepoints.size(), UnknownScriptTag);
			for (const TextScriptRun& run : FindTextScriptRuns(codepoints))
			{
				for (uint32_t index = run.Offset; index < run.Offset + run.Length && index < scripts.size(); ++index)
					scripts[index] = run.Script;
			}

			uint32_t runIndex = 0;
			uint32_t first = 0;
			const auto size = static_cast<uint32_t>(codepoints.size());
			while (first < size)
			{
				const uint8_t level = bidi.GetLevel(first);
				const uint32_t font = paragraph.Codepoints[first].Font;
				uint32_t end = first + 1;
				while (end < size && bidi.GetLevel(end) == level && scripts[end] == scripts[first] && paragraph.Codepoints[end].Font == font)
					++end;

				bool shaped = false;
				if (shaper.CanShape(font))
				{
					const std::vector<TextShapedGlyph> glyphs = shaper.Shape(font, codepoints, first, end - first, scripts[first], (level & 1U) != 0);
					shaped = AppendShapedClusters(paragraph, *fonts[font], glyphs, first, end, level, runIndex, result, skipped);
				}
				if (!shaped)
				{
					for (uint32_t index = first; index < end; ++index)
						result.Clusters.push_back(MakeCluster(paragraph, index, index + 1, level));
				}
				++runIndex;
				first = end;
			}
			return result;
		}

		// The kerning, in pixels, of the cluster `index` after the one before it on the line that starts at cluster
		// `lineStart`: both without shaping data, of one font and one level (0 otherwise).
		static float FindClusterKerning(std::span<const FontData* const> fonts, std::span<const TextCluster> clusters, size_t lineStart, size_t index, float size)
		{
			if (index <= lineStart)
				return 0.0f;
			const TextCluster& previous = clusters[index - 1];
			const TextCluster& current = clusters[index];
			if (previous.Shaped || current.Shaped || previous.Font != current.Font || previous.Level != current.Level)
				return 0.0f;
			return FindKerning(*fonts[current.Font], previous.Codepoint, current.Codepoint) * size;
		}

		// Finds where the line of the paragraph's clusters that starts at `start` ends (the file comment's rules).
		static LineBreak BreakLine(std::span<const FontData* const> fonts, std::span<const TextCluster> clusters, size_t start, uint32_t paragraphByteEnd,
			const TextMeasureOptions& options, bool wrap)
		{
			struct Opportunity
			{
				size_t Space = 0;
			};
			float pen = 0.0f;
			bool hasContent = false;
			std::optional<Opportunity> lastBreak;
			std::optional<ShapedSegment> segment;
			for (size_t index = start; index < clusters.size(); ++index)
			{
				const TextCluster& cluster = clusters[index];
				float next = 0.0f;
				if (cluster.Shaped)
				{
					if (!segment.has_value() || segment->Run != cluster.Run)
						segment = ShapedSegment{ .Run = cluster.Run, .Origin = pen, .Units = 0 };
					next = segment->Origin + UnitsToPixels(segment->Units + cluster.AdvanceUnits, *fonts[cluster.Font], options.Size);
				}
				else
				{
					segment.reset();
					const float kerning = FindClusterKerning(fonts, clusters, start, index, options.Size);
					next = pen + kerning + cluster.Glyph->Advance * options.Size;
				}
				if (wrap && hasContent)
				{
					if (cluster.Space)
					{
						if (pen <= options.MaxWidth || !lastBreak.has_value())
							lastBreak = Opportunity{ .Space = index };
					}
					else if (next > options.MaxWidth)
					{
						if (lastBreak.has_value())
						{
							const TextCluster& space = clusters[lastBreak->Space];
							return { .End = lastBreak->Space, .Resume = lastBreak->Space + 1, .ByteEnd = space.ByteBegin, .NextByteBegin = space.ByteEnd };
						}
						return { .End = index, .Resume = index, .ByteEnd = cluster.ByteBegin, .NextByteBegin = cluster.ByteBegin };
					}
				}
				if (segment.has_value())
					segment->Units += cluster.AdvanceUnits;
				pen = next;
				hasContent = hasContent || !cluster.Space;
			}
			return { .End = clusters.size(), .Resume = clusters.size(), .ByteEnd = paragraphByteEnd, .NextByteBegin = paragraphByteEnd };
		}

		// Lays out the line of clusters [start, end) of the paragraph in visual order from pen x = 0 (the file comment's
		// rules), appending its glyphs and clusters (their x relative to the line's pen start) to `result` with line index
		// `line`; returns its width.
		static float LayOutLine(const ParagraphClusters& paragraph, const TextBidiParagraph& bidi, size_t start, size_t end, uint32_t line,
			const LineLayout& layout, TextMeasurement& result)
		{
			if (start == end)
				return 0.0f;
			const std::span<const TextCluster> clusters = paragraph.Clusters;
			const uint32_t drawnFirst = clusters[start].First;
			const uint32_t drawnEnd = clusters[end - 1].End;
			// The cluster that starts at each drawn codepoint of the line, if any.
			std::vector<size_t> clusterAt(drawnEnd - drawnFirst, SIZE_MAX);
			for (size_t index = start; index < end; ++index)
				clusterAt[clusters[index].First - drawnFirst] = index;

			float pen = 0.0f;
			std::optional<size_t> previousCluster; // the last cluster laid out, in TextMeasurement::Clusters
			const auto beginCluster = [&result, &previousCluster, &clusters, line](size_t index, float origin, bool rightToLeft)
			{
				if (previousCluster.has_value())
					result.Clusters[*previousCluster].Right = origin;
				const TextCluster& cluster = clusters[index];
				result.Clusters.push_back({
					.ByteBegin = cluster.ByteBegin,
					.ByteEnd = cluster.ByteEnd,
					.Line = line,
					.Left = origin,
					.Right = origin,
					.RightToLeft = rightToLeft,
				});
				previousCluster = result.Clusters.size() - 1;
			};
			const auto pushGlyph = [&result, &layout](const FontGlyph& glyph, float x, float baseline, uint32_t font, uint32_t byteOffset)
			{
				// Plane coordinates are em units, y up from the baseline; the block's are pixels, y down from its top.
				if (glyph.PlaneMin == glyph.PlaneMax)
					return;
				result.Glyphs.push_back({
					.Min = glm::vec2(x + glyph.PlaneMin.x * layout.Size, baseline - glyph.PlaneMax.y * layout.Size),
					.Max = glm::vec2(x + glyph.PlaneMax.x * layout.Size, baseline - glyph.PlaneMin.y * layout.Size),
					.AtlasMin = glyph.AtlasMin,
					.AtlasMax = glyph.AtlasMax,
					.Font = font,
					.ByteOffset = byteOffset,
				});
			};

			for (const TextBidiRun& run : bidi.GetLineRuns(drawnFirst, drawnEnd - drawnFirst))
			{
				const bool rightToLeft = (run.Level & 1U) != 0;
				std::vector<size_t> order;
				for (uint32_t drawn = run.Offset; drawn < run.Offset + run.Length; ++drawn)
				{
					if (clusterAt[drawn - drawnFirst] != SIZE_MAX)
						order.push_back(clusterAt[drawn - drawnFirst]);
				}
				if (rightToLeft)
					std::ranges::reverse(order);
				const auto inRun = [&run](const TextCluster& cluster)
				{
					return cluster.First >= run.Offset && cluster.First < run.Offset + run.Length;
				};

				std::optional<ShapedSegment> segment;
				for (const size_t index : order)
				{
					const TextCluster& cluster = clusters[index];
					const FontData& font = *layout.Fonts[cluster.Font];
					if (cluster.Shaped)
					{
						if (!segment.has_value() || segment->Run != cluster.Run)
							segment = ShapedSegment{ .Run = cluster.Run, .Origin = pen, .Units = 0 };
						beginCluster(index, segment->Origin + UnitsToPixels(segment->Units, font, layout.Size), rightToLeft);
						for (uint32_t glyph = cluster.FirstGlyph; glyph < cluster.FirstGlyph + cluster.GlyphCount; ++glyph)
						{
							const ClusterGlyph& shaped = paragraph.Glyphs[glyph];
							if (shaped.Glyph == nullptr)
								continue;
							const float x = segment->Origin + UnitsToPixels(segment->Units + shaped.Pen + shaped.XOffset, font, layout.Size);
							const float baseline = layout.Baseline - UnitsToPixels(shaped.YOffset, font, layout.Size);
							pushGlyph(*shaped.Glyph, x, baseline, cluster.Font, cluster.ByteBegin);
						}
						segment->Units += cluster.AdvanceUnits;
						pen = segment->Origin + UnitsToPixels(segment->Units, font, layout.Size);
						continue;
					}

					// Without shaping data: the kerning with the cluster before it in logical order, when that one is in
					// this run too, lies between the two (before this one left to right, after it right to left).
					segment.reset();
					const bool kerned = index > start && inRun(clusters[index - 1]);
					const float kerning = kerned ? FindClusterKerning(layout.Fonts, clusters, start, index, layout.Size) : 0.0f;
					if (!rightToLeft)
						pen += kerning;
					beginCluster(index, pen, rightToLeft);
					pushGlyph(*cluster.Glyph, pen, layout.Baseline, cluster.Font, cluster.ByteBegin);
					pen += cluster.Glyph->Advance * layout.Size;
					if (rightToLeft)
						pen += kerning;
				}
			}
			if (previousCluster.has_value())
				result.Clusters[*previousCluster].Right = pen;
			return pen;
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

		[[nodiscard]] static TextBidiParagraph::Direction ToBidiDirection(TextDirection direction)
		{
			switch (direction)
			{
				case TextDirection::Auto:        return TextBidiParagraph::Direction::Auto;
				case TextDirection::LeftToRight: return TextBidiParagraph::Direction::LeftToRight;
				case TextDirection::RightToLeft: return TextBidiParagraph::Direction::RightToLeft;
			}
			ENGINE_CORE_ASSERT(false, "Unknown TextDirection {}", std::to_underlying(direction));
			return TextBidiParagraph::Direction::Auto;
		}

		// A cluster's leading edge (where a caret before its first byte stands) or trailing edge.
		[[nodiscard]] static float GetLeadingEdge(const TextMeasuredCluster& cluster)
		{
			return cluster.RightToLeft ? cluster.Right : cluster.Left;
		}

		[[nodiscard]] static float GetTrailingEdge(const TextMeasuredCluster& cluster)
		{
			return cluster.RightToLeft ? cluster.Left : cluster.Right;
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
		const FontData& first = *fonts.front();
		TextMeasurement result;
		result.LineHeight = (first.Ascent - first.Descent) * options.Size;
		result.Alignment = options.Alignment;
		if (measured.empty())
		{
			result.Size.x = bounded ? options.MaxWidth : 0.0f;
			return result;
		}

		const float pitch = (first.Ascent - first.Descent + first.LineGap) * options.Size * options.LineSpacing;
		const float ascent = first.Ascent * options.Size;
		const bool wrap = options.Wrap == TextWrap::Word && bounded;
		const TextShaper shaper(fonts);
		const std::vector<TextParagraph> paragraphs = Utils::DecodeText(fonts, measured, result.SkippedCodepoints);
		std::vector<uint32_t> codepoints;
		for (const TextParagraph& paragraph : paragraphs)
		{
			codepoints.clear();
			for (const DrawnCodepoint& drawn : paragraph.Codepoints)
				codepoints.push_back(drawn.Codepoint);
			const TextBidiParagraph bidi(codepoints, Utils::ToBidiDirection(options.Direction));
			const ParagraphClusters clusters = Utils::BuildClusters(fonts, shaper, paragraph, codepoints, bidi, result.SkippedCodepoints);

			size_t start = 0;
			uint32_t byteBegin = paragraph.ByteBegin;
			while (true)
			{
				const LineBreak end = Utils::BreakLine(fonts, clusters.Clusters, start, paragraph.ByteEnd, options, wrap);
				const float top = static_cast<float>(result.Lines.size()) * pitch;
				const LineLayout layout{ .Fonts = fonts, .Size = options.Size, .Baseline = top + ascent };
				const size_t firstGlyph = result.Glyphs.size();
				const float width = Utils::LayOutLine(clusters, bidi, start, end.End, static_cast<uint32_t>(result.Lines.size()), layout, result);
				result.Lines.push_back({
					.FirstGlyph = static_cast<uint32_t>(firstGlyph),
					.GlyphCount = static_cast<uint32_t>(result.Glyphs.size() - firstGlyph),
					.ByteBegin = byteBegin,
					.ByteEnd = end.ByteEnd,
					.Left = 0.0f,
					.Width = width,
					.Top = top,
					.Baseline = layout.Baseline,
				});
				if (end.End == clusters.Clusters.size())
					break;
				start = end.Resume;
				byteBegin = end.NextByteBegin;
			}
		}

		float blockWidth = 0.0f;
		for (const TextMeasuredLine& line : result.Lines)
			blockWidth = std::max(blockWidth, line.Width);
		if (bounded)
			blockWidth = options.MaxWidth;
		result.Size = glm::vec2(blockWidth, static_cast<float>(result.Lines.size() - 1) * pitch + (first.Ascent - first.Descent) * options.Size);

		size_t cluster = 0;
		for (size_t lineIndex = 0; lineIndex < result.Lines.size(); ++lineIndex)
		{
			TextMeasuredLine& line = result.Lines[lineIndex];
			line.Left = Utils::AlignLine(options.Alignment, blockWidth, line.Width);
			for (size_t index = line.FirstGlyph; index < line.FirstGlyph + line.GlyphCount; ++index)
			{
				result.Glyphs[index].Min.x += line.Left;
				result.Glyphs[index].Max.x += line.Left;
			}
			for (; cluster < result.Clusters.size() && result.Clusters[cluster].Line == lineIndex; ++cluster)
			{
				TextMeasuredCluster& measuredCluster = result.Clusters[cluster];
				// A cluster's extent is its two edges in order, whatever the signs of its advance and kerning.
				const float left = std::min(measuredCluster.Left, measuredCluster.Right);
				const float right = std::max(measuredCluster.Left, measuredCluster.Right);
				measuredCluster.Left = left + line.Left;
				measuredCluster.Right = right + line.Left;
			}
		}
		return result;
	}

	glm::vec2 MeasureTextSize(std::span<const FontData* const> fonts, std::string_view text, const TextMeasureOptions& options)
	{
		return MeasureText(fonts, text, options).Size;
	}

	TextCaret FindTextCaret(const TextMeasurement& measurement, uint32_t byteOffset)
	{
		if (measurement.Lines.empty())
		{
			// An empty text: its line 0 at the alignment's start of the block.
			return { .Top = glm::vec2(Utils::AlignLine(measurement.Alignment, measurement.Size.x, 0.0f), 0.0f), .Line = 0 };
		}
		const uint32_t target = std::min(byteOffset, measurement.Lines.back().ByteEnd);
		const auto caretAt = [&measurement](uint32_t line, float x)
		{
			return TextCaret{ .Top = glm::vec2(x, measurement.Lines[line].Top), .Line = line };
		};
		const std::span<const TextMeasuredCluster> clusters = measurement.Clusters;
		for (const TextMeasuredCluster& cluster : clusters)
		{
			if (cluster.ByteBegin == target)
				return caretAt(cluster.Line, Utils::GetLeadingEdge(cluster));
		}
		for (const TextMeasuredCluster& cluster : clusters)
		{
			if (cluster.ByteEnd == target)
				return caretAt(cluster.Line, Utils::GetTrailingEdge(cluster));
		}
		for (const TextMeasuredCluster& cluster : clusters)
		{
			if (cluster.ByteBegin < target && target < cluster.ByteEnd)
				return caretAt(cluster.Line, Utils::GetLeadingEdge(cluster));
		}

		// The byte's line: the last that starts at or before it (lines are in byte order).
		uint32_t line = 0;
		for (uint32_t index = 0; index < measurement.Lines.size(); ++index)
		{
			if (measurement.Lines[index].ByteBegin <= target)
				line = index;
		}
		const TextMeasuredCluster* lastBefore = nullptr;
		const TextMeasuredCluster* firstLogical = nullptr;
		for (const TextMeasuredCluster& cluster : clusters)
		{
			if (cluster.Line != line)
				continue;
			if (cluster.ByteEnd <= target && (lastBefore == nullptr || cluster.ByteEnd > lastBefore->ByteEnd))
				lastBefore = &cluster;
			if (firstLogical == nullptr || cluster.ByteBegin < firstLogical->ByteBegin)
				firstLogical = &cluster;
		}
		if (lastBefore != nullptr)
			return caretAt(line, Utils::GetTrailingEdge(*lastBefore));
		if (firstLogical != nullptr)
			return caretAt(line, Utils::GetLeadingEdge(*firstLogical));
		return caretAt(line, measurement.Lines[line].Left);
	}

}
