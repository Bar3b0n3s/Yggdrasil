#include "EnginePCH.h"
#include "Engine/Renderer/EditorOverlay.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace Engine {

	namespace Utils {

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
			glm::dvec3 minimum(std::numeric_limits<double>::max());
			glm::dvec3 maximum(-std::numeric_limits<double>::max());
			bool intersects = false;
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
					intersects = true;
				}
			}
			if (!intersects)
				return {};
			const double extent = std::max(maximum.x - minimum.x, maximum.z - minimum.z);
			if (extent <= 0.0)
				return {};
			const double spacing = std::pow(10.0, std::ceil(std::log10(extent / 80.0)));
			const glm::dvec3 cameraPosition(camera.Position);
			double fadeDistance = 0.0;
			for (double x : { minimum.x, maximum.x })
				for (double z : { minimum.z, maximum.z })
					fadeDistance = std::max(fadeDistance, glm::length(glm::dvec3(x, 0.0, z) - cameraPosition));
			for (int axis : { 0, 2 })
			{
				const int along = axis == 0 ? 2 : 0;
				const double first = std::ceil(minimum[axis] / spacing);
				const double last = std::floor(maximum[axis] / spacing);
				const int lineCount = static_cast<int>(std::min(82.0, std::max(0.0, last - first + 1.0)));
				for (int line = 0; line < lineCount; ++line)
				{
					const double coordinate = (first + line) * spacing;
					const bool worldAxis = coordinate == 0.0;
					for (int segment = 0; segment < 16; ++segment)
					{
						glm::dvec3 from(0.0), to(0.0);
						from[axis] = to[axis] = coordinate;
						from[along] = minimum[along] + (maximum[along] - minimum[along]) * segment / 16.0;
						to[along] = minimum[along] + (maximum[along] - minimum[along]) * (segment + 1) / 16.0;
						const double fade = std::clamp(1.0 - glm::length((from + to) * 0.5 - cameraPosition) / fadeDistance, 0.0, 1.0);
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
