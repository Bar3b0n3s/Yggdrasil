#include "EnginePCH.h"
#include "Engine/Physics/Private/PhysicsCollisionGroups.h"

namespace Engine {

	namespace Utils {

		// The 64-bit group a JPH::CollisionGroup of a PhysicsCollisionGroups carries (MakeCollisionGroup's split).
		[[nodiscard]] static uint64_t GetCollisionGroupValue(const JPH::CollisionGroup& group)
		{
			return (static_cast<uint64_t>(group.GetSubGroupID()) << 32) | static_cast<uint64_t>(group.GetGroupID());
		}

	}

	namespace Detail {

		PhysicsGroupFilter::PhysicsGroupFilter(const PhysicsCollisionGroups& groups)
			: m_Groups(&groups)
		{
			// Owned by its PhysicsCollisionGroups, not by the reference counts of the bodies that name it.
			SetEmbedded();
		}

		bool PhysicsGroupFilter::CanCollide(const JPH::CollisionGroup& group1, const JPH::CollisionGroup& group2) const
		{
			return m_Groups->CanCollide(group1, group2);
		}

		PhysicsCollisionGroups::PhysicsCollisionGroups()
			: m_SolidFilter(*this), m_MeshFilter(*this)
		{
		}

		JPH::CollisionGroup PhysicsCollisionGroups::MakeCollisionGroup(uint64_t group, bool holdsMesh) const
		{
			return JPH::CollisionGroup(holdsMesh ? &m_MeshFilter : &m_SolidFilter, static_cast<JPH::CollisionGroup::GroupID>(group & 0xffffffffu),
				static_cast<JPH::CollisionGroup::SubGroupID>(group >> 32));
		}

		bool PhysicsCollisionGroups::CanCollide(const JPH::CollisionGroup& group1, const JPH::CollisionGroup& group2) const
		{
			if (!IsOwnGroup(group1) || !IsOwnGroup(group2))
				return true;
			if (group1.GetGroupFilter() == &m_MeshFilter && group2.GetGroupFilter() == &m_MeshFilter)
				return false;
			const uint64_t value = Utils::GetCollisionGroupValue(group1);
			return value == 0 || value != Utils::GetCollisionGroupValue(group2);
		}

		bool PhysicsCollisionGroups::IsOwnGroup(const JPH::CollisionGroup& group) const
		{
			const JPH::GroupFilter* filter = group.GetGroupFilter();
			return filter == &m_SolidFilter || filter == &m_MeshFilter;
		}

	}

}
