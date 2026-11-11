#include "EnginePCH.h"
#include "Engine/AssetPipeline/Private/FontShaping.h"

#include "Engine/Asset/FontData.h"

#include <hb-ot.h>
#include <hb-subset.h>
#include <hb.h>

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// The HarfBuzz face and font of one font source, destroyed with it.
		class HarfBuzzFont
		{
		public:
			explicit HarfBuzzFont(std::span<const std::byte> source)
			{
				// HarfBuzz reads the font's bytes in place, as chars (byte-level interop); `source` outlives the object.
				hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(source.data()), static_cast<unsigned int>(source.size()), HB_MEMORY_MODE_READONLY,
					nullptr, nullptr);
				m_Face = hb_face_create(blob, 0);
				hb_blob_destroy(blob);
				m_Font = hb_font_create(m_Face);
				hb_ot_font_set_funcs(m_Font);
			}

			~HarfBuzzFont()
			{
				hb_font_destroy(m_Font);
				hb_face_destroy(m_Face);
			}

			HarfBuzzFont(const HarfBuzzFont&) = delete;
			HarfBuzzFont& operator=(const HarfBuzzFont&) = delete;

			[[nodiscard]] hb_face_t* GetFace() const { return m_Face; }
			[[nodiscard]] hb_font_t* GetFont() const { return m_Font; }
		private:
			hb_face_t* m_Face = nullptr;
			hb_font_t* m_Font = nullptr;
		};

		// A HarfBuzz object of type T, destroyed with the guard.
		template<typename T, void (*Destroy)(T*)>
		class HarfBuzzObject
		{
		public:
			explicit HarfBuzzObject(T* object)
				: m_Object(object)
			{
			}

			~HarfBuzzObject()
			{
				if (m_Object != nullptr)
					Destroy(m_Object);
			}

			HarfBuzzObject(const HarfBuzzObject&) = delete;
			HarfBuzzObject& operator=(const HarfBuzzObject&) = delete;

			[[nodiscard]] T* Get() const { return m_Object; }
		private:
			T* m_Object = nullptr;
		};

		using HarfBuzzBuffer = HarfBuzzObject<hb_buffer_t, &hb_buffer_destroy>;
		using HarfBuzzSet = HarfBuzzObject<hb_set_t, &hb_set_destroy>;
		using HarfBuzzSubsetInput = HarfBuzzObject<hb_subset_input_t, &hb_subset_input_destroy>;
		using HarfBuzzFace = HarfBuzzObject<hb_face_t, &hb_face_destroy>;
		using HarfBuzzBlob = HarfBuzzObject<hb_blob_t, &hb_blob_destroy>;

	}

	namespace Utils {

		// The largest font HarfBuzz is handed (its blob lengths are unsigned int).
		static constexpr size_t MaxShapedFontSize = std::numeric_limits<unsigned int>::max();

		[[nodiscard]] static Status CheckFontSize(std::span<const std::byte> source)
		{
			if (source.empty() || source.size() > MaxShapedFontSize)
				return MakeError(ErrorCode::ImportFailed, "a font of {} bytes cannot be shaped", source.size());
			return {};
		}

		// The members of `set`, in ascending order.
		[[nodiscard]] static std::vector<uint32_t> ReadSet(const hb_set_t* set)
		{
			std::vector<uint32_t> members;
			members.reserve(hb_set_get_population(set));
			hb_codepoint_t member = HB_SET_VALUE_INVALID;
			while (hb_set_next(set, &member))
				members.push_back(member);
			return members;
		}

		// Adds the glyphs `codepoints` reach at `script` in both directions to `glyphs`; false when HarfBuzz failed to
		// allocate.
		[[nodiscard]] static bool AddClosure(const HarfBuzzFont& font, hb_script_t script, std::span<const uint32_t> codepoints, hb_language_t language,
			hb_set_t* glyphs)
		{
			for (const hb_direction_t direction : { HB_DIRECTION_LTR, HB_DIRECTION_RTL })
			{
				const HarfBuzzBuffer buffer(hb_buffer_create());
				hb_buffer_add_codepoints(buffer.Get(), codepoints.data(), static_cast<int>(codepoints.size()), 0, static_cast<int>(codepoints.size()));
				hb_buffer_set_direction(buffer.Get(), direction);
				hb_buffer_set_script(buffer.Get(), script);
				hb_buffer_set_language(buffer.Get(), language);
				if (!hb_buffer_allocation_successful(buffer.Get()))
					return false;
				hb_ot_shape_glyphs_closure(font.GetFont(), buffer.Get(), nullptr, 0, glyphs);
			}
			return hb_set_allocation_successful(glyphs);
		}

		Result<FontShapingClosure> FindFontShapingClosure(std::span<const std::byte> source, std::span<const uint32_t> codepoints)
		{
			ENGINE_TRY(CheckFontSize(source));
			const HarfBuzzFont font(source);
			hb_unicode_funcs_t* unicode = hb_unicode_funcs_get_default();

			// The baked codepoints and the mirrored forms the font maps.
			FontShapingClosure closure;
			closure.Codepoints.assign(codepoints.begin(), codepoints.end());
			for (const uint32_t codepoint : codepoints)
			{
				const hb_codepoint_t mirrored = hb_unicode_mirroring(unicode, codepoint);
				hb_codepoint_t glyph = 0;
				if (mirrored != codepoint && hb_font_get_nominal_glyph(font.GetFont(), mirrored, &glyph))
					closure.Codepoints.push_back(mirrored);
			}
			std::ranges::sort(closure.Codepoints);
			const auto [duplicates, end] = std::ranges::unique(closure.Codepoints);
			closure.Codepoints.erase(duplicates, end);

			// By script, in the scripts' order; Common and Inherited codepoints join every buffer.
			std::vector<uint32_t> common;
			std::map<hb_script_t, std::vector<uint32_t>> scripts;
			for (const uint32_t codepoint : closure.Codepoints)
			{
				const hb_script_t script = hb_unicode_script(unicode, codepoint);
				if (script == HB_SCRIPT_COMMON || script == HB_SCRIPT_INHERITED)
					common.push_back(codepoint);
				else
					scripts[script].push_back(codepoint);
			}

			const HarfBuzzSet glyphs(hb_set_create());
			const hb_language_t language = hb_language_from_string("und", -1);
			bool allocated = true;
			for (auto& [script, members] : scripts)
			{
				members.insert(members.end(), common.begin(), common.end());
				allocated = allocated && AddClosure(font, script, members, language, glyphs.Get());
			}
			if (!common.empty())
				allocated = allocated && AddClosure(font, HB_SCRIPT_COMMON, common, language, glyphs.Get());
			if (!allocated)
				return MakeError(ErrorCode::ImportFailed, "HarfBuzz failed to allocate the font's shaping closure");

			const unsigned int glyphCount = hb_face_get_glyph_count(font.GetFace());
			for (const uint32_t glyph : ReadSet(glyphs.Get()))
			{
				if (glyph < glyphCount)
					closure.Glyphs.push_back(glyph);
			}
			return closure;
		}

		Result<Buffer> SubsetFontForShaping(std::span<const std::byte> source, std::span<const uint32_t> codepoints, std::span<const uint32_t> glyphs)
		{
			ENGINE_TRY(CheckFontSize(source));
			const HarfBuzzFont font(source);
			const HarfBuzzSubsetInput input(hb_subset_input_create_or_fail());
			if (input.Get() == nullptr)
				return MakeError(ErrorCode::ImportFailed, "HarfBuzz failed to allocate the font's subset input");

			hb_set_t* unicodes = hb_subset_input_unicode_set(input.Get());
			for (const uint32_t codepoint : codepoints)
				hb_set_add(unicodes, codepoint);
			hb_set_t* glyphSet = hb_subset_input_glyph_set(input.Get());
			for (const uint32_t glyph : glyphs)
				hb_set_add(glyphSet, glyph);
			// The legacy kern table carries the kerning of fonts without GPOS, which shaping applies: the subsetter drops it by
			// default and cannot subset it, so it is kept whole (glyph ids are retained, so its pairs stay valid).
			constexpr hb_tag_t KernTag = HB_TAG('k', 'e', 'r', 'n');
			hb_set_del(hb_subset_input_set(input.Get(), HB_SUBSET_SETS_DROP_TABLE_TAG), KernTag);
			hb_set_add(hb_subset_input_set(input.Get(), HB_SUBSET_SETS_NO_SUBSET_TABLE_TAG), KernTag);
			hb_set_t* features = hb_subset_input_set(input.Get(), HB_SUBSET_SETS_LAYOUT_FEATURE_TAG);
			hb_set_clear(features);
			hb_set_invert(features);
			hb_subset_input_set_flags(input.Get(),
				HB_SUBSET_FLAGS_RETAIN_GIDS | HB_SUBSET_FLAGS_NO_HINTING | HB_SUBSET_FLAGS_NO_LAYOUT_CLOSURE | HB_SUBSET_FLAGS_NO_BIDI_CLOSURE);
			if (!hb_set_allocation_successful(unicodes) || !hb_set_allocation_successful(glyphSet))
				return MakeError(ErrorCode::ImportFailed, "HarfBuzz failed to allocate the font's subset input");

			const HarfBuzzFace subset(hb_subset_or_fail(font.GetFace(), input.Get()));
			if (subset.Get() == nullptr)
			{
				return std::unexpected(Error(ErrorCode::ImportFailed, "HarfBuzz's subsetter could not subset the font for shaping")
						.WithHint("the font's tables may be damaged; check the font in a font validator"));
			}
			const HarfBuzzBlob blob(hb_face_reference_blob(subset.Get()));
			unsigned int length = 0;
			const char* data = hb_blob_get_data(blob.Get(), &length);
			if (length == 0 || data == nullptr)
				return MakeError(ErrorCode::ImportFailed, "HarfBuzz's subsetter made an empty font");
			if (length > MaxFontShapingSourceSize)
			{
				return std::unexpected(Error(ErrorCode::ImportFailed,
					std::format("the font's shaping subset holds {} bytes, more than the {} a font may embed", length, MaxFontShapingSourceSize))
						.WithHint("name fewer codepoints in Ranges"));
			}
			// The subset's bytes, copied out of HarfBuzz's blob (byte-level interop).
			const auto* bytes = reinterpret_cast<const std::byte*>(data);
			return Buffer(bytes, bytes + length);
		}

	}

}
