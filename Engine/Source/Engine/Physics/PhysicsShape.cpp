#include "EnginePCH.h"
#include "Engine/Physics/PhysicsShape.h"

#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Physics/Private/PhysicsConversions.h"
#include "Engine/Physics/Private/PhysicsMassProperties.h"
#include "Engine/Physics/Private/PhysicsWorldState.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaleHelpers.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <cmath>

namespace Engine {

	namespace Utils {

		using JoltShapeResult = Result<JPH::RefConst<JPH::Shape>>;

		// "PHYSICS_INVALID_SHAPE: collider <index>: <reason>".
		template<typename... Args>
		[[nodiscard]] static std::unexpected<Error> MakeInvalidCollider(size_t index, std::format_string<Args...> format, Args&&... args)
		{
			return MakeError(ErrorCode::Validation, "{}: collider {}: {}", PhysicsInvalidShapeCode, index, std::format(format, std::forward<Args>(args)...));
		}

		[[nodiscard]] static std::string FormatVector(const glm::vec3& vector)
		{
			return std::format("({}, {}, {})", vector.x, vector.y, vector.z);
		}

		[[nodiscard]] static bool IsPositive(const glm::vec3& vector)
		{
			return vector.x > 0.0f && vector.y > 0.0f && vector.z > 0.0f;
		}

		[[nodiscard]] static glm::vec3 Abs(const glm::vec3& vector)
		{
			return glm::vec3(std::abs(vector.x), std::abs(vector.y), std::abs(vector.z));
		}

		// The largest axis of a scale (spheres and capsules take a uniform one; this is its size).
		[[nodiscard]] static float GetLargestScaleAxis(const glm::vec3& scale)
		{
			return std::max({ std::abs(scale.x), std::abs(scale.y), std::abs(scale.z) });
		}

		// Whether `scale` turns a shape inside out (an odd number of mirrored axes).
		[[nodiscard]] static bool IsMirroring(const glm::vec3& scale)
		{
			const int mirrored = (scale.x < 0.0f ? 1 : 0) + (scale.y < 0.0f ? 1 : 0) + (scale.z < 0.0f ? 1 : 0);
			return mirrored % 2 == 1;
		}

		// PHYSICS_INVALID_SHAPE when a collider's `size` (its smallest extent, a mesh's largest) is below MinColliderSize.
		[[nodiscard]] static Status CheckColliderSize(float size, size_t index, std::string_view what)
		{
			if (size < PhysicsShape::MinColliderSize)
			{
				return MakeInvalidCollider(index, "the {} is {} m across after scaling, smaller than the {} m a collider needs", what, size,
					PhysicsShape::MinColliderSize);
			}
			return {};
		}

		// PHYSICS_INVALID_SHAPE when a collider's `size` (its largest extent after scaling) is above MaxColliderSize.
		[[nodiscard]] static Status CheckColliderMaxSize(float size, size_t index, std::string_view what)
		{
			if (!(size <= PhysicsShape::MaxColliderSize))
			{
				return MakeInvalidCollider(index, "the {} is {} m across after scaling, larger than the {} m a collider may be", what, size,
					PhysicsShape::MaxColliderSize);
			}
			return {};
		}

		// PHYSICS_INVALID_SHAPE when a coordinate of a collider's geometry (scaled) lies beyond MaxPhysicsCoordinate.
		[[nodiscard]] static Status CheckColliderRange(const glm::vec3& extent, size_t index, std::string_view what)
		{
			if (!IsWithinPhysicsRange(extent))
			{
				return MakeInvalidCollider(index, "the {} reaches {}, beyond the {} m physics handles", what, FormatVector(extent),
					MaxPhysicsCoordinate);
			}
			return {};
		}

