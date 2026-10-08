#pragma once

#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Physics/PhysicsTypes.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

// Collision shapes in engine types (Architecture §9.1, §9.2). A body's shape is built from one or more collider shapes:
// each is a primitive (box, sphere, capsule), a convex hull, a triangle mesh or an already built shape that bodies share
// (SharedShapeGeometry: the play session's mesh shape cache), scaled, then rotated and translated within the body (Jolt's
// RotatedTranslatedShape). Several colliders form a StaticCompoundShape.
//
// Collider identity (§9.2 "Collider identity in compounds"): each collider's index (ColliderShapeDescription::UserData)
// is the compound's sub-shape user data (CompoundShapeSettings::AddShape(..., userData)). The Physics module resolves a
// contact's or query hit's SubShapeID by its first level, the compound's sub-shape index (CompoundShape::
// GetSubShapeIndexFromID), then GetCompoundUserData; never through Shape::GetSubShapeUserData, which returns the leaf
// shape's own user data and so cannot tell apart colliders that share a leaf. A shape with a single collider is not a
// compound: every hit on it is that collider's UserData.
//
// Nothing invalid reaches Jolt (§9.1): every dimension and coordinate is checked finite and positive where it must be,
// shapes are created only through JPH::ShapeSettings::Create (a ShapeResult error becomes PHYSICS_INVALID_SHAPE with Jolt's
// message) and scales are checked with Shape::IsValidScale before they are applied.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 5).

namespace Engine {

	// A box centred on its origin. Every half extent > 0.
	struct BoxShapeGeometry
	{
		glm::vec3 HalfExtents = glm::vec3(0.5f);
	};

	// A sphere centred on its origin. Radius > 0.
	struct SphereShapeGeometry
	{
		float Radius = 0.5f;
	};

	// A capsule along the local Y axis centred on its origin: a cylinder of half height HalfHeight capped by hemispheres of
	// Radius (CapsuleColliderComponent's convention). Both > 0.
	struct CapsuleShapeGeometry
	{
		float HalfHeight = 0.5f;
		float Radius = 0.5f;
	};

	// The convex hull of Points (MeshCollider with Convex, §9.2): at least 4 points that span a volume, else
	// PHYSICS_INVALID_SHAPE from Jolt's hull builder.
	struct ConvexHullShapeGeometry
	{
		std::vector<glm::vec3> Points{};
	};

	// A triangle mesh (MeshCollider without Convex): Indices is a triangle list into Vertices (3 per triangle, each
	// < Vertices.size()). Static and Kinematic bodies only (PHYSICS_NONCONVEX_DYNAMIC); a mesh never collides with another
	// mesh (PhysicsWorld drops such pairs: Jolt has no mesh-versus-mesh collision). Degenerate triangles are dropped by
	// Jolt, and a mesh left without a triangle is PHYSICS_INVALID_SHAPE.
	struct MeshShapeGeometry
	{
		std::vector<glm::vec3> Vertices{};
		std::vector<uint32_t> Indices{};
	};

	class PhysicsShape;

	// An already built shape of exactly one collider (a PhysicsShape that Create made from a one-collider description),
	// shared by every body that places it: Scene's PhysicsMeshShapeCache builds each (mesh, version, scale) once and hands it
	// out this way (§9.2 "Mesh shapes are cached per (mesh handle, version, scale)"), so rebuilding a compound never
	// rebuilds its meshes. Its own collider UserData is ignored (the placing description's counts, see the file comment);
	// a mesh inside it still makes the shape unusable for Dynamic bodies (PHYSICS_NONCONVEX_DYNAMIC). A missing shape or
	// one with several colliders is PHYSICS_INVALID_SHAPE.
	struct SharedShapeGeometry
	{
		Ref<const PhysicsShape> Shape{};
	};

	using PhysicsShapeGeometry =
		std::variant<BoxShapeGeometry, SphereShapeGeometry, CapsuleShapeGeometry, ConvexHullShapeGeometry, MeshShapeGeometry, SharedShapeGeometry>;

	// One collider within its body: Geometry scaled by Scale (in the collider's own axes), then rotated by Rotation and
	// moved to Position, both relative to the body's origin. Spheres and capsules need a uniform Scale (Shape::IsValidScale;
	// Scene/PhysicsSystem passes the largest axis with PHYSICS_NONUNIFORM_SCALE first); a negative component mirrors.
	struct ColliderShapeDescription
	{
		PhysicsShapeGeometry Geometry = BoxShapeGeometry{};
		glm::vec3 Scale = glm::vec3(1.0f);
		glm::vec3 Position = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity, unit
		// The collider index contacts and queries report for this collider (§9.2; the compound's sub-shape user data, see
		// the file comment): Scene/PhysicsSystem's index into the body's collider table.
		uint32_t UserData = 0;
	};

	// A body's whole shape: one collider, or several forming a StaticCompoundShape (§5.3). At least one.
	struct BodyShapeDescription
	{
		std::vector<ColliderShapeDescription> Colliders{};
	};

