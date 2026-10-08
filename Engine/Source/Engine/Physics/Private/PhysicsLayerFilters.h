#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/PhysicsLayers.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>

// The three small classes of §9.2 "Layers" that implement Jolt's filter interfaces over a world's PhysicsLayerTable
// (PhysicsLayers.h: object layer (layerIndex << 2) | kind, three broad-phase layers, the matrix and the kind rule). Jolt
// calls them on its worker threads during a step and on the main thread for queries; they only read the table. Each holds
// a documented back-reference to the table, which its PhysicsWorld owns and keeps alive and unchanged while the Jolt
// system uses the filter. Private to the Physics module.

namespace Engine {

	namespace Detail {

		// JPH::BroadPhaseLayerInterface: an object layer's kind picks NonMoving, Moving or Sensor.
		class PhysicsBroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface
		{
		public:
			PhysicsBroadPhaseLayerMap() = default;

			[[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override;
			[[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override;
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
			[[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override;
#endif
		};

		// JPH::ObjectVsBroadPhaseLayerFilter: PhysicsLayerTable::ShouldCollide(objectLayer, broadPhaseLayer).
		class PhysicsObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
		{
		public:
			explicit PhysicsObjectVsBroadPhaseLayerFilter(const PhysicsLayerTable& layers);

			[[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broadPhaseLayer) const override;
		private:
			const PhysicsLayerTable* m_Layers = nullptr;
		};

		// JPH::ObjectLayerPairFilter: PhysicsLayerTable::ShouldCollide(a, b), the matrix and the kind rule.
		class PhysicsObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter
		{
		public:
			explicit PhysicsObjectLayerPairFilter(const PhysicsLayerTable& layers);

			[[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override;
		private:
			const PhysicsLayerTable* m_Layers = nullptr;
		};

	}

}