		// The extents of the bounding box of `points` (each scaled by `scale`).
		[[nodiscard]] static glm::vec3 GetScaledExtent(const std::vector<glm::vec3>& points, const glm::vec3& scale)
		{
			if (points.empty())
				return glm::vec3(0.0f);
			glm::vec3 minimum = points.front() * scale;
			glm::vec3 maximum = minimum;
			for (const glm::vec3& point : points)
			{
				minimum = glm::min(minimum, point * scale);
				maximum = glm::max(maximum, point * scale);
			}
			return maximum - minimum;
		}

		[[nodiscard]] static float GetSmallestAxis(const glm::vec3& extent)
		{
			return std::min({ extent.x, extent.y, extent.z });
		}

		[[nodiscard]] static float GetLargestAxis(const glm::vec3& extent)
		{
			return std::max({ extent.x, extent.y, extent.z });
		}

		[[nodiscard]] static std::string_view ToStringView(const JPH::String& text)
		{
			return std::string_view(text.data(), text.size());
		}

		// The shape `settings` creates (§9.1: shapes are built only through ShapeSettings::Create), or `onError` with Jolt's
		// message. The settings live on the caller's stack, marked embedded, and no created shape keeps a reference to them.
		template<typename OnError>
		[[nodiscard]] static JoltShapeResult CreateJoltShape(const JPH::ShapeSettings& settings, const OnError& onError)
		{
			const JPH::ShapeSettings::ShapeResult result = settings.Create();
			if (result.HasError())
				return onError(ToStringView(result.GetError()));
			if (!result.IsValid())
				return onError("Jolt created no shape");
			return JPH::RefConst<JPH::Shape>(result.Get());
		}

		[[nodiscard]] static JoltShapeResult CreateColliderShape(const JPH::ShapeSettings& settings, size_t index)
		{
			return CreateJoltShape(settings, [index](std::string_view message)
			{
				return MakeInvalidCollider(index, "{}", message);
			});
		}

		[[nodiscard]] static JoltShapeResult CreateBox(const BoxShapeGeometry& box, const glm::vec3& scale, size_t index)
		{
			if (!Detail::IsFinite(box.HalfExtents) || !IsPositive(box.HalfExtents))
				return MakeInvalidCollider(index, "a box needs finite, positive half extents (got {})", FormatVector(box.HalfExtents));
			const glm::vec3 halfExtents = box.HalfExtents * Abs(scale);
			if (!Detail::IsFinite(halfExtents) || !IsPositive(halfExtents))
				return MakeInvalidCollider(index, "the box's half extents {} scaled by {} are not finite and positive", FormatVector(box.HalfExtents), FormatVector(scale));
			ENGINE_TRY(CheckColliderSize(2.0f * GetSmallestAxis(halfExtents), index, "box"));
			ENGINE_TRY(CheckColliderMaxSize(2.0f * GetLargestAxis(halfExtents), index, "box"));

			// No convex radius: Jolt would round the box's edges with it, and two boxes side by side in a track compound would
			// then meet in a groove as deep as the radius, which a rolling ball feels at every seam (§9.2 "Seams").
			JPH::BoxShapeSettings settings(Detail::ToJolt(halfExtents), 0.0f);
			settings.SetEmbedded();
			settings.SetDensity(Detail::ColliderDensity);
			return CreateColliderShape(settings, index);
		}

		[[nodiscard]] static JoltShapeResult CreateSphere(const SphereShapeGeometry& sphere, const glm::vec3& scale, size_t index)
		{
			if (!Detail::IsFinite(sphere.Radius) || sphere.Radius <= 0.0f)
				return MakeInvalidCollider(index, "a sphere needs a finite, positive radius (got {})", sphere.Radius);
			const float radius = sphere.Radius * GetLargestScaleAxis(scale);
			if (!Detail::IsFinite(radius) || radius <= 0.0f)
				return MakeInvalidCollider(index, "the sphere's radius {} scaled by {} is not finite and positive", sphere.Radius, FormatVector(scale));
			ENGINE_TRY(CheckColliderSize(2.0f * radius, index, "sphere"));
			ENGINE_TRY(CheckColliderMaxSize(2.0f * radius, index, "sphere"));

			JPH::SphereShapeSettings settings(radius);
			settings.SetEmbedded();
			settings.SetDensity(Detail::ColliderDensity);
			return CreateColliderShape(settings, index);
		}

