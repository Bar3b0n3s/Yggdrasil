#include "EnginePCH.h"
#include "Engine/AssetPipeline/Private/TrueTypeTables.h"

#include <algorithm>
#include <array>
#include <bit>
#include <string>
#include <string_view>
#include <utility>

namespace Engine {

	namespace {

		// A bounds-checked big-endian view of the bytes of one font structure (a table or a subtable). Every read checks its
		// range first and fails with ImportFailed naming the structure, the offset and the size.
		class FontBytes
		{
		public:
			FontBytes(std::span<const std::byte> bytes, std::string name)
				: m_Bytes(bytes), m_Name(std::move(name))
			{
			}

			[[nodiscard]] size_t GetSize() const { return m_Bytes.size(); }
			[[nodiscard]] const std::string& GetName() const { return m_Name; }

			// Succeeds when [offset, offset + size) lies inside the view.
			[[nodiscard]] Status Require(uint64_t offset, uint64_t size) const
			{
				if (offset > m_Bytes.size() || size > m_Bytes.size() - offset)
				{
					return MakeError(ErrorCode::ImportFailed, "the font's {} is truncated or inconsistent: {} bytes at offset {} lie outside its {} bytes",
						m_Name, size, offset, m_Bytes.size());
				}
				return {};
			}

			[[nodiscard]] Result<uint16_t> ReadU16(uint64_t offset) const
			{
				ENGINE_TRY(Require(offset, 2));
				const size_t at = static_cast<size_t>(offset);
				return static_cast<uint16_t>((std::to_integer<uint32_t>(m_Bytes[at]) << 8) | std::to_integer<uint32_t>(m_Bytes[at + 1]));
			}

			[[nodiscard]] Result<int16_t> ReadI16(uint64_t offset) const
			{
				ENGINE_TRY_ASSIGN(const uint16_t value, ReadU16(offset));
				return std::bit_cast<int16_t>(value);
			}

			[[nodiscard]] Result<uint32_t> ReadU32(uint64_t offset) const
			{
				ENGINE_TRY(Require(offset, 4));
				const size_t at = static_cast<size_t>(offset);
				return (std::to_integer<uint32_t>(m_Bytes[at]) << 24) | (std::to_integer<uint32_t>(m_Bytes[at + 1]) << 16)
					| (std::to_integer<uint32_t>(m_Bytes[at + 2]) << 8) | std::to_integer<uint32_t>(m_Bytes[at + 3]);
			}

			// The bytes from `offset` to the end of this view, named `name`.
			[[nodiscard]] Result<FontBytes> From(uint64_t offset, std::string name) const
			{
				ENGINE_TRY(Require(offset, 0));
				return FontBytes(m_Bytes.subspan(static_cast<size_t>(offset)), std::move(name));
			}
		private:
			std::span<const std::byte> m_Bytes;
			std::string m_Name; // for errors: "'GPOS' table", "cmap subtable"
		};

		// One table record of the sfnt table directory.
		struct FontTableRecord
		{
			std::array<char, 4> Tag{};
			uint32_t Offset = 0;
			uint32_t Length = 0;
		};

		// The table directory, in file order. Find returns the first record with a tag, as stb_truetype finds it.
		struct FontTableDirectory
		{
			std::vector<FontTableRecord> Records{};

			[[nodiscard]] const FontTableRecord* Find(std::string_view tag) const
			{
				for (const FontTableRecord& record : Records)
				{
					if (std::string_view(record.Tag.data(), record.Tag.size()) == tag)
						return &record;
				}
				return nullptr;
			}
		};

		// The kerning state of one 'kern' lookup over the glyph set: which ordered pairs a subtable of the lookup already
		// matched (count x count), and the set's glyph indices sorted by glyph, each with its position in the set (several
		// codepoints may share a glyph).
		struct PairLookupState
		{
			std::span<const TrueTypeGlyphEntry> Glyphs{};
			std::vector<std::pair<uint16_t, size_t>> ByGlyph{};
			std::vector<char> Matched{};
		};

		// Whether a Coverage table covers a glyph, and its coverage index when it does.
		struct CoverageMatch
		{
			bool Covered = false;
			uint32_t Index = 0;
		};

		// Whether the ScriptList has a script, and the feature indices of its default language system.
		struct ScriptFeatures
		{
			bool Found = false;
			std::vector<uint16_t> Features{};
		};

	}

	namespace Utils {

		// The sfnt signatures stb_truetype reads: TrueType outlines (0x00010000, Apple's 'true') and CFF outlines ('OTTO').
		static constexpr uint32_t TrueTypeSignature = 0x00010000;
		static constexpr uint32_t AppleTrueTypeSignature = 0x74727565; // 'true'
		static constexpr uint32_t OpenTypeCffSignature = 0x4F54544F;   // 'OTTO'
		static constexpr uint32_t CollectionSignature = 0x74746366;    // 'ttcf'
		static constexpr size_t OffsetTableSize = 12;
		static constexpr size_t TableRecordSize = 16;
		// The fixed-offset fields stb_truetype reads: head.unitsPerEm (18) and head.indexToLocFormat (50), hhea.numberOfHMetrics
		// (34) and maxp.numGlyphs (4).
		static constexpr size_t HeadMinimumSize = 54;
		static constexpr size_t HheaMinimumSize = 36;
		static constexpr size_t MaxpMinimumSize = 6;
		// The smallest glyph description: numberOfContours and the bounding box.
		static constexpr uint32_t GlyphHeaderSize = 10;
		// GPOS lookup types and OpenType tags this reader uses.
		static constexpr uint16_t PairAdjustmentLookup = 2;
		static constexpr uint16_t ExtensionLookup = 9;
		static constexpr uint16_t ValueXAdvance = 0x0004;
		// Legacy kern subtable coverage flags.
		static constexpr uint16_t KernHorizontal = 0x0001;
		static constexpr uint16_t KernMinimum = 0x0002;
		static constexpr uint16_t KernCrossStream = 0x0004;
		static constexpr uint16_t KernOverride = 0x0008;

