#include "TestsPCH.h"
#include "Engine/Renderer/WaterGeometry.h"

#include "Engine/Physics/WaterWaves.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <set>
#include <utility>
#include <vector>

// The Water pass's clipmap and records (M47, Docs/Decisions/0065-m47-water.md decision 3 and the W47.2 record;
// Renderer/WaterGeometry.h), pure CPU.

namespace Engine {

	namespace {

		// Twice the signed area of a triangle of cells in (x, z): negative when it winds counter-clockwise seen from +Y.
		int64_t SignedArea(const glm::i16vec2& a, const glm::i16vec2& b, const glm::i16vec2& c)
		{
			return (static_cast<int64_t>(b.x) - a.x) * (static_cast<int64_t>(c.y) - a.y) - (static_cast<int64_t>(b.y) - a.y) * (static_cast<int64_t>(c.x) - a.x);
		}

		// The triangles' doubled area, after checking each winds counter-clockwise seen from +Y and has no unused vertex.
		int64_t CheckMesh(const WaterGridMesh& mesh)
		{
			REQUIRE(mesh.Indices.size() % 3 == 0);
			std::vector<bool> used(mesh.Cells.size(), false);
			int64_t area = 0;
			uint32_t clockwise = 0;
			for (size_t first = 0; first < mesh.Indices.size(); first += 3)
			{
				const std::array<uint32_t, 3> corners = { mesh.Indices[first], mesh.Indices[first + 1], mesh.Indices[first + 2] };
				for (const uint32_t corner : corners)
				{
					REQUIRE(corner < mesh.Cells.size());
					used[corner] = true;
				}
				const int64_t signedArea = SignedArea(mesh.Cells[corners[0]], mesh.Cells[corners[1]], mesh.Cells[corners[2]]);
				clockwise += signedArea >= 0 ? 1U : 0U;
				area -= signedArea;
			}
			CHECK(clockwise == 0);
			CHECK(std::ranges::all_of(used, [](bool value)
			{
				return value;
			}));
			return area;
		}

		// The edges a mesh's triangles have on one straight line between `from` and `to` along it: each pair of vertices on it
		// that a triangle joins, as (smaller, larger) coordinates along the line.
		std::set<std::pair<int32_t, int32_t>> GetEdgesOnLine(const WaterGridMesh& mesh, bool alongX, int32_t line, int32_t from, int32_t to)
		{
			std::set<std::pair<int32_t, int32_t>> edges;
			for (size_t first = 0; first < mesh.Indices.size(); first += 3)
			{
				for (size_t corner = 0; corner < 3; ++corner)
				{
					const glm::i16vec2 a = mesh.Cells[mesh.Indices[first + corner]];
					const glm::i16vec2 b = mesh.Cells[mesh.Indices[first + (corner + 1) % 3]];
					const int32_t fixedA = alongX ? a.y : a.x;
					const int32_t fixedB = alongX ? b.y : b.x;
					if (fixedA != line || fixedB != line)
						continue;
					const int32_t start = std::min<int32_t>(alongX ? a.x : a.y, alongX ? b.x : b.y);
					const int32_t end = std::max<int32_t>(alongX ? a.x : a.y, alongX ? b.x : b.y);
					if (start >= from && end <= to)
						edges.emplace(start, end);
				}
			}
			return edges;
		}

		WaterItem MakeWater(RenderWaterShape shape, const glm::mat4& world, std::span<const WaterWave> waves, double time)
		{
			const WaterSurfaceDesc surface = ResolveWaterSurface(waves, 1.0, glm::dvec2(0.0), MakeWaterFrame(glm::dmat4(world)));
			const std::array<double, MaxWaterSurfaceWaves> phases = ComputeWaterWavePhases(surface, time);
			WaterItem item;
			item.World = world;
			item.Shape = shape;
			item.WaveCount = surface.WaveCount;
			for (uint32_t index = 0; index < surface.WaveCount; ++index)
			{
				const WaterSurfaceWave& wave = surface.Waves[index];
				item.Waves[index] = WaterWaveItem{ .Direction = glm::vec2(wave.Direction),
					.WaveNumber = static_cast<float>(wave.WaveNumber),
					.Amplitude = static_cast<float>(wave.Amplitude),
					.Steepness = static_cast<float>(wave.Steepness),
					.Phase = static_cast<float>(phases[index]) };
			}
			return item;
		}

