#pragma once

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <concepts>
#include <format>
#include <string>

// glm helpers for tests (Architecture §15.2, Appendix A): ApproxEqual for vectors, quaternions and matrices, and
// doctest::StringMaker specializations so failed CHECKs print the values instead of "{?}".
//     CHECK(Test::ApproxEqual(transform.GetTranslation(), glm::vec3(0.0f, 2.0f, 0.0f)));

namespace Engine {

	namespace Test {

		// The default absolute tolerance of ApproxEqual.
		inline constexpr float DefaultGlmEpsilon = 1.0e-5f;

		// True when every component differs by at most `epsilon` (absolute). NaN never compares equal. Floating-point
		// component types only: integer vectors compare exactly with ==.
		template<glm::length_t L, typename T, glm::qualifier Q>
			requires std::floating_point<T>
		[[nodiscard]] bool ApproxEqual(const glm::vec<L, T, Q>& a, const glm::vec<L, T, Q>& b,
			T epsilon = static_cast<T>(DefaultGlmEpsilon))
		{
			for (glm::length_t index = 0; index < L; ++index)
			{
				if (!(std::abs(a[index] - b[index]) <= epsilon))
					return false;
			}
			return true;
		}

		// Component-wise (x, y, z, w). q and -q describe the same rotation but are not ApproxEqual.
		template<typename T, glm::qualifier Q>
			requires std::floating_point<T>
		[[nodiscard]] bool ApproxEqual(const glm::qua<T, Q>& a, const glm::qua<T, Q>& b,
			T epsilon = static_cast<T>(DefaultGlmEpsilon))
		{
			for (glm::length_t index = 0; index < 4; ++index)
			{
				if (!(std::abs(a[index] - b[index]) <= epsilon))
					return false;
			}
			return true;
		}

		// Column by column.
		template<glm::length_t C, glm::length_t R, typename T, glm::qualifier Q>
			requires std::floating_point<T>
		[[nodiscard]] bool ApproxEqual(const glm::mat<C, R, T, Q>& a, const glm::mat<C, R, T, Q>& b,
			T epsilon = static_cast<T>(DefaultGlmEpsilon))
		{
			for (glm::length_t column = 0; column < C; ++column)
			{
				if (!ApproxEqual(a[column], b[column], epsilon))
					return false;
			}
			return true;
		}

	}

}

// "vec3(1, 2, 3)"
template<glm::length_t L, typename T, glm::qualifier Q>
struct doctest::StringMaker<glm::vec<L, T, Q>>
{
	static doctest::String convert(const glm::vec<L, T, Q>& value)
	{
		std::string text = std::format("vec{}(", L);
		for (glm::length_t index = 0; index < L; ++index)
			text += std::format("{}{}", index == 0 ? "" : ", ", value[index]);
		text += ")";
		return doctest::String(text);
	}
};

// "quat(x, y, z, w)"
template<typename T, glm::qualifier Q>
struct doctest::StringMaker<glm::qua<T, Q>>
{
	static doctest::String convert(const glm::qua<T, Q>& value)
	{
		return doctest::String(std::format("quat({}, {}, {}, {})", value.x, value.y, value.z, value.w));
	}
};

// "mat4x4[(column 0), (column 1), ...]"
template<glm::length_t C, glm::length_t R, typename T, glm::qualifier Q>
struct doctest::StringMaker<glm::mat<C, R, T, Q>>
{
	static doctest::String convert(const glm::mat<C, R, T, Q>& value)
	{
		std::string text = std::format("mat{}x{}[", C, R);
		for (glm::length_t column = 0; column < C; ++column)
		{
			text += column == 0 ? "(" : ", (";
			for (glm::length_t row = 0; row < R; ++row)
				text += std::format("{}{}", row == 0 ? "" : ", ", value[column][row]);
			text += ")";
		}
		text += "]";
		return doctest::String(text);
	}
};
