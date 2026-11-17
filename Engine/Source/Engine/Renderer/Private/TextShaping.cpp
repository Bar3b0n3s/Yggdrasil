#include "EnginePCH.h"
#include "Engine/Renderer/Private/TextShaping.h"

#include "Engine/Asset/FontData.h"
#include "Engine/Core/Assert.h"

#include <SheenBidi/SheenBidi.h>
#include <hb-ot.h>
#include <hb.h>

#include <algorithm>
#include <utility>
#include <vector>

// SheenBidi's objects are created and released by each call (v2.9.0 keeps no process-level state), and HarfBuzz's by
// each shaper; HarfBuzz's own process-level state (its lazily created function tables, the interned language list) is of
// the kind Architecture §3 rule 5 allows a vendored library: thread-safe, and no result depends on it.

namespace Engine {

	namespace Utils {

		// The codepoint the bidirectional algorithm reads for a paragraph separator, so it never ends the paragraph.
		static constexpr uint32_t BidiSpaceCodepoint = 0x20;
		static constexpr uint32_t ZeroWidthNonJoiner = 0x200C;
		static constexpr uint32_t ZeroWidthJoiner = 0x200D;
		// ISO 15924's Unknown, for a script SheenBidi gives no tag.
		static constexpr uint32_t UnknownScriptTag = HB_TAG('Z', 'z', 'z', 'z');

		[[nodiscard]] static SBCodepointSequence MakeCodepointSequence(std::span<const uint32_t> codepoints)
		{
			return SBCodepointSequence{
				.stringEncoding = SBStringEncodingUTF32,
				.stringBuffer = codepoints.data(),
				.stringLength = codepoints.size(),
			};
		}

	}

	// The paragraph's codepoints as the algorithm reads them, its algorithm and paragraph objects (null when SheenBidi
	// failed to allocate them), and the base level the fallback uses.
	struct TextBidiParagraph::State
	{
		std::vector<uint32_t> Codepoints{};
		SBAlgorithmRef Algorithm = nullptr;
		SBParagraphRef Paragraph = nullptr;
		uint8_t BaseLevel = 0;

		State() = default;
		State(const State&) = delete;
		State& operator=(const State&) = delete;

		~State()
		{
			if (Paragraph != nullptr)
				SBParagraphRelease(Paragraph);
			if (Algorithm != nullptr)
				SBAlgorithmRelease(Algorithm);
		}
	};

	TextBidiParagraph::TextBidiParagraph(std::span<const uint32_t> codepoints, Direction direction)
		: m_State(CreateScope<State>())
	{
		m_State->Codepoints.reserve(codepoints.size());
		for (const uint32_t codepoint : codepoints)
			m_State->Codepoints.push_back(SBCodepointGetBidiType(codepoint) == SBBidiTypeB ? Utils::BidiSpaceCodepoint : codepoint);
		m_State->BaseLevel = direction == Direction::RightToLeft ? 1 : 0;
		if (m_State->Codepoints.empty())
			return;

		const SBCodepointSequence sequence = Utils::MakeCodepointSequence(m_State->Codepoints);
		m_State->Algorithm = SBAlgorithmCreate(&sequence);
		if (m_State->Algorithm == nullptr)
			return;
		SBLevel baseLevel = SBLevelDefaultLTR;
		if (direction == Direction::LeftToRight)
			baseLevel = 0;
		else if (direction == Direction::RightToLeft)
			baseLevel = 1;
		m_State->Paragraph = SBAlgorithmCreateParagraph(m_State->Algorithm, 0, m_State->Codepoints.size(), baseLevel);
		if (m_State->Paragraph == nullptr)
			return;
		// The algorithm reads the one paragraph whole: no codepoint of type B is left to end it early.
		ENGINE_CORE_ASSERT(SBParagraphGetLength(m_State->Paragraph) == m_State->Codepoints.size(), "the bidirectional paragraph covers {} of {} codepoints",
			SBParagraphGetLength(m_State->Paragraph), m_State->Codepoints.size());
		m_State->BaseLevel = SBParagraphGetBaseLevel(m_State->Paragraph);
	}

