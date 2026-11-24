#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <vector>

// The vocabulary of the physics world's buoyancy (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §9.12):
// PhysicsWorld::ApplyBuoyancy (PhysicsWorld.h; Physics/PhysicsWorldBuoyancy.cpp) submerges each collider of a Dynamic body
// against a fluid plane of its own through Jolt's exact submerged volumes and applies Jolt's buoyancy and drag impulse;
// Scene/PhysicsSystem's PreStep item 4c makes the planes from the Water components' surfaces (Scene/WaterQueries.h) for every
// Buoyancy. Plain value types in engine units (metres, kilograms, seconds); thread-compatible.

namespace Engine {

	// The fluid at one collider: whether it is in water at all and, if so, the plane of the water's surface there (a world
	// point on it and its world unit normal, out of the water) and the water's world velocity there (m/s: the waves' motion
	// plus the current). A collider out of water (InWater false) displaces nothing, whatever the other members hold.
	struct PhysicsFluidPlane
	{
		bool InWater = false;
		glm::vec3 Point = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec3 Velocity = glm::vec3(0.0f);
	};

	// The fluid's density (kg/m^3) and the body's response: BuoyancyScale multiplies the buoyant force (1 is physical), and
	// LinearDrag and AngularDrag are Jolt's drag coefficients (Body::ApplyBuoyancyImpulse). The Water and Buoyancy
	// components' bounds hold: Density in [1, 20000], BuoyancyScale in [0, 10], the drags in [0, 100].
	struct PhysicsBuoyancySettings
	{
		float Density = 1000.0f;
		float BuoyancyScale = 1.0f;
		float LinearDrag = 0.5f;
		float AngularDrag = 0.05f;
	};

	// What ApplyBuoyancy found: the body's total volume and the volume submerged under its planes (m^3, the sums over its
	// colliders), whether any of it was submerged, so an impulse was applied, and the volume submerged under each plane, indexed
	// like the planes (0 for a plane out of water or one no collider uses), so the caller can attribute it to the planes' waters.
	struct PhysicsBuoyancyResult
	{
		float SubmergedVolume = 0.0f;
		float TotalVolume = 0.0f;
		bool Submerged = false;
		std::vector<float> PlaneSubmergedVolumes{};
	};

}
