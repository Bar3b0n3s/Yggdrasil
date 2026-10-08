#include "EnginePCH.h"
#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Scene/Private/PhysicsBodySettings.h"
#include "Engine/Scene/Private/PhysicsSystemState.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

// The scene-level queries and bounds (Architecture §9.5; §11.5 Physics.Raycast, RaycastAll, SphereCast, OverlapSphere,
// OverlapBox, GetBodyBounds, GetColliderBounds, LayerMask) over the session's PhysicsWorld. Hits are resolved to the
// collider entity (the body's collider table indexed by the sub-shape user data, §9.2) and the body owner; a character's
// inner body resolves to the character's entity for both (§9.6). Every query sees sensors (ADR 0014 decision 7). Hits and
// bounds of entities pending destruction or disabled are left out, as the body functions treat those entities as having
// no body (their bodies stay in the world until FlushDestroyed, §5.7 step 8).
//
// Input is bounded as bodies are (§9.1 "nothing invalid reaches Jolt"): a query shape's radius or half extent lies between
// half PhysicsShape::MinColliderSize and half PhysicsShape::MaxColliderSize, as a collider's would, and every point a query
// starts, ends or is centred at lies within MaxPhysicsCoordinate.
//
// Order (§9.5 "results sorted by distance then UUID"): ray and shape-cast hits by distance, then collider UUID, then body
// UUID, then the hit point and normal, a total order, so the result never depends on the order in which Jolt found the hits;
// Raycast is the first hit of that order (it casts as RaycastAll does, so equally close hits are told apart by UUID too).
// Overlaps have no distance: by collider UUID, then body UUID, each (collider, body) once. SphereCast returns the world's
// closest shape-cast hit (PhysicsWorld::ShapeCast chooses among equally close ones by body handle, which bodies get in
// the canonical creation order).

namespace Engine {

	namespace Utils {

		// The shortest direction a query normalizes (PhysicsSystem.h).
		constexpr float MinDirectionLength = 1.0e-6f;

		// The smallest and largest radius or half extent of a query shape: half the smallest and the largest collider
		// (PhysicsShape::MinColliderSize, MaxColliderSize), beyond which Jolt's collision tolerances break down and its
		// penetration depth overflows as they do for bodies.
		constexpr float MinQueryExtent = PhysicsShape::MinColliderSize / 2.0f;
		constexpr float MaxQueryExtent = PhysicsShape::MaxColliderSize / 2.0f;

		[[nodiscard]] static Status CheckFinite(const glm::vec3& value, std::string_view name)
		{
			if (!IsFinite(value))
				return MakeError(ErrorCode::InvalidArgument, "the query's {} ({}, {}, {}) is not finite", name, value.x, value.y, value.z);
			return {};
		}

		// InvalidArgument unless `value` is finite and within MaxPhysicsCoordinate (a ray's origin or end, a shape's centre).
		[[nodiscard]] static Status CheckPoint(const glm::vec3& value, std::string_view name)
		{
			ENGINE_TRY(CheckFinite(value, name));
			if (!IsWithinPhysicsRange(value))
			{
				return MakeError(ErrorCode::InvalidArgument, "the query's {} ({}, {}, {}) is out of range: each coordinate must be at most {} m", name, value.x,
					value.y, value.z, MaxPhysicsCoordinate);
			}
			return {};
		}

		[[nodiscard]] static Status CheckPositive(float value, std::string_view name)
		{
			if (!std::isfinite(value) || !(value > 0.0f))
				return MakeError(ErrorCode::InvalidArgument, "the query's {} {} is not a finite value above 0", name, value);
			return {};
		}

		// InvalidArgument unless `value` (a radius or half extent) is from MinQueryExtent to MaxQueryExtent.
		[[nodiscard]] static Status CheckExtent(float value, std::string_view name)
		{
			if (!std::isfinite(value) || !(value >= MinQueryExtent) || value > MaxQueryExtent)
				return MakeError(ErrorCode::InvalidArgument, "the query's {} {} is not from {} to {} m", name, value, MinQueryExtent, MaxQueryExtent);
			return {};
		}

