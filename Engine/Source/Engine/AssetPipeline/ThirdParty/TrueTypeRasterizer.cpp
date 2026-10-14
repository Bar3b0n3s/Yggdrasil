// The one translation unit that compiles stb_truetype (imstb_truetype.h of the vendored Dear ImGui, stb_truetype v1.26),
// with STBTT_STATIC so its functions have internal linkage and never clash with Dear ImGui's copy (Architecture §7.4,
// Appendix C). Compiled without the precompiled header (the ThirdParty filter of Engine/premake5.lua), because the
// configuration macros below must precede the library. The cube root, cosine and arc cosine of its distance-field solver
// come from DetMath (§4.12), so a glyph's field does not depend on the C runtime, the CPU or the configuration; sqrt,
// fabs, fmod, floor and ceil are IEEE-exact already. stb_truetype reads fonts without bounds checks, so Open validates
// the structure it trusts first (TrueTypeTables.h).
//
// The distance field itself is computed here rather than by stbtt_GetGlyphSDF, whose solver handles only lines and
// quadratic curves and silently skips the cubic segments of CFF (OpenType) outlines, so every curved .otf glyph came out
// garbled (ADR 0022 "Integration"). RenderGlyphSdf replaces each cubic by quadratics within a fraction of a font unit
// (ReplaceCubics) and runs stb_truetype's own solver over the result (ComputeGlyphSdf, the body of stbtt_GetGlyphSDF with
// the vertices passed in, calling its winding and cubic-root helpers), so a TrueType glyph's field is unchanged byte for
// byte.

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#define STBTT_sqrt(x) std::sqrt(x)
#define STBTT_pow(x, y) ::Engine::DetMath::Pow(x, y)
#define STBTT_cos(x) ::Engine::DetMath::Cos(x)
#define STBTT_acos(x) ::Engine::DetMath::ACos(x)

#include "Engine/AssetPipeline/Private/TrueTypeFont.h"

#include "Engine/AssetPipeline/Private/TrueTypeTables.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"

