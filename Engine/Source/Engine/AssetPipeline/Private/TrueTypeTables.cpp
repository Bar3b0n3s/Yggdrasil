#include "EnginePCH.h"
#include "Engine/AssetPipeline/Private/TrueTypeTables.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <optional>
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
			FontBytes() = default;

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

			[[nodiscard]] Result<uint8_t> ReadU8(uint64_t offset) const
			{
				ENGINE_TRY(Require(offset, 1));
				return std::to_integer<uint8_t>(m_Bytes[static_cast<size_t>(offset)]);
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

			// The `size` bytes at `offset`, named `name`.
			[[nodiscard]] Result<FontBytes> Slice(uint64_t offset, uint64_t size, std::string name) const
			{
				ENGINE_TRY(Require(offset, size));
				return FontBytes(m_Bytes.subspan(static_cast<size_t>(offset), static_cast<size_t>(size)), std::move(name));
			}

			// The `size` bytes at `offset`, unnamed (the charstring interpreter's subroutine lookups).
			[[nodiscard]] Result<std::span<const std::byte>> Subspan(uint64_t offset, uint64_t size) const
			{
				ENGINE_TRY(Require(offset, size));
				return m_Bytes.subspan(static_cast<size_t>(offset), static_cast<size_t>(size));
			}
		private:
			std::span<const std::byte> m_Bytes{};
			std::string m_Name{}; // for errors: "'GPOS' table", "cmap subtable"
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

		// A CFF INDEX as stb_truetype delimits it (stbtt__cff_get_index): the whole structure (count, offset size, offsets and
		// data; the two count bytes alone when empty), its element count and its offset size (0 when empty).
		struct CffIndex
		{
			FontBytes Bytes{};
			uint32_t Count = 0;
			uint32_t OffsetSize = 0;
		};

		// One entry of a CFF DICT: its operator (an escaped operator 12 x as 0x100 | x) and the byte range of its operands.
		struct CffDictEntry
		{
			uint32_t Operator = 0;
			uint64_t OperandsBegin = 0;
			uint64_t OperandsEnd = 0;
		};

		// The CFF structures stb_truetype's charstring interpreter (stbtt__run_charstring) reads.
		struct CffOutlines
		{
			CffIndex CharStrings{};
			CffIndex GlobalSubrs{};
			CffIndex Subrs{};                  // the local subroutines of the Top DICT's Private DICT
			bool IsCid = false;                // a CID font: local subroutines come from the glyph's font dictionary
			std::vector<CffIndex> FontSubrs{}; // CID: the local subroutines of each font dictionary of the FDArray
			std::vector<int32_t> GlyphFonts{}; // CID: each glyph's font dictionary from the FDSelect, or -1 for none
		};

		// A charstring or subroutine as stb_truetype's interpreter reads it (stbtt__buf): a read past the end returns 0 and
		// does not move.
		struct CharstringCursor
		{
			std::span<const std::byte> Bytes{};
			size_t Position = 0;

			[[nodiscard]] bool AtEnd() const { return Position >= Bytes.size(); }

			[[nodiscard]] uint8_t Next()
			{
				if (AtEnd())
					return 0;
				return std::to_integer<uint8_t>(Bytes[Position++]);
			}

			// `count` (1 to 4) bytes, big-endian.
			[[nodiscard]] uint32_t Next(uint32_t count)
			{
				uint32_t value = 0;
				for (uint32_t index = 0; index < count; ++index)
					value = (value << 8) | Next();
				return value;
			}
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
		// head.unitsPerEm's valid range (OpenType 'head'), so a glyph's pixel size stays bounded at any import size.
		static constexpr uint16_t MinUnitsPerEm = 16;
		static constexpr uint16_t MaxUnitsPerEm = 16384;
		// The point flags of a simple glyph ('glyf') that decide how many bytes stb_truetype reads for a point.
		static constexpr uint8_t PointOnCurve = 0x01;
		static constexpr uint8_t PointXShort = 0x02;
		static constexpr uint8_t PointYShort = 0x04;
		static constexpr uint8_t PointRepeat = 0x08;
		static constexpr uint8_t PointXSameOrPositive = 0x10;
		static constexpr uint8_t PointYSameOrPositive = 0x20;
		// The component flags of a composite glyph that stb_truetype reads.
		static constexpr uint16_t ComponentArgsAreWords = 0x0001;
		static constexpr uint16_t ComponentArgsAreXyValues = 0x0002;
		static constexpr uint16_t ComponentHasScale = 0x0008;
		static constexpr uint16_t ComponentMoreComponents = 0x0020;
		static constexpr uint16_t ComponentHasXyScale = 0x0040;
		static constexpr uint16_t ComponentHasTwoByTwo = 0x0080;
		// stb_truetype expands composite glyphs recursively and without limits: nesting deeper than this, or an outline of
		// more points (counting stb_truetype's working space, points + 2 x contours, per simple glyph), is refused.
		static constexpr uint32_t MaxComponentDepth = 8;
		static constexpr uint64_t MaxGlyphOutlinePoints = 65536;
		// CFF outlines ('CFF ' table, OpenType fonts with the 'OTTO' signature). stb_truetype views the table through a 512 MiB
		// buffer from its start and reads anything inside that buffer, so every structure it reads must lie inside the
		// table. The header is at least 4 bytes. The DICT operators it reads: CharStrings (17), Private (18), Subrs (19),
		// CharstringType (12 6), FDArray (12 36) and FDSelect (12 37).
		static constexpr uint64_t CffMaxTableSize = 512ull * 1024 * 1024;
		static constexpr uint8_t CffHeaderSize = 4;
		static constexpr uint32_t CffCharStringsOperator = 17;
		static constexpr uint32_t CffPrivateOperator = 18;
		static constexpr uint32_t CffSubrsOperator = 19;
		static constexpr uint32_t CffCharstringTypeOperator = 0x106;
		static constexpr uint32_t CffFdArrayOperator = 0x124;
		static constexpr uint32_t CffFdSelectOperator = 0x125;
		static constexpr uint32_t CffType2Charstrings = 2;
		// stb_truetype's charstring interpreter: its operand stack and its subroutine nesting.
		static constexpr uint32_t CharstringStackSize = 48;
		static constexpr uint32_t CharstringSubroutineDepth = 10;
		// stb_truetype runs a charstring without limiting the work that nested subroutine calls multiply: a glyph that runs
		// more operators and operands than this, or a font whose glyphs together run more than MaxCffFontOperations, is
		// refused.
		static constexpr uint32_t MaxCharstringOperations = 65536;
		static constexpr uint64_t MaxCffFontOperations = 1ull << 26;
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

		// The byte range [Begin, End) of a glyph in glyf (ValidateGlyphLocations checked the locations); empty for a glyph
		// without an outline.
		struct GlyphRange
		{
			uint64_t Begin = 0;
			uint64_t End = 0;
		};

		[[nodiscard]] static Result<GlyphRange> GetGlyphRange(const FontBytes& loca, uint16_t indexToLocFormat, uint32_t glyph)
		{
			if (indexToLocFormat == 0)
			{
				ENGINE_TRY_ASSIGN(const uint16_t begin, loca.ReadU16(2ull * glyph));
				ENGINE_TRY_ASSIGN(const uint16_t end, loca.ReadU16(2ull * glyph + 2));
				return GlyphRange{ .Begin = 2ull * begin, .End = 2ull * end };
			}
			ENGINE_TRY_ASSIGN(const uint32_t begin, loca.ReadU32(4ull * glyph));
			ENGINE_TRY_ASSIGN(const uint32_t end, loca.ReadU32(4ull * glyph + 4));
			return GlyphRange{ .Begin = begin, .End = end };
		}

		// A simple glyph (numberOfContours > 0) as stb_truetype reads it (stbtt__GetGlyphShapeTT): the contour end points,
		// strictly increasing, so its contours start where it expects; the instructions; one flag per point, with repeats;
		// then the x and y coordinates each flag says are stored, all inside the glyph. The last contour may not be a single
		// off-curve point, from which stb_truetype reads one point past its outline. Returns the points stb_truetype
		// allocates for the outline (points + 2 x contours).
		[[nodiscard]] static Result<uint64_t> ValidateSimpleGlyph(const FontBytes& glyph, uint32_t index, uint16_t contourCount)
		{
			uint64_t offset = GlyphHeaderSize;
			uint32_t previousEnd = 0;
			uint32_t lastContourStart = 0;
			for (uint32_t contour = 0; contour < contourCount; ++contour)
			{
				ENGINE_TRY_ASSIGN(const uint16_t end, glyph.ReadU16(offset));
				if (contour > 0 && end <= previousEnd)
					return MakeError(ErrorCode::ImportFailed, "the font's glyph {} has contour end points that do not increase", index);
				lastContourStart = contour == 0 ? 0 : previousEnd + 1;
				previousEnd = end;
				offset += 2;
			}
			const uint32_t pointCount = previousEnd + 1;
			ENGINE_TRY_ASSIGN(const uint16_t instructionLength, glyph.ReadU16(offset));
			offset += 2;
			ENGINE_TRY(glyph.Require(offset, instructionLength));
			offset += instructionLength;

			std::vector<uint8_t> flags(pointCount);
			uint32_t repeats = 0;
			uint8_t flag = 0;
			for (uint32_t point = 0; point < pointCount; ++point)
			{
				if (repeats == 0)
				{
					ENGINE_TRY_ASSIGN(flag, glyph.ReadU8(offset));
					++offset;
					if ((flag & PointRepeat) != 0)
					{
						ENGINE_TRY_ASSIGN(const uint8_t count, glyph.ReadU8(offset));
						repeats = count;
						++offset;
					}
				}
				else
				{
					--repeats;
				}
				flags[point] = flag;
			}
			for (const auto [shortBit, sameBit] : { std::pair{ PointXShort, PointXSameOrPositive }, std::pair{ PointYShort, PointYSameOrPositive } })
			{
				for (const uint8_t pointFlags : flags)
				{
					const uint64_t size = (pointFlags & shortBit) != 0 ? 1 : ((pointFlags & sameBit) != 0 ? 0 : 2);
					ENGINE_TRY(glyph.Require(offset, size));
					offset += size;
				}
			}
			if (lastContourStart == pointCount - 1 && (flags[lastContourStart] & PointOnCurve) == 0)
				return MakeError(ErrorCode::ImportFailed, "the font's glyph {} ends with a contour of one off-curve point", index);
			return static_cast<uint64_t>(pointCount) + 2ull * contourCount;
		}

		// A composite glyph (numberOfContours < 0) as stb_truetype reads it: each component's flags and glyph index, its x and
		// y offsets (stb_truetype supports no point matching, ARGS_ARE_XY_VALUES must be set) and its transform, all inside the
		// glyph. Returns the component glyph indices.
		[[nodiscard]] static Result<std::vector<uint16_t>> ReadCompositeComponents(const FontBytes& glyph, uint32_t index)
		{
			std::vector<uint16_t> components;
			uint64_t offset = GlyphHeaderSize;
			bool more = true;
			while (more)
			{
				ENGINE_TRY_ASSIGN(const uint16_t flags, glyph.ReadU16(offset));
				ENGINE_TRY_ASSIGN(const uint16_t component, glyph.ReadU16(offset + 2));
				offset += 4;
				if ((flags & ComponentArgsAreXyValues) == 0)
				{
					return MakeError(ErrorCode::ImportFailed, "the font's glyph {} places a component by matching points, which is not supported",
						index);
				}
				offset += (flags & ComponentArgsAreWords) != 0 ? 4 : 2;
				if ((flags & ComponentHasScale) != 0)
					offset += 2;
				else if ((flags & ComponentHasXyScale) != 0)
					offset += 4;
				else if ((flags & ComponentHasTwoByTwo) != 0)
					offset += 8;
				ENGINE_TRY(glyph.Require(0, offset));
				components.push_back(component);
				more = (flags & ComponentMoreComponents) != 0;
			}
			return components;
		}

		// What ValidateGlyphOutlines knows about one glyph.
		struct GlyphOutline
		{
			std::vector<uint16_t> Components{}; // a composite glyph's components; empty for a simple glyph
			uint64_t Points = 0;                // a simple glyph's points as stb_truetype allocates them
			bool IsComposite = false;
		};

		// The outline points of `glyph` expanded through its components, as stb_truetype appends them. Errors: ImportFailed
		// for components nested deeper than MaxComponentDepth (a cycle among them) or an outline of more than
		// MaxGlyphOutlinePoints points.
		[[nodiscard]] static Result<uint64_t> ExpandGlyphPoints(std::span<const GlyphOutline> outlines, std::vector<std::optional<uint64_t>>& expanded,
			uint32_t glyph, uint32_t depth)
		{
			if (glyph >= outlines.size())
				return uint64_t{ 0 }; // stb_truetype reads a component outside the font as an empty outline
			if (expanded[glyph].has_value())
				return *expanded[glyph];
			const GlyphOutline& outline = outlines[glyph];
			uint64_t points = outline.Points;
			if (outline.IsComposite)
			{
				if (depth >= MaxComponentDepth)
				{
					return MakeError(ErrorCode::ImportFailed, "the font's glyph {} nests its components more than {} deep (or in a cycle)", glyph,
						MaxComponentDepth);
				}
				for (const uint16_t component : outline.Components)
				{
					ENGINE_TRY_ASSIGN(const uint64_t componentPoints, ExpandGlyphPoints(outlines, expanded, component, depth + 1));
					points += componentPoints;
					if (points > MaxGlyphOutlinePoints)
						break;
				}
			}
			if (points > MaxGlyphOutlinePoints)
				return MakeError(ErrorCode::ImportFailed, "the font's glyph {} has an outline of more than {} points", glyph, MaxGlyphOutlinePoints);
			expanded[glyph] = points;
			return points;
		}

		// Every glyph outline stb_truetype may read (stbtt_GetGlyphShape): simple glyphs and composite glyphs, whose
		// components expand to bounded outlines without cycles.
		[[nodiscard]] static Status ValidateGlyphOutlines(const FontBytes& loca, const FontBytes& glyf, uint16_t indexToLocFormat, uint16_t glyphCount)
		{
			std::vector<GlyphOutline> outlines(glyphCount);
			for (uint32_t index = 0; index < glyphCount; ++index)
			{
				ENGINE_TRY_ASSIGN(const GlyphRange range, GetGlyphRange(loca, indexToLocFormat, index));
				if (range.End == range.Begin)
					continue;
				ENGINE_TRY_ASSIGN(const FontBytes glyph, glyf.Slice(range.Begin, range.End - range.Begin, std::format("glyph {}", index)));
				ENGINE_TRY_ASSIGN(const int16_t contourCount, glyph.ReadI16(0));
				if (contourCount > 0)
				{
					ENGINE_TRY_ASSIGN(outlines[index].Points, ValidateSimpleGlyph(glyph, index, static_cast<uint16_t>(contourCount)));
				}
				else if (contourCount < 0)
				{
					outlines[index].IsComposite = true;
					ENGINE_TRY_ASSIGN(outlines[index].Components, ReadCompositeComponents(glyph, index));
				}
			}
			std::vector<std::optional<uint64_t>> expanded(glyphCount);
			for (uint32_t index = 0; index < glyphCount; ++index)
				ENGINE_TRY(ExpandGlyphPoints(outlines, expanded, index, 0));
			return {};
		}

		// `size` (1 to 4) bytes at `offset` as a big-endian unsigned integer.
		[[nodiscard]] static Result<uint32_t> ReadCffOffset(const FontBytes& bytes, uint64_t offset, uint32_t size)
		{
			uint32_t value = 0;
			for (uint32_t index = 0; index < size; ++index)
			{
				ENGINE_TRY_ASSIGN(const uint8_t byte, bytes.ReadU8(offset + index));
				value = (value << 8) | byte;
			}
			return value;
		}

		// The INDEX at `offset` of the CFF table, delimited as stbtt__cff_get_index delimits it: a count, then for a non-empty
		// INDEX an offset size of 1 to 4 bytes and count + 1 offsets, the first 1 and none smaller than the one before, then
		// the data they delimit, all inside the table.
		[[nodiscard]] static Result<CffIndex> ReadCffIndex(const FontBytes& cff, uint64_t offset, std::string_view name)
		{
			ENGINE_TRY_ASSIGN(const uint16_t count, cff.ReadU16(offset));
			if (count == 0)
			{
				ENGINE_TRY_ASSIGN(FontBytes bytes, cff.Slice(offset, 2, std::format("CFF {}", name)));
				return CffIndex{ .Bytes = std::move(bytes), .Count = 0, .OffsetSize = 0 };
			}
			ENGINE_TRY_ASSIGN(const uint8_t offsetSize, cff.ReadU8(offset + 2));
			if (offsetSize < 1 || offsetSize > 4)
				return MakeError(ErrorCode::ImportFailed, "the font's CFF {} has offset size {} (1 to 4 are valid)", name, offsetSize);
			ENGINE_TRY(cff.Require(offset + 3, (count + 1ull) * offsetSize));
			uint32_t previous = 0;
			for (uint32_t element = 0; element <= count; ++element)
			{
				ENGINE_TRY_ASSIGN(const uint32_t elementOffset, ReadCffOffset(cff, offset + 3 + static_cast<uint64_t>(element) * offsetSize, offsetSize));
				if ((element == 0 && elementOffset != 1) || elementOffset < previous)
				{
					return MakeError(ErrorCode::ImportFailed, "the font's CFF {} has inconsistent offsets (offset {} of element {})", name, elementOffset,
						element);
				}
				previous = elementOffset;
			}
			const uint64_t size = 3 + (count + 1ull) * offsetSize + (previous - 1ull);
			ENGINE_TRY_ASSIGN(FontBytes bytes, cff.Slice(offset, size, std::format("CFF {}", name)));
			return CffIndex{ .Bytes = std::move(bytes), .Count = count, .OffsetSize = offsetSize };
		}

		// Element `element` of an INDEX read by ReadCffIndex, as stbtt__cff_index_get returns it; empty for an element the
		// INDEX does not have.
		[[nodiscard]] static Result<std::span<const std::byte>> GetCffIndexElement(const CffIndex& index, uint32_t element)
		{
			if (element >= index.Count)
				return std::span<const std::byte>();
			const uint64_t offsets = 3;
			ENGINE_TRY_ASSIGN(const uint32_t begin, ReadCffOffset(index.Bytes, offsets + static_cast<uint64_t>(element) * index.OffsetSize, index.OffsetSize));
			ENGINE_TRY_ASSIGN(const uint32_t end, ReadCffOffset(index.Bytes, offsets + (element + 1ull) * index.OffsetSize, index.OffsetSize));
			// Offsets count from 1, the byte before the data.
			const uint64_t data = offsets + (index.Count + 1ull) * index.OffsetSize - 1;
			return index.Bytes.Subspan(data + begin, static_cast<uint64_t>(end) - begin);
		}

		// The integer operand at `offset` of a DICT, advancing past it, with the value stbtt__cff_int gives it (unsigned, as
		// stb_truetype stores it): 32 to 246 one byte, 247 to 254 two, 28 a 16-bit and 29 a 32-bit big-endian value.
		// Errors: ImportFailed for a real number (30), which stb_truetype asserts on where it reads an integer.
		[[nodiscard]] static Result<uint32_t> ReadCffInteger(const FontBytes& dict, uint64_t& offset, uint32_t op)
		{
			ENGINE_TRY_ASSIGN(const uint8_t first, dict.ReadU8(offset));
			++offset;
			if (first >= 32 && first <= 246)
				return static_cast<uint32_t>(static_cast<int32_t>(first) - 139);
			if (first >= 247 && first <= 254)
			{
				ENGINE_TRY_ASSIGN(const uint8_t second, dict.ReadU8(offset));
				++offset;
				const int32_t magnitude = (first <= 250 ? first - 247 : first - 251) * 256 + second + 108;
				return static_cast<uint32_t>(first <= 250 ? magnitude : -magnitude);
			}
			if (first == 28)
			{
				ENGINE_TRY_ASSIGN(const uint16_t value, dict.ReadU16(offset));
				offset += 2;
				return static_cast<uint32_t>(value);
			}
			if (first == 29)
			{
				ENGINE_TRY_ASSIGN(const uint32_t value, dict.ReadU32(offset));
				offset += 4;
				return value;
			}
			return MakeError(ErrorCode::ImportFailed, "the font's {} gives operator {} a real number where an integer is required", dict.GetName(), op);
		}

		// The entries of a CFF DICT, in order: operands (integers of 1, 2, 3 or 5 bytes, or real numbers ending in a 0xF
		// nibble) followed by an operator (one byte below 28, or 12 and a second byte). The reserved operand bytes 31 and 255,
		// which stb_truetype asserts on, an operand cut off by the end and operands without an operator are refused.
		[[nodiscard]] static Result<std::vector<CffDictEntry>> ReadCffDict(const FontBytes& dict)
		{
			std::vector<CffDictEntry> entries;
			uint64_t begin = 0;
			uint64_t offset = 0;
			while (offset < dict.GetSize())
			{
				ENGINE_TRY_ASSIGN(const uint8_t first, dict.ReadU8(offset));
				if (first < 28)
				{
					const uint64_t end = offset;
					uint32_t op = first;
					++offset;
					if (first == 12)
					{
						ENGINE_TRY_ASSIGN(const uint8_t second, dict.ReadU8(offset));
						op = 0x100u | second;
						++offset;
					}
					entries.push_back({ .Operator = op, .OperandsBegin = begin, .OperandsEnd = end });
					begin = offset;
					continue;
				}
				if (first == 31 || first == 255)
					return MakeError(ErrorCode::ImportFailed, "the font's {} has the reserved operand byte {} at offset {}", dict.GetName(), first, offset);
				uint64_t size = 1;
				if (first == 28)
					size = 3;
				else if (first == 29)
					size = 5;
				else if (first >= 247)
					size = 2;
				else if (first == 30)
				{
					// A real number: nibbles up to and including the byte with an 0xF nibble.
					bool terminated = false;
					while (!terminated)
					{
						ENGINE_TRY_ASSIGN(const uint8_t nibbles, dict.ReadU8(offset + size));
						terminated = (nibbles & 0x0F) == 0x0F || (nibbles >> 4) == 0x0F;
						++size;
					}
				}
				ENGINE_TRY(dict.Require(offset, size));
				offset += size;
			}
			if (begin != offset)
				return MakeError(ErrorCode::ImportFailed, "the font's {} ends with operands but no operator", dict.GetName());
			return entries;
		}

		// The first `count` (1 or 2) operands of the first `op` entry of a DICT, read as stbtt__dict_get_ints reads them; an
		// operand the entry lacks (or a missing entry) keeps its value from `values`.
		[[nodiscard]] static Result<std::array<uint32_t, 2>> ReadCffDictIntegers(const FontBytes& dict, std::span<const CffDictEntry> entries, uint32_t op,
			uint32_t count, std::array<uint32_t, 2> values)
		{
			for (const CffDictEntry& entry : entries)
			{
				if (entry.Operator != op)
					continue;
				uint64_t offset = entry.OperandsBegin;
				for (uint32_t index = 0; index < count && offset < entry.OperandsEnd; ++index)
				{
					ENGINE_TRY_ASSIGN(values[index], ReadCffInteger(dict, offset, op));
				}
				break;
			}
			return values;
		}

		// The local subroutines of a Top DICT or font dictionary, as stbtt__get_subrs finds them: its Private DICT (size and
		// offset, operator 18) inside the CFF table, and that DICT's Subrs INDEX (operator 19, relative to the Private DICT)
		// inside the table too. Empty without a Private DICT or without Subrs.
		[[nodiscard]] static Result<CffIndex> ReadCffLocalSubrs(const FontBytes& cff, const FontBytes& dict, std::span<const CffDictEntry> entries)
		{
			ENGINE_TRY_ASSIGN(const auto privateDict, ReadCffDictIntegers(dict, entries, CffPrivateOperator, 2, { 0, 0 }));
			const uint32_t privateSize = privateDict[0];
			const uint32_t privateOffset = privateDict[1];
			if (privateSize == 0 || privateOffset == 0)
				return CffIndex{};
			ENGINE_TRY_ASSIGN(const FontBytes privateBytes, cff.Slice(privateOffset, privateSize, "CFF Private DICT"));
			ENGINE_TRY_ASSIGN(const std::vector<CffDictEntry> privateEntries, ReadCffDict(privateBytes));
			ENGINE_TRY_ASSIGN(const auto subrs, ReadCffDictIntegers(privateBytes, privateEntries, CffSubrsOperator, 1, { 0, 0 }));
			if (subrs[0] == 0)
				return CffIndex{};
			return ReadCffIndex(cff, static_cast<uint64_t>(privateOffset) + subrs[0], "Local Subr INDEX");
		}

		// Each glyph's font dictionary as stbtt__cid_get_glyph_subrs finds it in the FDSelect at `offset`: format 0 (one
		// font dictionary per glyph) or format 3 (ranges with increasing first glyphs, then a sentinel), inside the CFF table
		// and naming only font dictionaries the FDArray has; -1 for a glyph no range covers, which CheckCharstring refuses
		// once the glyph calls a local subroutine.
		[[nodiscard]] static Result<std::vector<int32_t>> ReadCffGlyphFonts(const FontBytes& cff, uint64_t offset, uint16_t glyphCount, uint32_t fontCount)
		{
			std::vector<int32_t> fonts(glyphCount, -1);
			ENGINE_TRY_ASSIGN(const uint8_t format, cff.ReadU8(offset));
			if (format == 0)
			{
				ENGINE_TRY(cff.Require(offset + 1, glyphCount));
				for (uint32_t glyph = 0; glyph < glyphCount; ++glyph)
				{
					ENGINE_TRY_ASSIGN(const uint8_t font, cff.ReadU8(offset + 1 + glyph));
					if (font >= fontCount)
						return MakeError(ErrorCode::ImportFailed, "the font's CFF FDSelect gives glyph {} font dictionary {} of {}", glyph, font, fontCount);
					fonts[glyph] = font;
				}
				return fonts;
			}
			if (format != 3)
				return MakeError(ErrorCode::ImportFailed, "the font's CFF FDSelect has format {}; only formats 0 and 3 are read", format);
			ENGINE_TRY_ASSIGN(const uint16_t rangeCount, cff.ReadU16(offset + 1));
			// The ranges (first glyph, font dictionary), then the sentinel.
			ENGINE_TRY(cff.Require(offset + 3, 3ull * rangeCount + 2));
			ENGINE_TRY_ASSIGN(uint16_t start, cff.ReadU16(offset + 3));
			for (uint32_t range = 0; range < rangeCount; ++range)
			{
				const uint64_t record = offset + 3 + 3ull * range;
				ENGINE_TRY_ASSIGN(const uint8_t font, cff.ReadU8(record + 2));
				ENGINE_TRY_ASSIGN(const uint16_t end, cff.ReadU16(record + 3));
				if (font >= fontCount)
					return MakeError(ErrorCode::ImportFailed, "the font's CFF FDSelect range {} names font dictionary {} of {}", range, font, fontCount);
				if (end <= start)
					return MakeError(ErrorCode::ImportFailed, "the font's CFF FDSelect ranges do not increase (range {} ends at glyph {})", range, end);
				for (uint32_t glyph = start; glyph < end && glyph < glyphCount; ++glyph)
					fonts[glyph] = font;
				start = end;
			}
			return fonts;
		}

		// Subroutine `number` of a subroutine INDEX, biased by the INDEX's size as stbtt__get_subr biases it; empty for a
		// subroutine the INDEX does not have.
		[[nodiscard]] static Result<std::span<const std::byte>> GetCffSubroutine(const CffIndex& subrs, int32_t number)
		{
			const int64_t count = subrs.Count;
			const int64_t bias = count >= 33900 ? 32768 : (count >= 1240 ? 1131 : 107);
			const int64_t index = static_cast<int64_t>(number) + bias;
			if (index < 0 || index >= count)
				return std::span<const std::byte>();
			return GetCffIndexElement(subrs, static_cast<uint32_t>(index));
		}

		// The error for a charstring that draws before its first moveto: its outline would start with a line or curve, and
		// stb_truetype's winding test (stbtt__compute_crossings_x), like the distance field built on it, reads the vertex
		// before every line and curve.
		[[nodiscard]] static Status DrawsBeforeMove(uint32_t glyph)
		{
			return MakeError(ErrorCode::ImportFailed, "the font's CFF charstring of glyph {} draws before its first moveto", glyph);
		}

		// Runs glyph `glyph`'s charstring as stbtt__run_charstring runs it, without drawing: the same operand stack, hint
		// counting, subroutine calls and returns, and the same points where it stops (an operator without enough operands, a
		// missing subroutine, nesting deeper than 10, endchar, the end of the code). Errors: ImportFailed for a hint mask that
		// would run past the end of its charstring or subroutine, which stb_truetype asserts on, as it does on a CID font's
		// local subroutine call from a glyph its FDSelect gives no font dictionary, for a line or curve before the first
		// moveto (DrawsBeforeMove), and for a glyph that runs more than MaxCharstringOperations operators and operands, or
		// pushes the font past MaxCffFontOperations.
		[[nodiscard]] static Status CheckCharstring(const CffOutlines& cff, uint32_t glyph, uint64_t& fontOperations)
		{
			ENGINE_TRY_ASSIGN(const std::span<const std::byte> charstring, GetCffIndexElement(cff.CharStrings, glyph));
			CharstringCursor code{ .Bytes = charstring, .Position = 0 };
			std::array<float, CharstringStackSize> stack{};
			std::array<CharstringCursor, CharstringSubroutineDepth> callers{};
			int32_t depth = 0;
			int32_t count = 0; // the operand stack's size
			int64_t maskBits = 0;
			bool inHeader = true;
			bool moved = false; // a moveto has started the outline
			bool hasSubrs = false;
			const CffIndex* subrs = &cff.Subrs;
			uint32_t operations = 0;
			while (!code.AtEnd())
			{
				++fontOperations;
				if (++operations > MaxCharstringOperations)
				{
					return MakeError(ErrorCode::ImportFailed, "the font's CFF charstring of glyph {} runs more than {} operators and operands", glyph,
						MaxCharstringOperations);
				}
				if (fontOperations > MaxCffFontOperations)
				{
					return MakeError(ErrorCode::ImportFailed, "the font's CFF charstrings run more than {} operators and operands in all (at glyph {})",
						MaxCffFontOperations, glyph);
				}
				bool clearStack = true;
				const uint8_t op = code.Next();
				switch (op)
				{
					case 0x13: // hintmask
					case 0x14: // cntrmask
					{
						if (inHeader)
							maskBits += count / 2; // the implicit vstem
						inHeader = false;
						const uint64_t maskBytes = static_cast<uint64_t>((maskBits + 7) / 8);
						if (maskBytes > code.Bytes.size() - code.Position)
						{
							return MakeError(ErrorCode::ImportFailed, "the font's CFF charstring of glyph {} has a {}-byte hint mask past the end of its code",
								glyph, maskBytes);
						}
						code.Position += static_cast<size_t>(maskBytes);
						break;
					}
					case 0x01: // hstem
					case 0x03: // vstem
					case 0x12: // hstemhm
					case 0x17: // vstemhm
						maskBits += count / 2;
						break;
					case 0x15: // rmoveto
						inHeader = false;
						if (count < 2)
							return {};
						moved = true;
						break;
					case 0x04: // vmoveto
					case 0x16: // hmoveto
						inHeader = false;
						if (count < 1)
							return {};
						moved = true;
						break;
					case 0x05: // rlineto
					case 0x06: // hlineto
					case 0x07: // vlineto
					case 0x1E: // vhcurveto
					case 0x1F: // hvcurveto
					case 0x1A: // vvcurveto
					case 0x1B: // hhcurveto
					case 0x08: // rrcurveto
					{
						// Each draws at least one segment once it has its operands.
						const int32_t needed = op == 0x05 ? 2 : (op == 0x06 || op == 0x07 ? 1 : (op == 0x08 ? 6 : 4));
						if (count < needed)
							return {};
						if (!moved)
							return DrawsBeforeMove(glyph);
						break;
					}
					case 0x18: // rcurveline: curves, then a line from the last two operands
					{
						if (count < 8)
							return {};
						if (!moved)
							return DrawsBeforeMove(glyph);
						int32_t index = 0;
						while (index + 5 < count - 2)
							index += 6;
						if (index + 1 >= count)
							return {};
						break;
					}
					case 0x19: // rlinecurve: lines, then a curve from the last six operands
					{
						if (count < 8)
							return {};
						if (!moved)
							return DrawsBeforeMove(glyph);
						int32_t index = 0;
						while (index + 1 < count - 6)
							index += 2;
						if (index + 5 >= count)
							return {};
						break;
					}
					case 0x0A: // callsubr
						if (!hasSubrs)
						{
							// A CID font's glyph takes its local subroutines from the font dictionary its FDSelect gives it,
							// looked up before any operand is read. stb_truetype asserts on a glyph the FDSelect leaves without
							// one (stbtt__cid_get_glyph_subrs reads FDArray element -1).
							if (cff.IsCid)
							{
								const int32_t font = glyph < cff.GlyphFonts.size() ? cff.GlyphFonts[glyph] : -1;
								if (font < 0)
								{
									return MakeError(ErrorCode::ImportFailed,
										"the font's CFF FDSelect gives glyph {} no font dictionary, but its charstring calls a local subroutine", glyph);
								}
								subrs = &cff.FontSubrs[static_cast<size_t>(font)];
							}
							hasSubrs = true;
						}
						[[fallthrough]];
					case 0x1D: // callgsubr
					{
						if (count < 1)
							return {};
						const int32_t number = static_cast<int32_t>(stack[static_cast<size_t>(--count)]);
						if (depth >= static_cast<int32_t>(CharstringSubroutineDepth))
							return {};
						callers[static_cast<size_t>(depth++)] = code;
						ENGINE_TRY_ASSIGN(const std::span<const std::byte> subroutine, GetCffSubroutine(op == 0x0A ? *subrs : cff.GlobalSubrs, number));
						if (subroutine.empty())
							return {};
						code = CharstringCursor{ .Bytes = subroutine, .Position = 0 };
						clearStack = false;
						break;
					}
					case 0x0B: // return
						if (depth <= 0)
							return {};
						code = callers[static_cast<size_t>(--depth)];
						clearStack = false;
						break;
					case 0x0E: // endchar
						return {};
					case 0x0C: // two-byte operators: only the flex family
					{
						const uint8_t escaped = code.Next();
						const int32_t needed = escaped == 0x22 ? 7 : (escaped == 0x23 ? 13 : (escaped == 0x24 ? 9 : (escaped == 0x25 ? 11 : -1)));
						if (needed < 0 || count < needed)
							return {};
						if (!moved)
							return DrawsBeforeMove(glyph);
						break;
					}
					default:
					{
						if (op != 255 && op != 28 && op < 32)
							return {}; // a reserved operator
						float value = 0.0f;
						if (op == 255)
							value = static_cast<float>(static_cast<int32_t>(code.Next(4))) / 0x10000;
						else if (op == 28)
							value = static_cast<float>(static_cast<int16_t>(code.Next(2)));
						else if (op <= 246)
							value = static_cast<float>(static_cast<int32_t>(op) - 139);
						else
						{
							const int32_t magnitude = (op <= 250 ? op - 247 : op - 251) * 256 + code.Next() + 108;
							value = static_cast<float>(op <= 250 ? magnitude : -magnitude);
						}
						if (count >= static_cast<int32_t>(CharstringStackSize))
							return {};
						stack[static_cast<size_t>(count++)] = value;
						clearStack = false;
						break;
					}
				}
				if (clearStack)
					count = 0;
			}
			return {};
		}

		// CFF outlines as stbtt_InitFont reads them, and every glyph's charstring as its interpreter runs it: the header, the
		// Name, Top DICT (at least one), String and Global Subr INDEXes in sequence, the first Top DICT with its CharStrings
		// (Type 2, at least one per glyph), its Private DICT's local subroutines, and for a CID font (FDArray and FDSelect)
		// each font dictionary's local subroutines and an FDSelect naming font dictionaries the FDArray has (and one for
		// every glyph that calls a local subroutine, CheckCharstring), every structure inside the table and every DICT
		// operand stb_truetype reads an integer.
		[[nodiscard]] static Status ValidateCffOutlines(const FontBytes& cff, uint16_t glyphCount)
		{
			if (cff.GetSize() > CffMaxTableSize)
				return MakeError(ErrorCode::ImportFailed, "the font's CFF table of {} bytes is larger than the {} bytes stb_truetype reads", cff.GetSize(),
					CffMaxTableSize);
			ENGINE_TRY_ASSIGN(const uint8_t headerSize, cff.ReadU8(2));
			if (headerSize < CffHeaderSize)
				return MakeError(ErrorCode::ImportFailed, "the font's CFF header declares {} bytes, fewer than the header itself", headerSize);
			ENGINE_TRY_ASSIGN(const CffIndex names, ReadCffIndex(cff, headerSize, "Name INDEX"));
			const uint64_t topDictsOffset = headerSize + static_cast<uint64_t>(names.Bytes.GetSize());
			ENGINE_TRY_ASSIGN(const CffIndex topDicts, ReadCffIndex(cff, topDictsOffset, "Top DICT INDEX"));
			// Defense in depth with a precise message: without this check an empty Top DICT INDEX reads as an empty Top DICT
			// (GetCffIndexElement returns no bytes for a missing element), which has no CharStrings operator and is refused
			// below, so no input that this check refuses reaches stb_truetype without it.
			if (topDicts.Count == 0)
				return MakeError(ErrorCode::ImportFailed, "the font's CFF table has no Top DICT");
			const uint64_t stringsOffset = topDictsOffset + topDicts.Bytes.GetSize();
			ENGINE_TRY_ASSIGN(const CffIndex strings, ReadCffIndex(cff, stringsOffset, "String INDEX"));
			CffOutlines outlines;
			ENGINE_TRY_ASSIGN(outlines.GlobalSubrs, ReadCffIndex(cff, stringsOffset + strings.Bytes.GetSize(), "Global Subr INDEX"));

			ENGINE_TRY_ASSIGN(const std::span<const std::byte> topDictBytes, GetCffIndexElement(topDicts, 0));
			const FontBytes topDict(topDictBytes, "CFF Top DICT");
			ENGINE_TRY_ASSIGN(const std::vector<CffDictEntry> topEntries, ReadCffDict(topDict));
			ENGINE_TRY_ASSIGN(const auto charStrings, ReadCffDictIntegers(topDict, topEntries, CffCharStringsOperator, 1, { 0, 0 }));
			ENGINE_TRY_ASSIGN(const auto charstringType, ReadCffDictIntegers(topDict, topEntries, CffCharstringTypeOperator, 1, { CffType2Charstrings, 0 }));
			ENGINE_TRY_ASSIGN(const auto fdArray, ReadCffDictIntegers(topDict, topEntries, CffFdArrayOperator, 1, { 0, 0 }));
			ENGINE_TRY_ASSIGN(const auto fdSelect, ReadCffDictIntegers(topDict, topEntries, CffFdSelectOperator, 1, { 0, 0 }));
			ENGINE_TRY_ASSIGN(outlines.Subrs, ReadCffLocalSubrs(cff, topDict, topEntries));
			if (charstringType[0] != CffType2Charstrings)
				return MakeError(ErrorCode::ImportFailed, "the font's CFF charstrings are of type {}; only Type 2 charstrings are read", charstringType[0]);
			if (charStrings[0] == 0)
				return MakeError(ErrorCode::ImportFailed, "the font's CFF Top DICT has no CharStrings");

			if (fdArray[0] != 0)
			{
				if (fdSelect[0] == 0)
					return MakeError(ErrorCode::ImportFailed, "the font's CFF Top DICT has an FDArray but no FDSelect");
				outlines.IsCid = true;
				ENGINE_TRY_ASSIGN(const CffIndex fonts, ReadCffIndex(cff, fdArray[0], "FDArray INDEX"));
				for (uint32_t font = 0; font < fonts.Count; ++font)
				{
					ENGINE_TRY_ASSIGN(const std::span<const std::byte> fontDictBytes, GetCffIndexElement(fonts, font));
					const FontBytes fontDict(fontDictBytes, std::format("CFF font dictionary {}", font));
					ENGINE_TRY_ASSIGN(const std::vector<CffDictEntry> fontEntries, ReadCffDict(fontDict));
					ENGINE_TRY_ASSIGN(CffIndex fontSubrs, ReadCffLocalSubrs(cff, fontDict, fontEntries));
					outlines.FontSubrs.push_back(std::move(fontSubrs));
				}
				ENGINE_TRY_ASSIGN(outlines.GlyphFonts, ReadCffGlyphFonts(cff, fdSelect[0], glyphCount, fonts.Count));
			}

			ENGINE_TRY_ASSIGN(outlines.CharStrings, ReadCffIndex(cff, charStrings[0], "CharStrings INDEX"));
			if (outlines.CharStrings.Count < glyphCount)
			{
				return MakeError(ErrorCode::ImportFailed, "the font's CFF table has {} charstrings for the {} glyphs maxp declares",
					outlines.CharStrings.Count, glyphCount);
			}
			uint64_t fontOperations = 0;
			for (uint32_t glyph = 0; glyph < glyphCount; ++glyph)
				ENGINE_TRY(CheckCharstring(outlines, glyph, fontOperations));
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
		if (unitsPerEm < Utils::MinUnitsPerEm || unitsPerEm > Utils::MaxUnitsPerEm)
		{
			return MakeError(ErrorCode::ImportFailed, "the font's head table has {} units per em (OpenType allows {} to {})", unitsPerEm,
				Utils::MinUnitsPerEm, Utils::MaxUnitsPerEm);
		}
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
			ENGINE_TRY(Utils::ValidateGlyphOutlines(loca, glyf, indexToLocFormat, glyphCount));
		}
		else
		{
			ENGINE_TRY_ASSIGN(const FontBytes cff, Utils::GetTable(font, directory, "CFF "));
			ENGINE_TRY(Utils::ValidateCffOutlines(cff, glyphCount));
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
