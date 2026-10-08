#include "EnginePCH.h"
#include "Engine/Physics/PhysicsWorld.h"

#include "Engine/Core/Assert.h"
#include "Engine/Physics/Private/PhysicsConversions.h"
#include "Engine/Physics/Private/PhysicsWorldState.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/TransformedShape.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>
#include <vector>

// The world's queries (Architecture §9.5): rays, shape casts and overlaps through the NarrowPhaseQuery of the world's
// JPH::PhysicsSystem on the main thread, between steps, filtered by a PhysicsQueryFilter; and the world bounds of bodies
// and of their colliders' sub-shapes. Hits name the collider through the compound's sub-shape user data (§9.2,
// ResolveColliderUserData).
//
// Determinism: Jolt's broad and narrow phases visit bodies in an order that depends only on the world's history, so the
// same calls give the same results in every configuration. The closest-hit queries keep every hit at the smallest
// fraction and choose among them by body handle, then sub-shape ID, so the choice never depends on the order in which
// equally close hits arrived; the multi-hit queries return Jolt's order, which callers sort (Scene/PhysicsSystem sorts by
// distance, then UUID).

namespace Engine {

	namespace Utils {

		// The Jolt ID of a handle; the invalid ID for the invalid handle and for a value Jolt never hands out (the broad
		// phase's bit set), so a handle made from any value can be locked safely (Jolt asserts on such a value otherwise).
		[[nodiscard]] static JPH::BodyID ToBodyId(BodyHandle body)
		{
			return Detail::ToJoltBodyID(body).value_or(JPH::BodyID());
		}

		// The rigid transform of a pose (its rotation normalized: Jolt asserts on a quaternion further from unit length
		// than the tolerance the engine accepts).
		[[nodiscard]] static JPH::RMat44 ToTransform(const PhysicsPose& pose)
		{
			return JPH::RMat44::sRotationTranslation(Detail::ToJoltRotation(glm::normalize(pose.Rotation)), Detail::ToJolt(pose.Position));
		}

		[[nodiscard]] static bool IsValidPose(const PhysicsPose& pose)
		{
			return Detail::IsFinite(pose.Position) && IsPhysicsUnitRotation(pose.Rotation);
		}

		// The Jolt shape of a query primitive (box, sphere or capsule), built through its ShapeSettings::Create (§9.1); null
		// for any other geometry or for a dimension that is not finite and positive.
		[[nodiscard]] static JPH::RefConst<JPH::Shape> CreateQueryShape(const PhysicsShapeGeometry& geometry)
		{
			JPH::ShapeSettings::ShapeResult result;
			if (const auto* box = std::get_if<BoxShapeGeometry>(&geometry))
			{
				const glm::vec3& half = box->HalfExtents;
				if (!Detail::IsFinite(half) || !(half.x > 0.0f && half.y > 0.0f && half.z > 0.0f))
					return nullptr;
				// No convex radius, as the bodies' boxes (PhysicsShape.cpp): a query box has the sharp corners of a BoxCollider
				// of the same size.
				JPH::BoxShapeSettings settings(Detail::ToJolt(half), 0.0f);
				settings.SetEmbedded();
				result = settings.Create();
			}
			else if (const auto* sphere = std::get_if<SphereShapeGeometry>(&geometry))
			{
				if (!std::isfinite(sphere->Radius) || !(sphere->Radius > 0.0f))
					return nullptr;
				JPH::SphereShapeSettings settings(sphere->Radius);
				settings.SetEmbedded();
				result = settings.Create();
			}
			else if (const auto* capsule = std::get_if<CapsuleShapeGeometry>(&geometry))
			{
				if (!std::isfinite(capsule->HalfHeight) || !std::isfinite(capsule->Radius) || !(capsule->HalfHeight > 0.0f) || !(capsule->Radius > 0.0f))
					return nullptr;
				JPH::CapsuleShapeSettings settings(capsule->HalfHeight, capsule->Radius);
				settings.SetEmbedded();
				result = settings.Create();
			}
			else
			{
				return nullptr;
			}
			if (result.HasError())
				return nullptr;
			return result.Get();
		}