	TextBidiParagraph::~TextBidiParagraph() = default;

	uint8_t TextBidiParagraph::GetBaseLevel() const
	{
		return m_State->BaseLevel;
	}

	uint8_t TextBidiParagraph::GetLevel(size_t index) const
	{
		ENGINE_CORE_ASSERT(index < m_State->Codepoints.size(), "codepoint {} of a paragraph of {}", index, m_State->Codepoints.size());
		if (m_State->Paragraph == nullptr)
			return m_State->BaseLevel;
		return SBParagraphGetLevelsPtr(m_State->Paragraph)[index];
	}

	std::vector<TextBidiRun> TextBidiParagraph::GetLineRuns(uint32_t offset, uint32_t length) const
	{
		ENGINE_CORE_ASSERT(length > 0 && static_cast<size_t>(offset) + length <= m_State->Codepoints.size(), "the line [{}, +{}) of a paragraph of {}",
			offset, length, m_State->Codepoints.size());
		std::vector<TextBidiRun> runs;
		SBLineRef line = m_State->Paragraph != nullptr ? SBParagraphCreateLine(m_State->Paragraph, offset, length) : nullptr;
		if (line == nullptr)
		{
			runs.push_back({ .Offset = offset, .Length = length, .Level = m_State->BaseLevel });
			return runs;
		}
		const SBUInteger count = SBLineGetRunCount(line);
		const SBRun* lineRuns = SBLineGetRunsPtr(line);
		runs.reserve(count);
		for (SBUInteger index = 0; index < count; ++index)
		{
			runs.push_back({
				.Offset = static_cast<uint32_t>(lineRuns[index].offset),
				.Length = static_cast<uint32_t>(lineRuns[index].length),
				.Level = lineRuns[index].level,
			});
		}
		SBLineRelease(line);
		return runs;
	}

	// Per font of the chain, its HarfBuzz font (null without shaping data); the buffer every run is shaped in, cleared
	// before each (so its random state, which the rand feature draws from, starts at 1 in every run); and the language.
	struct TextShaper::State
	{
		std::vector<hb_font_t*> Fonts{};
		hb_buffer_t* Buffer = nullptr;
		hb_language_t Language = nullptr;

		State() = default;
		State(const State&) = delete;
		State& operator=(const State&) = delete;

		~State()
		{
			for (hb_font_t* font : Fonts)
			{
				if (font != nullptr)
					hb_font_destroy(font);
			}
			if (Buffer != nullptr)
				hb_buffer_destroy(Buffer);
		}
	};

