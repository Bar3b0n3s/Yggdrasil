#include "EnginePCH.h"
#include "Engine/Physics/Physics2DWorld.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Physics/PhysicsDiagnostics.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

// The 2D world (Docs/Decisions/0032-m21-contract.md decision 8): a PhysicsWorld whose API is laid in the plane z = 0 at
// this boundary. Everything handed to the wrapped world is planar (positions and forces with z = 0, rotations, torques and
// angular velocities about Z only), and everything read back is projected onto the plane: poses keep X, Y and the twist
// about Z, velocities X, Y and the angular velocity about Z, and ray hits and contact records their points at z = 0 with
// their normals in the plane. Jolt's plane locks (Physics2DDofs) and the depth rule (GetPhysics2DHalfDepth) keep the
// simulation itself in the plane; the projection of what is read back removes what the extrusion leaves in Jolt's results:
// a contact manifold's points spread over the shapes' common depth along Z, and the rounding that leaves a little Z in the
// contact normal of some pairs.

namespace Engine {

	namespace Utils {

		[[nodiscard]] static bool IsFinite(const glm::vec2& vector)
		{
			return std::isfinite(vector.x) && std::isfinite(vector.y);
		}

		[[nodiscard]] static glm::vec3 ToPlane(const glm::vec2& vector)
		{
			return glm::vec3(vector.x, vector.y, 0.0f);
		}

		// The rotation of `angle` radians about +Z, computed in double precision through DetMath and rounded to float, so it
		// is identical in every configuration; the identity, exactly, for an angle of 0.
		[[nodiscard]] static glm::quat MakeRotationAboutZ(float angle)
		{
			const SinCosResult<double> half = DetMath::SinCos(static_cast<double>(angle) * 0.5);
			return glm::quat(static_cast<float>(half.Cos), 0.0f, 0.0f, static_cast<float>(half.Sin));
		}

		// Whether a 2D body can be placed at `pose`: a finite position within MaxPhysicsCoordinate and a finite angle (the
		// planar half of IsPlaceablePhysicsPose; the rotation ToPhysicsPose makes of a finite angle is unit).
		[[nodiscard]] static bool IsPlaceablePhysics2DPose(const Physics2DPose& pose)
		{
			return IsWithinPhysicsRange(ToPlane(pose.Position)) && std::isfinite(pose.Angle);
		}

		// `normal` (unit) laid in the plane: unchanged when its z is already 0, otherwise its z dropped and the rest
		// normalized again; `fallback` when less than half of it lies in the plane (a normal along Z, which the depth rule
		// keeps from occurring).
		[[nodiscard]] static glm::vec3 ToPlanarNormal(const glm::vec3& normal, const glm::vec3& fallback)
		{
			if (normal.z == 0.0f)
				return normal;
			const glm::vec2 planar(normal.x, normal.y);
			const float lengthSquared = glm::dot(planar, planar);
			if (!(lengthSquared >= 0.25f) || !std::isfinite(lengthSquared))
				return fallback;
			return ToPlane(planar / std::sqrt(lengthSquared));
		}

		// A ray hit of the plane: its point at z = 0 and its normal in the plane (the reversed ray direction when the normal
		// leaves it).
		static void LayHitInPlane(PhysicsQueryHit& hit, const glm::vec2& direction)
		{
			hit.Point.z = 0.0f;
			hit.Normal = ToPlanarNormal(hit.Normal, -glm::normalize(ToPlane(direction)));
		}

		// "PHYSICS_INVALID_SHAPE: collider <index>: <reason>", as PhysicsShape::Create reports a collider.
		template<typename... Args>
		[[nodiscard]] static std::unexpected<Error> MakeInvalidCollider2D(size_t index, std::format_string<Args...> format, Args&&... args)
		{
			return MakeError(ErrorCode::Validation, "{}: collider {}: {}", PhysicsInvalidShapeCode, index, std::format(format, std::forward<Args>(args)...));
		}

