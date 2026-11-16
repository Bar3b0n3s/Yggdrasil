#include "EnginePCH.h"
#include "Engine/Scene/NavigationBakeInput.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/NavMeshData.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Core/Hash.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsComposition.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <format>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <variant>

// The navigation bake's input (NavigationBakeInput.h; Docs/Decisions/0045-m30-navigation.md decision 6).

namespace Engine {

	namespace Utils {

		// A triangle soup in a collider's local space, before its placement.
		struct LocalTriangles
		{
			std::vector<glm::vec3> Vertices{};
			std::vector<uint32_t> Indices{};

			void AddTriangle(uint32_t a, uint32_t b, uint32_t c) { Indices.insert(Indices.end(), { a, b, c }); }
			void AddQuad(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
			{
				AddTriangle(a, b, c);
				AddTriangle(a, c, d);
			}
		};

		// A box's 12 triangles, outward.
		static LocalTriangles TriangulateBox(const glm::vec3& halfExtents)
		{
			LocalTriangles box;
			for (uint32_t corner = 0; corner < 8; ++corner)
			{
				box.Vertices.emplace_back((corner & 1U) != 0 ? halfExtents.x : -halfExtents.x, (corner & 2U) != 0 ? halfExtents.y : -halfExtents.y,
					(corner & 4U) != 0 ? halfExtents.z : -halfExtents.z);
			}
			// Corner bits: 1 = +X, 2 = +Y, 4 = +Z. Each face counter-clockwise seen from outside.
			box.AddQuad(1, 3, 7, 5); // +X
			box.AddQuad(0, 4, 6, 2); // -X
			box.AddQuad(2, 6, 7, 3); // +Y
			box.AddQuad(0, 1, 5, 4); // -Y
			box.AddQuad(4, 5, 7, 6); // +Z
			box.AddQuad(0, 2, 3, 1); // -Z
			return box;
		}

		// The point at latitude `ring` of `rings` (0 the north pole) and longitude `segment` of a unit sphere, raised by
		// `lift` along Y: the rings of spheres and of a capsule's hemispheres.
		static glm::vec3 SpherePoint(uint32_t ring, uint32_t rings, uint32_t segment, float radius, float lift)
		{
			const float polar = glm::pi<float>() * static_cast<float>(ring) / static_cast<float>(rings);
			const float azimuth = 2.0f * glm::pi<float>() * static_cast<float>(segment) / static_cast<float>(NavBakeSphereSegments);
			const SinCosResult<float> latitude = DetMath::SinCos(polar);
			const SinCosResult<float> longitude = DetMath::SinCos(azimuth);
			return glm::vec3(radius * latitude.Sin * longitude.Cos, radius * latitude.Cos + lift, -radius * latitude.Sin * longitude.Sin);
		}

		// A capsule along Y of half height `halfHeight` (0 for a sphere) capped by hemispheres of `radius`: the north
		// hemisphere's rings raised by halfHeight, the south's lowered, each pole one vertex, outward.
		static LocalTriangles TriangulateRoundShape(float radius, float halfHeight)
		{
			LocalTriangles shape;
			constexpr uint32_t Half = NavBakeSphereRings / 2;
			// Rows from the north pole (row 0) to the south pole: the north hemisphere's rings 0..Half, then the south
			// hemisphere's Half..Rings; a capsule's equator appears twice (its cylinder's two ends), a sphere's once.
			std::vector<std::pair<uint32_t, float>> rows;
			for (uint32_t ring = 0; ring <= Half; ++ring)
				rows.emplace_back(ring, halfHeight);
			for (uint32_t ring = halfHeight > 0.0f ? Half : Half + 1; ring <= NavBakeSphereRings; ++ring)
				rows.emplace_back(ring, -halfHeight);
			std::vector<uint32_t> rowStart;
			for (const auto& [ring, lift] : rows)
			{
				rowStart.push_back(static_cast<uint32_t>(shape.Vertices.size()));
				const bool pole = ring == 0 || ring == NavBakeSphereRings;
				const uint32_t count = pole ? 1 : NavBakeSphereSegments;
				for (uint32_t segment = 0; segment < count; ++segment)
					shape.Vertices.push_back(SpherePoint(ring, NavBakeSphereRings, segment, radius, lift));
			}
			const auto at = [&rowStart, &rows](size_t row, uint32_t segment)
			{
				const bool pole = rows[row].first == 0 || rows[row].first == NavBakeSphereRings;
				return rowStart[row] + (pole ? 0U : segment % NavBakeSphereSegments);
			};
			for (size_t row = 0; row + 1 < rows.size(); ++row)
			{
				for (uint32_t segment = 0; segment < NavBakeSphereSegments; ++segment)
				{
					const uint32_t a = at(row, segment);
					const uint32_t b = at(row, segment + 1);
					const uint32_t c = at(row + 1, segment + 1);
					const uint32_t d = at(row + 1, segment);
					// Longitude grows toward -Z from +X (SpherePoint), counter-clockwise seen from above: outward is a, d, c and
					// a, c, b; a pole row has a == b, its first triangle alone.
					if (a != b)
						shape.AddTriangle(a, c, b);
					if (c != d)
						shape.AddTriangle(a, d, c);
				}
			}
			return shape;
		}