		[[nodiscard]] static JoltShapeResult CreateCapsule(const CapsuleShapeGeometry& capsule, const glm::vec3& scale, size_t index)
		{
			if (!Detail::IsFinite(capsule.HalfHeight) || !Detail::IsFinite(capsule.Radius) || capsule.HalfHeight <= 0.0f || capsule.Radius <= 0.0f)
			{
				return MakeInvalidCollider(index, "a capsule needs a finite, positive half height and radius (got {} and {})", capsule.HalfHeight,
					capsule.Radius);
			}
			const float factor = GetLargestScaleAxis(scale);
			const float halfHeight = capsule.HalfHeight * factor;
			const float radius = capsule.Radius * factor;
			if (!Detail::IsFinite(halfHeight) || !Detail::IsFinite(radius) || halfHeight <= 0.0f || radius <= 0.0f)
			{
				return MakeInvalidCollider(index, "the capsule's half height {} and radius {} scaled by {} are not finite and positive", capsule.HalfHeight,
					capsule.Radius, FormatVector(scale));
			}
			ENGINE_TRY(CheckColliderSize(2.0f * radius, index, "capsule"));
			ENGINE_TRY(CheckColliderMaxSize(2.0f * (halfHeight + radius), index, "capsule"));

			JPH::CapsuleShapeSettings settings(halfHeight, radius);
			settings.SetEmbedded();
			settings.SetDensity(Detail::ColliderDensity);
			return CreateColliderShape(settings, index);
		}

		[[nodiscard]] static JoltShapeResult CreateConvexHull(const ConvexHullShapeGeometry& hull, const glm::vec3& scale, size_t index)
		{
			JPH::Array<JPH::Vec3> points;
			points.reserve(hull.Points.size());
			for (size_t point = 0; point < hull.Points.size(); ++point)
			{
				const glm::vec3 scaled = hull.Points[point] * scale;
				if (!Detail::IsFinite(hull.Points[point]) || !Detail::IsFinite(scaled))
					return MakeInvalidCollider(index, "hull point {} {} scaled by {} is not finite", point, FormatVector(hull.Points[point]), FormatVector(scale));
				ENGINE_TRY(CheckColliderRange(scaled, index, "convex hull"));
				points.push_back(Detail::ToJolt(scaled));
			}
			if (hull.Points.size() >= 4)
			{
				const glm::vec3 extent = GetScaledExtent(hull.Points, scale);
				ENGINE_TRY(CheckColliderSize(GetSmallestAxis(extent), index, "convex hull"));
				ENGINE_TRY(CheckColliderMaxSize(GetLargestAxis(extent), index, "convex hull"));
			}

			// Too few points are refused by Jolt's hull builder with its message. Points in one plane give it a flat hull of two
			// faces without volume, which is refused here: no closed volume (a tetrahedron has the fewest faces, 4) has no mass.
			JPH::ConvexHullShapeSettings settings(points);
			settings.SetEmbedded();
			settings.SetDensity(Detail::ColliderDensity);
			JoltShapeResult created = CreateColliderShape(settings, index);
			if (created.has_value() && (*created)->GetSubType() == JPH::EShapeSubType::ConvexHull)
			{
				const JPH::ConvexHullShape& built = static_cast<const JPH::ConvexHullShape&>(**created);
				if (built.GetNumFaces() < 4 || !(built.GetVolume() > 0.0f))
					return MakeInvalidCollider(index, "the hull's {} points span no volume (they lie in one plane or on one line)", hull.Points.size());
			}
			return created;
		}