		// A table tag for messages: its four characters, with '?' for anything outside printable ASCII.
		[[nodiscard]] static std::string FormatTag(const std::array<char, 4>& tag)
		{
			std::string text;
			for (const char character : tag)
				text.push_back(character >= ' ' && character <= '~' ? character : '?');
			return text;
		}

		[[nodiscard]] static Result<FontTableDirectory> ReadTableDirectory(std::span<const std::byte> font)
		{
			const FontBytes file(font, "file");
			if (font.size() < OffsetTableSize)
				return MakeError(ErrorCode::ImportFailed, "not a font: {} bytes are too few for an sfnt header", font.size());
			ENGINE_TRY_ASSIGN(const uint32_t signature, file.ReadU32(0));
			if (signature == CollectionSignature)
				return MakeError(ErrorCode::ImportFailed, "font collections (.ttc) are not supported; import a single .ttf or .otf font");
			if (signature != TrueTypeSignature && signature != AppleTrueTypeSignature && signature != OpenTypeCffSignature)
				return MakeError(ErrorCode::ImportFailed, "not a TrueType or OpenType font (sfnt signature 0x{:08x})", signature);

			ENGINE_TRY_ASSIGN(const uint16_t tableCount, file.ReadU16(4));
			const uint64_t directoryEnd = OffsetTableSize + static_cast<uint64_t>(tableCount) * TableRecordSize;
			if (directoryEnd > font.size())
				return MakeError(ErrorCode::ImportFailed, "the font's table directory of {} tables is truncated ({} bytes)", tableCount, font.size());

			FontTableDirectory directory;
			directory.Records.reserve(tableCount);
			for (uint16_t index = 0; index < tableCount; ++index)
			{
				const uint64_t at = OffsetTableSize + static_cast<uint64_t>(index) * TableRecordSize;
				FontTableRecord record;
				for (size_t character = 0; character < record.Tag.size(); ++character)
					record.Tag[character] = static_cast<char>(std::to_integer<uint8_t>(font[static_cast<size_t>(at) + character]));
				ENGINE_TRY_ASSIGN(record.Offset, file.ReadU32(at + 8));
				ENGINE_TRY_ASSIGN(record.Length, file.ReadU32(at + 12));
				if (static_cast<uint64_t>(record.Offset) + record.Length > font.size())
				{
					return MakeError(ErrorCode::ImportFailed, "the font's '{}' table lies outside the file (offset {}, {} bytes, file of {} bytes)",
						FormatTag(record.Tag), record.Offset, record.Length, font.size());
				}
				directory.Records.push_back(record);
			}
			return directory;
		}

		// The bytes of table `tag`. Errors: ImportFailed when the font has no such table.
		[[nodiscard]] static Result<FontBytes> GetTable(std::span<const std::byte> font, const FontTableDirectory& directory, std::string_view tag)
		{
			const FontTableRecord* record = directory.Find(tag);
			if (record == nullptr)
				return MakeError(ErrorCode::ImportFailed, "the font has no '{}' table", tag);
			return FontBytes(font.subspan(record->Offset, record->Length), std::format("'{}' table", tag));
		}