		// The broad-phase side of a PhysicsQueryFilter: every broad-phase layer, Sensor only when sensors are included
		// (PhysicsLayers.h: sensors are alone in their broad-phase layer).
		class QueryBroadPhaseLayerFilter final : public JPH::BroadPhaseLayerFilter
		{
		public:
			explicit QueryBroadPhaseLayerFilter(bool includeSensors)
				: m_IncludeSensors(includeSensors)
			{
			}

			bool ShouldCollide(JPH::BroadPhaseLayer layer) const override
			{
				constexpr JPH::BroadPhaseLayer SensorLayer(static_cast<JPH::BroadPhaseLayer::Type>(PhysicsBroadPhaseLayer::Sensor));
				return m_IncludeSensors || layer != SensorLayer;
			}
		private:
			bool m_IncludeSensors = true;
		};

		// The object-layer side: the project layer (object layer >> 2) must be one the table declares and the mask selects,
		// and the kind may be Sensor only when sensors are included.
		class QueryObjectLayerFilter final : public JPH::ObjectLayerFilter
		{
		public:
			QueryObjectLayerFilter(const PhysicsQueryFilter& filter, uint32_t layerCount)
				: m_Layers(filter.Layers), m_LayerCount(layerCount), m_IncludeSensors(filter.IncludeSensors)
			{
			}

			bool ShouldCollide(JPH::ObjectLayer layer) const override
			{
				const auto objectLayer = static_cast<PhysicsObjectLayer>(layer);
				const uint32_t index = GetPhysicsLayerIndex(objectLayer);
				if (index >= m_LayerCount || !PhysicsLayerTable::MaskContains(m_Layers, index))
					return false;
				return m_IncludeSensors || GetPhysicsObjectKind(objectLayer) != PhysicsObjectKind::Sensor;
			}
		private:
			PhysicsLayerMask m_Layers = AllPhysicsLayers;
			uint32_t m_LayerCount = 0;
			bool m_IncludeSensors = true;
		};

		// The body side: the ignored body, and sensors (by the body's own flag, whatever its layer) unless included.
		class QueryBodyFilter final : public JPH::BodyFilter
		{
		public:
			explicit QueryBodyFilter(const PhysicsQueryFilter& filter)
				: m_Ignored(ToBodyId(filter.IgnoreBody)), m_IncludeSensors(filter.IncludeSensors)
			{
			}

			bool ShouldCollide(const JPH::BodyID& body) const override
			{
				return body != m_Ignored;
			}

			bool ShouldCollideLocked(const JPH::Body& body) const override
			{
				return m_IncludeSensors || !body.IsSensor();
			}
		private:
			JPH::BodyID m_Ignored{};
			bool m_IncludeSensors = true;
		};

		// The three filters of one query.
		struct QueryFilters
		{
			QueryFilters(const PhysicsQueryFilter& filter, uint32_t layerCount)
				: BroadPhaseLayers(filter.IncludeSensors), ObjectLayers(filter, layerCount), Bodies(filter)
			{
			}

			QueryBroadPhaseLayerFilter BroadPhaseLayers;
			QueryObjectLayerFilter ObjectLayers;
			QueryBodyFilter Bodies;
		};

		// One ray hit with the surface normal, read while Jolt held the body (the collector's context).
		struct RayHit
		{
			JPH::RayCastResult Hit{};
			JPH::Vec3 Normal = JPH::Vec3::sAxisY();
		};

		// The world-space surface normal of a ray hit, pointing against the ray: the shape's normal at the hit, except for a
		// ray that starts inside a convex shape (fraction 0, no surface crossed) or a degenerate normal, which give the
		// reversed direction.
		[[nodiscard]] static JPH::Vec3 GetRayHitNormal(const JPH::TransformedShape& shape, const JPH::RRayCast& ray, const JPH::RayCastResult& result)
		{
			const JPH::Vec3 reversed = -ray.mDirection.NormalizedOr(JPH::Vec3::sAxisY());
			if (result.mFraction <= 0.0f)
				return reversed;
			const JPH::Vec3 normal = shape.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, ray.GetPointOnRay(result.mFraction));
			if (!Detail::IsFinite(Detail::ToGlm(normal)) || normal.LengthSq() < 0.25f)
				return reversed;
			return normal.Normalized();
		}