		[[nodiscard]] static JoltShapeResult CreateMesh(const MeshShapeGeometry& mesh, const glm::vec3& scale, size_t index)
		{
			if (mesh.Indices.size() % 3 != 0)
				return MakeInvalidCollider(index, "a triangle list needs a multiple of 3 indices (got {})", mesh.Indices.size());

			JPH::VertexList vertices;
			vertices.reserve(mesh.Vertices.size());
			for (size_t vertex = 0; vertex < mesh.Vertices.size(); ++vertex)
			{
				const glm::vec3 scaled = mesh.Vertices[vertex] * scale;
				if (!Detail::IsFinite(mesh.Vertices[vertex]) || !Detail::IsFinite(scaled))
				{
					return MakeInvalidCollider(index, "mesh vertex {} {} scaled by {} is not finite", vertex, FormatVector(mesh.Vertices[vertex]),
						FormatVector(scale));
				}
				ENGINE_TRY(CheckColliderRange(scaled, index, "triangle mesh"));
				vertices.push_back(JPH::Float3(scaled.x, scaled.y, scaled.z));
			}
			if (!mesh.Vertices.empty())
			{
				const float size = GetLargestAxis(GetScaledExtent(mesh.Vertices, scale));
				ENGINE_TRY(CheckColliderSize(size, index, "triangle mesh"));
				ENGINE_TRY(CheckColliderMaxSize(size, index, "triangle mesh"));
			}

			// A mirroring scale turns the mesh inside out; reversing each triangle keeps its faces pointing outwards.
			const bool isMirrored = IsMirroring(scale);
			JPH::IndexedTriangleList triangles;
			triangles.reserve(mesh.Indices.size() / 3);
			for (size_t first = 0; first < mesh.Indices.size(); first += 3)
			{
				for (size_t corner = first; corner < first + 3; ++corner)
				{
					if (mesh.Indices[corner] >= mesh.Vertices.size())
					{
						return MakeInvalidCollider(index, "mesh index {} at position {} is out of range: the mesh has {} vertices", mesh.Indices[corner], corner,
							mesh.Vertices.size());
					}
				}
				const uint32_t a = mesh.Indices[first];
				const uint32_t b = mesh.Indices[first + 1];
				const uint32_t c = mesh.Indices[first + 2];
				triangles.push_back(isMirrored ? JPH::IndexedTriangle(a, c, b, 0) : JPH::IndexedTriangle(a, b, c, 0));
			}

			// The settings drop degenerate triangles; Jolt refuses a mesh left without any.
			JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
			settings.SetEmbedded();
			return CreateColliderShape(settings, index);
		}

		[[nodiscard]] static std::string_view GetGeometryName(const PhysicsShapeGeometry& geometry)
		{
			if (std::holds_alternative<BoxShapeGeometry>(geometry))
				return "box";
			if (std::holds_alternative<SphereShapeGeometry>(geometry))
				return "sphere";
			if (std::holds_alternative<CapsuleShapeGeometry>(geometry))
				return "capsule";
			if (std::holds_alternative<ConvexHullShapeGeometry>(geometry))
				return "convex hull";
			if (std::holds_alternative<MeshShapeGeometry>(geometry))
				return "triangle mesh";
			return "shared shape";
		}

		// The largest coordinate, in metres, of a whole shape's bounds and centre of mass in body space: every collider
		// Create accepts lies within about 2.8 * MaxPhysicsCoordinate of the body's origin (an offset of up to
		// MaxPhysicsCoordinate per axis plus a rotated extent of up to MaxPhysicsCoordinate per axis). Far below Jolt's
		// cLargeFloat (1e30), which bounds the world bounds its broad phase takes.
		constexpr float MaxShapeCoordinate = 4.0f * MaxPhysicsCoordinate;

