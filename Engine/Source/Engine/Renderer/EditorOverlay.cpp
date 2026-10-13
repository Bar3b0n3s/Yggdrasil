#include "EnginePCH.h"
#include "Engine/Renderer/EditorOverlay.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Renderer/RenderPrepare.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>

namespace Engine {

	namespace Utils {

		// The grid (OverlayGrid): a cell spans a tenth of the view's scale, rounded up to a power of ten. A perspective grid
		// covers GridHalfLines cells on each side of its centre; an orthographic grid covers the frustum's footprint, at most
		// GridOrthographicHalfLines cells on each side. Each line is drawn as GridSegmentsPerLine segments so its fade follows
		// distance.
		constexpr double GridScalePerCell = 10.0;
		constexpr double GridHalfLines = 40.0;
		constexpr double GridOrthographicHalfLines = 100.0;
		constexpr int GridSegmentsPerLine = 16;

		static bool OverlayFinite(const glm::dvec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)
				&& glm::all(glm::lessThanEqual(glm::abs(value), glm::dvec3(std::numeric_limits<float>::max())));
		}

		static bool OverlayFiniteMatrix(const glm::mat4& matrix)
		{
			for (int column = 0; column < 4; ++column)
				for (int row = 0; row < 4; ++row)
					if (!std::isfinite(matrix[column][row]))
						return false;
			return true;
		}

		static Status OverlayValidateCamera(const CameraData& camera)
		{
			if (!OverlayFinite(camera.Position) || !OverlayFiniteMatrix(camera.View) || !OverlayFiniteMatrix(camera.Projection)
				|| camera.ViewportWidth == 0 || camera.ViewportHeight == 0
				|| !std::isfinite(camera.NearClip) || !std::isfinite(camera.FarClip) || camera.NearClip <= 0.0f || camera.FarClip <= camera.NearClip
				|| (camera.ProjectionKind != RenderProjection::Perspective && camera.ProjectionKind != RenderProjection::Orthographic)
				|| glm::determinant(glm::dmat4(camera.View)) == 0.0 || glm::determinant(glm::dmat4(camera.Projection)) == 0.0)
				return MakeError(ErrorCode::InvalidArgument, "Overlay camera values and invertible matrices must be finite and valid");
			return {};
		}

		// The distance from `point` to the convex polygon whose vertices are `vertices`, given in any order (they are sorted in
		// place): 0 inside a polygon with area; otherwise the distance to its nearest edge.
		static double OverlayDistanceToConvexPolygon(const glm::dvec2& point, std::span<glm::dvec2> vertices)
		{
			glm::dvec2 centroid(0.0);
			for (const glm::dvec2& vertex : vertices)
				centroid += vertex;
			centroid /= static_cast<double>(vertices.size());
			std::sort(vertices.begin(), vertices.end(), [&centroid](const glm::dvec2& a, const glm::dvec2& b)
			{
				return std::atan2(a.y - centroid.y, a.x - centroid.x) < std::atan2(b.y - centroid.y, b.x - centroid.x);
			});
			bool inside = true;
			double area = 0.0;
			double distance = std::numeric_limits<double>::max();
			for (size_t index = 0; index < vertices.size(); ++index)
			{
				const glm::dvec2 a = vertices[index];
				const glm::dvec2 b = vertices[(index + 1) % vertices.size()];
				const glm::dvec2 edge = b - a;
				const glm::dvec2 offset = point - a;
				area += a.x * b.y - b.x * a.y;
				// Counter-clockwise vertices keep the inside on each edge's left.
				if (edge.x * offset.y - edge.y * offset.x < 0.0)
					inside = false;
				const double length = glm::dot(edge, edge);
				const double t = length > 0.0 ? std::clamp(glm::dot(offset, edge) / length, 0.0, 1.0) : 0.0;
				distance = std::min(distance, glm::length(offset - edge * t));
			}
			return inside && area > 0.0 ? 0.0 : distance;
		}