		// Collects ray hits with their normals: every hit, or (Closest) only those at the smallest fraction so far. Jolt only
		// reports hits below the early-out fraction, so the closest mode keeps it one float above the best fraction, which
		// lets equally close hits through.
		class RayHitCollector final : public JPH::CastRayCollector
		{
		public:
			RayHitCollector(const JPH::RRayCast& ray, bool closest)
				: m_Ray(ray), m_Closest(closest)
			{
			}

			void AddHit(const JPH::RayCastResult& result) override
			{
				if (m_Closest)
				{
					if (!m_Hits.empty() && result.mFraction > m_Best)
						return;
					if (m_Hits.empty() || result.mFraction < m_Best)
					{
						m_Hits.clear();
						m_Best = result.mFraction;
						const float earlyOut = std::nextafter(result.mFraction, std::numeric_limits<float>::infinity());
						UpdateEarlyOutFraction(std::min(earlyOut, GetEarlyOutFraction()));
					}
				}
				const JPH::TransformedShape* shape = GetContext();
				const JPH::Vec3 normal = shape != nullptr ? GetRayHitNormal(*shape, m_Ray, result) : -m_Ray.mDirection.NormalizedOr(JPH::Vec3::sAxisY());
				m_Hits.push_back(RayHit{ .Hit = result, .Normal = normal });
			}

			[[nodiscard]] const std::vector<RayHit>& GetHits() const { return m_Hits; }
		private:
			JPH::RRayCast m_Ray;
			bool m_Closest = false;
			float m_Best = 0.0f;
			std::vector<RayHit> m_Hits;
		};

		// Collects the shape-cast hits at the smallest early-out value (the fraction, or minus the penetration depth for
		// hits at fraction 0, so the deepest initial overlap wins), ties included as for rays.
		class ClosestShapeCastCollector final : public JPH::CastShapeCollector
		{
		public:
			void AddHit(const JPH::ShapeCastResult& result) override
			{
				const float value = result.GetEarlyOutFraction();
				if (!m_Hits.empty() && value > m_Best)
					return;
				if (m_Hits.empty() || value < m_Best)
				{
					m_Hits.clear();
					m_Best = value;
					const float earlyOut = std::nextafter(value, std::numeric_limits<float>::infinity());
					UpdateEarlyOutFraction(std::min(earlyOut, GetEarlyOutFraction()));
				}
				m_Hits.push_back(result);
			}

			[[nodiscard]] const std::vector<JPH::ShapeCastResult>& GetHits() const { return m_Hits; }
		private:
			float m_Best = 0.0f;
			std::vector<JPH::ShapeCastResult> m_Hits;
		};

		// Collects each overlapping body sub-shape once (the collide-shape query may report one sub-shape several times), in
		// the order Jolt first reported them.
		class OverlapCollector final : public JPH::CollideShapeCollector
		{
		public:
			// (body ID value, sub-shape ID value).
			using Key = std::pair<uint32_t, uint32_t>;

			void AddHit(const JPH::CollideShapeResult& result) override
			{
				const Key key{ result.mBodyID2.GetIndexAndSequenceNumber(), result.mSubShapeID2.GetValue() };
				if (m_Seen.insert(key).second)
					m_Hits.push_back(key);
			}

			[[nodiscard]] const std::vector<Key>& GetHits() const { return m_Hits; }
		private:
			std::set<Key> m_Seen;
			std::vector<Key> m_Hits;
		};