		// PHYSICS_INVALID_SHAPE unless `shape`'s centre of mass and bounds in body space are finite and within
		// MaxShapeCoordinate: Jolt computes a compound's centre of mass from its colliders' masses and places its sub-shapes
		// around it, so a sum that overflowed would give the broad phase infinite bounds, which it asserts on. `what` names
		// the shape in the message ("collider 0", "the compound of 3 colliders").
		[[nodiscard]] static Status CheckShapeExtent(const JPH::Shape& shape, std::string_view what)
		{
			JPH::AABox bounds = shape.GetLocalBounds();
			bounds.Translate(shape.GetCenterOfMass());
			const glm::vec3 minimum = Detail::ToGlm(bounds.mMin);
			const glm::vec3 maximum = Detail::ToGlm(bounds.mMax);
			const auto isWithin = [](const glm::vec3& point)
			{
				return std::abs(point.x) <= MaxShapeCoordinate && std::abs(point.y) <= MaxShapeCoordinate && std::abs(point.z) <= MaxShapeCoordinate;
			};
			if (!isWithin(Detail::ToGlm(shape.GetCenterOfMass())) || !isWithin(minimum) || !isWithin(maximum))
			{
				return MakeError(ErrorCode::Validation, "{}: {}: its bounds {} to {} are not finite or reach beyond the {} m physics handles", PhysicsInvalidShapeCode,
					what, FormatVector(minimum), FormatVector(maximum), MaxShapeCoordinate);
			}
			return {};
		}

	}