		// The rules MakeBody2DShape applies to a collider's geometry under `scale`, which GetPhysics2DHalfDepth assumes: a
		// finite scale without a zero component; a box's finite, positive half extents; a circle's finite, positive radius
		// under a uniform scale (|x| = |y|); a polygon's 3 or more finite points.
		[[nodiscard]] static Status CheckGeometry2D(const Shape2DGeometry& geometry, const glm::vec2& scale, size_t index)
		{
			if (!IsFinite(scale) || scale.x == 0.0f || scale.y == 0.0f)
				return MakeInvalidCollider2D(index, "the scale ({}, {}) must be finite with no zero component", scale.x, scale.y);
			if (const BoxShape2D* box = std::get_if<BoxShape2D>(&geometry))
			{
				if (!IsFinite(box->HalfExtents) || !(box->HalfExtents.x > 0.0f && box->HalfExtents.y > 0.0f))
					return MakeInvalidCollider2D(index, "a box needs finite, positive half extents (got ({}, {}))", box->HalfExtents.x, box->HalfExtents.y);
			}
			else if (const CircleShape2D* circle = std::get_if<CircleShape2D>(&geometry))
			{
				if (!std::isfinite(circle->Radius) || !(circle->Radius > 0.0f))
					return MakeInvalidCollider2D(index, "a circle needs a finite, positive radius (got {})", circle->Radius);
				if (std::abs(scale.x) != std::abs(scale.y))
					return MakeInvalidCollider2D(index, "a circle needs a uniform scale (got ({}, {}))", scale.x, scale.y);
			}
			else if (const PolygonShape2D* polygon = std::get_if<PolygonShape2D>(&geometry))
			{
				if (polygon->Points.size() < 3)
					return MakeInvalidCollider2D(index, "a polygon needs at least 3 points (got {})", polygon->Points.size());
				for (size_t point = 0; point < polygon->Points.size(); ++point)
				{
					if (!IsFinite(polygon->Points[point]))
						return MakeInvalidCollider2D(index, "polygon point {} ({}, {}) is not finite", point, polygon->Points[point].x, polygon->Points[point].y);
				}
			}
			return {};
		}

		// The query shape of a 2D overlap, laid as a body's collider is (a box extruded by its half depth, a circle as a
		// sphere); nullopt for a shape MakeBody2DShape would refuse (a dimension that is not finite and positive, or a box whose
		// diagonal, its depth, overflows a float).
		[[nodiscard]] static std::optional<PhysicsShapeGeometry> MakeOverlapGeometry(const Overlap2DShape& shape)
		{
			if (const BoxShape2D* box = std::get_if<BoxShape2D>(&shape))
			{
				if (!IsFinite(box->HalfExtents) || !(box->HalfExtents.x > 0.0f && box->HalfExtents.y > 0.0f))
					return std::nullopt;
				const float halfDepth = GetPhysics2DHalfDepth(*box, glm::vec2(1.0f));
				if (!std::isfinite(halfDepth))
					return std::nullopt;
				return BoxShapeGeometry{ .HalfExtents = glm::vec3(box->HalfExtents.x, box->HalfExtents.y, halfDepth) };
			}
			const float radius = std::get<CircleShape2D>(shape).Radius;
			if (!std::isfinite(radius) || !(radius > 0.0f))
				return std::nullopt;
			return SphereShapeGeometry{ .Radius = radius };
		}

	}

	// The wrapped world and the layer table it was created with.
	struct Physics2DWorld::State
	{
		PhysicsLayerTable Layers{};
		Scope<PhysicsWorld> World{};
	};

	PhysicsPose ToPhysicsPose(const Physics2DPose& pose)
	{
		return PhysicsPose{ .Position = Utils::ToPlane(pose.Position), .Rotation = Utils::MakeRotationAboutZ(pose.Angle) };
	}

	Physics2DPose ToPhysics2DPose(const PhysicsPose& pose)
	{
		// The twist about Z of (w, x, y, z) is (w, 0, 0, z) up to its length, an angle of 2 atan2(z, w). A rotation without a
		// twist part (w = z = 0, a half turn about an axis in the plane) gives 0. Otherwise, taking the representative with
		// w >= 0 (and z > 0 at w = 0, where atan2 gives pi/2 whatever the sign of the zero) keeps atan2 in [-pi/2, pi/2], so
		// the angle lies in [-pi, pi]; -pi (the same rotation as pi) becomes pi.
		const glm::vec2 position(pose.Position.x, pose.Position.y);
		double w = static_cast<double>(pose.Rotation.w);
		double z = static_cast<double>(pose.Rotation.z);
		if (w == 0.0 && z == 0.0)
			return Physics2DPose{ .Position = position, .Angle = 0.0f };
		if (w < 0.0 || (w == 0.0 && z < 0.0))
		{
			w = -w;
			z = -z;
		}
		float angle = static_cast<float>(2.0 * DetMath::ATan2(z, w));
		if (angle <= -std::numbers::pi_v<float>)
			angle = std::numbers::pi_v<float>;
		return Physics2DPose{ .Position = position, .Angle = angle };
	}