		// Common/Waves.slang's displacement of rest point `rest` (an offset from the record's Origin) in double, unfaded.
		glm::dvec3 DisplaceFromRecord(const WaterRecord& record, const glm::dvec2& rest)
		{
			glm::dvec3 point(rest.x + static_cast<double>(record.Origin.x), static_cast<double>(record.BaseHeight), rest.y + static_cast<double>(record.Origin.y));
			for (uint32_t index = 0; index < record.WaveCount; ++index)
			{
				const glm::dvec4 wave(record.Waves[index]);
				const glm::dvec4 shape(record.WavePhases[index]);
				const double phase = shape.y + wave.z * (wave.x * rest.x + wave.y * rest.y);
				point.x += shape.x * wave.w * wave.x * std::cos(phase);
				point.z += shape.x * wave.w * wave.y * std::cos(phase);
				point.y += wave.w * std::sin(phase);
			}
			return point;
		}

		constexpr std::array<WaterWave, 3> Swell = {
			WaterWave{ .Direction = 15.0f, .Wavelength = 40.0f, .Amplitude = 0.6f, .Steepness = 0.7f, .Speed = 0.0f, .Phase = 10.0f },
			WaterWave{ .Direction = 130.0f, .Wavelength = 7.5f, .Amplitude = 0.08f, .Steepness = 0.5f, .Speed = 0.0f, .Phase = 300.0f },
			WaterWave{ .Direction = 250.0f, .Wavelength = 0.9f, .Amplitude = 0.01f, .Steepness = 0.4f, .Speed = 1.5f, .Phase = 45.0f },
		};

	}