		// A cmap subtable of a format stb_truetype reads, with every range its lookup touches inside `subtable` (the bytes
		// from the subtable to the end of the cmap table).
		[[nodiscard]] static Status ValidateCmapSubtable(const FontBytes& subtable)
		{
			ENGINE_TRY_ASSIGN(const uint16_t format, subtable.ReadU16(0));
			switch (format)
			{
				case 0:
				{
					// Byte encoding: the codepoints below the declared length minus the 6-byte header.
					ENGINE_TRY_ASSIGN(const uint16_t length, subtable.ReadU16(2));
					return subtable.Require(0, length);
				}
				case 4:
				{
					ENGINE_TRY_ASSIGN(const uint16_t segmentCountX2, subtable.ReadU16(6));
					ENGINE_TRY_ASSIGN(const uint16_t searchRangeX2, subtable.ReadU16(8));
					ENGINE_TRY_ASSIGN(const uint16_t rangeShiftX2, subtable.ReadU16(12));
					const uint32_t segmentCount = segmentCountX2 / 2u;
					const uint32_t searchRange = searchRangeX2 / 2u;
					const uint32_t rangeShift = rangeShiftX2 / 2u;
					// stb_truetype's binary search reads end codes up to index searchRange + rangeShift - 1.
					if (segmentCount == 0 || searchRange == 0 || searchRange + rangeShift > segmentCount)
					{
						return MakeError(ErrorCode::ImportFailed, "the font's format 4 cmap subtable is inconsistent ({} segments, search range {}, range shift {})",
							segmentCount, searchRange, rangeShift);
					}
					// Header, endCode[], reservedPad, startCode[], idDelta[], idRangeOffset[].
					ENGINE_TRY(subtable.Require(0, 16 + 4ull * segmentCountX2));
					const uint64_t startCodes = 16 + static_cast<uint64_t>(segmentCountX2);
					const uint64_t rangeOffsets = 16 + 3ull * segmentCountX2;
					for (uint32_t segment = 0; segment < segmentCount; ++segment)
					{
						ENGINE_TRY_ASSIGN(const uint16_t end, subtable.ReadU16(14 + 2ull * segment));
						ENGINE_TRY_ASSIGN(const uint16_t start, subtable.ReadU16(startCodes + 2ull * segment));
						ENGINE_TRY_ASSIGN(const uint16_t rangeOffset, subtable.ReadU16(rangeOffsets + 2ull * segment));
						// A segment with an idRangeOffset reads its glyph ids relative to that offset's own position.
						if (rangeOffset != 0 && end >= start)
							ENGINE_TRY(subtable.Require(rangeOffsets + 2ull * segment + rangeOffset, 2ull * (end - start + 1u)));
					}
					return {};
				}
				case 6:
				{
					ENGINE_TRY_ASSIGN(const uint16_t entryCount, subtable.ReadU16(8));
					return subtable.Require(0, 10 + 2ull * entryCount);
				}
				case 12:
				case 13:
				{
					ENGINE_TRY_ASSIGN(const uint32_t groupCount, subtable.ReadU32(12));
					return subtable.Require(0, 16 + 12ull * groupCount);
				}
				default:
					break;
			}
			return MakeError(ErrorCode::ImportFailed, "the font's Unicode cmap subtable has format {}; only formats 0, 4, 6, 12 and 13 are read",
				format);
		}

		// The Unicode cmap subtable stb_truetype selects (the last record for platform 3 with encoding 1 or 10, or for
		// platform 0), validated.
		[[nodiscard]] static Status ValidateCmap(const FontBytes& cmap)
		{
			ENGINE_TRY_ASSIGN(const uint16_t recordCount, cmap.ReadU16(2));
			ENGINE_TRY(cmap.Require(4, 8ull * recordCount));
			bool found = false;
			uint32_t selected = 0;
			for (uint16_t index = 0; index < recordCount; ++index)
			{
				const uint64_t record = 4 + 8ull * index;
				ENGINE_TRY_ASSIGN(const uint16_t platform, cmap.ReadU16(record));
				ENGINE_TRY_ASSIGN(const uint16_t encoding, cmap.ReadU16(record + 2));
				ENGINE_TRY_ASSIGN(const uint32_t offset, cmap.ReadU32(record + 4));
				if ((platform == 3 && (encoding == 1 || encoding == 10)) || platform == 0)
				{
					found = true;
					selected = offset;
				}
			}
			if (!found)
				return MakeError(ErrorCode::ImportFailed, "the font has no Unicode character map (cmap platform 0, or platform 3 encoding 1 or 10)");
			ENGINE_TRY_ASSIGN(const FontBytes subtable, cmap.From(selected, "cmap subtable"));
			return ValidateCmapSubtable(subtable);
		}

		// TrueType outlines: indexToLocFormat 0 or 1, numGlyphs + 1 glyph locations, non-decreasing, inside glyf, and every
		// non-empty glyph at least its header long.
		[[nodiscard]] static Status ValidateGlyphLocations(const FontBytes& loca, const FontBytes& glyf, uint16_t indexToLocFormat, uint16_t glyphCount)
		{
			if (indexToLocFormat > 1)
				return MakeError(ErrorCode::ImportFailed, "the font's head table has an unknown indexToLocFormat {}", indexToLocFormat);
			const uint64_t entrySize = indexToLocFormat == 0 ? 2 : 4;
			ENGINE_TRY(loca.Require(0, (static_cast<uint64_t>(glyphCount) + 1) * entrySize));
			uint64_t previous = 0;
			for (uint32_t glyph = 0; glyph <= glyphCount; ++glyph)
			{
				uint64_t location = 0;
				if (indexToLocFormat == 0)
				{
					ENGINE_TRY_ASSIGN(const uint16_t halfOffset, loca.ReadU16(2ull * glyph));
					location = 2ull * halfOffset;
				}
				else
				{
					ENGINE_TRY_ASSIGN(location, loca.ReadU32(4ull * glyph));
				}
				if (location > glyf.GetSize() || (glyph > 0 && location < previous))
				{
					return MakeError(ErrorCode::ImportFailed, "the font's glyph location {} ({}) is decreasing or outside the {}-byte glyf table", glyph,
						location, glyf.GetSize());
				}
				if (glyph > 0 && location != previous && location - previous < GlyphHeaderSize)
					return MakeError(ErrorCode::ImportFailed, "the font's glyph {} is shorter than a glyph header", glyph - 1);
				previous = location;
			}
			return {};
		}

		// The byte size of a ValueRecord of `format` (two bytes per set bit of the eight defined fields).
		[[nodiscard]] static uint32_t GetValueRecordSize(uint16_t format)
		{
			return 2u * static_cast<uint32_t>(std::popcount(static_cast<unsigned>(format & 0x00FF)));
		}