	float GetPhysics2DHalfDepth(const Shape2DGeometry& geometry, const glm::vec2& scale)
	{
		ENGINE_CORE_ASSERT(Utils::CheckGeometry2D(geometry, scale, 0).has_value(),
			"GetPhysics2DHalfDepth: the geometry and scale must be ones MakeBody2DShape accepts");
		// In double precision: a product of two floats is exact there and the sum of squares cannot overflow, so only a
		// diagonal beyond the float range rounds to infinity (which MakeBody2DShape refuses).
		const double scaleX = std::abs(static_cast<double>(scale.x));
		const double scaleY = std::abs(static_cast<double>(scale.y));
		if (const BoxShape2D* box = std::get_if<BoxShape2D>(&geometry))
		{
			const double halfWidth = static_cast<double>(box->HalfExtents.x) * scaleX;
			const double halfHeight = static_cast<double>(box->HalfExtents.y) * scaleY;
			return static_cast<float>(2.0 * std::sqrt(halfWidth * halfWidth + halfHeight * halfHeight));
		}
		if (const CircleShape2D* circle = std::get_if<CircleShape2D>(&geometry))
			return circle->Radius * std::abs(scale.x);

		// The diagonal of the bounding rectangle of the scaled points.
		const std::vector<glm::vec2>& points = std::get<PolygonShape2D>(geometry).Points;
		if (points.empty())
			return 0.0f;
		double minimumX = static_cast<double>(points.front().x) * static_cast<double>(scale.x);
		double minimumY = static_cast<double>(points.front().y) * static_cast<double>(scale.y);
		double maximumX = minimumX;
		double maximumY = minimumY;
		for (const glm::vec2& point : points)
		{
			const double x = static_cast<double>(point.x) * static_cast<double>(scale.x);
			const double y = static_cast<double>(point.y) * static_cast<double>(scale.y);
			minimumX = std::min(minimumX, x);
			minimumY = std::min(minimumY, y);
			maximumX = std::max(maximumX, x);
			maximumY = std::max(maximumY, y);
		}
		const double width = maximumX - minimumX;
		const double height = maximumY - minimumY;
		return static_cast<float>(std::sqrt(width * width + height * height));
	}