	TextShaper::TextShaper(std::span<const FontData* const> fonts)
		: m_State(CreateScope<State>())
	{
		m_State->Fonts.reserve(fonts.size());
		for (const FontData* font : fonts)
		{
			// Shaping data is whole in a valid font (ValidateFontData); a font built by hand without its units per em is laid out
			// without shaping rather than scaled by 0.
			if (font->ShapingSource.empty() || font->UnitsPerEm == 0)
			{
				m_State->Fonts.push_back(nullptr);
				continue;
			}
			// The face reads the cooked bytes in place (they outlive the shaper; byte-level interop: HarfBuzz takes them as
			// chars); ValidateFontData bounds their size.
			hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(font->ShapingSource.data()), static_cast<unsigned int>(font->ShapingSource.size()),
				HB_MEMORY_MODE_READONLY, nullptr, nullptr);
			hb_face_t* face = hb_face_create(blob, 0);
			hb_blob_destroy(blob);
			hb_font_t* shapingFont = hb_font_create(face);
			hb_face_destroy(face);
			hb_ot_font_set_funcs(shapingFont);
			// Positions in font units: the scale is the units per em the importer recorded.
			const int32_t scale = static_cast<int32_t>(font->UnitsPerEm);
			hb_font_set_scale(shapingFont, scale, scale);
			m_State->Fonts.push_back(shapingFont);
		}
		m_State->Buffer = hb_buffer_create();
		m_State->Language = hb_language_from_string("und", -1);
	}

	TextShaper::~TextShaper() = default;

	bool TextShaper::CanShape(uint32_t font) const
	{
		return font < m_State->Fonts.size() && m_State->Fonts[font] != nullptr;
	}

	std::vector<TextShapedGlyph> TextShaper::Shape(uint32_t font, std::span<const uint32_t> codepoints, uint32_t offset, uint32_t length, uint32_t script,
		bool rightToLeft) const
	{
		ENGINE_CORE_ASSERT(CanShape(font), "font {} of the chain has no shaping data", font);
		ENGINE_CORE_ASSERT(static_cast<size_t>(offset) + length <= codepoints.size(), "the run [{}, +{}) of {} codepoints", offset, length, codepoints.size());
		hb_buffer_t* buffer = m_State->Buffer;
		hb_buffer_clear_contents(buffer);
		hb_buffer_set_cluster_level(buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_CHARACTERS);
		hb_buffer_set_flags(buffer, HB_BUFFER_FLAG_DO_NOT_INSERT_DOTTED_CIRCLE);
		hb_buffer_add_codepoints(buffer, codepoints.data(), static_cast<int>(codepoints.size()), offset, static_cast<int>(length));
		hb_buffer_set_direction(buffer, rightToLeft ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
		hb_buffer_set_script(buffer, hb_script_from_iso15924_tag(script));
		hb_buffer_set_language(buffer, m_State->Language);

		std::vector<TextShapedGlyph> glyphs;
		if (!hb_buffer_allocation_successful(buffer) || !hb_shape_full(m_State->Fonts[font], buffer, nullptr, 0, nullptr))
			return glyphs;
		unsigned int count = 0;
		const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
		const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, nullptr);
		glyphs.reserve(count);
		for (unsigned int index = 0; index < count; ++index)
		{
			glyphs.push_back({
				.GlyphIndex = infos[index].codepoint,
				.Cluster = infos[index].cluster,
				.XAdvance = positions[index].x_advance,
				.XOffset = positions[index].x_offset,
				.YOffset = positions[index].y_offset,
			});
		}
		return glyphs;
	}

	namespace Utils {

		std::vector<TextScriptRun> FindTextScriptRuns(std::span<const uint32_t> codepoints)
		{
			std::vector<TextScriptRun> runs;
			if (codepoints.empty())
				return runs;
			SBScriptLocatorRef locator = SBScriptLocatorCreate();
			if (locator == nullptr)
			{
				runs.push_back({ .Offset = 0, .Length = static_cast<uint32_t>(codepoints.size()), .Script = UnknownScriptTag });
				return runs;
			}
			const SBCodepointSequence sequence = MakeCodepointSequence(codepoints);
			SBScriptLocatorLoadCodepoints(locator, &sequence);
			while (SBScriptLocatorMoveNext(locator))
			{
				const SBScriptAgent* agent = SBScriptLocatorGetAgent(locator);
				const uint32_t tag = SBScriptGetUnicodeTag(agent->script);
				runs.push_back({
					.Offset = static_cast<uint32_t>(agent->offset),
					.Length = static_cast<uint32_t>(agent->length),
					.Script = tag != 0 ? tag : UnknownScriptTag,
				});
			}
			SBScriptLocatorRelease(locator);
			return runs;
		}

		bool IsTextMarkOrJoiner(uint32_t codepoint)
		{
			return codepoint == ZeroWidthJoiner || codepoint == ZeroWidthNonJoiner || SBGeneralCategoryIsMark(SBCodepointGetGeneralCategory(codepoint));
		}

	}

}