		// The XAdvance of the ValueRecord of `format` at `offset`, or 0 when the format has none.
		[[nodiscard]] static Result<int32_t> ReadXAdvance(const FontBytes& bytes, uint64_t offset, uint16_t format)
		{
			if ((format & ValueXAdvance) == 0)
				return 0;
			// XPlacement and YPlacement precede XAdvance.
			const uint64_t before = 2u * static_cast<uint32_t>(std::popcount(static_cast<unsigned>(format & 0x0003)));
			ENGINE_TRY_ASSIGN(const int16_t advance, bytes.ReadI16(offset + before));
			return static_cast<int32_t>(advance);
		}

		// Whether a Coverage table covers `glyph`, and its coverage index.
		[[nodiscard]] static Result<CoverageMatch> FindCoverageIndex(const FontBytes& coverage, uint16_t glyph)
		{
			ENGINE_TRY_ASSIGN(const uint16_t format, coverage.ReadU16(0));
			ENGINE_TRY_ASSIGN(const uint16_t count, coverage.ReadU16(2));
			if (format == 1)
			{
				ENGINE_TRY(coverage.Require(4, 2ull * count));
				for (uint32_t index = 0; index < count; ++index)
				{
					ENGINE_TRY_ASSIGN(const uint16_t covered, coverage.ReadU16(4 + 2ull * index));
					if (covered == glyph)
						return CoverageMatch{ .Covered = true, .Index = index };
				}
				return CoverageMatch{};
			}
			if (format == 2)
			{
				ENGINE_TRY(coverage.Require(4, 6ull * count));
				for (uint32_t index = 0; index < count; ++index)
				{
					const uint64_t record = 4 + 6ull * index;
					ENGINE_TRY_ASSIGN(const uint16_t start, coverage.ReadU16(record));
					ENGINE_TRY_ASSIGN(const uint16_t end, coverage.ReadU16(record + 2));
					ENGINE_TRY_ASSIGN(const uint16_t startIndex, coverage.ReadU16(record + 4));
					if (glyph >= start && glyph <= end)
						return CoverageMatch{ .Covered = true, .Index = static_cast<uint32_t>(startIndex) + (glyph - start) };
				}
				return CoverageMatch{};
			}
			return MakeError(ErrorCode::ImportFailed, "the font's {} has a coverage table of unknown format {}", coverage.GetName(), format);
		}

		// The class of `glyph` in a ClassDef table (0 for a glyph it does not list, as OpenType specifies).
		[[nodiscard]] static Result<uint32_t> FindGlyphClass(const FontBytes& classes, uint16_t glyph)
		{
			ENGINE_TRY_ASSIGN(const uint16_t format, classes.ReadU16(0));
			if (format == 1)
			{
				ENGINE_TRY_ASSIGN(const uint16_t startGlyph, classes.ReadU16(2));
				ENGINE_TRY_ASSIGN(const uint16_t count, classes.ReadU16(4));
				ENGINE_TRY(classes.Require(6, 2ull * count));
				if (glyph < startGlyph || glyph - startGlyph >= count)
					return 0u;
				ENGINE_TRY_ASSIGN(const uint16_t glyphClass, classes.ReadU16(6 + 2ull * (glyph - startGlyph)));
				return static_cast<uint32_t>(glyphClass);
			}
			if (format == 2)
			{
				ENGINE_TRY_ASSIGN(const uint16_t count, classes.ReadU16(2));
				ENGINE_TRY(classes.Require(4, 6ull * count));
				for (uint32_t index = 0; index < count; ++index)
				{
					const uint64_t record = 4 + 6ull * index;
					ENGINE_TRY_ASSIGN(const uint16_t start, classes.ReadU16(record));
					ENGINE_TRY_ASSIGN(const uint16_t end, classes.ReadU16(record + 2));
					if (glyph >= start && glyph <= end)
					{
						ENGINE_TRY_ASSIGN(const uint16_t glyphClass, classes.ReadU16(record + 4));
						return static_cast<uint32_t>(glyphClass);
					}
				}
				return 0u;
			}
			return MakeError(ErrorCode::ImportFailed, "the font's {} has a class definition of unknown format {}", classes.GetName(), format);
		}

		// The flat index of the ordered pair (first, second) of a set of `count` glyphs.
		[[nodiscard]] static size_t PairIndex(size_t first, size_t second, size_t count)
		{
			return first * count + second;
		}

		// The glyph indices of the set sorted by glyph, each with its position in the set.
		[[nodiscard]] static std::vector<std::pair<uint16_t, size_t>> SortByGlyph(std::span<const TrueTypeGlyphEntry> glyphs)
		{
			std::vector<std::pair<uint16_t, size_t>> byGlyph;
			byGlyph.reserve(glyphs.size());
			for (size_t index = 0; index < glyphs.size(); ++index)
				byGlyph.emplace_back(glyphs[index].Glyph, index);
			std::ranges::sort(byGlyph);
			return byGlyph;
		}

		// Records the adjustment of one ordered pair unless an earlier subtable of the lookup matched it.
		static void ApplyPairAdjustment(PairLookupState& state, std::vector<int32_t>& adjustments, size_t first, size_t second, int32_t advance)
		{
			const size_t pair = PairIndex(first, second, state.Glyphs.size());
			if (state.Matched[pair] != 0)
				return;
			state.Matched[pair] = 1;
			adjustments[pair] += advance;
		}