		// A height field's cells (Asset/TerrainGeometry.h's split), sample (x, z) at (x * CellSize.x, height, z * CellSize.y).
		static LocalTriangles TriangulateHeightField(const HeightFieldShapeGeometry& field)
		{
			LocalTriangles terrain;
			const uint32_t count = field.SampleCount;
			for (uint32_t z = 0; z < count; ++z)
			{
				for (uint32_t x = 0; x < count; ++x)
				{
					terrain.Vertices.emplace_back(static_cast<float>(x) * field.CellSize.x, field.Heights[x + static_cast<size_t>(count) * z],
						static_cast<float>(z) * field.CellSize.y);
				}
			}
			for (uint32_t z = 0; z + 1 < count; ++z)
			{
				for (uint32_t x = 0; x + 1 < count; ++x)
				{
					const uint32_t corner = x + count * z;
					// (x, z), (x, z + 1), (x + 1, z + 1) and (x, z), (x + 1, z + 1), (x + 1, z): both face up.
					terrain.AddTriangle(corner, corner + count, corner + count + 1);
					terrain.AddTriangle(corner, corner + count + 1, corner + 1);
				}
			}
			return terrain;
		}

		// The mesh a MeshCollider uses (PhysicsComposition's rule): its Mesh, else its entity's MeshRenderer mesh.
		static AssetHandle ResolveColliderMesh(ConstEntity entity)
		{
			const MeshColliderComponent& collider = entity.GetComponent<MeshColliderComponent>();
			if (collider.Mesh.IsValid())
				return collider.Mesh.GetHandle();
			if (const MeshRendererComponent* renderer = entity.TryGetComponent<MeshRendererComponent>())
				return renderer->Mesh.GetHandle();
			return AssetHandle();
		}

		// The local triangles of `shape`, the collider `collider` of `plan`. A convex hull's points are its mesh's vertex
		// positions in order (DescribePhysicsBodyShape), so the mesh's indices triangulate it.
		static LocalTriangles TriangulateCollider(const Scene& scene, const PhysicsBodyPlan& plan, const ColliderShapeDescription& shape, AssetManager& assets)
		{
			LocalTriangles local;
			if (const auto* box = std::get_if<BoxShapeGeometry>(&shape.Geometry))
				return TriangulateBox(box->HalfExtents);
			if (const auto* sphere = std::get_if<SphereShapeGeometry>(&shape.Geometry))
				return TriangulateRoundShape(sphere->Radius, 0.0f);
			if (const auto* capsule = std::get_if<CapsuleShapeGeometry>(&shape.Geometry))
				return TriangulateRoundShape(capsule->Radius, capsule->HalfHeight);
			if (const auto* mesh = std::get_if<MeshShapeGeometry>(&shape.Geometry))
			{
				local.Vertices = mesh->Vertices;
				local.Indices = mesh->Indices;
				return local;
			}
			if (const auto* hull = std::get_if<ConvexHullShapeGeometry>(&shape.Geometry))
			{
				const ConstEntity entity = scene.FindEntityByID(plan.Colliders[shape.UserData].Entity);
				const AssetRef<MeshData> data = assets.GetOrPlaceholder<MeshData>(ResolveColliderMesh(entity));
				local.Vertices = hull->Points;
				for (const uint32_t index : data->Indices)
				{
					if (index >= local.Vertices.size())
						return LocalTriangles{};
				}
				local.Indices = data->Indices;
				return local;
			}
			if (const auto* field = std::get_if<HeightFieldShapeGeometry>(&shape.Geometry))
				return TriangulateHeightField(*field);
			// A SharedShapeGeometry comes only with a mesh shape cache, which the bake never passes.
			return local;
		}