	TEST_SUITE("Renderer")
	{
		// Level 0 is centred on the camera to within its snapping, each level's corner is a multiple of twice its cell, each
		// ring's hole (16 or 17 cells in on each axis) is exactly the level below, and the camera lies at least 30 cells of each
		// level inside it, for cameras near the origin, far out, on either side of zero and on a snapping boundary.
		TEST_CASE("WaterGeometry: the levels nest around the camera, each snapped to twice its cell with its hole on the level below")
		{
			constexpr std::array<glm::dvec2, 6> Cameras = { glm::dvec2(0.0), glm::dvec2(0.37, -0.12), glm::dvec2(-1234.5678, 2500.125),
				glm::dvec2(2500.0, -2500.0), glm::dvec2(-0.25, 0.5), glm::dvec2(98765.4321, -45678.9) };
			for (const glm::dvec2& camera : Cameras)
			{
				CAPTURE(camera.x);
				CAPTURE(camera.y);
				const std::array<WaterLevelPlacement, WaterLevelCount> levels = PlaceWaterLevels(camera);
				for (uint32_t index = 0; index < WaterLevelCount; ++index)
				{
					CAPTURE(index);
					const WaterLevelPlacement& level = levels[index];
					CHECK(level.CellSize == static_cast<double>(WaterFinestCellSize) * std::ldexp(1.0, static_cast<int>(index)));
					const glm::dvec2 steps = level.Corner / (2.0 * level.CellSize);
					CHECK(steps.x == std::floor(steps.x));
					CHECK(steps.y == std::floor(steps.y));
					const glm::dvec2 inside = (camera - level.Corner) / level.CellSize;
					CHECK(inside.x >= 30.0);
					CHECK(inside.y >= 30.0);
					CHECK(inside.x <= static_cast<double>(WaterGridCells) - 30.0);
					CHECK(inside.y <= static_cast<double>(WaterGridCells) - 30.0);
					if (index == 0)
						continue;
					CHECK((level.Hole.x == 16 || level.Hole.x == 17));
					CHECK((level.Hole.y == 16 || level.Hole.y == 17));
					const glm::dvec2 hole = level.Corner + glm::dvec2(level.Hole) * level.CellSize;
					CHECK(hole == levels[index - 1].Corner);
					CHECK(GetWaterLevelMesh(level, index) == 1 + (level.Hole.x - 16) + 2 * (level.Hole.y - 16));
				}
				CHECK(GetWaterLevelMesh(levels[0], 0) == 0);
			}
		}

		// Level 0's mesh covers its whole grid and each ring its grid less its 32 x 32 hole, every triangle wound counter-clockwise
		// seen from +Y. The outer edge keeps only its even vertices, each segment two cells long, so it meets the next level's
		// coarser hole edge; a ring's hole edge keeps every vertex, one cell apart, where the finer level's even ones meet it.
		TEST_CASE("WaterGeometry: each level's mesh covers its grid less its hole, its outer edge stitched to every other vertex")
		{
			constexpr int64_t Cells = WaterGridCells;
			for (uint32_t meshIndex = 0; meshIndex < WaterSkirtMesh; ++meshIndex)
			{
				CAPTURE(meshIndex);
				const WaterGridMesh mesh = BuildWaterMesh(meshIndex);
				const int64_t hole = meshIndex == 0 ? 0 : static_cast<int64_t>(WaterHoleCells) * WaterHoleCells;
				CHECK(CheckMesh(mesh) == 2 * (Cells * Cells - hole));
				for (const auto& [alongX, line] : { std::pair{ true, 0 }, std::pair{ true, static_cast<int32_t>(Cells) }, std::pair{ false, 0 },
						 std::pair{ false, static_cast<int32_t>(Cells) } })
				{
					std::set<std::pair<int32_t, int32_t>> expected;
					for (int32_t from = 0; from < Cells; from += 2)
						expected.emplace(from, from + 2);
					CHECK(GetEdgesOnLine(mesh, alongX, line, 0, static_cast<int32_t>(Cells)) == expected);
				}
				if (meshIndex == 0)
					continue;
				const int32_t holeX = 16 + static_cast<int32_t>((meshIndex - 1) % 2);
				const int32_t holeZ = 16 + static_cast<int32_t>((meshIndex - 1) / 2);
				const int32_t span = static_cast<int32_t>(WaterHoleCells);
				for (const auto& [alongX, line, first] : { std::tuple{ true, holeZ, holeX }, std::tuple{ true, holeZ + span, holeX },
						 std::tuple{ false, holeX, holeZ }, std::tuple{ false, holeX + span, holeZ } })
				{
					std::set<std::pair<int32_t, int32_t>> expected;
					for (int32_t from = first; from < first + span; ++from)
						expected.emplace(from, from + 1);
					CHECK(GetEdgesOnLine(mesh, alongX, line, first, first + span) == expected);
				}
			}
		}

		// The skirt's inner edge is the last level's stitched outer edge (its even cells), its outer edge the same points pushed
		// out from the level's centre to at least WaterHorizonDistance from any camera the level holds, and between them it is
		// a closed band of counter-clockwise quads.
		TEST_CASE("WaterGeometry: the skirt meets the last level's outer edge and reaches the horizon")
		{
			const WaterGridMesh skirt = BuildWaterMesh(WaterSkirtMesh);
			const int32_t scale = GetWaterSkirtScale();
			const auto cells = static_cast<int64_t>(WaterGridCells);
			const int64_t outer = cells * scale;
			CHECK(CheckMesh(skirt) == 2 * (outer * outer - cells * cells));
			std::set<std::pair<int32_t, int32_t>> inner;
			for (const glm::i16vec2& cell : skirt.Cells)
			{
				if (cell.x >= 0 && cell.x <= cells && cell.y >= 0 && cell.y <= cells)
				{
					CHECK((cell.x % 2 == 0 && cell.y % 2 == 0));
					inner.emplace(cell.x, cell.y);
				}
			}
			CHECK(inner.size() == 4 * WaterHoleCells);
			const double lastCell = static_cast<double>(WaterFinestCellSize) * std::ldexp(1.0, static_cast<int>(WaterLevelCount - 1));
			for (const glm::dvec2& camera : { glm::dvec2(0.0), glm::dvec2(-3333.3, 1717.17) })
			{
				const std::array<WaterLevelPlacement, WaterLevelCount> levels = PlaceWaterLevels(camera);
				const WaterSquare square = GetWaterLevelSquare(levels, WaterLevelCount);
				CHECK(levels[WaterLevelCount - 1].CellSize == lastCell);
				CHECK(camera.x - square.Min.x >= WaterHorizonDistance);
				CHECK(camera.y - square.Min.y >= WaterHorizonDistance);
				CHECK(square.Max.x - camera.x >= WaterHorizonDistance);
				CHECK(square.Max.y - camera.y >= WaterHorizonDistance);
				const WaterSquare last = GetWaterLevelSquare(levels, WaterLevelCount - 1);
				CHECK(last.Min == levels[WaterLevelCount - 1].Corner);
				CHECK(last.Max == levels[WaterLevelCount - 1].Corner + static_cast<double>(WaterGridCells) * lastCell);
			}
		}

		// A record's phases at the view's origin make Common/Waves.slang's displacement of a rest point the wave function's
		// (Physics/WaterWaves.h's DisplaceWaterSurfacePoint), within the float rounding of the snapshot's waves, for a turned water
		// 2.5 km from the origin; with a previous phase or pose the record flags them for the motion vectors.
		TEST_CASE("WaterGeometry: a water's record phases its waves at the view's origin as the wave function does")
		{
			const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(2500.0f, 3.0f, -1800.0f)) * glm::rotate(glm::mat4(1.0f), glm::radians(35.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			const double time = 41.75;
			WaterItem water = MakeWater(RenderWaterShape::Ocean, world, Swell, time);
			const glm::dvec2 camera(2610.3, -1690.6);
			const glm::dvec2 origin = PlaceWaterLevels(camera)[0].Corner;
			const WaterRecord record = MakeWaterRecord(water, origin, camera, 1);
			CHECK(record.WaveCount == 3);
			CHECK(record.WaterCount == 1);
			CHECK(record.Flags == 0);
			CHECK(record.BaseHeight == 3.0f);
			CHECK(record.Origin == glm::vec2(origin));
			CHECK(record.CameraOffset.x == doctest::Approx(camera.x - origin.x));
			CHECK(record.CameraOffset.y == doctest::Approx(camera.y - origin.y));
			const WaterSurfaceDesc surface = ResolveWaterSurface(Swell, 1.0, glm::dvec2(0.0), MakeWaterFrame(glm::dmat4(world)));
			double worst = 0.0;
			for (const glm::dvec2& offset : { glm::dvec2(0.0), glm::dvec2(3.75, -2.5), glm::dvec2(-7.25, 6.0), glm::dvec2(15.5, 15.5) })
			{
				const glm::dvec3 expected = DisplaceWaterSurfacePoint(surface, origin + offset, time);
				worst = std::max(worst, glm::length(DisplaceFromRecord(record, offset) - expected));
			}
			CHECK(worst < 1.0e-4);

			water.PreviousPhases = std::array<float, MaxWaterWaves>{};
			water.PreviousWorld = world;
			CHECK(MakeWaterRecord(water, origin, camera, 1).Flags == WaterRecordFlagAdvanced);
			for (uint32_t index = 0; index < MaxWaterWaves; ++index)
				(*water.PreviousPhases)[index] = water.Waves[index].Phase;
			CHECK(MakeWaterRecord(water, origin, camera, 1).Flags == 0);
			water.PreviousWorld = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.5f, 0.0f)) * world;
			const WaterRecord moved = MakeWaterRecord(water, origin, camera, 1);
			CHECK(moved.Flags == WaterRecordFlagMoved);
			const glm::vec4 point = moved.PreviousFromCurrent * glm::vec4(2490.0f, 3.25f, -1810.0f, 1.0f);
			CHECK(point.x == doctest::Approx(2490.0).epsilon(1e-6));
			CHECK(point.y == doctest::Approx(2.75).epsilon(1e-4));
			CHECK(point.z == doctest::Approx(-1810.0).epsilon(1e-6));
		}