		// Applies one PairPos subtable (format 1 or 2) to every ordered pair of the set, with OpenType's matching rule: a
		// format 1 subtable matches a pair when it covers the first glyph and that glyph's pair set lists the second; a
		// format 2 subtable whenever it covers the first glyph and both classes are in range.
		[[nodiscard]] static Status ApplyPairSubtable(const FontBytes& subtable, PairLookupState& state, std::vector<int32_t>& adjustments)
		{
			ENGINE_TRY_ASSIGN(const uint16_t format, subtable.ReadU16(0));
			ENGINE_TRY_ASSIGN(const uint16_t coverageOffset, subtable.ReadU16(2));
			ENGINE_TRY_ASSIGN(const uint16_t valueFormat1, subtable.ReadU16(4));
			ENGINE_TRY_ASSIGN(const uint16_t valueFormat2, subtable.ReadU16(6));
			if (format != 1 && format != 2)
				return MakeError(ErrorCode::ImportFailed, "the font's GPOS pair adjustment subtable has unknown format {}", format);
			ENGINE_TRY_ASSIGN(const FontBytes coverage, subtable.From(coverageOffset, "GPOS coverage table"));
			const uint32_t valueSize1 = GetValueRecordSize(valueFormat1);
			const uint32_t valueSize2 = GetValueRecordSize(valueFormat2);
			const size_t count = state.Glyphs.size();

			if (format == 1)
			{
				ENGINE_TRY_ASSIGN(const uint16_t pairSetCount, subtable.ReadU16(8));
				const uint64_t recordSize = 2ull + valueSize1 + valueSize2;
				for (size_t first = 0; first < count; ++first)
				{
					ENGINE_TRY_ASSIGN(const CoverageMatch coverageMatch, FindCoverageIndex(coverage, state.Glyphs[first].Glyph));
					if (!coverageMatch.Covered || coverageMatch.Index >= pairSetCount)
						continue;
					ENGINE_TRY_ASSIGN(const uint16_t pairSetOffset, subtable.ReadU16(10 + 2ull * coverageMatch.Index));
					ENGINE_TRY_ASSIGN(const FontBytes pairSet, subtable.From(pairSetOffset, "GPOS pair set"));
					ENGINE_TRY_ASSIGN(const uint16_t pairCount, pairSet.ReadU16(0));
					ENGINE_TRY(pairSet.Require(2, recordSize * pairCount));
					for (uint32_t index = 0; index < pairCount; ++index)
					{
						const uint64_t record = 2 + recordSize * index;
						ENGINE_TRY_ASSIGN(const uint16_t secondGlyph, pairSet.ReadU16(record));
						const auto [begin, end] = std::ranges::equal_range(state.ByGlyph, secondGlyph, {}, &std::pair<uint16_t, size_t>::first);
						if (begin == end)
							continue;
						ENGINE_TRY_ASSIGN(const int32_t advance, ReadXAdvance(pairSet, record + 2, valueFormat1));
						for (auto second = begin; second != end; ++second)
							ApplyPairAdjustment(state, adjustments, first, second->second, advance);
					}
				}
				return {};
			}

			ENGINE_TRY_ASSIGN(const uint16_t classDef1Offset, subtable.ReadU16(8));
			ENGINE_TRY_ASSIGN(const uint16_t classDef2Offset, subtable.ReadU16(10));
			ENGINE_TRY_ASSIGN(const uint16_t class1Count, subtable.ReadU16(12));
			ENGINE_TRY_ASSIGN(const uint16_t class2Count, subtable.ReadU16(14));
			ENGINE_TRY_ASSIGN(const FontBytes classDef1, subtable.From(classDef1Offset, "GPOS class definition"));
			ENGINE_TRY_ASSIGN(const FontBytes classDef2, subtable.From(classDef2Offset, "GPOS class definition"));
			const uint64_t recordSize = static_cast<uint64_t>(valueSize1) + valueSize2;
			ENGINE_TRY(subtable.Require(16, static_cast<uint64_t>(class1Count) * class2Count * recordSize));
			std::vector<uint32_t> secondClasses(count);
			for (size_t second = 0; second < count; ++second)
			{
				ENGINE_TRY_ASSIGN(secondClasses[second], FindGlyphClass(classDef2, state.Glyphs[second].Glyph));
			}
			for (size_t first = 0; first < count; ++first)
			{
				ENGINE_TRY_ASSIGN(const CoverageMatch coverageMatch, FindCoverageIndex(coverage, state.Glyphs[first].Glyph));
				if (!coverageMatch.Covered)
					continue;
				ENGINE_TRY_ASSIGN(const uint32_t class1, FindGlyphClass(classDef1, state.Glyphs[first].Glyph));
				if (class1 >= class1Count)
					continue;
				for (size_t second = 0; second < count; ++second)
				{
					const uint32_t class2 = secondClasses[second];
					if (class2 >= class2Count)
						continue;
					const uint64_t record = 16 + (static_cast<uint64_t>(class1) * class2Count + class2) * recordSize;
					ENGINE_TRY_ASSIGN(const int32_t advance, ReadXAdvance(subtable, record, valueFormat1));
					ApplyPairAdjustment(state, adjustments, first, second, advance);
				}
			}
			return {};
		}