		// Appends `local` placed by `shape` within its body and the body at (`position`, `rotation`) to `input`, turning the
		// winding back when the placement mirrors it.
		static void AppendPlaced(NavigationBakeInput& input, const LocalTriangles& local, const ColliderShapeDescription& shape, const glm::vec3& position,
			const glm::quat& rotation)
		{
			const uint32_t base = static_cast<uint32_t>(input.Vertices.size());
			for (const glm::vec3& vertex : local.Vertices)
				input.Vertices.push_back(position + rotation * (shape.Position + shape.Rotation * (shape.Scale * vertex)));
			const bool mirrored = shape.Scale.x * shape.Scale.y * shape.Scale.z < 0.0f;
			for (size_t index = 0; index + 2 < local.Indices.size(); index += 3)
			{
				const uint32_t a = base + local.Indices[index];
				const uint32_t b = base + local.Indices[index + 1];
				const uint32_t c = base + local.Indices[index + 2];
				if (mirrored)
					input.Indices.insert(input.Indices.end(), { a, c, b });
				else
					input.Indices.insert(input.Indices.end(), { a, b, c });
			}
		}

		[[nodiscard]] static bool IsInRange(float value, float minimum, float maximum)
		{
			return std::isfinite(value) && value >= minimum && value <= maximum;
		}

		[[nodiscard]] static bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// Whether every corner and the vertical extent of `modifier` are finite.
		[[nodiscard]] static bool IsFinite(const NavBakeModifier& modifier)
		{
			bool finite = std::isfinite(modifier.MinY) && std::isfinite(modifier.MaxY);
			for (const glm::vec2& corner : modifier.Corners)
				finite = finite && std::isfinite(corner.x) && std::isfinite(corner.y);
			return finite;
		}

		// The component's settings against its ranges (the registry's, which a file or a patch may have bypassed).
		[[nodiscard]] static Status CheckSettings(const NavigationComponent& navigation)
		{
			if (navigation.AgentTypes.empty() || navigation.AgentTypes.size() > MaxNavAgentTypes)
				return MakeError(ErrorCode::Validation, "it has {} agent types; a Navigation component has 1 to {}", navigation.AgentTypes.size(), MaxNavAgentTypes);
			std::set<std::string_view> names;
			for (const NavAgentType& agent : navigation.AgentTypes)
			{
				if (agent.Name.empty() || agent.Name.size() > MaxNavAgentNameLength || !names.insert(agent.Name).second)
					return MakeError(ErrorCode::Validation, "the agent type '{}' needs a unique name of 1 to {} bytes", agent.Name, MaxNavAgentNameLength);
				if (!IsInRange(agent.Radius, MinNavAgentRadius, MaxNavAgentRadius) || !IsInRange(agent.Height, MinNavAgentHeight, MaxNavAgentHeight)
					|| !IsInRange(agent.MaxSlope, 0.0f, MaxNavAgentSlope) || !IsInRange(agent.MaxStep, 0.0f, MaxNavAgentStep))
				{
					return MakeError(ErrorCode::Validation, "the agent type '{}' has a Radius, Height, MaxSlope or MaxStep outside its range", agent.Name);
				}
			}
			if (!IsInRange(navigation.CellSize, MinNavCellSize, MaxNavCellSize) || !IsInRange(navigation.CellHeight, MinNavCellSize, MaxNavCellSize))
			{
				return MakeError(ErrorCode::Validation, "its cells are {} x {} metres; each is in [{}, {}]", navigation.CellSize, navigation.CellHeight, MinNavCellSize,
					MaxNavCellSize);
			}
			if (!IsInRange(navigation.MinRegionArea, 0.0f, 10000.0f))
				return MakeError(ErrorCode::Validation, "its MinRegionArea is {}; it is in [0, 10000] square metres", navigation.MinRegionArea);
			return {};
		}

