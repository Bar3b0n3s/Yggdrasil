#include "EnginePCH.h"
#include "Engine/Physics/Private/PhysicsLayerFilters.h"

namespace Engine {

	namespace Detail {

		JPH::uint PhysicsBroadPhaseLayerMap::GetNumBroadPhaseLayers() const
		{
			return PhysicsBroadPhaseLayerCount;
		}

		JPH::BroadPhaseLayer PhysicsBroadPhaseLayerMap::GetBroadPhaseLayer(JPH::ObjectLayer layer) const
		{
			return JPH::BroadPhaseLayer(std::to_underlying(GetPhysicsBroadPhaseLayer(layer)));
		}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
		const char* PhysicsBroadPhaseLayerMap::GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const
		{
			switch (static_cast<PhysicsBroadPhaseLayer>(layer.GetValue()))
			{
				case PhysicsBroadPhaseLayer::NonMoving: return "NonMoving";
				case PhysicsBroadPhaseLayer::Moving:    return "Moving";
				case PhysicsBroadPhaseLayer::Sensor:    return "Sensor";
			}
			return "Unknown";
		}
#endif

		PhysicsObjectVsBroadPhaseLayerFilter::PhysicsObjectVsBroadPhaseLayerFilter(const PhysicsLayerTable& layers)
			: m_Layers(&layers)
		{
		}

		bool PhysicsObjectVsBroadPhaseLayerFilter::ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhaseLayer) const
		{
			const JPH::BroadPhaseLayer::Type value = broadPhaseLayer.GetValue();
			if (value >= PhysicsBroadPhaseLayerCount)
				return false;
			return m_Layers->ShouldCollide(static_cast<PhysicsObjectLayer>(layer), static_cast<PhysicsBroadPhaseLayer>(value));
		}

		PhysicsObjectLayerPairFilter::PhysicsObjectLayerPairFilter(const PhysicsLayerTable& layers)
			: m_Layers(&layers)
		{
		}

		bool PhysicsObjectLayerPairFilter::ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const
		{
			return m_Layers->ShouldCollide(static_cast<PhysicsObjectLayer>(a), static_cast<PhysicsObjectLayer>(b));
		}

	}

}