	Result<BodyShapeDescription> MakeBody2DShape(const Body2DShapeDescription& description)
	{
		if (description.Colliders.empty())
			return MakeError(ErrorCode::Validation, "{}: a body shape needs at least one collider", PhysicsInvalidShapeCode);

		BodyShapeDescription laid;
		laid.Colliders.reserve(description.Colliders.size());
		for (size_t index = 0; index < description.Colliders.size(); ++index)
		{
			const Collider2DShapeDescription& collider = description.Colliders[index];
			if (!Utils::IsFinite(collider.Position))
				return Utils::MakeInvalidCollider2D(index, "the position ({}, {}) is not finite", collider.Position.x, collider.Position.y);
			if (!std::isfinite(collider.Angle))
				return Utils::MakeInvalidCollider2D(index, "the angle {} is not finite", collider.Angle);
			ENGINE_TRY(Utils::CheckGeometry2D(collider.Geometry, collider.Scale, index));
			const float halfDepth = GetPhysics2DHalfDepth(collider.Geometry, collider.Scale);
			if (!std::isfinite(halfDepth) || !(halfDepth > 0.0f))
			{
				return Utils::MakeInvalidCollider2D(index, "its depth along Z, twice {} m, is not finite and positive (the shape is too large or has no extent)",
					halfDepth);
			}

			ColliderShapeDescription& laidCollider = laid.Colliders.emplace_back();
			laidCollider.Position = Utils::ToPlane(collider.Position);
			laidCollider.Rotation = Utils::MakeRotationAboutZ(collider.Angle);
			laidCollider.UserData = collider.UserData;
			// The scale acts in the plane; along Z it is 1, so the half depth, computed after the scale, is not scaled again.
			// A circle's sphere takes a uniform scale.
			laidCollider.Scale = glm::vec3(collider.Scale.x, collider.Scale.y, 1.0f);
			if (const BoxShape2D* box = std::get_if<BoxShape2D>(&collider.Geometry))
			{
				laidCollider.Geometry = BoxShapeGeometry{ .HalfExtents = glm::vec3(box->HalfExtents.x, box->HalfExtents.y, halfDepth) };
			}
			else if (const CircleShape2D* circle = std::get_if<CircleShape2D>(&collider.Geometry))
			{
				laidCollider.Geometry = SphereShapeGeometry{ .Radius = circle->Radius };
				laidCollider.Scale.z = std::abs(collider.Scale.x);
			}
			else
			{
				const std::vector<glm::vec2>& points = std::get<PolygonShape2D>(collider.Geometry).Points;
				ConvexHullShapeGeometry hull;
				hull.Points.reserve(points.size() * 2);
				for (const glm::vec2& point : points)
				{
					hull.Points.emplace_back(point.x, point.y, -halfDepth);
					hull.Points.emplace_back(point.x, point.y, halfDepth);
				}
				laidCollider.Geometry = std::move(hull);
			}
		}
		return laid;
	}