		// The modifier box of `entity`'s NavModifier (see the file comment): the yaw axes from the world rotation's X axis
		// (its Z axis when X points straight up or down), with square roots only.
		static NavBakeModifier MakeModifier(ConstEntity entity, const NavModifierComponent& modifier)
		{
			const glm::mat4 world = TransformSystem::ComputeWorldMatrix(entity);
			const glm::vec3 center(world[3]);
			const glm::quat rotation = TransformSystem::GetWorldRotation(entity);
			const glm::vec3 scale = glm::abs(TransformSystem::GetWorldScale(entity));
			const glm::vec3 rotatedX = rotation * glm::vec3(1.0f, 0.0f, 0.0f);
			glm::vec2 axisX(rotatedX.x, rotatedX.z);
			glm::vec2 axisZ{ 0.0f };
			if (glm::dot(axisX, axisX) > 1.0e-12f)
			{
				axisX /= std::sqrt(glm::dot(axisX, axisX));
				axisZ = glm::vec2(-axisX.y, axisX.x);
			}
			else
			{
				const glm::vec3 rotatedZ = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
				axisZ = glm::vec2(rotatedZ.x, rotatedZ.z);
				const float length = std::sqrt(glm::dot(axisZ, axisZ));
				axisZ = length > 0.0f ? axisZ / length : glm::vec2(0.0f, 1.0f);
				axisX = glm::vec2(axisZ.y, -axisZ.x);
			}
			const glm::vec2 center2(center.x, center.z);
			const glm::vec2 alongX = axisX * (modifier.HalfExtents.x * scale.x);
			const glm::vec2 alongZ = axisZ * (modifier.HalfExtents.z * scale.z);
			const float halfHeight = modifier.HalfExtents.y * scale.y;
			NavBakeModifier box;
			box.Entity = entity.GetUUID();
			box.Kind = modifier.Kind;
			box.Cost = modifier.Cost;
			box.Corners = { center2 + alongX + alongZ, center2 + alongX - alongZ, center2 - alongX - alongZ, center2 - alongX + alongZ };
			box.MinY = center.y - halfHeight;
			box.MaxY = center.y + halfHeight;
			return box;
		}

	}