		// The pair adjustment subtables of one lookup (type 2, or type 9 wrapping type 2), in order; empty for any other
		// lookup type.
		[[nodiscard]] static Result<std::vector<FontBytes>> GetPairSubtables(const FontBytes& lookup)
		{
			ENGINE_TRY_ASSIGN(const uint16_t lookupType, lookup.ReadU16(0));
			ENGINE_TRY_ASSIGN(const uint16_t subtableCount, lookup.ReadU16(4));
			std::vector<FontBytes> subtables;
			if (lookupType != PairAdjustmentLookup && lookupType != ExtensionLookup)
				return subtables;
			for (uint32_t index = 0; index < subtableCount; ++index)
			{
				ENGINE_TRY_ASSIGN(const uint16_t offset, lookup.ReadU16(6 + 2ull * index));
				ENGINE_TRY_ASSIGN(FontBytes subtable, lookup.From(offset, "GPOS lookup subtable"));
				if (lookupType == ExtensionLookup)
				{
					ENGINE_TRY_ASSIGN(const uint16_t extensionType, subtable.ReadU16(2));
					ENGINE_TRY_ASSIGN(const uint32_t extensionOffset, subtable.ReadU32(4));
					if (extensionType != PairAdjustmentLookup)
						return std::vector<FontBytes>();
					ENGINE_TRY_ASSIGN(subtable, subtable.From(extensionOffset, "GPOS extension subtable"));
				}
				subtables.push_back(std::move(subtable));
			}
			return subtables;
		}

		// The feature indices of the default language system of `script` in the ScriptList (Found false without that script).
		[[nodiscard]] static Result<ScriptFeatures> FindScriptFeatures(const FontBytes& scriptList, std::string_view script)
		{
			ENGINE_TRY_ASSIGN(const uint16_t scriptCount, scriptList.ReadU16(0));
			ENGINE_TRY(scriptList.Require(2, 6ull * scriptCount));
			for (uint32_t index = 0; index < scriptCount; ++index)
			{
				const uint64_t record = 2 + 6ull * index;
				ENGINE_TRY_ASSIGN(const uint32_t tag, scriptList.ReadU32(record));
				const std::array<char, 4> tagText = { static_cast<char>(tag >> 24), static_cast<char>((tag >> 16) & 0xFF),
					static_cast<char>((tag >> 8) & 0xFF), static_cast<char>(tag & 0xFF) };
				if (std::string_view(tagText.data(), tagText.size()) != script)
					continue;
				ENGINE_TRY_ASSIGN(const uint16_t scriptOffset, scriptList.ReadU16(record + 4));
				ENGINE_TRY_ASSIGN(const FontBytes scriptTable, scriptList.From(scriptOffset, "GPOS script table"));
				ENGINE_TRY_ASSIGN(const uint16_t defaultOffset, scriptTable.ReadU16(0));
				ScriptFeatures result;
				result.Found = true;
				if (defaultOffset == 0)
					return result;
				ENGINE_TRY_ASSIGN(const FontBytes languageSystem, scriptTable.From(defaultOffset, "GPOS language system"));
				ENGINE_TRY_ASSIGN(const uint16_t featureCount, languageSystem.ReadU16(4));
				for (uint32_t feature = 0; feature < featureCount; ++feature)
				{
					ENGINE_TRY_ASSIGN(const uint16_t featureIndex, languageSystem.ReadU16(6 + 2ull * feature));
					result.Features.push_back(featureIndex);
				}
				return result;
			}
			return ScriptFeatures{};
		}

		// The indices of the lookups of the 'kern' features, sorted and unique: those the 'latn' script's default language
		// system names, else the 'DFLT' script's, else every 'kern' feature's.
		[[nodiscard]] static Result<std::vector<uint16_t>> FindKernLookups(const FontBytes& gpos)
		{
			ENGINE_TRY_ASSIGN(const uint16_t scriptListOffset, gpos.ReadU16(4));
			ENGINE_TRY_ASSIGN(const uint16_t featureListOffset, gpos.ReadU16(6));
			ENGINE_TRY_ASSIGN(const FontBytes scriptList, gpos.From(scriptListOffset, "GPOS script list"));
			ENGINE_TRY_ASSIGN(const FontBytes featureList, gpos.From(featureListOffset, "GPOS feature list"));
			ENGINE_TRY_ASSIGN(const uint16_t featureCount, featureList.ReadU16(0));
			ENGINE_TRY(featureList.Require(2, 6ull * featureCount));

			ENGINE_TRY_ASSIGN(ScriptFeatures script, FindScriptFeatures(scriptList, "latn"));
			if (!script.Found)
			{
				ENGINE_TRY_ASSIGN(script, FindScriptFeatures(scriptList, "DFLT"));
			}
			if (!script.Found)
			{
				for (uint16_t index = 0; index < featureCount; ++index)
					script.Features.push_back(index);
			}

			std::vector<uint16_t> lookups;
			for (const uint16_t featureIndex : script.Features)
			{
				if (featureIndex >= featureCount)
					return MakeError(ErrorCode::ImportFailed, "the font's GPOS language system names feature {} of {}", featureIndex, featureCount);
				const uint64_t record = 2 + 6ull * featureIndex;
				ENGINE_TRY_ASSIGN(const uint32_t tag, featureList.ReadU32(record));
				if (tag != 0x6B65726E) // 'kern'
					continue;
				ENGINE_TRY_ASSIGN(const uint16_t featureOffset, featureList.ReadU16(record + 4));
				ENGINE_TRY_ASSIGN(const FontBytes feature, featureList.From(featureOffset, "GPOS feature table"));
				ENGINE_TRY_ASSIGN(const uint16_t lookupCount, feature.ReadU16(2));
				for (uint32_t index = 0; index < lookupCount; ++index)
				{
					ENGINE_TRY_ASSIGN(const uint16_t lookupIndex, feature.ReadU16(4 + 2ull * index));
					lookups.push_back(lookupIndex);
				}
			}
			std::ranges::sort(lookups);
			const auto [first, last] = std::ranges::unique(lookups);
			lookups.erase(first, last);
			return lookups;
		}

