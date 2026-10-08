#include "TestsPCH.h"

#include "Engine/Renderer/RenderSnapshot.h"

#include <glm/glm.hpp>

// The render snapshot's reverse-Z projection (Architecture §8.3). Skipped skeleton of the M7 contract
// (Docs/Decisions/0012-m7-decisions.md decision 6): stream B implements the projection and removes the skip.

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("RenderSnapshot: the reverse-Z projection maps the near plane to depth 1 for both projection kinds" * doctest::skip(true))
		{
			// §8.3: perspective m[0][0] = f / aspect, m[1][1] = f, m[2][3] = -1, m[3][2] = near, infinite far plane.
			const glm::mat4 perspective = ComputeReverseZProjection(RenderProjection::Perspective, 90.0f, 10.0f, 0.1f, 1000.0f, 200, 100);
			CHECK(perspective[0][0] == doctest::Approx(0.5f));
			CHECK(perspective[1][1] == doctest::Approx(1.0f));
			CHECK(perspective[2][3] == doctest::Approx(-1.0f));
			CHECK(perspective[3][2] == doctest::Approx(0.1f));
			CHECK(perspective[2][2] == 0.0f);
			// The near plane maps to depth 1.
			const glm::vec4 nearPoint = perspective * glm::vec4(0.0f, 0.0f, -0.1f, 1.0f);
			CHECK(nearPoint.z / nearPoint.w == doctest::Approx(1.0f));

			// Orthographic: d = (far + zView) / (far - near), 1 at the near plane, 0 at the far plane.
			const glm::mat4 orthographic = ComputeReverseZProjection(RenderProjection::Orthographic, 60.0f, 5.0f, 1.0f, 11.0f, 100, 100);
			CHECK((orthographic * glm::vec4(0.0f, 0.0f, -1.0f, 1.0f)).z == doctest::Approx(1.0f));
			CHECK((orthographic * glm::vec4(0.0f, 0.0f, -11.0f, 1.0f)).z == doctest::Approx(0.0f));
			CHECK((orthographic * glm::vec4(0.0f, 5.0f, -2.0f, 1.0f)).y == doctest::Approx(1.0f));
		}
	}

}
