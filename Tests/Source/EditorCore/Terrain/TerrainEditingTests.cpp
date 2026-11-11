#include "TestsPCH.h"

#include "EditorCore/Terrain/TerrainEditing.h"

#include <cstdint>
#include <vector>

// Terrain sculpting and painting (M26, C26.1, C-TERR-EDIT; Docs/Decisions/0037-m26-contract.md decision 23, stream T-E): the
// brush's pure rules, skipped until stream T-E implements them. The edit's round trip (terrain.edit's bytes equal the editor
// tool's, and undo restores them) is stream T-E's EditorCore case and Python test.

namespace Engine {

	namespace {

		// A 9 x 9 flat terrain over 8 x 8 metres, 65535 samples = 8 metres (a sample per 8 / 65535 m).
		constexpr TerrainExtent Extent{ .Size = glm::vec2(8.0f), .HeightScale = 8.0f };

		TerrainStroke MakeStroke(TerrainTool tool)
		{
			TerrainStroke stroke;
			stroke.Tool = tool;
			stroke.Points = { glm::vec2(4.0f, 4.0f) };
			stroke.Radius = 2.0f;
			stroke.Strength = 1.0f;
			return stroke;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("TerrainEditing: a Raise dab lifts the samples under the brush by its falloff and leaves the rest" * doctest::skip(true))
		{
			HeightfieldData heightfield = MakeFlatHeightfield(9);
			ApplyTerrainSculpt(heightfield, Extent, MakeStroke(TerrainTool::Raise));
			// The centre sample (4, 4) rises by Strength * w(0) = 1 metre: 65535 / 8 samples, rounded.
			CHECK(heightfield.Heights[4 + 9 * 4] == 8192);
			// One metre off the centre, w = (1 - (1 / 2)^2)^2 = 0.5625: 0.5625 metres.
			CHECK(heightfield.Heights[5 + 9 * 4] == 4608);
			// Beyond the radius nothing changes.
			CHECK(heightfield.Heights[7 + 9 * 4] == 0);
			CHECK(heightfield.Heights[0] == 0);
			// Lower undoes Raise exactly, and a second application of the same stroke gives the same bytes.
			HeightfieldData again = heightfield;
			ApplyTerrainSculpt(heightfield, Extent, MakeStroke(TerrainTool::Lower));
			CHECK(heightfield.Heights == MakeFlatHeightfield(9).Heights);
			ApplyTerrainSculpt(again, Extent, MakeStroke(TerrainTool::Lower));
			CHECK(again.Heights == heightfield.Heights);
		}

		TEST_CASE("TerrainEditing: Flatten and Smooth move samples towards their targets, never past them" * doctest::skip(true))
		{
			HeightfieldData heightfield = MakeFlatHeightfield(9);
			TerrainStroke flatten = MakeStroke(TerrainTool::Flatten);
			flatten.Height = 2.0f;
			ApplyTerrainSculpt(heightfield, Extent, flatten);
			CHECK(heightfield.Heights[4 + 9 * 4] == 16384);
			CHECK(heightfield.Heights[5 + 9 * 4] < 16384);
			ApplyTerrainSculpt(heightfield, Extent, MakeStroke(TerrainTool::Smooth));
			CHECK(heightfield.Heights[4 + 9 * 4] < 16384);
			CHECK(heightfield.Heights[4 + 9 * 4] > 0);
		}

		TEST_CASE("TerrainEditing: a Paint dab raises its layer's weight and keeps every texel's weights summing to 255" * doctest::skip(true))
		{
			TerrainSplatImage splat;
			splat.Width = 8;
			splat.Height = 8;
			for (uint32_t texel = 0; texel < 64; ++texel)
				splat.Texels.insert(splat.Texels.end(), { 255, 0, 0, 0 });
			TerrainStroke paint = MakeStroke(TerrainTool::Paint);
			paint.Layer = 2;
			paint.Strength = 0.5f;
			ApplyTerrainPaint(splat, Extent, paint);
			// Texel (3, 3) covers (3.5, 3.5): inside the brush, so blue rose and red fell.
			const size_t centre = (3 + 8 * 3) * 4;
			CHECK(splat.Texels[centre + 2] > 0);
			CHECK(splat.Texels[centre + 0] < 255);
			for (size_t texel = 0; texel < 64; ++texel)
			{
				CAPTURE(texel);
				const uint32_t sum = uint32_t{ splat.Texels[texel * 4] } + splat.Texels[texel * 4 + 1] + splat.Texels[texel * 4 + 2] + splat.Texels[texel * 4 + 3];
				CHECK(sum == 255);
			}
			// A corner texel lies outside the brush.
			CHECK(splat.Texels[0] == 255);
		}
	}

}