		// A Rectangle's record holds its frame (its base point from the view's origin, its turned local X axis, its half extents
		// scaled by its X and Z columns) and volume, and its footprint bounds hold its turned corners; an Ocean's bounds are the
		// whole plane. The look is the item's, kept within the registry's ranges (a non-finite value takes the default).
		TEST_CASE("WaterGeometry: a Rectangle's record holds its frame and volume, and the look stays within its ranges")
		{
			const glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, -2.0f, 20.0f)) * glm::rotate(glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f))
				* glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 3.0f));
			WaterItem pool = MakeWater(RenderWaterShape::Rectangle, world, {}, 0.0);
			pool.Size = glm::vec2(4.0f, 6.0f);
			pool.Depth = 3.0f;
			pool.Headroom = 7.0f;
			pool.Clarity = std::numeric_limits<float>::quiet_NaN();
			pool.Roughness = 5.0f;
			pool.FoamAmount = -1.0f;
			pool.PickId = 9;
			const glm::dvec2 origin(4.0, 12.0);
			const WaterRecord record = MakeWaterRecord(pool, origin, glm::dvec2(5.0, 13.0), 2, 3);
			CHECK(record.Flags == WaterRecordFlagRectangle);
			CHECK(record.FrameOrigin == glm::vec2(6.0f, 8.0f));
			const double angle = glm::radians(30.0);
			CHECK(record.AxisX.x == doctest::Approx(std::cos(angle)));
			CHECK(record.AxisX.y == doctest::Approx(-std::sin(angle)));
			CHECK(record.HalfExtents.x == doctest::Approx(4.0));
			CHECK(record.HalfExtents.y == doctest::Approx(9.0));
			CHECK(record.Depth == 3.0f);
			CHECK(record.Headroom == 7.0f);
			CHECK(record.BaseHeight == -2.0f);
			CHECK(record.WaveCount == 0);
			CHECK(record.WaterCount == 2);
			CHECK(record.PickId == 9);
			CHECK(record.PlanarSlot == 3);
			CHECK(record.ShallowColor.a == WaterItem{}.Clarity);
			CHECK(record.DeepColor.a == 1.0f);
			CHECK(record.Look.y == 0.0f);

			const WaterSquare bounds = GetWaterFootprintBounds(pool);
			const glm::dvec2 axisX(std::cos(angle), -std::sin(angle));
			const glm::dvec2 axisZ(std::sin(angle), std::cos(angle));
			for (const double x : { -4.0, 4.0 })
			{
				for (const double z : { -9.0, 9.0 })
				{
					const glm::dvec2 corner = glm::dvec2(10.0, 20.0) + x * axisX + z * axisZ;
					// Within the float rounding of the item's turned matrix.
					CHECK(corner.x >= bounds.Min.x - 1e-5);
					CHECK(corner.y >= bounds.Min.y - 1e-5);
					CHECK(corner.x <= bounds.Max.x + 1e-5);
					CHECK(corner.y <= bounds.Max.y + 1e-5);
				}
			}
			CHECK(bounds.Max.x - bounds.Min.x == doctest::Approx(2.0 * (4.0 * std::cos(angle) + 9.0 * std::sin(angle))));

			WaterItem ocean = pool;
			ocean.Shape = RenderWaterShape::Ocean;
			const WaterSquare everywhere = GetWaterFootprintBounds(ocean);
			CHECK(std::isinf(everywhere.Min.x));
			CHECK(everywhere.Max.y > 0.0);
			const WaterRecord oceanRecord = MakeWaterRecord(ocean, origin, glm::dvec2(5.0, 13.0), 2);
			CHECK(oceanRecord.Flags == 0);
			CHECK(oceanRecord.HalfExtents == glm::vec2(0.0f));
			CHECK(oceanRecord.PlanarSlot == WaterNoPlanarSlot);
		}

		// W47.3's Underwater pass records when the camera's near plane may lie under a water, judged on the snapshot alone: a
		// corner of the near plane below an Ocean's base plus its summed amplitudes, or below a Rectangle's (within its
		// Headroom), above its floor and inside its footprint widened by its waves' summed Q A.
		TEST_CASE("WaterGeometry: the near plane is under a water when it meets the region below the water's highest crest")
		{
			const auto camera = [](const glm::vec3& eye, const glm::vec3& target)
			{
				CameraData data;
				data.Position = eye;
				data.View = glm::lookAt(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
				data.NearClip = 0.1f;
				data.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.1f, 100.0f, 64, 64);
				return data;
			};
			// A swell of two waves: amplitudes 0.4 and 0.1 over a base at y = 1.
			const std::array<WaterWave, 2> waves = { WaterWave{ .Direction = 0.0f, .Wavelength = 30.0f, .Amplitude = 0.4f, .Steepness = 0.5f },
				WaterWave{ .Direction = 90.0f, .Wavelength = 12.0f, .Amplitude = 0.1f, .Steepness = 0.5f } };
			const WaterItem sea = MakeWater(RenderWaterShape::Ocean, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, 0.0f)), waves, 0.0);
			const std::array<WaterItem, 1> ocean = { sea };
			CHECK(IsNearPlaneUnderWater(ocean, camera(glm::vec3(0.0f, -3.0f, 0.0f), glm::vec3(0.0f, -3.0f, -1.0f))));
			// Within the crests' reach (1.5) the near plane may be under; above it, not.
			CHECK(IsNearPlaneUnderWater(ocean, camera(glm::vec3(0.0f, 1.45f, 0.0f), glm::vec3(0.0f, 1.45f, -1.0f))));
			CHECK_FALSE(IsNearPlaneUnderWater(ocean, camera(glm::vec3(0.0f, 1.7f, 0.0f), glm::vec3(0.0f, 1.7f, -1.0f))));
			CHECK_FALSE(IsNearPlaneUnderWater(ocean, camera(glm::vec3(0.0f, 20.0f, 0.0f), glm::vec3(0.0f, 0.0f, -20.0f))));
			CHECK_FALSE(IsNearPlaneUnderWater({}, camera(glm::vec3(0.0f, -3.0f, 0.0f), glm::vec3(0.0f, -3.0f, -1.0f))));

			// A calm pool 4 x 4 m at (10, 0, 0), 2 m deep with 3 m of headroom.
			WaterItem pool = MakeWater(RenderWaterShape::Rectangle, glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)), {}, 0.0);
			pool.Size = glm::vec2(4.0f);
			pool.Depth = 2.0f;
			pool.Headroom = 3.0f;
			const std::array<WaterItem, 1> pools = { pool };
			CHECK(IsNearPlaneUnderWater(pools, camera(glm::vec3(10.0f, -1.0f, 0.0f), glm::vec3(10.0f, -1.0f, -1.0f))));
			CHECK_FALSE(IsNearPlaneUnderWater(pools, camera(glm::vec3(10.0f, 0.5f, 0.0f), glm::vec3(10.0f, 0.5f, -1.0f))));
			CHECK_FALSE(IsNearPlaneUnderWater(pools, camera(glm::vec3(10.0f, -2.5f, 0.0f), glm::vec3(10.0f, -2.5f, -1.0f))));
			CHECK_FALSE(IsNearPlaneUnderWater(pools, camera(glm::vec3(13.0f, -1.0f, 0.0f), glm::vec3(13.0f, -1.0f, -1.0f))));
			// Its waves widen the footprint by their Q A: a 0.5 m wave at Q 0.8 reaches 0.4 m past the edge.
			const std::array<WaterWave, 1> ripple = { WaterWave{ .Direction = 0.0f, .Wavelength = 20.0f, .Amplitude = 0.5f, .Steepness = 0.8f } };
			WaterItem wavy = MakeWater(RenderWaterShape::Rectangle, glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)), ripple, 0.0);
			wavy.Size = glm::vec2(4.0f);
			wavy.Depth = 2.0f;
			wavy.Headroom = 3.0f;
			const std::array<WaterItem, 1> wavyPools = { wavy };
			CHECK(IsNearPlaneUnderWater(wavyPools, camera(glm::vec3(12.3f, -1.0f, 0.0f), glm::vec3(12.3f, -1.0f, -1.0f))));
			CHECK_FALSE(IsNearPlaneUnderWater(wavyPools, camera(glm::vec3(12.6f, -1.0f, 0.0f), glm::vec3(12.6f, -1.0f, -1.0f))));
		}
	}

}
