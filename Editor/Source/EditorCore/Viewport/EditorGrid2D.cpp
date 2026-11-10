#include "EditorPCH.h"
#include "EditorCore/Viewport/EditorGrid2D.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Engine {

	namespace Utils {

		// Each line is drawn in this many segments, so its fade follows the distance from the grid's centre.
		constexpr int Grid2DSegmentsPerLine = 16;
		// A cell spans a tenth of the view's scale, rounded up to a power of ten.
		constexpr double Grid2DScalePerCell = 10.0;

		static bool IsFiniteGrid2DPoint(const glm::dvec3& point)
		{
			return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
		}

	}

	Status AppendEditorGrid2D(const CameraData& camera, DebugDrawList& output)
	{
		const glm::dmat4 inverse = glm::inverse(glm::dmat4(camera.Projection) * glm::dmat4(camera.View));
		const bool perspective = camera.ProjectionKind == RenderProjection::Perspective;
		// The frustum's corners: the near plane at reverse-Z depth 1, the far one at 0 (orthographic) or near / far.
		std::array<glm::dvec3, 8> corners{};
		for (int index = 0; index < 8; ++index)
		{
			const double depth = (index & 4) == 0 ? 1.0 : perspective ? static_cast<double>(camera.NearClip) / static_cast<double>(camera.FarClip)
																	  : 0.0;
			const glm::dvec4 world = inverse * glm::dvec4((index & 1) ? 1.0 : -1.0, (index & 2) ? 1.0 : -1.0, depth, 1.0);
			if (world.w == 0.0 || !Utils::IsFiniteGrid2DPoint(glm::dvec3(world) / world.w))
				return MakeError(ErrorCode::InvalidArgument, "The 2D grid's camera frustum overflows world coordinates");
			corners[static_cast<size_t>(index)] = glm::dvec3(world) / world.w;
		}
		// The part of the plane z = 0 the frustum sees: the bounds of its edges' intersections with the plane.
		glm::dvec2 minimum(std::numeric_limits<double>::max());
		glm::dvec2 maximum(std::numeric_limits<double>::lowest());
		bool seen = false;
		for (size_t index = 0; index < corners.size(); ++index)
		{
			for (size_t bit = 1; bit <= 4; bit <<= 1)
			{
				if ((index & bit) != 0)
					continue;
				const glm::dvec3 a = corners[index];
				const glm::dvec3 b = corners[index | bit];
				if ((a.z > 0.0 && b.z > 0.0) || (a.z < 0.0 && b.z < 0.0))
					continue;
				const double t = a.z == b.z ? 0.0 : a.z / (a.z - b.z);
				const glm::dvec3 point = a + (b - a) * t;
				minimum = glm::min(minimum, glm::dvec2(point));
				maximum = glm::max(maximum, glm::dvec2(point));
				seen = true;
			}
		}
		if (!seen)
			return {};

		// The view's scale: the orthographic half height, or the perspective camera's distance from the plane (at least its
		// near distance), as the renderer's ground grid measures it.
		const glm::dvec3 position(camera.Position);
		const double halfHeight = 1.0 / std::abs(static_cast<double>(camera.Projection[1][1]));
		const double halfWidth = 1.0 / std::abs(static_cast<double>(camera.Projection[0][0]));
		const double scale = perspective ? std::max(std::abs(position.z), static_cast<double>(camera.NearClip)) : halfHeight;
		if (!std::isfinite(scale) || scale <= 0.0 || !std::isfinite(halfWidth))
			return MakeError(ErrorCode::InvalidArgument, "The 2D grid's camera has no scale");
		const double spacing = std::pow(10.0, std::ceil(std::log10(scale / Utils::Grid2DScalePerCell)));
		// The grid's centre: where the view axis meets the plane (orthographic), or the point below the camera
		// (perspective, or a view axis along the plane).
		const glm::dvec3 forward = -glm::normalize(glm::dvec3(glm::inverse(glm::dmat4(camera.View))[2]));
		glm::dvec3 centre(position.x, position.y, 0.0);
		if (!perspective && forward.z != 0.0)
			centre = position + forward * (-position.z / forward.z);
		const double limit = GridHalfLines2D * spacing;
		minimum = glm::max(minimum, glm::dvec2(centre) - limit);
		maximum = glm::min(maximum, glm::dvec2(centre) + limit);
		if (maximum.x < minimum.x || maximum.y < minimum.y)
			return {};
		// The fade reaches zero at twice the orthographic view's half diagonal (its corners keep a quarter of the alpha),
		// measured in the view plane; in perspective, at the grid's extent, measured in the plane.
		const double fadeRange = perspective ? limit : 2.0 * std::sqrt(halfHeight * halfHeight + halfWidth * halfWidth);
		const auto fadeDistance = [perspective, &centre, &forward](const glm::dvec3& point)
		{
			const glm::dvec3 offset = point - centre;
			return perspective ? glm::length(glm::dvec2(offset)) : glm::length(offset - forward * glm::dot(offset, forward));
		};
		for (int axis : { 0, 1 })
		{
			const int along = 1 - axis;
			const double first = std::ceil(minimum[axis] / spacing);
			const double last = std::floor(maximum[axis] / spacing);
			const int lineCount = static_cast<int>(std::min(2.0 * GridHalfLines2D + 2.0, std::max(0.0, last - first + 1.0)));
			for (int line = 0; line < lineCount; ++line)
			{
				const double coordinate = (first + line) * spacing;
				const bool worldAxis = coordinate == 0.0;
				// x = 0 is the Y axis (green), y = 0 the X axis (red).
				const glm::vec3 rgb = worldAxis ? (axis == 0 ? glm::vec3(0.15f, 0.75f, 0.12f) : glm::vec3(0.9f, 0.12f, 0.08f)) : glm::vec3(0.35f);
				for (int segment = 0; segment < Utils::Grid2DSegmentsPerLine; ++segment)
				{
					glm::dvec3 from(0.0);
					glm::dvec3 to(0.0);
					from[axis] = to[axis] = coordinate;
					from[along] = minimum[along] + (maximum[along] - minimum[along]) * segment / Utils::Grid2DSegmentsPerLine;
					to[along] = minimum[along] + (maximum[along] - minimum[along]) * (segment + 1) / Utils::Grid2DSegmentsPerLine;
					const double fade = std::clamp(1.0 - fadeDistance((from + to) * 0.5) / fadeRange, 0.0, 1.0);
					output.AddLine(glm::vec3(from), glm::vec3(to), glm::vec4(rgb, static_cast<float>(fade * fade) * (worldAxis ? 0.8f : 0.4f)));
				}
			}
		}
		return {};
	}

}
