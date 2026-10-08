#pragma once

#include "Engine/Core/Base.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystem.h>

namespace JPH {

	class PhysicsSystem;

}

// The parts of PhysicsEngine's process-level state (Physics/PhysicsEngine.cpp) that the rest of the Physics module uses:
// the job system every PhysicsWorld steps on, and the registry of live worlds behind PhysicsEngine::GetLiveWorldCount and
// GetLiveBodyCount. Private to the Physics module (§3 rule 3: Jolt types never reach a public header).

namespace Engine {

	namespace Detail {

		// The process's JobSystemThreadPool; nullptr while the PhysicsEngine is not initialized. Main thread only.
		[[nodiscard]] JPH::JobSystem* GetPhysicsJobSystem();

		// True on the thread that initialized the PhysicsEngine while it is initialized.
		[[nodiscard]] bool IsPhysicsMainThread();

		// Registers the Jolt system of a PhysicsWorld from its creation until its destruction (a documented back-reference:
		// PhysicsWorld unregisters it before the system is destroyed). The live counters count the registered systems and the
		// bodies in them, character inner bodies included. Thread-safe.
		void RegisterPhysicsWorld(const JPH::PhysicsSystem& system);
		void UnregisterPhysicsWorld(const JPH::PhysicsSystem& system);

	}

}
