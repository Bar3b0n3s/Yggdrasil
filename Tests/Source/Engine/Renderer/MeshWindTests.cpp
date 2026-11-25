#include "TestsPCH.h"

#include "Engine/Renderer/MeshWind.h"

#include <cmath>
#include <cstdint>

// The wind's GPU data (M46; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3G): the view's
// WindConstants from the snapshot's RenderWind, and an instance record's WindResponse from its material's wind block.

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("MeshWind: a wind's frame holds its horizontal direction, its gust wavelength and its waves' phases at its time")
		{
			RenderWind wind;
			wind.Enabled = true;
			wind.Direction = glm::vec3(3.0f, 7.0f, -4.0f);
			wind.Strength = 8.0f;
			wind.GustStrength = 2.0f;
			wind.GustFrequency = 0.25f;
			wind.Turbulence = 0.5f;
			wind.Time = 1000003.5;
			const WindFrame frame = MakeWindFrame(wind);
			// Only the horizontal part counts, normalized.
			CHECK(frame.Direction.x == doctest::Approx(0.6f));
			CHECK(frame.Direction.y == doctest::Approx(-0.8f));
			CHECK(frame.Strength == 8.0f);
			CHECK(frame.GustStrength == 2.0f);
			CHECK(frame.Turbulence == 0.5f);
			// The gusts travel with the wind: GustFrequency / Strength per metre.
			CHECK(frame.InverseGustWavelength == doctest::Approx(0.25f / 8.0f));
			// The phases are wrapped in double precision, so a long session keeps them exact: 0.25 x 1000003.5 = 250000.875.
			CHECK(frame.GustPhase == 0.875f);
			CHECK(frame.HalfGustPhase == 0.4375f);
			CHECK(frame.BranchPhase == doctest::Approx(std::fmod(WindBranchFrequency * 1000003.5, 1.0)));
			CHECK(frame.FlutterPhase == doctest::Approx(std::fmod(WindFlutterFrequency * 1000003.5, 1.0)));
			for (const float phase : { frame.GustPhase, frame.HalfGustPhase, frame.BranchPhase, frame.FlutterPhase })
			{
				CHECK(phase >= 0.0f);
				CHECK(phase < 1.0f);
			}

			// A direction without a horizontal part blows along +X; no strength or frequency gives no wavelength.
			wind.Direction = glm::vec3(0.0f, 1.0f, 0.0f);
			wind.Strength = 0.0f;
			const WindFrame upward = MakeWindFrame(wind);
			CHECK(upward.Direction == glm::vec2(1.0f, 0.0f));
			CHECK(upward.InverseGustWavelength == 0.0f);
			// A disabled wind moves nothing.
			wind.Enabled = false;
			CHECK(MakeWindFrame(wind) == WindFrame());
		}

		TEST_CASE("MeshWind: the constants hold the current and the previous frame")
		{
			RenderWind wind;
			wind.Enabled = true;
			wind.Time = 2.0;
			const WindFrame current = MakeWindFrame(wind);
			wind.Time = 1.5;
			wind.Strength = 9.0f;
			const WindFrame previous = MakeWindFrame(wind);
			const WindConstants constants = MakeWindConstants(current, previous);
			CHECK(constants.Strength == current.Strength);
			CHECK(constants.GustPhase == current.GustPhase);
			CHECK(constants.FlutterPhase == current.FlutterPhase);
			CHECK(constants.PreviousStrength == 9.0f);
			CHECK(constants.PreviousGustPhase == previous.GustPhase);
			CHECK(constants.PreviousBranchPhase == previous.BranchPhase);
			CHECK(constants.PreviousDirection.x == previous.Direction.x);
		}

		TEST_CASE("MeshWind: a material's wind block packs into four unorm8 bytes over its ranges")
		{
			MaterialWind wind;
			wind.Bend = 1.0f;
			wind.Branch = 0.5f;
			wind.Flutter = WindFlutterRange;
			wind.Stiffness = 0.0f;
			CHECK(PackWindResponse(wind) == (255U | (128U << 8) | (255U << 16)));
			// Each is clamped to its range; a value that is not finite packs as 0.
			wind.Bend = 4.0f;
			wind.Branch = -1.0f;
			wind.Flutter = std::nanf("");
			wind.Stiffness = 1.0f;
			CHECK(PackWindResponse(wind) == (255U | (255U << 24)));
			// The defaults: Bend 0.1, Branch 0.1, Flutter 0.02 of 0.25 and Stiffness 0.5.
			const uint32_t defaults = PackWindResponse(MaterialWind());
			CHECK((defaults & 0xFFU) == 26U);
			CHECK(((defaults >> 8) & 0xFFU) == 26U);
			CHECK(((defaults >> 16) & 0xFFU) == 20U);
			CHECK((defaults >> 24) == 128U);
		}
	}

}
