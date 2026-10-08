#include "EnginePCH.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include "Engine/Core/DetMath.h"

#include <array>
#include <cstddef>
#include <utility>

// The reverse-Z projection of §8.3 through Core/DetMath (RenderSnapshot.h): Scene's render extraction calls it on the
// simulation path's lint scope, and the matrix must be bit-identical in every configuration. Only IEEE-exact operations
// besides DetMath::Tan, in double precision, rounded to float once per entry.

namespace Engine {

	namespace Utils {

		constexpr double ProjectionDegreesToRadians = 0.017453292519943295769236907684886; // pi / 180

		// The debug views and their names, in enumerator order.
		constexpr std::array<std::pair<RenderDebugView, std::string_view>, RenderDebugViewCount> DebugViewNames = { {
			{ RenderDebugView::Lit, "Lit" },
			{ RenderDebugView::Albedo, "Albedo" },
			{ RenderDebugView::Normals, "Normals" },
			{ RenderDebugView::Roughness, "Roughness" },
			{ RenderDebugView::Metallic, "Metallic" },
			{ RenderDebugView::Emissive, "Emissive" },
		} };

		// Every view has its entry at its enumerator's index with a name; an entry the array value-initialized because a view
		// was appended without one fails here.
		[[nodiscard]] static consteval bool AreDebugViewNamesComplete()
		{
			for (size_t index = 0; index < DebugViewNames.size(); ++index)
			{
				if (static_cast<size_t>(DebugViewNames[index].first) != index || DebugViewNames[index].second.empty())
					return false;
			}
			return true;
		}

		static_assert(AreDebugViewNamesComplete(), "every RenderDebugView needs its name, in enumerator order");

		[[nodiscard]] static constexpr char ToLowerAscii(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		[[nodiscard]] static bool EqualsIgnoringAsciiCase(std::string_view left, std::string_view right)
		{
			if (left.size() != right.size())
				return false;
			for (size_t index = 0; index < left.size(); ++index)
			{
				if (ToLowerAscii(left[index]) != ToLowerAscii(right[index]))
					return false;
			}
			return true;
		}

	}

	std::string_view RenderDebugViewToString(RenderDebugView view)
	{
		for (const auto& [value, name] : Utils::DebugViewNames)
		{
			if (value == view)
				return name;
		}
		return "Unknown";
	}

	std::optional<RenderDebugView> ParseRenderDebugView(std::string_view name)
	{
		for (const auto& [value, spelling] : Utils::DebugViewNames)
		{
			if (Utils::EqualsIgnoringAsciiCase(name, spelling))
				return value;
		}
		return std::nullopt;
	}

	glm::mat4 ComputeReverseZProjection(RenderProjection projection, float verticalFovDegrees, float orthographicSize, float nearClip, float farClip,
		uint32_t width, uint32_t height)
	{
		const double aspect = static_cast<double>(width) / static_cast<double>(height);
		// glm matrices are column-major: m[column][row].
		glm::mat4 matrix(0.0f);
		if (projection == RenderProjection::Perspective)
		{
			// m[0][0] = f / aspect, m[1][1] = f, m[2][3] = -1, m[3][2] = near: clip w = -zView and clip z = near, so the
			// depth near / -zView is 1 at the near plane and tends to 0 at infinity (an infinite far plane).
			const double f = 1.0 / DetMath::Tan(static_cast<double>(verticalFovDegrees) * Utils::ProjectionDegreesToRadians * 0.5);
			matrix[0][0] = static_cast<float>(f / aspect);
			matrix[1][1] = static_cast<float>(f);
			matrix[2][3] = -1.0f;
			matrix[3][2] = nearClip;
			return matrix;
		}

		// d = (far + zView) / (far - near): 1 at zView = -near, 0 at zView = -far, linear in view depth; w stays 1.
		const double halfHeight = static_cast<double>(orthographicSize);
		const double range = static_cast<double>(farClip) - static_cast<double>(nearClip);
		matrix[0][0] = static_cast<float>(1.0 / (halfHeight * aspect));
		matrix[1][1] = static_cast<float>(1.0 / halfHeight);
		matrix[2][2] = static_cast<float>(1.0 / range);
		matrix[3][2] = static_cast<float>(static_cast<double>(farClip) / range);
		matrix[3][3] = 1.0f;
		return matrix;
	}

}