		// GPOS kerning of every ordered pair of `glyphs` into `adjustments` (count x count, font units). Returns false when
		// the font has no 'kern' feature, so the legacy kern table applies instead.
		[[nodiscard]] static Result<bool> ReadGposKerning(const FontBytes& gpos, std::span<const TrueTypeGlyphEntry> glyphs,
			std::vector<int32_t>& adjustments)
		{
			ENGINE_TRY_ASSIGN(const uint16_t majorVersion, gpos.ReadU16(0));
			if (majorVersion != 1)
				return MakeError(ErrorCode::ImportFailed, "the font's GPOS table has major version {}; only version 1 is read", majorVersion);
			ENGINE_TRY_ASSIGN(const std::vector<uint16_t> kernLookups, FindKernLookups(gpos));
			if (kernLookups.empty())
				return false;

			ENGINE_TRY_ASSIGN(const uint16_t lookupListOffset, gpos.ReadU16(8));
			ENGINE_TRY_ASSIGN(const FontBytes lookupList, gpos.From(lookupListOffset, "GPOS lookup list"));
			ENGINE_TRY_ASSIGN(const uint16_t lookupCount, lookupList.ReadU16(0));

			PairLookupState state;
			state.Glyphs = glyphs;
			state.ByGlyph = SortByGlyph(glyphs);
			state.Matched.resize(glyphs.size() * glyphs.size());
			for (const uint16_t lookupIndex : kernLookups)
			{
				if (lookupIndex >= lookupCount)
					return MakeError(ErrorCode::ImportFailed, "the font's GPOS 'kern' feature names lookup {} of {}", lookupIndex, lookupCount);
				ENGINE_TRY_ASSIGN(const uint16_t lookupOffset, lookupList.ReadU16(2 + 2ull * lookupIndex));
				ENGINE_TRY_ASSIGN(const FontBytes lookup, lookupList.From(lookupOffset, "GPOS lookup"));
				ENGINE_TRY_ASSIGN(const std::vector<FontBytes> subtables, GetPairSubtables(lookup));

				// Within one lookup the first subtable that matches a pair applies; lookups then add up.
				std::ranges::fill(state.Matched, char{ 0 });
				for (const FontBytes& subtable : subtables)
					ENGINE_TRY(ApplyPairSubtable(subtable, state, adjustments));
			}
			return true;
		}

		// Legacy kern table kerning (version 0, horizontal format 0 subtables) of every ordered pair of `glyphs`.
		[[nodiscard]] static Status ReadKernTableKerning(const FontBytes& kern, std::span<const TrueTypeGlyphEntry> glyphs,
			std::vector<int32_t>& adjustments)
		{
			ENGINE_TRY_ASSIGN(const uint16_t version, kern.ReadU16(0));
			if (version != 0)
				return {}; // Apple's kern table (version 1.0) is not read: the font then has no kerning.
			ENGINE_TRY_ASSIGN(const uint16_t subtableCount, kern.ReadU16(2));
			const size_t count = glyphs.size();
			const std::vector<std::pair<uint16_t, size_t>> byGlyph = SortByGlyph(glyphs);
			uint64_t offset = 4;
			for (uint32_t index = 0; index < subtableCount; ++index)
			{
				ENGINE_TRY_ASSIGN(const uint16_t length, kern.ReadU16(offset + 2));
				if (length < 6)
					return MakeError(ErrorCode::ImportFailed, "the font's kern subtable {} declares {} bytes, less than its header", index, length);
				ENGINE_TRY_ASSIGN(const uint16_t coverage, kern.ReadU16(offset + 4));
				const uint16_t format = static_cast<uint16_t>(coverage >> 8);
				const bool applies = format == 0 && (coverage & KernHorizontal) != 0 && (coverage & (KernMinimum | KernCrossStream)) == 0;
				if (applies)
				{
					ENGINE_TRY_ASSIGN(const uint16_t pairCount, kern.ReadU16(offset + 6));
					ENGINE_TRY(kern.Require(offset + 14, 6ull * pairCount));
					for (uint32_t pair = 0; pair < pairCount; ++pair)
					{
						const uint64_t record = offset + 14 + 6ull * pair;
						ENGINE_TRY_ASSIGN(const uint16_t left, kern.ReadU16(record));
						ENGINE_TRY_ASSIGN(const uint16_t right, kern.ReadU16(record + 2));
						ENGINE_TRY_ASSIGN(const int16_t value, kern.ReadI16(record + 4));
						const auto [leftBegin, leftEnd] = std::ranges::equal_range(byGlyph, left, {}, &std::pair<uint16_t, size_t>::first);
						const auto [rightBegin, rightEnd] = std::ranges::equal_range(byGlyph, right, {}, &std::pair<uint16_t, size_t>::first);
						for (auto first = leftBegin; first != leftEnd; ++first)
						{
							for (auto second = rightBegin; second != rightEnd; ++second)
							{
								int32_t& adjustment = adjustments[PairIndex(first->second, second->second, count)];
								adjustment = (coverage & KernOverride) != 0 ? value : adjustment + value;
							}
						}
					}
				}
				offset += length;
			}
			return {};
		}

	}