		static Status OverlayGrid(const CameraData& camera, DebugDrawList& output)
		{
			const glm::dmat4 inverse = glm::inverse(glm::dmat4(camera.Projection) * glm::dmat4(camera.View));
			std::array<glm::dvec3, 8> corners{};
			for (int index = 0; index < 8; ++index)
			{
				const double z = (index & 4) == 0 ? 1.0 : camera.ProjectionKind == RenderProjection::Perspective ? camera.NearClip / camera.FarClip
																												 : 0.0;
				const glm::dvec4 clip((index & 1) ? 1.0 : -1.0, (index & 2) ? 1.0 : -1.0, z, 1.0);
				const glm::dvec4 world = inverse * clip;
				if (world.w == 0.0 || !OverlayFinite(glm::dvec3(world) / world.w))
					return MakeError(ErrorCode::InvalidArgument, "Overlay camera frustum overflows world coordinates");
				corners[static_cast<size_t>(index)] = glm::dvec3(world) / world.w;
			}
			// The part of the plane y = 0 the frustum can see (its footprint): the convex polygon of its edges' intersections with
			// the plane, and their XZ bounds.
			glm::dvec3 minimum(std::numeric_limits<double>::max());
			glm::dvec3 maximum(-std::numeric_limits<double>::max());
			std::array<glm::dvec2, 12> footprint{};
			size_t footprintCount = 0;
			for (size_t index = 0; index < corners.size(); ++index)
			{
				for (size_t bit = 1; bit <= 4; bit <<= 1)
				{
					if ((index & bit) != 0)
						continue;
					const glm::dvec3 a = corners[index], b = corners[index | bit];
					if ((a.y > 0.0 && b.y > 0.0) || (a.y < 0.0 && b.y < 0.0))
						continue;
					const double t = a.y == b.y ? 0.0 : a.y / (a.y - b.y);
					const glm::dvec3 point = a + (b - a) * t;
					minimum = glm::min(minimum, point);
					maximum = glm::max(maximum, point);
					footprint[footprintCount++] = glm::dvec2(point.x, point.z);
				}
			}
			if (footprintCount == 0)
				return {};

			// Spacing in powers of ten from the view's scale, never from the far plane: a tenth of a cell's span, rounded up, so a
			// camera 3 m up or a view 20 m tall gets 1 m cells. In orthographic the scale is half the view height. In perspective
			// it is the camera's height above the plane, but at least half the distance from the camera's ground point to the
			// nearest ground the frustum sees, so a view that sees only distant ground (a narrow field of view, a grazing view)
			// still reaches it: the grid then extends at least twice that distance, where the fade keeps at least half.
			const glm::dvec3 cameraPosition(camera.Position);
			const bool perspective = camera.ProjectionKind == RenderProjection::Perspective;
			const double orthographicHalfHeight = 1.0 / std::abs(static_cast<double>(camera.Projection[1][1]));
			const glm::dvec2 groundPoint(cameraPosition.x, cameraPosition.z);
			const double scale = perspective ? std::max({ std::abs(cameraPosition.y), static_cast<double>(camera.NearClip),
												   0.5 * OverlayDistanceToConvexPolygon(groundPoint, std::span(footprint.data(), footprintCount)) })
											 : orthographicHalfHeight;
			if (!std::isfinite(scale) || scale <= 0.0)
				return MakeError(ErrorCode::InvalidArgument, "Overlay camera has no grid scale");
			const double spacing = std::pow(10.0, std::ceil(std::log10(scale / GridScalePerCell)));
			// A perspective grid covers GridHalfLines cells on each side of the camera's ground point, within the footprint, and
			// fades with the distance on the plane from that point, reaching zero at the grid's extent. An orthographic grid is
			// centred on the view axis's intersection with the plane (the camera's position for a horizontal axis) and fades
			// with the distance in the view plane (ignoring depth along the view axis, which an orthographic image does not
			// show), reaching zero at twice the view's half diagonal, so the view's corners keep a quarter of the alpha. Its
			// region is the whole footprint, where that fade never reaches zero, so the grid shows no edge. Only when the
			// footprint reaches more than GridOrthographicHalfLines cells from the centre (a view at a very shallow pitch) does
			// the fade range shrink until the part of the plane where the fade is not zero fits within that many cells, and the
			// region is cut to that part.
			const glm::dvec3 forward = -glm::normalize(glm::dvec3(glm::inverse(glm::dmat4(camera.View))[2]));
			glm::dvec3 centre(cameraPosition.x, 0.0, cameraPosition.z);
			if (!perspective && forward.y != 0.0)
			{
				const double distance = -cameraPosition.y / forward.y;
				centre = cameraPosition + forward * distance;
			}
			const double orthographicHalfWidth = 1.0 / std::abs(static_cast<double>(camera.Projection[0][0]));
			double fadeRange = 0.0;
			if (perspective)
			{
				fadeRange = GridHalfLines * spacing;
				minimum = glm::max(minimum, centre - fadeRange);
				maximum = glm::min(maximum, centre + fadeRange);
			}
			else
			{
				fadeRange = 2.0 * std::sqrt(orthographicHalfHeight * orthographicHalfHeight + orthographicHalfWidth * orthographicHalfWidth);
				const double limit = GridOrthographicHalfLines * spacing;
				const glm::dvec3 reached = glm::max(centre - minimum, maximum - centre);
				if (reached.x > limit || reached.z > limit)
				{
					// The plane points within view-plane distance r of the centre form an ellipse whose XZ bounds reach
					// r * sqrt(1 - forward.z^2) / |forward.y| along x and r * sqrt(1 - forward.x^2) / |forward.y| along z.
					const glm::dvec2 reach(std::sqrt(std::max(0.0, 1.0 - forward.z * forward.z)), std::sqrt(std::max(0.0, 1.0 - forward.x * forward.x)));
					fadeRange = std::min(fadeRange, limit * std::abs(forward.y) / std::max(reach.x, reach.y));
					if (!(fadeRange > 0.0))
						return {}; // the plane is seen edge-on
					const glm::dvec3 bounds(fadeRange * reach.x / std::abs(forward.y), 0.0, fadeRange * reach.y / std::abs(forward.y));
					minimum = glm::max(minimum, centre - bounds);
					maximum = glm::min(maximum, centre + bounds);
				}
			}
			if (maximum.x < minimum.x || maximum.z < minimum.z)
				return {};
			const auto fadeDistance = [perspective, &centre, &forward](const glm::dvec3& point)
			{
				const glm::dvec3 offset = point - centre;
				return perspective ? glm::length(glm::dvec2(offset.x, offset.z)) : glm::length(offset - forward * glm::dot(offset, forward));
			};
			const double maxHalfLines = perspective ? GridHalfLines : GridOrthographicHalfLines;
			for (int axis : { 0, 2 })
			{
				const int along = axis == 0 ? 2 : 0;
				const double first = std::ceil(minimum[axis] / spacing);
				const double last = std::floor(maximum[axis] / spacing);
				const int lineCount = static_cast<int>(std::min(2.0 * maxHalfLines + 2.0, std::max(0.0, last - first + 1.0)));
				for (int line = 0; line < lineCount; ++line)
				{
					const double coordinate = (first + line) * spacing;
					const bool worldAxis = coordinate == 0.0;
					for (int segment = 0; segment < GridSegmentsPerLine; ++segment)
					{
						glm::dvec3 from(0.0), to(0.0);
						from[axis] = to[axis] = coordinate;
						from[along] = minimum[along] + (maximum[along] - minimum[along]) * segment / GridSegmentsPerLine;
						to[along] = minimum[along] + (maximum[along] - minimum[along]) * (segment + 1) / GridSegmentsPerLine;
						const double fade = std::clamp(1.0 - fadeDistance((from + to) * 0.5) / fadeRange, 0.0, 1.0);
						const glm::vec3 rgb = worldAxis ? (along == 0 ? glm::vec3(0.9f, 0.12f, 0.08f) : glm::vec3(0.08f, 0.25f, 0.9f)) : glm::vec3(0.35f);
						output.AddLine(glm::vec3(from), glm::vec3(to), glm::vec4(rgb, static_cast<float>(fade * fade) * (worldAxis ? 0.8f : 0.4f)));
					}
				}
			}
			return {};
		}