	Result<NavigationBakeInput> CollectNavigationBakeInput(const Scene& scene, const PhysicsLayerTable& layers, AssetManager& assets)
	{
		NavigationBakeInput input;
		bool found = false;
		scene.ForEachCanonical([&input, &found](ConstEntity entity)
		{
			if (found || !entity.IsActive())
				return;
			if (const NavigationComponent* navigation = entity.TryGetComponent<NavigationComponent>())
			{
				input.NavigationEntity = entity.GetUUID();
				input.Settings = *navigation;
				found = true;
			}
		});
		if (!found)
		{
			return std::unexpected(Error(ErrorCode::NotFound, std::format("scene '{}' has no Navigation component on an enabled entity", scene.GetName()))
					.WithHint("add a Navigation component (entity.update {components: {Navigation: {}}}) to an entity of the scene"));
		}
		const std::string navigationEntity = input.NavigationEntity.ToString();
		ENGINE_TRY(WithContext(Utils::CheckSettings(input.Settings), std::format("the Navigation component of {}", navigationEntity)));

		std::set<uint32_t> readLayers;
		for (const std::string& name : input.Settings.Layers)
		{
			const std::optional<uint32_t> index = layers.FindLayer(name);
			if (!index.has_value())
			{
				return std::unexpected(
					Error(ErrorCode::Validation, std::format("the Navigation component of {} names the layer '{}', which the project does not define", navigationEntity, name))
						.WithHint("name layers of the project's Physics.Layers settings, or leave Layers empty to read every layer"));
			}
			readLayers.insert(*index);
		}

		const PhysicsComposition composition = ComposePhysicsBodies(scene, layers);
		for (const PhysicsBodyPlan& plan : composition.Bodies)
		{
			if (!plan.IsCreatable || plan.IsSensor || plan.MotionType != PhysicsMotionType::Static || plan.Origin == PhysicsBodyOrigin::Character)
				continue;
			if (!readLayers.empty() && !readLayers.contains(plan.Layer))
				continue;
			const Result<BodyShapeDescription> description = DescribePhysicsBodyShape(scene, plan, &assets);
			if (!description.has_value())
			{
				input.SkippedBodies.push_back(plan.Owner);
				continue;
			}
			const ConstEntity owner = scene.FindEntityByID(plan.Owner);
			const glm::vec3 position(TransformSystem::ComputeWorldMatrix(owner)[3]);
			const glm::quat rotation = TransformSystem::GetWorldRotation(owner);
			const size_t first = input.Vertices.size();
			for (const ColliderShapeDescription& shape : description->Colliders)
				Utils::AppendPlaced(input, Utils::TriangulateCollider(scene, plan, shape, assets), shape, position, rotation);
			for (size_t vertex = first; vertex < input.Vertices.size(); ++vertex)
			{
				if (!Utils::IsFinite(input.Vertices[vertex]))
				{
					return std::unexpected(Error(ErrorCode::Validation,
						std::format("the static body of {} is placed where a coordinate is not finite: its world transform overflows", plan.Owner.ToString()))
							.WithHint("keep the entity's and its parents' translations and scales within float's range"));
				}
			}
		}

		std::optional<UUID> unplaced;
		scene.ForEachCanonical([&input, &unplaced](ConstEntity entity)
		{
			if (unplaced.has_value() || !entity.IsActive())
				return;
			if (const NavModifierComponent* modifier = entity.TryGetComponent<NavModifierComponent>())
			{
				input.Modifiers.push_back(Utils::MakeModifier(entity, *modifier));
				if (!Utils::IsFinite(input.Modifiers.back()))
					unplaced = entity.GetUUID();
			}
		});
		if (unplaced.has_value())
		{
			return std::unexpected(Error(ErrorCode::Validation,
				std::format("the NavModifier of {} is placed where a coordinate is not finite: its world transform overflows", unplaced->ToString()))
					.WithHint("keep the entity's and its parents' translations and scales within float's range"));
		}
		return input;
	}

	Buffer SerializeNavigationBakeInput(const NavigationBakeInput& input)
	{
		BinaryWriter writer;
		writer.WriteU32(NavigationBakeInputVersion);
		const NavigationComponent& settings = input.Settings;
		writer.WriteU32(static_cast<uint32_t>(settings.AgentTypes.size()));
		for (const NavAgentType& agent : settings.AgentTypes)
		{
			writer.WriteString(agent.Name);
			for (const float value : { agent.Radius, agent.Height, agent.MaxSlope, agent.MaxStep })
				writer.WriteF32(value);
		}
		writer.WriteU32(static_cast<uint32_t>(settings.Layers.size()));
		for (const std::string& layer : settings.Layers)
			writer.WriteString(layer);
		for (const float value : { settings.CellSize, settings.CellHeight, settings.MinRegionArea })
			writer.WriteF32(value);
		writer.WriteU64(input.Vertices.size());
		for (const glm::vec3& vertex : input.Vertices)
		{
			for (glm::length_t axis = 0; axis < 3; ++axis)
				writer.WriteF32(vertex[axis]);
		}
		writer.WriteU64(input.Indices.size());
		for (const uint32_t index : input.Indices)
			writer.WriteU32(index);
		writer.WriteU32(static_cast<uint32_t>(input.Modifiers.size()));
		for (const NavBakeModifier& modifier : input.Modifiers)
		{
			writer.WriteU8(static_cast<uint8_t>(modifier.Kind));
			// A NotWalkable box's Cost is never read by the bake, so it never makes a mesh stale.
			if (modifier.Kind == NavModifierKind::Cost)
				writer.WriteF32(modifier.Cost);
			for (const glm::vec2& corner : modifier.Corners)
			{
				writer.WriteF32(corner.x);
				writer.WriteF32(corner.y);
			}
			writer.WriteF32(modifier.MinY);
			writer.WriteF32(modifier.MaxY);
		}
		return writer.TakeBuffer();
	}

	uint64_t HashNavigationBakeInput(const NavigationBakeInput& input)
	{
		return XXH64(SerializeNavigationBakeInput(input));
	}

}
