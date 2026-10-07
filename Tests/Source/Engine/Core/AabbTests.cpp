#include "TestsPCH.h"

#include "Engine/Core/Aabb.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Engine {

	TEST_SUITE("Core")
	{
		TEST_CASE("Aabb: a default box is empty and extending it by a point gives that point")
		{
			Aabb box;
			CHECK(box.IsEmpty());
			box.Extend(glm::vec3(1.0f, -2.0f, 3.0f));
			CHECK_FALSE(box.IsEmpty());
			CHECK(box.Min == glm::vec3(1.0f, -2.0f, 3.0f));
			CHECK(box.Max == glm::vec3(1.0f, -2.0f, 3.0f));
			box.Extend(glm::vec3(-1.0f, 4.0f, 3.0f));
			CHECK(box.Min == glm::vec3(-1.0f, -2.0f, 3.0f));
			CHECK(box.Max == glm::vec3(1.0f, 4.0f, 3.0f));
			CHECK(box.GetCenter() == glm::vec3(0.0f, 1.0f, 3.0f));
			CHECK(box.GetSize() == glm::vec3(2.0f, 6.0f, 0.0f));
		}

		TEST_CASE("Aabb: extending by an empty box changes nothing")
		{
			Aabb box;
			box.Extend(glm::vec3(0.5f));
			const Aabb before = box;
			box.Extend(Aabb());
			CHECK(box == before);
		}

		TEST_CASE("Aabb: a transformed box holds the transformed corners")
		{
			Aabb unit;
			unit.Extend(glm::vec3(-0.5f));
			unit.Extend(glm::vec3(0.5f));
			// A translation and a uniform scale by 2: exactly representable, so the result is exact.
			const glm::mat4 matrix = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, -4.0f)), glm::vec3(2.0f));
			const Aabb moved = unit.Transformed(matrix);
			CHECK(moved.Min == glm::vec3(9.0f, -1.0f, -5.0f));
			CHECK(moved.Max == glm::vec3(11.0f, 1.0f, -3.0f));
			CHECK(Aabb().Transformed(matrix).IsEmpty());
			CHECK(unit.GetCorners()[0] == unit.Min);
			CHECK(unit.GetCorners()[7] == unit.Max);
		}
	}

}