	PhysicsShape::PhysicsShape(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	PhysicsShape::~PhysicsShape() = default;

	Result<Ref<const PhysicsShape>> PhysicsShape::Create(const BodyShapeDescription& description)
	{
		if (!PhysicsEngine::IsInitialized())
			return MakeError(ErrorCode::InvalidState, "physics shapes need the physics engine, which is not initialized");
		if (description.Colliders.empty())
			return MakeError(ErrorCode::Validation, "{}: a body shape needs at least one collider", PhysicsInvalidShapeCode);

		Ref<PhysicsShape> shape = CreateRef<PhysicsShape>(ConstructionKey{});
		State& state = *shape->m_State;
		std::vector<JPH::RefConst<JPH::Shape>> colliders;
		colliders.reserve(description.Colliders.size());
		for (size_t index = 0; index < description.Colliders.size(); ++index)
		{
			const ColliderShapeDescription& collider = description.Colliders[index];
			if (!Detail::IsFinite(collider.Scale))
				return Utils::MakeInvalidCollider(index, "the scale {} is not finite", Utils::FormatVector(collider.Scale));
			if (!Detail::IsFinite(collider.Position))
				return Utils::MakeInvalidCollider(index, "the position {} is not finite", Utils::FormatVector(collider.Position));
			ENGINE_TRY(Utils::CheckColliderRange(collider.Position, index, "collider's position"));
			if (!IsPhysicsUnitRotation(collider.Rotation))
			{
				return Utils::MakeInvalidCollider(index, "the rotation ({}, {}, {}, {}) is not a unit quaternion", collider.Rotation.w, collider.Rotation.x,
					collider.Rotation.y, collider.Rotation.z);
			}

			const SharedShapeGeometry* shared = std::get_if<SharedShapeGeometry>(&collider.Geometry);
			if (shared != nullptr && (shared->Shape == nullptr || shared->Shape->m_State->Shape == nullptr))
				return Utils::MakeInvalidCollider(index, "a shared shape needs a built shape");
			if (shared != nullptr && shared->Shape->m_State->ColliderUserData.size() != 1)
			{
				return Utils::MakeInvalidCollider(index, "a shared shape holds exactly one collider (got {})",
					shared->Shape->m_State->ColliderUserData.size());
			}
			if (!IsValidScale(collider.Geometry, collider.Scale))
			{
				return Utils::MakeInvalidCollider(index, "the scale {} is not valid for a {} (every axis must be non-zero; spheres and capsules need a "
														 "uniform scale)",
					Utils::FormatVector(collider.Scale), Utils::GetGeometryName(collider.Geometry));
			}

			Utils::JoltShapeResult created = MakeError(ErrorCode::Unknown, "no geometry");
			if (const BoxShapeGeometry* box = std::get_if<BoxShapeGeometry>(&collider.Geometry))
				created = Utils::CreateBox(*box, collider.Scale, index);
			else if (const SphereShapeGeometry* sphere = std::get_if<SphereShapeGeometry>(&collider.Geometry))
				created = Utils::CreateSphere(*sphere, collider.Scale, index);
			else if (const CapsuleShapeGeometry* capsule = std::get_if<CapsuleShapeGeometry>(&collider.Geometry))
				created = Utils::CreateCapsule(*capsule, collider.Scale, index);
			else if (const ConvexHullShapeGeometry* hull = std::get_if<ConvexHullShapeGeometry>(&collider.Geometry))
				created = Utils::CreateConvexHull(*hull, collider.Scale, index);
			else if (const MeshShapeGeometry* mesh = std::get_if<MeshShapeGeometry>(&collider.Geometry))
			{
				created = Utils::CreateMesh(*mesh, collider.Scale, index);
				state.HasMesh = true;
			}
			else if (shared != nullptr)
			{
				const State& sharedState = *shared->Shape->m_State;
				state.HasMesh = state.HasMesh || sharedState.HasMesh;
				if (collider.Scale == glm::vec3(1.0f))
				{
					created = sharedState.Shape;
				}
				else
				{
					// The shared shape passed the size check when it was built; scaled, it must pass again.
					const glm::vec3 extent = Detail::ToGlm(sharedState.Shape->GetLocalBounds().GetSize()) * Utils::Abs(collider.Scale);
					ENGINE_TRY(Utils::CheckColliderSize(sharedState.HasMesh ? Utils::GetLargestAxis(extent) : Utils::GetSmallestAxis(extent), index, "shared shape"));
					ENGINE_TRY(Utils::CheckColliderMaxSize(Utils::GetLargestAxis(extent), index, "shared shape"));
					JPH::ScaledShapeSettings settings(sharedState.Shape.GetPtr(), Detail::ToJolt(collider.Scale));
					settings.SetEmbedded();
					created = Utils::CreateColliderShape(settings, index);
				}
			}
			if (!created.has_value())
				return std::unexpected(std::move(created).error());
			colliders.push_back(std::move(*created));
			state.ColliderUserData.push_back(collider.UserData);
		}

		if (colliders.size() == 1)
		{
			// One collider is its own shape, placed by a RotatedTranslatedShape when it is not at the body's origin.
			const ColliderShapeDescription& collider = description.Colliders.front();
			if (collider.Position == glm::vec3(0.0f) && collider.Rotation == glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
			{
				state.Shape = colliders.front();
			}
			else
			{
				JPH::RotatedTranslatedShapeSettings settings(Detail::ToJolt(collider.Position), Detail::ToJoltRotation(collider.Rotation),
					colliders.front().GetPtr());
				settings.SetEmbedded();
				ENGINE_TRY_ASSIGN(state.Shape, Utils::CreateColliderShape(settings, 0));
			}
			ENGINE_TRY(Utils::CheckShapeExtent(*state.Shape, "collider 0"));
			return Ref<const PhysicsShape>(std::move(shape));
		}

		// Several colliders form a StaticCompoundShape (§5.3), each placed by the compound with its collider index as the
		// sub-shape user data (§9.2 "Collider identity in compounds").
		JPH::StaticCompoundShapeSettings compound;
		compound.SetEmbedded();
		for (size_t index = 0; index < colliders.size(); ++index)
		{
			const ColliderShapeDescription& collider = description.Colliders[index];
			compound.AddShape(Detail::ToJolt(collider.Position), Detail::ToJoltRotation(collider.Rotation), colliders[index].GetPtr(), collider.UserData);
		}
		const size_t colliderCount = colliders.size();
		ENGINE_TRY_ASSIGN(state.Shape, Utils::CreateJoltShape(compound, [colliderCount](std::string_view message)
		{
			return MakeError(ErrorCode::Validation, "{}: the compound of {} colliders: {}", PhysicsInvalidShapeCode, colliderCount, message);
		}));
		ENGINE_TRY(Utils::CheckShapeExtent(*state.Shape, std::format("the compound of {} colliders", colliderCount)));
		return Ref<const PhysicsShape>(std::move(shape));
	}

	Status PhysicsShape::CheckDynamicBody(float mass, PhysicsDofs allowedDofs) const
	{
		if (m_State->Shape == nullptr)
			return MakeError(ErrorCode::InvalidState, "the shape was not built");
		const Result<JPH::MassProperties> properties = Detail::GetDynamicMassProperties(*m_State->Shape, m_State->HasMesh, mass, allowedDofs);
		if (!properties)
			return std::unexpected(properties.error());
		return {};
	}

	bool PhysicsShape::IsValidScale(const PhysicsShapeGeometry& geometry, const glm::vec3& scale)
	{
		if (!Detail::IsFinite(scale))
			return false;
		// Jolt's rules (Shape::IsValidScale and its overrides): no axis below ScaleHelpers::cMinScale, and a uniform scale
		// for spheres and capsules. They need no shape, so primitives are checked without building one.
		const JPH::Vec3 joltScale = Detail::ToJolt(scale);
		if (JPH::ScaleHelpers::IsZeroScale(joltScale))
			return false;
		if (const SharedShapeGeometry* shared = std::get_if<SharedShapeGeometry>(&geometry))
			return shared->Shape != nullptr && shared->Shape->m_State->Shape != nullptr && shared->Shape->m_State->Shape->IsValidScale(joltScale);
		if (std::holds_alternative<SphereShapeGeometry>(geometry) || std::holds_alternative<CapsuleShapeGeometry>(geometry))
			return JPH::ScaleHelpers::IsUniformScale(joltScale.Abs());
		return true;
	}

	Aabb PhysicsShape::GetLocalBounds() const
	{
		if (m_State->Shape == nullptr)
			return {};
		// Jolt's local bounds are centred on the centre of mass; body space has the body's origin.
		JPH::AABox bounds = m_State->Shape->GetLocalBounds();
		bounds.Translate(m_State->Shape->GetCenterOfMass());
		return Detail::ToAabb(bounds);
	}

	uint32_t PhysicsShape::GetColliderCount() const
	{
		return static_cast<uint32_t>(m_State->ColliderUserData.size());
	}

	std::optional<Aabb> PhysicsShape::GetColliderLocalBounds(uint32_t userData) const
	{
		if (m_State->Shape == nullptr)
			return std::nullopt;
		if (m_State->ColliderUserData.size() == 1)
		{
			if (m_State->ColliderUserData.front() != userData)
				return std::nullopt;
			return GetLocalBounds();
		}

		// The compound's sub-shapes are placed relative to its centre of mass.
		const JPH::CompoundShape& compound = static_cast<const JPH::CompoundShape&>(*m_State->Shape);
		const JPH::Mat44 compoundToBody = JPH::Mat44::sTranslation(compound.GetCenterOfMass());
		std::optional<Aabb> bounds;
		for (JPH::uint subShape = 0; subShape < compound.GetNumSubShapes(); ++subShape)
		{
			if (compound.GetCompoundUserData(subShape) != userData)
				continue;
			const JPH::CompoundShape::SubShape& part = compound.GetSubShape(subShape);
			const Aabb partBounds = Detail::ToAabb(part.mShape->GetWorldSpaceBounds(compoundToBody * part.GetLocalTransformNoScale(JPH::Vec3::sOne()), JPH::Vec3::sOne()));
			if (bounds.has_value())
				bounds->Extend(partBounds);
			else
				bounds = partBounds;
		}
		return bounds;
	}

}
