#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Session/PlaySession.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

// Scene building blocks for the physics tests (Architecture §9.7; Docs/Decisions/0014-m11-decisions.md decision 19):
// bodies and colliders added to an edit scene, play sessions over it, a listener that records the sorted events, and a
// scoped Jolt worker thread count. Shared by the physics test files.

namespace Engine {

	namespace Test {

		// Adds an entity `name` (the last child of `parent` when valid, else the last root) at `position` with a BoxCollider
		// of `halfExtents` and, when `type` is set, a RigidBody of that type (default settings otherwise).
		Entity AddBoxBody(Scene& scene, std::string_view name, const glm::vec3& position, const glm::vec3& halfExtents, std::optional<BodyType> type,
			Entity parent = {});

		// As AddBoxBody with a SphereCollider of `radius`.
		Entity AddSphereBody(Scene& scene, std::string_view name, const glm::vec3& position, float radius, std::optional<BodyType> type, Entity parent = {});

		// A Static ground box 100 m x 1 m x 100 m whose top face is at y = 0, named `name`.
		Entity AddGround(Scene& scene, std::string_view name = "Ground");

		// Patches `entity`'s RigidBody (present, asserted) through Entity::Patch.
		void PatchRigidBody(Entity entity, const std::function<void(RigidBodyComponent&)>& patch);

		// A play session specification over `fixture`'s registry: `seed`, `mode`, the default project settings (one layer
		// "Default" colliding with itself, gravity (0, -9.81, 0), 60 Hz) and no asset manager.
		[[nodiscard]] PlaySessionSpecification MakePhysicsSessionSpecification(SceneTestFixture& fixture, uint64_t seed = 42, PlayMode mode = PlayMode::Play);

		// PlaySession::CreateFromScene of `fixture`'s scene with `specification`.
		[[nodiscard]] Result<Scope<PlaySession>> StartPhysicsSession(SceneTestFixture& fixture, const PlaySessionSpecification& specification);

		// Runs `ticks` lockstep ticks (PlaySession::Tick).
		void RunTicks(PlaySession& session, uint32_t ticks);

		// The world position of the entity at `path` in the session's scene (valid, asserted).
		[[nodiscard]] glm::vec3 GetWorldPosition(PlaySession& session, std::string_view path);

		// The UUID of the entity at `path` in `scene` (the invalid UUID when no entity has that path).
		[[nodiscard]] UUID GetEntityId(const Scene& scene, std::string_view path);

		// Sets the process's Jolt worker thread count (PhysicsEngine::SetWorkerThreadCount) for its lifetime and restores the
		// previous count on every exit path, a failed REQUIRE included, so no later test runs with another count (tests never
		// depend on order, CodeStyle §14).
		class ScopedWorkerThreadCount
		{
		public:
			explicit ScopedWorkerThreadCount(uint32_t count);
			~ScopedWorkerThreadCount();

			ScopedWorkerThreadCount(const ScopedWorkerThreadCount&) = delete;
			ScopedWorkerThreadCount& operator=(const ScopedWorkerThreadCount&) = delete;
		private:
			uint32_t m_Original = 0;
		};

		// Records every physics event and diagnostic it receives, in order, and calls OnEvent (when set) after recording each
		// event.
		class RecordingPhysicsListener final : public IPhysicsEventListener
		{
		public:
			void OnPhysicsEvent(const PhysicsEvent& event) override;
			void OnPhysicsDiagnostic(const PhysicsDiagnostic& diagnostic) override;

			// The recorded events whose Self is `self` and whose type is `type`.
			[[nodiscard]] std::vector<PhysicsEvent> Find(UUID self, PhysicsEventType type) const;

			std::vector<PhysicsEvent> Events{};
			std::vector<PhysicsDiagnostic> Diagnostics{};
			std::function<void(const PhysicsEvent&)> OnEvent{};
		};

	}

}
