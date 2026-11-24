#include "TestsPCH.h"

#include "Engine/Scene/Wind.h"

#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/WindComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/SceneTestFixture.h"

#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <string>

// The scene's wind (M46; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3F): SampleWind in double
// precision with IEEE-exact operations, floor and sqrt, so its bits are committed and the Debug and Release unit runs both
// check them (decision 6), and FindSceneWind.

namespace Engine {

	namespace {

		// A gale from the south-west with every field away from its default.
		WindComponent MakeGale()
		{
			WindComponent wind;
			wind.Direction = glm::vec3(3.0f, 7.0f, 4.0f);
			wind.Strength = 18.0f;
			wind.GustStrength = 6.5f;
			wind.GustFrequency = 0.35f;
			wind.Turbulence = 0.6f;
			return wind;
		}

		std::string Bits(const glm::dvec3& value)
		{
			return std::format("{:#018x} {:#018x} {:#018x}", std::bit_cast<uint64_t>(value.x), std::bit_cast<uint64_t>(value.y),
				std::bit_cast<uint64_t>(value.z));
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Wind: Sample gives the committed velocity at fixed points and times")
		{
			const WindComponent gale = MakeGale();
			// The direction is the horizontal part normalized: (3, 0, 4) / 5.
			CHECK(GetWindDirection(gale) == glm::dvec3(0.6, 0.0, 0.8));
			CHECK(GetWindGustWavelength(gale) == static_cast<double>(18.0f) / static_cast<double>(0.35f));
			// The gust wave: 0 at whole numbers, 1 halfway, the cubic's 0.15625 a quarter of the way at 0.25.
			CHECK(WindGustWave(0.0) == 0.0);
			CHECK(WindGustWave(3.5) == 1.0);
			CHECK(WindGustWave(-0.75) == 0.5);
			CHECK(WindGustWave(0.125) == 0.15625);

			struct Sample
			{
				glm::dvec3 Position{ 0.0 };
				double Time = 0.0;
				std::string_view Expected;
			};
			const std::array<Sample, 3> samples = { {
				{ glm::dvec3(0.0), 0.0, "0x4025999999999999 0x0000000000000000 0x402ccccccccccccd" },
				{ glm::dvec3(12.5, 3.0, -40.25), 7.25, "0x4031293010885a54 0x0000000000000000 0x40319b7fd01ffa12" },
				{ glm::dvec3(-310.0, 0.0, 95.75), 1234.5, "0x40270a29f9a6645c 0x0000000000000000 0x4030ec13b0988f43" },
			} };
			for (const Sample& sample : samples)
			{
				const glm::dvec3 velocity = SampleWind(gale, sample.Position, sample.Time);
				CAPTURE(sample.Time);
				CHECK(Bits(velocity) == sample.Expected);
				// No vertical part, and the speed stays within the steady speed plus the gusts and their turn.
				CHECK(velocity.y == 0.0);
				CHECK(glm::length(velocity) <= 18.0 + 6.5 * 1.7);
			}

			// A calm wind is still, and a wind without gusts blows at its Strength along its direction everywhere.
			WindComponent calm = gale;
			calm.Strength = 0.0f;
			calm.GustStrength = 0.0f;
			CHECK(SampleWind(calm, glm::dvec3(5.0, 1.0, -3.0), 42.0) == glm::dvec3(0.0));
			WindComponent steady = gale;
			steady.GustStrength = 0.0f;
			CHECK(SampleWind(steady, glm::dvec3(-8.0, 0.0, 300.0), 9.5) == glm::dvec3(0.6, 0.0, 0.8) * static_cast<double>(18.0f));
			// Gusts travel with the wind: a point one wavelength downwind sees the same gust at the same time.
			const double wavelength = GetWindGustWavelength(gale);
			const glm::dvec3 upwind = SampleWind(gale, glm::dvec3(0.0), 3.0);
			const glm::dvec3 downwind = SampleWind(gale, glm::dvec3(0.6, 0.0, 0.8) * wavelength * 4.0, 3.0);
			CHECK(glm::length(upwind - downwind) < 1e-9);
			// A Direction without a horizontal part, which the component refuses, blows along +X.
			WindComponent upward = gale;
			upward.Direction = glm::vec3(0.0f, 1.0f, 0.0f);
			CHECK(GetWindDirection(upward) == glm::dvec3(1.0, 0.0, 0.0));
		}

		TEST_CASE("Wind: FindSceneWind returns the scene's effectively enabled Wind")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			CHECK(FindSceneWind(scene) == nullptr);
			const Entity first = scene.CreateEntity("First");
			first.AddComponent<WindComponent>(MakeGale());
			REQUIRE(FindSceneWind(scene) != nullptr);
			CHECK(FindSceneWind(scene)->Strength == 18.0f);
			// A Wind under a disabled hierarchy does not blow.
			first.SetActive(false);
			REQUIRE(first.HasComponent<HierarchyDisabledTag>());
			CHECK(FindSceneWind(scene) == nullptr);
		}
	}

}