		// `direction` scaled to unit length: by its largest component first, so a long but finite vector never overflows.
		[[nodiscard]] static Result<glm::vec3> NormalizeDirection(const glm::vec3& direction)
		{
			ENGINE_TRY(CheckFinite(direction, "direction"));
			const float largest = std::max(std::abs(direction.x), std::max(std::abs(direction.y), std::abs(direction.z)));
			const glm::vec3 scaled = largest > 0.0f ? direction / largest : glm::vec3(0.0f);
			const float length = glm::length(scaled);
			if (!(largest * length >= MinDirectionLength))
			{
				return MakeError(ErrorCode::InvalidArgument, "the query's direction ({}, {}, {}) is shorter than {} and has no direction", direction.x,
					direction.y, direction.z, MinDirectionLength);
			}
			return scaled / length;
		}

		// A rotation normalized like a direction: a zero or non-finite quaternion is refused.
		[[nodiscard]] static Result<glm::quat> NormalizeRotation(const glm::quat& rotation)
		{
			if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w))
				return MakeError(ErrorCode::InvalidArgument, "the query's rotation is not finite");
			const float largest = std::max(std::max(std::abs(rotation.x), std::abs(rotation.y)), std::max(std::abs(rotation.z), std::abs(rotation.w)));
			if (!(largest > 0.0f))
				return MakeError(ErrorCode::InvalidArgument, "the query's rotation is the zero quaternion");
			const glm::quat scaled(rotation.w / largest, rotation.x / largest, rotation.y / largest, rotation.z / largest);
			const float length = glm::length(scaled);
			if (!(largest * length >= MinDirectionLength))
				return MakeError(ErrorCode::InvalidArgument, "the query's rotation is shorter than {}", MinDirectionLength);
			return glm::normalize(scaled);
		}

		// A ray of the world (direction normalized). Errors: non-finite values, a direction without length, a distance <= 0,
		// or an origin or end point beyond MaxPhysicsCoordinate.
		[[nodiscard]] static Result<PhysicsRay> MakeRay(const glm::vec3& origin, const glm::vec3& direction, float maxDistance)
		{
			ENGINE_TRY(CheckPoint(origin, "origin"));
			ENGINE_TRY_ASSIGN(const glm::vec3 unit, NormalizeDirection(direction));
			ENGINE_TRY(CheckPositive(maxDistance, "distance"));
			ENGINE_TRY(CheckPoint(origin + unit * maxDistance, "end point"));
			return PhysicsRay{ .Origin = origin, .Direction = unit, .MaxDistance = maxDistance };
		}

		[[nodiscard]] static bool IsBefore(const PhysicsRaycastHit& a, const PhysicsRaycastHit& b)
		{
			return std::tie(a.Distance, a.Entity, a.Body, a.Point.x, a.Point.y, a.Point.z, a.Normal.x, a.Normal.y, a.Normal.z)
				< std::tie(b.Distance, b.Entity, b.Body, b.Point.x, b.Point.y, b.Point.z, b.Normal.x, b.Normal.y, b.Normal.z);
		}

		[[nodiscard]] static bool IsBefore(const PhysicsOverlapHit& a, const PhysicsOverlapHit& b)
		{
			return std::tie(a.Entity, a.Body) < std::tie(b.Entity, b.Body);
		}

	}

	std::optional<PhysicsOverlapHit> PhysicsSystem::State::ResolveHit(BodyHandle body, uint32_t collider) const
	{
		if (const auto found = Bodies.find(body.GetValue()); found != Bodies.end())
		{
			const PhysicsBodyEntry& entry = found->second;
			const UUID entity = collider < entry.Colliders.size() ? entry.Colliders[collider] : entry.Owner;
			if (!IsPresent(entry.Owner) || !IsPresent(entity))
				return std::nullopt;
			return PhysicsOverlapHit{ .Entity = entity, .Body = entry.Owner };
		}
		if (const UUID character = FindCharacterOwner(body); character.IsValid() && IsPresent(character))
			return PhysicsOverlapHit{ .Entity = character, .Body = character };
		return std::nullopt;
	}

	BodyHandle PhysicsSystem::State::FindOwnedBodyHandle(UUID owner) const
	{
		if (const PhysicsBodyEntry* body = FindOwnedBody(owner, PhysicsBodyOrigin::RigidBody))
			return body->Handle;
		if (const auto character = Characters.find(owner); character != Characters.end())
			return character->second.Controller->GetInnerBody();
		if (const PhysicsBodyEntry* body = FindOwnedBody(owner, PhysicsBodyOrigin::ImplicitStatic))
			return body->Handle;
		if (const PhysicsBodyEntry* body = FindOwnedBody(owner, PhysicsBodyOrigin::ImplicitSensor))
			return body->Handle;
		return BodyHandle();
	}

	Result<std::optional<PhysicsRaycastHit>> PhysicsSystem::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
		PhysicsLayerMask layers) const
	{
		ENGINE_TRY_ASSIGN(std::vector<PhysicsRaycastHit> hits, RaycastAll(origin, direction, maxDistance, layers));
		if (hits.empty())
			return std::optional<PhysicsRaycastHit>();
		return std::optional<PhysicsRaycastHit>(hits.front());
	}

	Result<std::vector<PhysicsRaycastHit>> PhysicsSystem::RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
		PhysicsLayerMask layers) const
	{
		ENGINE_TRY_ASSIGN(const PhysicsRay ray, Utils::MakeRay(origin, direction, maxDistance));
		std::vector<PhysicsRaycastHit> hits;
		if (m_State->World == nullptr)
			return hits;
		for (const PhysicsQueryHit& hit : m_State->World->RaycastAll(ray, { .Layers = layers }))
		{
			if (const std::optional<PhysicsOverlapHit> owner = m_State->ResolveHit(hit.Body, hit.Collider))
				hits.push_back(PhysicsRaycastHit{ .Entity = owner->Entity, .Body = owner->Body, .Point = hit.Point, .Normal = hit.Normal, .Distance = hit.Distance });
		}
		std::sort(hits.begin(), hits.end(), [](const PhysicsRaycastHit& a, const PhysicsRaycastHit& b)
		{
			return Utils::IsBefore(a, b);
		});
		return hits;
	}

	Result<std::optional<PhysicsRaycastHit>> PhysicsSystem::SphereCast(const glm::vec3& origin, float radius, const glm::vec3& direction,
		float maxDistance, PhysicsLayerMask layers) const
	{
		ENGINE_TRY(Utils::CheckExtent(radius, "radius"));
		ENGINE_TRY_ASSIGN(const PhysicsRay ray, Utils::MakeRay(origin, direction, maxDistance));
		if (m_State->World == nullptr)
			return std::optional<PhysicsRaycastHit>();
		const std::optional<PhysicsQueryHit> hit =
			m_State->World->ShapeCast(SphereShapeGeometry{ .Radius = radius }, { .Position = ray.Origin }, ray.Direction, ray.MaxDistance, { .Layers = layers });
		if (!hit.has_value())
			return std::optional<PhysicsRaycastHit>();
		const std::optional<PhysicsOverlapHit> owner = m_State->ResolveHit(hit->Body, hit->Collider);
		if (!owner.has_value())
			return std::optional<PhysicsRaycastHit>();
		return std::optional<PhysicsRaycastHit>(
			PhysicsRaycastHit{ .Entity = owner->Entity, .Body = owner->Body, .Point = hit->Point, .Normal = hit->Normal, .Distance = hit->Distance });
	}

	Result<std::vector<PhysicsOverlapHit>> PhysicsSystem::OverlapSphere(const glm::vec3& center, float radius, PhysicsLayerMask layers) const
	{
		ENGINE_TRY(Utils::CheckPoint(center, "centre"));
		ENGINE_TRY(Utils::CheckExtent(radius, "radius"));
		std::vector<PhysicsOverlapHit> hits;
		if (m_State->World == nullptr)
			return hits;
		for (const PhysicsOverlap& overlap : m_State->World->Overlap(SphereShapeGeometry{ .Radius = radius }, { .Position = center }, { .Layers = layers }))
		{
			if (const std::optional<PhysicsOverlapHit> owner = m_State->ResolveHit(overlap.Body, overlap.Collider))
				hits.push_back(*owner);
		}
		std::sort(hits.begin(), hits.end(), [](const PhysicsOverlapHit& a, const PhysicsOverlapHit& b)
		{
			return Utils::IsBefore(a, b);
		});
		hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
		return hits;
	}

	Result<std::vector<PhysicsOverlapHit>> PhysicsSystem::OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
		PhysicsLayerMask layers) const
	{
		ENGINE_TRY(Utils::CheckPoint(center, "centre"));
		ENGINE_TRY(Utils::CheckExtent(halfExtents.x, "half extent x"));
		ENGINE_TRY(Utils::CheckExtent(halfExtents.y, "half extent y"));
		ENGINE_TRY(Utils::CheckExtent(halfExtents.z, "half extent z"));
		ENGINE_TRY_ASSIGN(const glm::quat unit, Utils::NormalizeRotation(rotation));
		std::vector<PhysicsOverlapHit> hits;
		if (m_State->World == nullptr)
			return hits;
		for (const PhysicsOverlap& overlap :
			m_State->World->Overlap(BoxShapeGeometry{ .HalfExtents = halfExtents }, { .Position = center, .Rotation = unit }, { .Layers = layers }))
		{
			if (const std::optional<PhysicsOverlapHit> owner = m_State->ResolveHit(overlap.Body, overlap.Collider))
				hits.push_back(*owner);
		}
		std::sort(hits.begin(), hits.end(), [](const PhysicsOverlapHit& a, const PhysicsOverlapHit& b)
		{
			return Utils::IsBefore(a, b);
		});
		hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
		return hits;
	}

	std::optional<Aabb> PhysicsSystem::GetBodyBounds(UUID entity) const
	{
		if (m_State->World == nullptr || !m_State->IsPresent(entity))
			return std::nullopt;
		const BodyHandle body = m_State->FindOwnedBodyHandle(entity);
		if (!body.IsValid())
			return std::nullopt;
		return m_State->World->GetBodyBounds(body);
	}

	std::optional<Aabb> PhysicsSystem::GetColliderBounds(UUID entity) const
	{
		if (m_State->World == nullptr || !entity.IsValid() || !m_State->IsPresent(entity))
			return std::nullopt;
		// An entity's colliders may sit in two bodies: its solid ones in an ancestor's compound, its triggers in its own
		// implicit sensor body (§5.3). Every one counts.
		Aabb bounds;
		for (const auto& [value, entry] : m_State->Bodies)
		{
			if (!m_State->IsPresent(entry.Owner))
				continue;
			for (size_t index = 0; index < entry.Colliders.size(); ++index)
			{
				if (entry.Colliders[index] != entity)
					continue;
				if (const std::optional<Aabb> collider = m_State->World->GetSubShapeBounds(entry.Handle, static_cast<uint32_t>(index)))
					bounds.Extend(*collider);
			}
		}
		if (bounds.IsEmpty())
			return std::nullopt;
		return bounds;
	}

	Result<PhysicsLayerMask> PhysicsSystem::MakeLayerMask(std::span<const std::string_view> names) const
	{
		return m_State->Layers.MakeMask(names);
	}

}