	Physics2DWorld::Physics2DWorld(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	Physics2DWorld::~Physics2DWorld() = default;

	Result<Scope<Physics2DWorld>> Physics2DWorld::Create(const Physics2DWorldSpecification& specification)
	{
		PhysicsWorldSpecification world3D;
		world3D.Gravity = Utils::ToPlane(specification.Gravity);
		world3D.Layers = specification.Layers;
		world3D.Limits = specification.Limits;
		ENGINE_TRY_ASSIGN(Scope<PhysicsWorld> world, PhysicsWorld::Create(world3D));
		Scope<Physics2DWorld> created = CreateScope<Physics2DWorld>(ConstructionKey{});
		created->m_State->Layers = specification.Layers;
		created->m_State->World = std::move(world);
		return created;
	}

	Result<BodyHandle> Physics2DWorld::CreateBody(const Body2DDescription& description)
	{
		if (!Utils::IsFinite(description.Pose.Position) || !std::isfinite(description.Pose.Angle))
		{
			return MakeError(ErrorCode::InvalidArgument, "the pose ({}, {}) at {} rad is not finite", description.Pose.Position.x, description.Pose.Position.y,
				description.Pose.Angle);
		}

		BodyDescription body;
		body.Shape = description.Shape;
		body.MotionType = description.MotionType;
		body.Pose = ToPhysicsPose(description.Pose);
		body.Layer = description.Layer;
		body.IsSensor = description.IsSensor;
		body.CollideKinematicVsNonDynamic = description.CollideKinematicVsNonDynamic;
		body.MotionQuality = description.MotionQuality;
		// The plane locks act on Dynamic bodies; the world gives every other body all its degrees of freedom, and moves them
		// only to the planar poses and velocities this class hands it.
		body.AllowedDofs = description.FixedRotation ? Physics2DTranslationDofs : Physics2DDofs;
		body.Mass = description.Mass;
		body.Friction = description.Friction;
		body.Restitution = description.Restitution;
		body.LinearDamping = description.LinearDamping;
		body.AngularDamping = description.AngularDamping;
		body.GravityFactor = description.GravityFactor;
		body.MaxLinearVelocity = description.MaxLinearVelocity;
		body.MaxAngularVelocity = description.MaxAngularVelocity;
		body.AllowSleeping = description.AllowSleeping;
		body.EnhancedInternalEdgeRemoval = description.EnhancedInternalEdgeRemoval;
		// A circle is a sphere laid in the plane, which turns about Z as a disc (M21's milestone review).
		body.SpheresAsDiscs = true;
		body.LinearVelocity = Utils::ToPlane(description.LinearVelocity);
		body.AngularVelocity = glm::vec3(0.0f, 0.0f, description.AngularVelocity);
		body.UserData = description.UserData;
		body.CollisionGroup = description.CollisionGroup;
		return m_State->World->CreateBody(body);
	}

	void Physics2DWorld::DestroyBody(BodyHandle body)
	{
		m_State->World->DestroyBody(body);
	}

	bool Physics2DWorld::IsBodyValid(BodyHandle body) const
	{
		return m_State->World->IsBodyValid(body);
	}

	uint64_t Physics2DWorld::GetUserData(BodyHandle body) const
	{
		return m_State->World->GetUserData(body);
	}

	PhysicsMotionType Physics2DWorld::GetMotionType(BodyHandle body) const
	{
		return m_State->World->GetMotionType(body);
	}

	bool Physics2DWorld::IsSensor(BodyHandle body) const
	{
		return m_State->World->IsSensor(body);
	}

	uint32_t Physics2DWorld::GetLayer(BodyHandle body) const
	{
		return m_State->World->GetLayer(body);
	}

	Ref<const PhysicsShape> Physics2DWorld::GetShape(BodyHandle body) const
	{
		return m_State->World->GetShape(body);
	}

	Status Physics2DWorld::SetShape(BodyHandle body, const Ref<const PhysicsShape>& shape)
	{
		return m_State->World->SetShape(body, shape);
	}

	uint32_t Physics2DWorld::GetSubShapeCollider(BodyHandle body, uint32_t subShape) const
	{
		return m_State->World->GetSubShapeCollider(body, subShape);
	}

	Physics2DPose Physics2DWorld::GetPose(BodyHandle body) const
	{
		return ToPhysics2DPose(m_State->World->GetPose(body));
	}

	void Physics2DWorld::SetPose(BodyHandle body, const Physics2DPose& pose, bool activate)
	{
		const bool isPlaceable = Utils::IsPlaceablePhysics2DPose(pose);
		ENGINE_CORE_ASSERT(isPlaceable, "Physics2DWorld::SetPose: the pose must be finite and within MaxPhysicsCoordinate");
		if (!isPlaceable)
			return;
		m_State->World->SetPose(body, ToPhysicsPose(pose), activate);
	}

	void Physics2DWorld::MoveKinematic(BodyHandle body, const Physics2DPose& target, float deltaTime)
	{
		const bool isPlaceable = Utils::IsPlaceablePhysics2DPose(target);
		ENGINE_CORE_ASSERT(isPlaceable, "Physics2DWorld::MoveKinematic: the target must be finite and within MaxPhysicsCoordinate");
		if (!isPlaceable)
			return;
		m_State->World->MoveKinematic(body, ToPhysicsPose(target), deltaTime);
	}

	void Physics2DWorld::AddForce(BodyHandle body, const glm::vec2& force)
	{
		m_State->World->AddForce(body, Utils::ToPlane(force));
	}

	void Physics2DWorld::AddForceAtPosition(BodyHandle body, const glm::vec2& force, const glm::vec2& position)
	{
		m_State->World->AddForceAtPosition(body, Utils::ToPlane(force), Utils::ToPlane(position));
	}

	void Physics2DWorld::AddTorque(BodyHandle body, float torque)
	{
		m_State->World->AddTorque(body, glm::vec3(0.0f, 0.0f, torque));
	}

	void Physics2DWorld::AddImpulse(BodyHandle body, const glm::vec2& impulse)
	{
		m_State->World->AddImpulse(body, Utils::ToPlane(impulse));
	}

	void Physics2DWorld::AddAngularImpulse(BodyHandle body, float angularImpulse)
	{
		m_State->World->AddAngularImpulse(body, glm::vec3(0.0f, 0.0f, angularImpulse));
	}

	glm::vec2 Physics2DWorld::GetLinearVelocity(BodyHandle body) const
	{
		const glm::vec3 velocity = m_State->World->GetLinearVelocity(body);
		return glm::vec2(velocity.x, velocity.y);
	}

	void Physics2DWorld::SetLinearVelocity(BodyHandle body, const glm::vec2& velocity)
	{
		m_State->World->SetLinearVelocity(body, Utils::ToPlane(velocity));
	}

	float Physics2DWorld::GetAngularVelocity(BodyHandle body) const
	{
		return m_State->World->GetAngularVelocity(body).z;
	}

	void Physics2DWorld::SetAngularVelocity(BodyHandle body, float velocity)
	{
		m_State->World->SetAngularVelocity(body, glm::vec3(0.0f, 0.0f, velocity));
	}

	bool Physics2DWorld::IsSleeping(BodyHandle body) const
	{
		return m_State->World->IsSleeping(body);
	}

	void Physics2DWorld::WakeUp(BodyHandle body)
	{
		m_State->World->WakeUp(body);
	}

	std::optional<PhysicsQueryHit> Physics2DWorld::Raycast(const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
		const PhysicsQueryFilter& filter) const
	{
		std::optional<PhysicsQueryHit> hit =
			m_State->World->Raycast(PhysicsRay{ .Origin = Utils::ToPlane(origin), .Direction = Utils::ToPlane(direction), .MaxDistance = maxDistance }, filter);
		if (hit.has_value())
			Utils::LayHitInPlane(*hit, direction);
		return hit;
	}

	std::vector<PhysicsQueryHit> Physics2DWorld::RaycastAll(const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
		const PhysicsQueryFilter& filter) const
	{
		std::vector<PhysicsQueryHit> hits =
			m_State->World->RaycastAll(PhysicsRay{ .Origin = Utils::ToPlane(origin), .Direction = Utils::ToPlane(direction), .MaxDistance = maxDistance }, filter);
		for (PhysicsQueryHit& hit : hits)
			Utils::LayHitInPlane(hit, direction);
		return hits;
	}

	std::vector<PhysicsOverlap> Physics2DWorld::Overlap(const Overlap2DShape& shape, const Physics2DPose& pose, const PhysicsQueryFilter& filter) const
	{
		const std::optional<PhysicsShapeGeometry> geometry = Utils::MakeOverlapGeometry(shape);
		const bool isValid = geometry.has_value() && Utils::IsFinite(pose.Position) && std::isfinite(pose.Angle);
		ENGINE_CORE_ASSERT(isValid, "Physics2DWorld::Overlap: needs a box or a circle with finite positive dimensions (a box's diagonal finite) and a finite pose");
		if (!isValid)
			return {};
		return m_State->World->Overlap(*geometry, ToPhysicsPose(pose), filter);
	}

	std::optional<Aabb> Physics2DWorld::GetBodyBounds(BodyHandle body) const
	{
		return m_State->World->GetBodyBounds(body);
	}

	std::optional<Aabb> Physics2DWorld::GetSubShapeBounds(BodyHandle body, uint32_t collider) const
	{
		return m_State->World->GetSubShapeBounds(body, collider);
	}

	Status Physics2DWorld::Step(float deltaTime, uint32_t collisionSteps)
	{
		return m_State->World->Step(deltaTime, collisionSteps);
	}

	glm::vec2 Physics2DWorld::GetGravity() const
	{
		const glm::vec3 gravity = m_State->World->GetGravity();
		return glm::vec2(gravity.x, gravity.y);
	}

	Status Physics2DWorld::SetGravity(const glm::vec2& gravity)
	{
		return m_State->World->SetGravity(Utils::ToPlane(gravity));
	}

	std::vector<ContactEvent> Physics2DWorld::DrainContactEvents()
	{
		// A manifold's points lie on the contact face, spread over the shapes' common depth along Z; each projects onto the
		// plane to the 2D contact point. Removed records carry no point or normal.
		std::vector<ContactEvent> events = m_State->World->DrainContactEvents();
		for (ContactEvent& event : events)
		{
			if (event.Kind == ContactEventKind::Removed)
				continue;
			event.Point.z = 0.0f;
			event.Normal = Utils::ToPlanarNormal(event.Normal, ContactEvent{}.Normal);
		}
		return events;
	}

	const PhysicsLayerTable& Physics2DWorld::GetLayers() const
	{
		return m_State->Layers;
	}

	PhysicsWorldStats Physics2DWorld::GetStats() const
	{
		return m_State->World->GetStats();
	}

	const PhysicsWorld* Physics2DWorld::GetWorld() const
	{
		return m_State->World.get();
	}

}