		static Status OverlayIcon(const RenderIcon& icon, const CameraData& camera, DebugDrawList& output)
		{
			if (icon.Kind > RenderIconKind::AudioListener || !OverlayFinite(icon.Position) || !std::isfinite(icon.Size) || icon.Size <= 0.0f)
				return MakeError(ErrorCode::InvalidArgument, "Overlay icon kind, position and positive size must be valid");
			for (int component = 0; component < 4; ++component)
				if (!std::isfinite(icon.Color[component]) || icon.Color[component] < 0.0f || (component == 3 && icon.Color[component] > 1.0f))
					return MakeError(ErrorCode::InvalidArgument, "Overlay icon color must be finite and nonnegative");
			const glm::dvec4 viewPosition = glm::dmat4(camera.View) * glm::dvec4(icon.Position, 1.0);
			if (viewPosition.z >= 0.0)
				return {};
			const double projectionY = std::abs(static_cast<double>(camera.Projection[1][1]));
			if (projectionY == 0.0)
				return MakeError(ErrorCode::InvalidArgument, "Overlay icon camera has a zero vertical projection scale");
			const double scale = icon.Size * 2.0 / (camera.ViewportHeight * projectionY)
				* (camera.ProjectionKind == RenderProjection::Perspective ? -viewPosition.z : 1.0);
			const glm::dmat4 inverseView = glm::inverse(glm::dmat4(camera.View));
			const glm::dvec3 right = glm::normalize(glm::dvec3(inverseView[0]));
			const glm::dvec3 up = glm::normalize(glm::dvec3(inverseView[1]));
			bool valid = true;
			const auto line = [&output, &icon, &right, &up, scale, &valid](const glm::dvec2& a, const glm::dvec2& b)
			{
				const glm::dvec3 from = glm::dvec3(icon.Position) + scale * (right * a.x + up * a.y);
				const glm::dvec3 to = glm::dvec3(icon.Position) + scale * (right * b.x + up * b.y);
				if (!OverlayFinite(from) || !OverlayFinite(to))
					valid = false;
				else
					output.AddLine(glm::vec3(from), glm::vec3(to), icon.Color);
			};
			const auto circle = [&line](double radius)
			{
				for (int step = 0; step < 16; ++step)
				{
					const double a = step * std::numbers::pi / 8.0;
					const double b = (step + 1) * std::numbers::pi / 8.0;
					line(radius * glm::dvec2(std::cos(a), std::sin(a)), radius * glm::dvec2(std::cos(b), std::sin(b)));
				}
			};
			switch (icon.Kind)
			{
				case RenderIconKind::Camera:
					line({ -0.4, -0.25 }, { 0.15, -0.25 });
					line({ 0.15, -0.25 }, { 0.15, 0.25 });
					line({ 0.15, 0.25 }, { -0.4, 0.25 });
					line({ -0.4, 0.25 }, { -0.4, -0.25 });
					line({ 0.15, -0.1 }, { 0.45, -0.3 });
					line({ 0.45, -0.3 }, { 0.45, 0.3 });
					line({ 0.45, 0.3 }, { 0.15, 0.1 });
					break;
				case RenderIconKind::DirectionalLight:
				case RenderIconKind::PointLight:
					circle(0.22);
					for (int ray = 0; ray < 8; ++ray)
					{
						const double angle = ray * std::numbers::pi / 4.0;
						const glm::dvec2 direction(std::cos(angle), std::sin(angle));
						line(direction * 0.3, direction * 0.48);
					}
					if (icon.Kind == RenderIconKind::DirectionalLight)
					{
						line({ 0.0, 0.18 }, { 0.0, -0.18 });
						line({ 0.0, -0.18 }, { -0.1, -0.06 });
						line({ 0.0, -0.18 }, { 0.1, -0.06 });
					}
					break;
				case RenderIconKind::SpotLight:
					line({ -0.12, 0.4 }, { 0.12, 0.4 });
					line({ 0.12, 0.4 }, { 0.45, -0.4 });
					line({ 0.45, -0.4 }, { -0.45, -0.4 });
					line({ -0.45, -0.4 }, { -0.12, 0.4 });
					break;
				case RenderIconKind::AudioSource:
					line({ -0.4, -0.15 }, { -0.2, -0.15 });
					line({ -0.2, -0.15 }, { 0.05, -0.4 });
					line({ 0.05, -0.4 }, { 0.05, 0.4 });
					line({ 0.05, 0.4 }, { -0.2, 0.15 });
					line({ -0.2, 0.15 }, { -0.4, 0.15 });
					line({ -0.4, 0.15 }, { -0.4, -0.15 });
					line({ 0.2, -0.25 }, { 0.35, 0.0 });
					line({ 0.35, 0.0 }, { 0.2, 0.25 });
					break;
				case RenderIconKind::AudioListener:
					circle(0.35);
					line({ -0.35, 0.0 }, { -0.35, -0.4 });
					line({ 0.35, 0.0 }, { 0.35, -0.4 });
					line({ -0.35, -0.4 }, { -0.15, -0.4 });
					line({ 0.35, -0.4 }, { 0.15, -0.4 });
					break;
			}
			if (!valid)
				return MakeError(ErrorCode::InvalidArgument, "Overlay icon vertices overflow world coordinates");
			return {};
		}

	}

	Status AppendEditorOverlay(const RenderSnapshot& snapshot, DebugDrawList& output)
	{
		if (!snapshot.HasCamera || !HasFlag(snapshot.Flags, RenderViewFlags::EditorOverlays)
			|| (!HasFlag(snapshot.Flags, RenderViewFlags::Grid) && !HasFlag(snapshot.Flags, RenderViewFlags::Icons)))
			return {};
		ENGINE_TRY(Utils::OverlayValidateCamera(snapshot.Camera));
		DebugDrawList pending = output;
		if (HasFlag(snapshot.Flags, RenderViewFlags::Grid))
			ENGINE_TRY(Utils::OverlayGrid(snapshot.Camera, pending));
		if (HasFlag(snapshot.Flags, RenderViewFlags::Icons))
			for (const RenderIcon& icon : snapshot.Icons)
				ENGINE_TRY(Utils::OverlayIcon(icon, snapshot.Camera, pending));
		output = std::move(pending);
		return {};
	}

	Status AppendWireframeOverlay(const RenderSnapshot& snapshot, AssetManager& assets, DebugDrawList& output)
	{
		if (!snapshot.HasCamera || snapshot.DebugView != RenderDebugView::Lit || !HasFlag(snapshot.Flags, RenderViewFlags::Wireframe))
			return {};
		ENGINE_TRY(Utils::OverlayValidateCamera(snapshot.Camera));
		DebugDrawList pending = output;
		const glm::dvec3 towardCamera = glm::dvec3(glm::inverse(glm::dmat4(snapshot.Camera.View))[2]);
		for (const MeshDrawItem& item : snapshot.Meshes)
		{
			if (!Utils::OverlayFiniteMatrix(item.World) || item.World[0][3] != 0.0f || item.World[1][3] != 0.0f || item.World[2][3] != 0.0f || item.World[3][3] != 1.0f)
				return MakeError(ErrorCode::InvalidArgument, "Wireframe transforms must be finite and affine");
			if (!item.Mesh.IsValid())
				continue;
			const auto mesh = assets.GetOrPlaceholder<MeshData>(item.Mesh);
			const double determinant = glm::determinant(glm::dmat3(item.World));
			for (const MeshSubmesh& submesh : mesh->Submeshes)
			{
				AssetHandle materialHandle = BuiltinAssetHandles::DefaultMaterial;
				if (submesh.MaterialSlot < item.Materials.size() && item.Materials[submesh.MaterialSlot].IsValid())
					materialHandle = item.Materials[submesh.MaterialSlot];
				else if (submesh.MaterialSlot < mesh->Slots.size() && mesh->Slots[submesh.MaterialSlot].DefaultMaterial.IsValid())
					materialHandle = mesh->Slots[submesh.MaterialSlot].DefaultMaterial;
				// The solid passes' culling, FarClip included: an edge of a submesh they do not draw would show where Lit shows
				// nothing and spend the command budget before visible meshes later in canonical order.
				if (IsOutsideView(submesh.Bounds, item.World, snapshot.Camera))
					continue;
				const auto material = assets.GetOrPlaceholder<MaterialData>(materialHandle);
				for (uint32_t offset = 0; offset < submesh.IndexCount; offset += 3)
				{
					std::array<glm::dvec3, 3> triangle{};
					for (size_t corner = 0; corner < triangle.size(); ++corner)
					{
						const uint32_t index = mesh->Indices[submesh.IndexOffset + offset + corner];
						triangle[corner] = glm::dvec3(glm::dmat4(item.World) * glm::dvec4(mesh->Vertices[index].Position, 1.0));
						if (!Utils::OverlayFinite(triangle[corner]))
							return MakeError(ErrorCode::InvalidArgument, "Wireframe vertices overflow world coordinates");
					}
					const glm::dvec3 normal = glm::cross(triangle[1] - triangle[0], triangle[2] - triangle[0]);
					if (glm::dot(normal, normal) == 0.0)
						continue;
					const glm::dvec3 toward = snapshot.Camera.ProjectionKind == RenderProjection::Perspective ? glm::dvec3(snapshot.Camera.Position) - triangle[0] : towardCamera;
					const double facing = glm::dot(normal, toward) * (determinant < 0.0 ? -1.0 : 1.0);
					if (!material->DoubleSided && facing <= 0.0)
						continue;
					for (size_t corner = 0; corner < triangle.size(); ++corner)
						pending.AddLine(glm::vec3(triangle[corner]), glm::vec3(triangle[(corner + 1) % 3]), glm::vec4(1.0f));
				}
			}
		}
		output = std::move(pending);
		return {};
	}

}