	// An immutable collision shape (a pimpl over a Jolt shape reference, JPH::RefConst<JPH::Shape>): shared by the bodies
	// built from it and, through SharedShapeGeometry, by the compounds that place a cached mesh shape (§9.2: "Mesh shapes
	// are cached per (mesh handle, version, scale)"), so it is handed out as Ref<const PhysicsShape> (shared immutable
	// data, §4.7). Creation needs the process's PhysicsEngine (Jolt's factory and allocator). Thread-safe to share; created
	// and destroyed on the main thread.
	class PhysicsShape
	{
	public:
		// Restricts construction to Create; CreateRef still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class PhysicsShape;
		};

		// Use Create.
		explicit PhysicsShape(ConstructionKey key);
		~PhysicsShape();

		PhysicsShape(const PhysicsShape&) = delete;
		PhysicsShape& operator=(const PhysicsShape&) = delete;

		// The shape of `description`: each collider's geometry through its JPH::ShapeSettings::Create (a SharedShapeGeometry's
		// shape as it is), Scale applied (a ScaledShape, or baked into a primitive's dimensions), placed by a
		// RotatedTranslatedShape when Position or Rotation is not the identity, and with several colliders a
		// StaticCompoundShape whose sub-shape user data are the colliders' UserData. A one-collider description at the
		// identity placement gives that collider's (scaled) shape itself, which is what SharedShapeGeometry takes. Errors:
		// InvalidState when the PhysicsEngine is not initialized; Validation "PHYSICS_INVALID_SHAPE: collider <i>: <reason>"
		// for no collider, a non-finite or non-positive dimension, a non-finite or zero scale component, a scale IsValidScale
		// refuses, a non-unit rotation, a hull or mesh index out of range, a SharedShapeGeometry without a shape or with
		// several colliders, a collider smaller than MinColliderSize or larger than MaxColliderSize after scaling, a position or
		// scaled coordinate beyond MaxPhysicsCoordinate, a ShapeResult error (its text quoted), or a whole shape whose bounds or centre of mass are not
		// finite or lie beyond four times MaxPhysicsCoordinate. Colliders are built with a uniform density far below Jolt's
		// default (a power of two times it), so the sums Jolt makes for a compound's centre of mass stay finite; no body uses
		// a shape's own mass.
		// Nothing is created on error.
		[[nodiscard]] static Result<Ref<const PhysicsShape>> Create(const BodyShapeDescription& description);

		// The smallest collider Create accepts, in metres after scaling: twice the registry's 1 mm minimum dimension
		// (MinColliderDimension), measured on the collider's smallest extent (a triangle mesh, which may be flat, on its
		// largest). Below it Jolt's collision tolerances (about 0.1 mm) break down and its asserts can be reached.
		static constexpr float MinColliderSize = 0.002f;
		// The largest collider Create accepts, in metres after scaling, measured on the collider's largest extent (a box's
		// edge, a sphere's diameter, a capsule's height, a hull's or mesh's bounding box). Jolt finds how deep two convex
		// shapes (a mesh's triangles included) penetrate with float products of about the sixth power of their size (EPA),
		// which overflow for colliders a few thousand kilometres across and lose contacts (in MSVC Debug, Jolt's floating-
		// point exceptions end the process) well before; at 100 km two colliders keep five orders of magnitude of margin.
		// Positions and offsets may still reach MaxPhysicsCoordinate.
		static constexpr float MaxColliderSize = 1.0e5f;

		// Whether `scale` can be applied to `geometry` (Jolt's Shape::IsValidScale on the unscaled shape, a
		// SharedShapeGeometry's on its shape: spheres and capsules need |x| = |y| = |z|; every component non-zero and
		// finite). A SharedShapeGeometry without a shape takes no scale.
		[[nodiscard]] static bool IsValidScale(const PhysicsShapeGeometry& geometry, const glm::vec3& scale);

		// Whether a Dynamic body of `mass` kilograms whose degrees of freedom are `allowedDofs` can take this shape: the
		// check PhysicsWorld::CreateBody and SetShape make for every Dynamic body (its mass properties are the shape's scaled
		// to `mass`), so the edit-time validation reports what the world would refuse. Errors: InvalidArgument for a mass
		// that is not finite or not in (0, MaxPhysicsMass]; Validation "PHYSICS_ALL_DOFS_LOCKED: ..." for no degree of
		// freedom, or a body that cannot translate whose shape has no rotational inertia; Validation
		// "PHYSICS_NONCONVEX_DYNAMIC: ..." for a shape holding a triangle mesh; Validation "PHYSICS_INVALID_SHAPE: ..." for a
		// shape without volume, or one whose inertia at `mass` exceeds 1e18 kg m^2 (beyond it Jolt's float decomposition of
		// the inertia overflows: a box of MaxPhysicsMass about 75 km across).
		[[nodiscard]] Status CheckDynamicBody(float mass, PhysicsDofs allowedDofs) const;

		// The bounds of the whole shape in body space.
		[[nodiscard]] Aabb GetLocalBounds() const;
		// The number of colliders the shape was created from.
		[[nodiscard]] uint32_t GetColliderCount() const;
		// The bounds of the collider whose UserData is `userData`, in body space; nullopt when no collider has it.
		[[nodiscard]] std::optional<Aabb> GetColliderLocalBounds(uint32_t userData) const;
	private:
		// The Jolt shape and the collider table (Physics/PhysicsShape.cpp; the world reaches the Jolt shape through
		// Physics/Private/).
		struct State;
	private:
		Scope<State> m_State;
	private:
		friend class PhysicsWorld;
		friend class CharacterController;
	};

}
