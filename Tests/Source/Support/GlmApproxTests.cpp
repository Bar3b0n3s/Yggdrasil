#include "TestsPCH.h"

#include "Support/GlmApprox.h"

#include <limits>

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("GlmApprox: ApproxEqual compares every component within epsilon")
		{
			CHECK(Test::ApproxEqual(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(1.0f, 2.0f, 3.000001f)));
			CHECK_FALSE(Test::ApproxEqual(glm::vec3(1.0f, 2.0f, 3.0f), glm::vec3(1.0f, 2.1f, 3.0f)));
			CHECK(Test::ApproxEqual(glm::vec2(0.0f), glm::vec2(0.05f), 0.1f));
			const glm::vec4 notANumber(std::numeric_limits<float>::quiet_NaN());
			CHECK_FALSE(Test::ApproxEqual(notANumber, notANumber));

			const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
			CHECK(Test::ApproxEqual(identity, glm::quat(1.0f, 0.0f, 0.0f, 0.000001f)));
			CHECK_FALSE(Test::ApproxEqual(identity, -identity)); // the same rotation, but not component-wise equal

			glm::mat4 almostIdentity(1.0f);
			almostIdentity[3][0] = 0.000002f;
			CHECK(Test::ApproxEqual(glm::mat4(1.0f), almostIdentity));
			almostIdentity[2][1] = 0.5f;
			CHECK_FALSE(Test::ApproxEqual(glm::mat4(1.0f), almostIdentity));
		}

		TEST_CASE("GlmApprox: StringMaker prints vectors, quaternions and matrices")
		{
			CHECK(doctest::toString(glm::vec3(1.0f, 2.5f, -3.0f)) == "vec3(1, 2.5, -3)");
			CHECK(doctest::toString(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) == "quat(0, 0, 0, 1)");
			CHECK(doctest::toString(glm::mat2(1.0f)) == "mat2x2[(1, 0), (0, 1)]");
		}
	}

}