		// The total order among equally close hits: body handle, then sub-shape ID.
		[[nodiscard]] static bool IsBefore(const JPH::BodyID& bodyA, const JPH::SubShapeID& subShapeA, const JPH::BodyID& bodyB, const JPH::SubShapeID& subShapeB)
		{
			if (bodyA != bodyB)
				return bodyA.GetIndexAndSequenceNumber() < bodyB.GetIndexAndSequenceNumber();
			return subShapeA.GetValue() < subShapeB.GetValue();
		}

	}

	uint32_t PhysicsWorld::State::ResolveColliderUserData(const PhysicsShape& shape, uint32_t subShape)
	{
		const PhysicsShape::State& state = *shape.m_State;
		const uint32_t singleCollider = state.ColliderUserData.empty() ? 0 : state.ColliderUserData.front();
		if (state.Shape == nullptr)
			return singleCollider;
		JPH::SubShapeID id;
		id.SetValue(subShape);
		return Detail::ResolveColliderIndex(*state.Shape, id, singleCollider);
	}

	std::optional<PhysicsQueryHit> PhysicsWorld::Raycast(const PhysicsRay& ray, const PhysicsQueryFilter& filter) const
	{
		const bool valid = Detail::IsFinite(ray.Origin) && Detail::IsUnitVector(ray.Direction) && std::isfinite(ray.MaxDistance) && ray.MaxDistance > 0.0f
			&& Detail::IsFinite(ray.Origin + ray.Direction * ray.MaxDistance);
		ENGINE_CORE_ASSERT(valid, "PhysicsWorld::Raycast: the ray needs a finite origin, a unit direction and a finite distance > 0");
		if (!valid)
			return std::nullopt;

		const JPH::RRayCast joltRay(Detail::ToJolt(ray.Origin), Detail::ToJolt(glm::normalize(ray.Direction) * ray.MaxDistance));
		const Utils::QueryFilters filters(filter, m_State->Specification.Layers.GetLayerCount());
		Utils::RayHitCollector collector(joltRay, true);
		m_State->System->GetNarrowPhaseQuery().CastRay(joltRay, JPH::RayCastSettings(), collector, filters.BroadPhaseLayers, filters.ObjectLayers, filters.Bodies);

		const std::vector<Utils::RayHit>& hits = collector.GetHits();
		if (hits.empty())
			return std::nullopt;
		const Utils::RayHit* closest = &hits.front();
		for (const Utils::RayHit& hit : hits)
		{
			if (Utils::IsBefore(hit.Hit.mBodyID, hit.Hit.mSubShapeID2, closest->Hit.mBodyID, closest->Hit.mSubShapeID2))
				closest = &hit;
		}

		const BodyHandle body(closest->Hit.mBodyID.GetIndexAndSequenceNumber());
		const Ref<const PhysicsShape> shape = GetShape(body);
		return PhysicsQueryHit{ .Body = body,
			.Collider = shape != nullptr ? State::ResolveColliderUserData(*shape, closest->Hit.mSubShapeID2.GetValue()) : 0,
			.Point = Detail::ToGlm(joltRay.GetPointOnRay(closest->Hit.mFraction)),
			.Normal = Detail::ToGlm(closest->Normal),
			.Distance = std::clamp(closest->Hit.mFraction * ray.MaxDistance, 0.0f, ray.MaxDistance) };
	}

	std::vector<PhysicsQueryHit> PhysicsWorld::RaycastAll(const PhysicsRay& ray, const PhysicsQueryFilter& filter) const
	{
		const bool valid = Detail::IsFinite(ray.Origin) && Detail::IsUnitVector(ray.Direction) && std::isfinite(ray.MaxDistance) && ray.MaxDistance > 0.0f
			&& Detail::IsFinite(ray.Origin + ray.Direction * ray.MaxDistance);
		ENGINE_CORE_ASSERT(valid, "PhysicsWorld::RaycastAll: the ray needs a finite origin, a unit direction and a finite distance > 0");
		if (!valid)
			return {};

		const JPH::RRayCast joltRay(Detail::ToJolt(ray.Origin), Detail::ToJolt(glm::normalize(ray.Direction) * ray.MaxDistance));
		const Utils::QueryFilters filters(filter, m_State->Specification.Layers.GetLayerCount());
		Utils::RayHitCollector collector(joltRay, false);
		m_State->System->GetNarrowPhaseQuery().CastRay(joltRay, JPH::RayCastSettings(), collector, filters.BroadPhaseLayers, filters.ObjectLayers, filters.Bodies);

		std::vector<PhysicsQueryHit> results;
		results.reserve(collector.GetHits().size());
		for (const Utils::RayHit& hit : collector.GetHits())
		{
			const BodyHandle body(hit.Hit.mBodyID.GetIndexAndSequenceNumber());
			const Ref<const PhysicsShape> shape = GetShape(body);
			results.push_back(PhysicsQueryHit{ .Body = body,
				.Collider = shape != nullptr ? State::ResolveColliderUserData(*shape, hit.Hit.mSubShapeID2.GetValue()) : 0,
				.Point = Detail::ToGlm(joltRay.GetPointOnRay(hit.Hit.mFraction)),
				.Normal = Detail::ToGlm(hit.Normal),
				.Distance = std::clamp(hit.Hit.mFraction * ray.MaxDistance, 0.0f, ray.MaxDistance) });
		}
		return results;
	}

	std::optional<PhysicsQueryHit> PhysicsWorld::ShapeCast(const PhysicsShapeGeometry& shape, const PhysicsPose& start, const glm::vec3& direction,
		float maxDistance, const PhysicsQueryFilter& filter) const
	{
		const JPH::RefConst<JPH::Shape> queryShape = Utils::CreateQueryShape(shape);
		const bool valid = queryShape != nullptr && Utils::IsValidPose(start) && Detail::IsUnitVector(direction) && std::isfinite(maxDistance) && maxDistance > 0.0f
			&& Detail::IsFinite(start.Position + direction * maxDistance);
		ENGINE_CORE_ASSERT(valid,
			"PhysicsWorld::ShapeCast: needs a box, sphere or capsule with finite positive dimensions, a finite pose with a unit rotation, a unit direction "
			"and a finite distance > 0");
		if (!valid)
			return std::nullopt;

		const glm::vec3 displacement = glm::normalize(direction) * maxDistance;
		const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(queryShape, JPH::Vec3::sOne(), Utils::ToTransform(start), Detail::ToJolt(displacement));
		const Utils::QueryFilters filters(filter, m_State->Specification.Layers.GetLayerCount());
		Utils::ClosestShapeCastCollector collector;
		m_State->System->GetNarrowPhaseQuery().CastShape(cast, JPH::ShapeCastSettings(), JPH::RVec3::sZero(), collector, filters.BroadPhaseLayers, filters.ObjectLayers,
			filters.Bodies);

		const std::vector<JPH::ShapeCastResult>& hits = collector.GetHits();
		if (hits.empty())
			return std::nullopt;
		const JPH::ShapeCastResult* closest = &hits.front();
		for (const JPH::ShapeCastResult& hit : hits)
		{
			if (Utils::IsBefore(hit.mBodyID2, hit.mSubShapeID2, closest->mBodyID2, closest->mSubShapeID2))
				closest = &hit;
		}

		// The penetration axis points from the cast shape into the one it hit; the surface normal points back.
		const JPH::Vec3 reversed = -Detail::ToJolt(glm::normalize(direction));
		JPH::Vec3 normal = -closest->mPenetrationAxis.NormalizedOr(-reversed);
		if (!Detail::IsFinite(Detail::ToGlm(normal)))
			normal = reversed;
		const BodyHandle body(closest->mBodyID2.GetIndexAndSequenceNumber());
		const Ref<const PhysicsShape> bodyShape = GetShape(body);
		const glm::vec3 point = Detail::ToGlm(closest->mContactPointOn2);
		return PhysicsQueryHit{ .Body = body,
			.Collider = bodyShape != nullptr ? State::ResolveColliderUserData(*bodyShape, closest->mSubShapeID2.GetValue()) : 0,
			.Point = Detail::IsFinite(point) ? point : start.Position,
			.Normal = Detail::ToGlm(normal),
			.Distance = std::clamp(closest->mFraction * maxDistance, 0.0f, maxDistance) };
	}

	std::vector<PhysicsOverlap> PhysicsWorld::Overlap(const PhysicsShapeGeometry& shape, const PhysicsPose& pose, const PhysicsQueryFilter& filter) const
	{
		const JPH::RefConst<JPH::Shape> queryShape = Utils::CreateQueryShape(shape);
		const bool valid = queryShape != nullptr && Utils::IsValidPose(pose);
		ENGINE_CORE_ASSERT(valid, "PhysicsWorld::Overlap: needs a box, sphere or capsule with finite positive dimensions and a finite pose with a unit rotation");
		if (!valid)
			return {};

		const JPH::RMat44 centerOfMass = Utils::ToTransform(pose) * JPH::Mat44::sTranslation(queryShape->GetCenterOfMass());
		const Utils::QueryFilters filters(filter, m_State->Specification.Layers.GetLayerCount());
		Utils::OverlapCollector collector;
		m_State->System->GetNarrowPhaseQuery().CollideShape(queryShape, JPH::Vec3::sOne(), centerOfMass, JPH::CollideShapeSettings(), JPH::RVec3::sZero(),
			collector, filters.BroadPhaseLayers, filters.ObjectLayers, filters.Bodies);

		// One entry per collider of each body: a mesh collider's triangles are sub-shapes of one collider.
		std::vector<PhysicsOverlap> results;
		for (const auto& [bodyValue, subShape] : collector.GetHits())
		{
			const BodyHandle body(bodyValue);
			const Ref<const PhysicsShape> bodyShape = GetShape(body);
			const PhysicsOverlap overlap{ .Body = body, .Collider = bodyShape != nullptr ? State::ResolveColliderUserData(*bodyShape, subShape) : 0 };
			const bool seen = std::any_of(results.begin(), results.end(), [&overlap](const PhysicsOverlap& other)
			{
				return other.Body == overlap.Body && other.Collider == overlap.Collider;
			});
			if (!seen)
				results.push_back(overlap);
		}
		return results;
	}

	std::optional<Aabb> PhysicsWorld::GetBodyBounds(BodyHandle body) const
	{
		const JPH::BodyLockRead lock(m_State->System->GetBodyLockInterface(), Utils::ToBodyId(body));
		if (!lock.Succeeded())
			return std::nullopt;
		return Detail::ToAabb(lock.GetBody().GetWorldSpaceBounds());
	}

	std::optional<Aabb> PhysicsWorld::GetSubShapeBounds(BodyHandle body, uint32_t collider) const
	{
		const Ref<const PhysicsShape> shape = IsBodyValid(body) ? GetShape(body) : nullptr;
		if (shape == nullptr)
			return std::nullopt;

		const JPH::BodyLockRead lock(m_State->System->GetBodyLockInterface(), Utils::ToBodyId(body));
		if (!lock.Succeeded())
			return std::nullopt;
		const JPH::Body& joltBody = lock.GetBody();
		const JPH::Shape* joltShape = joltBody.GetShape();
		if (joltShape->GetType() != JPH::EShapeType::Compound)
		{
			// One collider: the whole body is it.
			const std::vector<uint32_t>& colliders = shape->m_State->ColliderUserData;
			if (colliders.empty() || colliders.front() != collider)
				return std::nullopt;
			return Detail::ToAabb(joltBody.GetWorldSpaceBounds());
		}

		// The compound's sub-shapes are placed relative to its centre of mass, which the body's transform carries.
		const auto& compound = static_cast<const JPH::CompoundShape&>(*joltShape);
		const JPH::RMat44 centerOfMass = joltBody.GetCenterOfMassTransform();
		Aabb bounds;
		for (const JPH::CompoundShape::SubShape& subShape : compound.GetSubShapes())
		{
			if (subShape.mUserData != collider)
				continue;
			const JPH::Vec3 scale = subShape.TransformScale(JPH::Vec3::sOne());
			const JPH::AABox box = subShape.mShape->GetWorldSpaceBounds(centerOfMass * subShape.GetLocalTransformNoScale(JPH::Vec3::sOne()), scale);
			bounds.Extend(Detail::ToAabb(box));
		}
		if (bounds.IsEmpty())
			return std::nullopt;
		return bounds;
	}

}