	Status Utils::ValidateTrueTypeTables(std::span<const std::byte> font)
	{
		ENGINE_TRY_ASSIGN(const FontTableDirectory directory, Utils::ReadTableDirectory(font));

		ENGINE_TRY_ASSIGN(const FontBytes head, Utils::GetTable(font, directory, "head"));
		ENGINE_TRY(head.Require(0, Utils::HeadMinimumSize));
		ENGINE_TRY_ASSIGN(const uint16_t unitsPerEm, head.ReadU16(18));
		if (unitsPerEm == 0)
			return MakeError(ErrorCode::ImportFailed, "the font's head table has 0 units per em");
		ENGINE_TRY_ASSIGN(const uint16_t indexToLocFormat, head.ReadU16(50));

		ENGINE_TRY_ASSIGN(const FontBytes hhea, Utils::GetTable(font, directory, "hhea"));
		ENGINE_TRY(hhea.Require(0, Utils::HheaMinimumSize));
		ENGINE_TRY_ASSIGN(const uint16_t longMetricCount, hhea.ReadU16(34));

		ENGINE_TRY_ASSIGN(const FontBytes maxp, Utils::GetTable(font, directory, "maxp"));
		ENGINE_TRY(maxp.Require(0, Utils::MaxpMinimumSize));
		ENGINE_TRY_ASSIGN(const uint16_t glyphCount, maxp.ReadU16(4));
		if (glyphCount == 0 || longMetricCount == 0)
			return MakeError(ErrorCode::ImportFailed, "the font declares {} glyphs and {} horizontal metrics", glyphCount, longMetricCount);

		// hmtx: longMetricCount (advance, bearing) pairs, then one bearing per remaining glyph.
		ENGINE_TRY_ASSIGN(const FontBytes hmtx, Utils::GetTable(font, directory, "hmtx"));
		const uint64_t shortMetricCount = glyphCount > longMetricCount ? glyphCount - longMetricCount : 0;
		ENGINE_TRY(hmtx.Require(0, 4ull * longMetricCount + 2 * shortMetricCount));

		ENGINE_TRY_ASSIGN(const FontBytes cmap, Utils::GetTable(font, directory, "cmap"));
		ENGINE_TRY(Utils::ValidateCmap(cmap));

		if (directory.Find("glyf") != nullptr)
		{
			ENGINE_TRY_ASSIGN(const FontBytes glyf, Utils::GetTable(font, directory, "glyf"));
			ENGINE_TRY_ASSIGN(const FontBytes loca, Utils::GetTable(font, directory, "loca"));
			ENGINE_TRY(Utils::ValidateGlyphLocations(loca, glyf, indexToLocFormat, glyphCount));
		}
		else
		{
			// CFF outlines: stb_truetype parses the CFF table itself.
			ENGINE_TRY_ASSIGN(const FontBytes cff, Utils::GetTable(font, directory, "CFF "));
			ENGINE_TRY(cff.Require(0, 4));
		}
		return {};
	}

	Result<std::vector<TrueTypeKerningEntry>> Utils::ReadTrueTypeKerning(std::span<const std::byte> font, std::span<const TrueTypeGlyphEntry> glyphs)
	{
		ENGINE_TRY_ASSIGN(const FontTableDirectory directory, Utils::ReadTableDirectory(font));
		const size_t count = glyphs.size();
		std::vector<int32_t> adjustments(count * count);

		bool fromGpos = false;
		if (directory.Find("GPOS") != nullptr)
		{
			ENGINE_TRY_ASSIGN(const FontBytes gpos, Utils::GetTable(font, directory, "GPOS"));
			ENGINE_TRY_ASSIGN(fromGpos, Utils::ReadGposKerning(gpos, glyphs, adjustments));
		}
		if (!fromGpos && directory.Find("kern") != nullptr)
		{
			ENGINE_TRY_ASSIGN(const FontBytes kern, Utils::GetTable(font, directory, "kern"));
			ENGINE_TRY(Utils::ReadKernTableKerning(kern, glyphs, adjustments));
		}

		std::vector<TrueTypeKerningEntry> kerning;
		for (size_t first = 0; first < count; ++first)
		{
			for (size_t second = 0; second < count; ++second)
			{
				const int32_t adjustment = adjustments[Utils::PairIndex(first, second, count)];
				if (adjustment != 0)
					kerning.push_back({ .First = glyphs[first].Codepoint, .Second = glyphs[second].Codepoint, .Adjustment = adjustment });
			}
		}
		return kerning;
	}

}