#include <imstb_truetype.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Engine {

	struct TrueTypeFont::State
	{
		stbtt_fontinfo Info{}; // points into the caller's font bytes, which outlive the font
		int32_t UnitsPerEm = 0;
		int32_t GlyphCount = 0;
	};

	namespace {

		// Releases a glyph shape stb_truetype allocated.
		struct StbShapeDeleter
		{
			const stbtt_fontinfo* Info = nullptr;
			void operator()(stbtt_vertex* vertices) const { stbtt_FreeShape(Info, vertices); }
		};

		// How closely the quadratics that replace a cubic segment follow it, in font units, before their points are rounded
		// to whole units like every stb_truetype vertex (at most half a unit more). With Inter's 2048 units per em the whole
		// error stays below 0.02 px at the importer's 48 px per em.
		constexpr double CubicToleranceUnits = 0.25;
		// A cubic spanning a whole 2048-unit em needs about 15 pieces at that tolerance.
		constexpr uint32_t MaxCubicPieces = 256;

		struct OutlinePoint
		{
			double X = 0.0;
			double Y = 0.0;
		};

	}

	namespace Utils {

		[[nodiscard]] static const unsigned char* AsFontBytes(std::span<const std::byte> data)
		{
			return reinterpret_cast<const unsigned char*>(data.data());
		}

		// head.unitsPerEm, a big-endian uint16 at offset 18 of the head table (present and checked by
		// ValidateTrueTypeTables).
		[[nodiscard]] static int32_t ReadUnitsPerEm(const stbtt_fontinfo& info)
		{
			const unsigned char* head = info.data + info.head;
			return (static_cast<int32_t>(head[18]) << 8) | static_cast<int32_t>(head[19]);
		}

		// A coordinate in whole font units, as stb_truetype stores every vertex (round half up, clamped to its range).
		[[nodiscard]] static stbtt_vertex_type ToFontUnits(double value)
		{
			const double rounded = std::floor(value + 0.5);
			constexpr double Lowest = static_cast<double>(std::numeric_limits<stbtt_vertex_type>::lowest());
			constexpr double Highest = static_cast<double>(std::numeric_limits<stbtt_vertex_type>::max());
			return static_cast<stbtt_vertex_type>(std::clamp(rounded, Lowest, Highest));
		}

		[[nodiscard]] static OutlinePoint EvaluateCubic(const OutlinePoint (&p)[4], double t)
		{
			const double s = 1.0 - t;
			const double a = s * s * s;
			const double b = 3.0 * s * s * t;
			const double c = 3.0 * s * t * t;
			const double d = t * t * t;
			return { a * p[0].X + b * p[1].X + c * p[2].X + d * p[3].X, a * p[0].Y + b * p[1].Y + c * p[2].Y + d * p[3].Y };
		}

		[[nodiscard]] static OutlinePoint CubicDerivative(const OutlinePoint (&p)[4], double t)
		{
			const double s = 1.0 - t;
			const double a = 3.0 * s * s;
			const double b = 6.0 * s * t;
			const double c = 3.0 * t * t;
			return { a * (p[1].X - p[0].X) + b * (p[2].X - p[1].X) + c * (p[3].X - p[2].X),
				a * (p[1].Y - p[0].Y) + b * (p[2].Y - p[1].Y) + c * (p[3].Y - p[2].Y) };
		}

		// The glyph's vertices with every cubic segment (STBTT_vcubic, which only CFF outlines have) replaced by quadratic
		// curves (STBTT_vcurve), so stb_truetype's distance-field solver sees the whole outline. A cubic P0..P3 is cut into n
		// equal parameter pieces, n the smallest count for which the quadratic approximation error bound
		// sqrt(3) / 36 * |P3 - 3 P2 + 3 P1 - P0| / n^3 is at most CubicToleranceUnits; each piece [t0, t1] becomes the
		// quadratic through C(t0) and C(t1) whose control point (C(t0) + C(t1)) / 2 + (t1 - t0) (C'(t0) - C'(t1)) / 4 is
		// the piece's best single quadratic, (3 (c1 + c2) - (c0 + c3)) / 4 of its own control points. Points are rounded to
		// whole font units, and adjacent pieces share the end point of the same parameter, so every contour stays closed.
		// Only arithmetic and comparisons, so the result is the same on every CPU and configuration. A shape without cubics
		// (every TrueType outline) is returned unchanged.
		[[nodiscard]] static std::vector<stbtt_vertex> ReplaceCubics(std::span<const stbtt_vertex> shape)
		{
			std::vector<stbtt_vertex> vertices;
			vertices.reserve(shape.size());
			for (const stbtt_vertex& vertex : shape)
			{
				if (vertex.type != STBTT_vcubic || vertices.empty())
				{
					vertices.push_back(vertex);
					continue;
				}
				const stbtt_vertex previous = vertices.back();
				const OutlinePoint p[4] = {
					{ static_cast<double>(previous.x), static_cast<double>(previous.y) },
					{ static_cast<double>(vertex.cx), static_cast<double>(vertex.cy) },
					{ static_cast<double>(vertex.cx1), static_cast<double>(vertex.cy1) },
					{ static_cast<double>(vertex.x), static_cast<double>(vertex.y) },
				};
				const double dx = p[3].X - 3.0 * p[2].X + 3.0 * p[1].X - p[0].X;
				const double dy = p[3].Y - 3.0 * p[2].Y + 3.0 * p[1].Y - p[0].Y;
				// error^2 = 3 / 1296 * |d|^2, compared with (tolerance * n^3)^2.
				const double errorSquared = 3.0 / 1296.0 * (dx * dx + dy * dy);
				uint32_t pieces = 1;
				while (pieces < MaxCubicPieces)
				{
					const double cube = static_cast<double>(pieces) * pieces * pieces;
					if (errorSquared <= CubicToleranceUnits * CubicToleranceUnits * cube * cube)
						break;
					++pieces;
				}
				for (uint32_t piece = 0; piece < pieces; ++piece)
				{
					const double t0 = static_cast<double>(piece) / pieces;
					const double t1 = static_cast<double>(piece + 1) / pieces;
					const OutlinePoint start = EvaluateCubic(p, t0);
					const OutlinePoint end = piece + 1 == pieces ? p[3] : EvaluateCubic(p, t1);
					const OutlinePoint startTangent = CubicDerivative(p, t0);
					const OutlinePoint endTangent = CubicDerivative(p, t1);
					const double quarterStep = (t1 - t0) * 0.25;
					stbtt_vertex quadratic = vertex;
					quadratic.type = STBTT_vcurve;
					quadratic.cx = ToFontUnits((start.X + end.X) * 0.5 + quarterStep * (startTangent.X - endTangent.X));
					quadratic.cy = ToFontUnits((start.Y + end.Y) * 0.5 + quarterStep * (startTangent.Y - endTangent.Y));
					quadratic.cx1 = 0;
					quadratic.cy1 = 0;
					quadratic.x = ToFontUnits(end.X);
					quadratic.y = ToFontUnits(end.Y);
					vertices.push_back(quadratic);
				}
			}
			return vertices;
		}

		// stb_truetype v1.26's distance-field solver, the body of stbtt_GetGlyphSDF from its stbtt_GetGlyphShape call on,
		// over `vertices` (lines and quadratic curves only, see ReplaceCubics) and the padded bitmap box [x0, x1) x
		// [y0, y1). The arithmetic is stb_truetype's, in the same order and precision, and its winding test
		// (stbtt__compute_crossings_x) and cubic solver (stbtt__solve_cubic) are called as they are, so for a TrueType
		// outline the field equals stbtt_GetGlyphSDF's byte for byte.
		[[nodiscard]] static std::vector<unsigned char> ComputeGlyphSdf(std::vector<stbtt_vertex>& vertices, float scale, int x0Box, int y0Box, int x1Box,
			int y1Box, unsigned char onEdgeValue, float pixelDistanceScale)
		{
			const float scaleX = scale;
			// Inverted for the y-down bitmap.
			const float scaleY = -scale;
			const size_t width = static_cast<size_t>(x1Box - x0Box);
			const size_t height = static_cast<size_t>(y1Box - y0Box);
			const int count = static_cast<int>(vertices.size());
			stbtt_vertex* verts = vertices.data();
			std::vector<unsigned char> data(width * height);
			std::vector<float> precompute(vertices.size(), 0.0f);
			const auto scaled = [](stbtt_vertex_type value, float factor)
			{
				return static_cast<float>(value) * factor;
			};

			for (int i = 0, j = count - 1; i < count; j = i++)
			{
				if (verts[i].type == STBTT_vline)
				{
					const float x0 = scaled(verts[i].x, scaleX), y0 = scaled(verts[i].y, scaleY);
					const float x1 = scaled(verts[j].x, scaleX), y1 = scaled(verts[j].y, scaleY);
					const float dist = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
					precompute[static_cast<size_t>(i)] = (dist == 0.0f) ? 0.0f : 1.0f / dist;
				}
				else if (verts[i].type == STBTT_vcurve)
				{
					const float x2 = scaled(verts[j].x, scaleX), y2 = scaled(verts[j].y, scaleY);
					const float x1 = scaled(verts[i].cx, scaleX), y1 = scaled(verts[i].cy, scaleY);
					const float x0 = scaled(verts[i].x, scaleX), y0 = scaled(verts[i].y, scaleY);
					const float bx = x0 - 2.0f * x1 + x2, by = y0 - 2.0f * y1 + y2;
					const float len2 = bx * bx + by * by;
					precompute[static_cast<size_t>(i)] = len2 != 0.0f ? 1.0f / (bx * bx + by * by) : 0.0f;
				}
			}

			for (int y = y0Box; y < y1Box; ++y)
			{
				for (int x = x0Box; x < x1Box; ++x)
				{
					float minDist = 999999.0f;
					const float sx = static_cast<float>(x) + 0.5f;
					const float sy = static_cast<float>(y) + 0.5f;
					const float xGlyphSpace = sx / scaleX;
					const float yGlyphSpace = sy / scaleY;
					const int winding = stbtt__compute_crossings_x(xGlyphSpace, yGlyphSpace, count, verts);

					for (int i = 0; i < count; ++i)
					{
						const float x0 = scaled(verts[i].x, scaleX), y0 = scaled(verts[i].y, scaleY);
						const float pre = precompute[static_cast<size_t>(i)];
						if (verts[i].type == STBTT_vline && pre != 0.0f)
						{
							const float x1 = scaled(verts[i - 1].x, scaleX), y1 = scaled(verts[i - 1].y, scaleY);
							const float dist2 = (x0 - sx) * (x0 - sx) + (y0 - sy) * (y0 - sy);
							if (dist2 < minDist * minDist)
								minDist = std::sqrt(dist2);
							const float dist = std::fabs((x1 - x0) * (y0 - sy) - (y1 - y0) * (x0 - sx)) * pre;
							if (dist < minDist)
							{
								// The foot of the perpendicular must lie on the segment.
								const float dx = x1 - x0, dy = y1 - y0;
								const float px = x0 - sx, py = y0 - sy;
								const float t = -(px * dx + py * dy) / (dx * dx + dy * dy);
								if (t >= 0.0f && t <= 1.0f)
									minDist = dist;
							}
						}
						else if (verts[i].type == STBTT_vcurve)
						{
							const float x2 = scaled(verts[i - 1].x, scaleX), y2 = scaled(verts[i - 1].y, scaleY);
							const float x1 = scaled(verts[i].cx, scaleX), y1 = scaled(verts[i].cy, scaleY);
							const float boxX0 = std::min(std::min(x0, x1), x2);
							const float boxY0 = std::min(std::min(y0, y1), y2);
							const float boxX1 = std::max(std::max(x0, x1), x2);
							const float boxY1 = std::max(std::max(y0, y1), y2);
							// Coarse culling against the curve's box, so the cubic is solved only where it can matter.
							if (sx > boxX0 - minDist && sx < boxX1 + minDist && sy > boxY0 - minDist && sy < boxY1 + minDist)
							{
								int num = 0;
								const float ax = x1 - x0, ay = y1 - y0;
								const float bx = x0 - 2.0f * x1 + x2, by = y0 - 2.0f * y1 + y2;
								const float mx = x0 - sx, my = y0 - sy;
								float res[3] = { 0.0f, 0.0f, 0.0f };
								if (pre == 0.0f)
								{
									// A degenerate curve: the quadratic formula, or a line when a is 0 too.
									const float a = 3.0f * (ax * bx + ay * by);
									const float b = 2.0f * (ax * ax + ay * ay) + (mx * bx + my * by);
									const float c = mx * ax + my * ay;
									if (a == 0.0f)
									{
										if (b != 0.0f)
											res[num++] = -c / b;
									}
									else
									{
										const float discriminant = b * b - 4.0f * a * c;
										if (discriminant >= 0.0f)
										{
											const float root = std::sqrt(discriminant);
											res[0] = (-b - root) / (2.0f * a);
											res[1] = (-b + root) / (2.0f * a);
											num = 2;
										}
									}
								}
								else
								{
									const float b = 3.0f * (ax * bx + ay * by) * pre;
									const float c = (2.0f * (ax * ax + ay * ay) + (mx * bx + my * by)) * pre;
									const float d = (mx * ax + my * ay) * pre;
									num = stbtt__solve_cubic(b, c, d, res);
								}
								const float dist2 = (x0 - sx) * (x0 - sx) + (y0 - sy) * (y0 - sy);
								if (dist2 < minDist * minDist)
									minDist = std::sqrt(dist2);
								for (int root = 0; root < num && root < 3; ++root)
								{
									const float t = res[root];
									if (t < 0.0f || t > 1.0f)
										continue;
									const float it = 1.0f - t;
									const float px = it * it * x0 + 2.0f * t * it * x1 + t * t * x2;
									const float py = it * it * y0 + 2.0f * t * it * y1 + t * t * y2;
									const float rootDist2 = (px - sx) * (px - sx) + (py - sy) * (py - sy);
									if (rootDist2 < minDist * minDist)
										minDist = std::sqrt(rootDist2);
								}
							}
						}
					}
					// Outside the shape the distance is negative.
					if (winding == 0)
						minDist = -minDist;
					const float value = std::clamp(static_cast<float>(onEdgeValue) + pixelDistanceScale * minDist, 0.0f, 255.0f);
					data[static_cast<size_t>(y - y0Box) * width + static_cast<size_t>(x - x0Box)] = static_cast<unsigned char>(value);
				}
			}
			return data;
		}

	}

	TrueTypeFont::TrueTypeFont(Scope<State> state)
		: m_State(std::move(state))
	{
	}

	TrueTypeFont::~TrueTypeFont() = default;

	TrueTypeFont::TrueTypeFont(TrueTypeFont&& other) noexcept = default;

	TrueTypeFont& TrueTypeFont::operator=(TrueTypeFont&& other) noexcept = default;

	int32_t TrueTypeFont::GetUnitsPerEm() const
	{
		return m_State->UnitsPerEm;
	}

	TrueTypeVerticalMetrics TrueTypeFont::GetVerticalMetrics() const
	{
		int ascent = 0;
		int descent = 0;
		int lineGap = 0;
		stbtt_GetFontVMetrics(&m_State->Info, &ascent, &descent, &lineGap);
		return { .Ascent = ascent, .Descent = descent, .LineGap = lineGap };
	}

	std::optional<int32_t> TrueTypeFont::FindGlyph(uint32_t codepoint) const
	{
		if (codepoint > static_cast<uint32_t>(INT_MAX))
			return std::nullopt;
		const int glyph = stbtt_FindGlyphIndex(&m_State->Info, static_cast<int>(codepoint));
		// A glyph index beyond maxp.numGlyphs would make stb_truetype read past the metrics; treat it as missing.
		if (glyph <= 0 || glyph >= m_State->GlyphCount)
			return std::nullopt;
		return glyph;
	}

	TrueTypeGlyphMetrics TrueTypeFont::GetGlyphMetrics(int32_t glyph) const
	{
		ENGINE_CORE_ASSERT(glyph >= 0 && glyph < m_State->GlyphCount, "glyph {} of a font with {} glyphs", glyph, m_State->GlyphCount);
		int advance = 0;
		int leftSideBearing = 0;
		stbtt_GetGlyphHMetrics(&m_State->Info, glyph, &advance, &leftSideBearing);
		return { .Advance = advance, .LeftSideBearing = leftSideBearing };
	}

	bool TrueTypeFont::IsGlyphEmpty(int32_t glyph) const
	{
		ENGINE_CORE_ASSERT(glyph >= 0 && glyph < m_State->GlyphCount, "glyph {} of a font with {} glyphs", glyph, m_State->GlyphCount);
		return stbtt_IsGlyphEmpty(&m_State->Info, glyph) != 0;
	}

	Result<TrueTypeGlyphSdf> TrueTypeFont::RenderGlyphSdf(int32_t glyph, float pixelsPerEm, int32_t padding, float valuePerPixel) const
	{
		ENGINE_CORE_ASSERT(glyph >= 0 && glyph < m_State->GlyphCount, "glyph {} of a font with {} glyphs", glyph, m_State->GlyphCount);
		ENGINE_CORE_ASSERT(pixelsPerEm > 0.0f && padding >= 0 && valuePerPixel > 0.0f, "invalid distance field parameters");
		if (IsGlyphEmpty(glyph))
			return TrueTypeGlyphSdf{};

		constexpr unsigned char OnEdgeValue = 128;
		const float scale = stbtt_ScaleForMappingEmToPixels(&m_State->Info, pixelsPerEm);
		int x0 = 0;
		int y0 = 0;
		int x1 = 0;
		int y1 = 0;
		stbtt_GetGlyphBitmapBoxSubpixel(&m_State->Info, glyph, scale, scale, 0.0f, 0.0f, &x0, &y0, &x1, &y1);
		// An outline too small to cover a pixel at this size has an empty bounding box; it draws nothing.
		if (x0 == x1 || y0 == y1)
			return TrueTypeGlyphSdf{};
		x0 -= padding;
		y0 -= padding;
		x1 += padding;
		y1 += padding;
		if (x1 <= x0 || y1 <= y0)
			return MakeError(ErrorCode::ImportFailed, "glyph {} has an inverted bounding box", glyph);
		// The glyph header's bounding box sizes the field, and the field costs work for every pixel and segment: a box no
		// atlas could hold is refused before anything is allocated.
		if (x1 - x0 > MaxGlyphBitmapSide || y1 - y0 > MaxGlyphBitmapSide)
		{
			return MakeError(ErrorCode::ImportFailed, "glyph {} would be a {}x{} pixel distance field; at most {} pixels on a side are supported", glyph,
				x1 - x0, y1 - y0, MaxGlyphBitmapSide);
		}

		stbtt_vertex* shapeVertices = nullptr;
		const int shapeCount = stbtt_GetGlyphShape(&m_State->Info, glyph, &shapeVertices);
		const std::unique_ptr<stbtt_vertex, StbShapeDeleter> shape(shapeVertices, StbShapeDeleter{ &m_State->Info });
		if (shape == nullptr || shapeCount <= 0)
			return MakeError(ErrorCode::ImportFailed, "stb_truetype could not read the outline of glyph {}", glyph);
		std::vector<stbtt_vertex> vertices = Utils::ReplaceCubics(std::span<const stbtt_vertex>(shape.get(), static_cast<size_t>(shapeCount)));
		if (vertices.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
			return MakeError(ErrorCode::ImportFailed, "the outline of glyph {} has too many segments", glyph);
		const std::vector<unsigned char> field = Utils::ComputeGlyphSdf(vertices, scale, x0, y0, x1, y1, OnEdgeValue, valuePerPixel);

		TrueTypeGlyphSdf sdf;
		sdf.Width = static_cast<uint32_t>(x1 - x0);
		sdf.Height = static_cast<uint32_t>(y1 - y0);
		sdf.OffsetX = x0;
		sdf.OffsetY = y0;
		const auto* pixels = reinterpret_cast<const std::byte*>(field.data());
		sdf.Pixels.assign(pixels, pixels + field.size());
		return sdf;
	}

	Result<TrueTypeFont> TrueTypeFont::Open(std::span<const std::byte> data)
	{
		ENGINE_TRY(Utils::ValidateTrueTypeTables(data));
		Scope<State> state = CreateScope<State>();
		if (stbtt_InitFont(&state->Info, Utils::AsFontBytes(data), 0) == 0)
			return MakeError(ErrorCode::ImportFailed, "stb_truetype could not read the font (unsupported outlines or tables)");
		state->UnitsPerEm = Utils::ReadUnitsPerEm(state->Info);
		state->GlyphCount = state->Info.numGlyphs;
		return TrueTypeFont(std::move(state));
	}

}
