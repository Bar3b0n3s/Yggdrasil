#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The recreation protocol (M18, C18.3; Docs/Decisions/0029-m18-contract.md decisions 25 to 33; Architecture §8.14): how
// every GPU object of the engine is released when the device is lost and created again on the recreated device, so the
// editor and the Runtime keep running after a device loss.
//
// The placement rule (decision 27). A GPU object lives in one of three places, each of which registers itself here, or its
// owner registers explicitly:
//   1. SceneRendererPipelines: the pipelines, binding layouts, samplers and startup resources of a declared pass
//      (RenderPassDeclaration.h). Released with the set, and created again through SceneRendererPipelines::CreatePipelines,
//      the one creation entry that startup and the shader reload also use.
//   2. SceneRenderer::State: a renderer's per-view objects, its targets acquired from the state's RenderTargetPool, its
//      per-view buffers and its binding and material sets. Released with the renderer and created again by it (the
//      size-dependent targets and per-view buffers at once, everything else lazily by its next Render).
//   3. GpuResourceCache: the asset mirrors, released with the cache and uploaded again from the assets' loaded data by the
//      next Get.
//   4. Anything else: its owner registers here explicitly (GpuRecreationRegistry::Register) with hooks that release and
//      create again what it holds: the frame's swapchain or offscreen target, pacer, profiler and command list, the ImGui
//      renderer, the shader library, the viewport capture, the environment baker, the editor's views and thumbnails, the
//      Runtime's blit, and every later GPU owner outside the three places (M24's platform windows among them).
//
// Each registration names one owner object; an object never registers twice, and an object owned by another registered
// owner (a renderer's pool, a capture's private renderer, a pass of the set) is covered by its own class or its owner's
// hooks, never by a second registration of the same object. The device's recovery (GraphicsDevice::Recover) checks that
// nothing escaped: once every owner released, no GPU object the device tracks may still be alive (GpuResourceTracker).
//
// M18 (C18.4, decision 43): the same registry serves out-of-memory after startup. An owner that keeps GPU objects it can
// create again on demand (a pool's free targets, a cache's stale mirrors) also sets GpuRecreationHooks::TrimUnused, which
// GraphicsDevice::TrimUnused calls before a request that ran out of memory retries (Graphics/GpuOutOfMemory.h).

namespace Engine {

	class GpuRecreationRegistry;

	// What one owner does when the device is lost and when it exists again (decision 26). Both hooks run on the main thread,
	// between frames, from GraphicsDevice::Recover; neither may register or unregister an owner. M18 (C18.4,
	// Docs/Decisions/0029-m18-contract.md decision 43): an optional third hook, TrimUnused, runs when a request after startup
	// runs out of GPU memory (GraphicsDevice::TrimUnused), on a healthy device; it may not register or unregister an owner
	// either.
	struct GpuRecreationHooks
	{
		// The owner's name in logs, errors and GpuRecreationRegistry::GetOwnerNames ("SceneRendererPipelines", "Swapchain",
		// "EditorHostViewports"). Not empty.
		std::string Name{};
		// Drops every GPU object the owner holds (every NVRHI handle, and every raw pointer or key into one, such as an
		// ImTextureID), and forgets what it recorded for a submission that will never complete (a pending pick, a recorded
		// frame slot). It runs on a lost device, possibly in the middle of a frame that the loss ended (an open command list,
		// an ImGui frame begun), so it calls nothing on the device but the release of handles, never waits for the GPU, and
		// tolerates a half-recorded frame. It cannot fail. Required.
		std::function<void()> Release{};
		// Creates again, on the recreated device, what the owner needs before its next use: what it created eagerly at its
		// own creation (pipelines, the size-dependent targets, a swapchain); what it creates lazily is created by its next
		// use. Returns the first error; an error makes the recovery fail (GraphicsDevice::Recover). Empty: nothing to create.
		std::function<Status()> Recreate{};
		// M18 (C18.4, decision 43): drops what the owner holds that it can create again and that nothing uses now, so a
		// request that ran out of GPU memory can retry: free pooled targets (RenderTargetPool::TrimUnused), mirrors of
		// replaced versions and unloaded assets (GpuResourceCache::TrimUnused), a capture's targets between captures
		// (ViewportCapture), any other cache of GPU objects the owner rebuilds on demand. It runs on a healthy device, between
		// recordings (never while the owner or anyone else records with what it drops, or holds a reference into its tables),
		// possibly from inside the owner's own request that ran out of memory (a renderer's Resize trims before its retry),
		// so it drops only what that request does not use and leaves the owner usable. GraphicsDevice::TrimUnused has waited
		// for the device's last submission before it runs the hooks, so a submitted use of what a hook drops has completed,
		// but no wait covers a command list that is still open: a hook keeps what an open list may record into without NVRHI
		// referencing it (a pool keeps the targets acquired since its last EndFrame). It calls nothing on the device but the
		// release of handles (GraphicsDevice::TrimUnused then collects them) and GraphicsDevice::GetCompletedSubmissionID,
		// which does not wait; it never waits, and cannot fail. Empty: nothing to trim. Every owner of a pool, a cache or
		// targets it can rebuild sets it, later milestones' included.
		std::function<void()> TrimUnused{};
	};

	// One registration: unregisters its owner when destroyed or reset. Movable, not copyable; an empty registration (the
	// default, or a moved-from one) does nothing. Main thread only. It must not outlive its registry: an owner holds it as a
	// member, and every owner is destroyed before the device (§8.14 item 4).
	class GpuRecreationRegistration
	{
	public:
		GpuRecreationRegistration() = default;
		~GpuRecreationRegistration();

		GpuRecreationRegistration(GpuRecreationRegistration&& other) noexcept;
		GpuRecreationRegistration& operator=(GpuRecreationRegistration&& other) noexcept;
		GpuRecreationRegistration(const GpuRecreationRegistration&) = delete;
		GpuRecreationRegistration& operator=(const GpuRecreationRegistration&) = delete;

		// Unregisters now (no effect when empty); afterwards the registration is empty.
		void Reset();
		// Whether the registration still names an owner.
		[[nodiscard]] bool IsRegistered() const { return m_Registry != nullptr; }
	private:
		friend class GpuRecreationRegistry;
		GpuRecreationRegistration(GpuRecreationRegistry& registry, uint64_t id);
	private:
		GpuRecreationRegistry* m_Registry = nullptr; // documented back-reference: outlives the registration
		uint64_t m_Id = 0;
	};

	// The owners of one GraphicsDevice's GPU objects (GraphicsDevice::GetRecreation), in registration order. Main thread only
	// (§4.11); not copyable or movable.
	//
	// Order (decision 26): ReleaseAll calls every owner's Release in the reverse order of registration and RecreateAll every
	// Recreate in the order of registration, like destruction and construction: an owner registers when it is created, after
	// the owners it uses (a renderer after its pipeline set and its cache, the editor's views after the ImGui renderer), so it
	// is released before them and created again after them. That is §8.14 item 4's teardown order.
	class GpuRecreationRegistry
	{
	public:
		GpuRecreationRegistry();
		// Every registration must be gone (asserted): owners are destroyed before their device.
		~GpuRecreationRegistry();

		GpuRecreationRegistry(const GpuRecreationRegistry&) = delete;
		GpuRecreationRegistry& operator=(const GpuRecreationRegistry&) = delete;

		// Adds an owner at the end of the order and returns its registration. `hooks.Name` must not be empty and
		// `hooks.Release` must be set (asserted). Not during ReleaseAll or RecreateAll (asserted).
		[[nodiscard]] GpuRecreationRegistration Register(GpuRecreationHooks hooks);

		// Calls every owner's Release, last registered first. Not reentrant (asserted).
		void ReleaseAll();
		// Calls every owner's Recreate, first registered first, and stops at the first error, which it returns with the
		// owner's name as context ("while recreating '<Name>'"); the owners after it stay released, and the caller's recovery
		// fails. Not reentrant (asserted).
		[[nodiscard]] Status RecreateAll();
		// M18 (C18.4, decision 43): calls every owner's TrimUnused, last registered first (an owner trims before the owners
		// it uses, as Release runs), skipping owners without one; returns how many owners trimmed. For
		// GraphicsDevice::TrimUnused, after its wait for the device's last submission. Not reentrant and not during ReleaseAll
		// or RecreateAll (asserted: IsRunning; RetryAfterTrim never trims while the registry runs).
		[[nodiscard]] size_t TrimUnused();

		// The registered owners' names, in registration order (for logs, errors and the loss tests).
		[[nodiscard]] std::vector<std::string> GetOwnerNames() const;
		[[nodiscard]] size_t GetOwnerCount() const;
		// Whether ReleaseAll, RecreateAll or (M18, C18.4) TrimUnused is running.
		[[nodiscard]] bool IsRunning() const { return m_Running; }
	private:
		friend class GpuRecreationRegistration;
		// Removes the owner `id` (a registration's destructor or Reset).
		void Unregister(uint64_t id);
	private:
		struct Owner
		{
			// GpuRecreationRegistration's. Register numbers the owners from 1 (0 is an empty registration's) with
			// m_LastOwnerId, a counter that only grows, so an id is never reused and no two registrations ever name the same
			// owner.
			uint64_t Id = 0;
			GpuRecreationHooks Hooks{};
		};
		std::vector<Owner> m_Owners{}; // in registration order
		uint64_t m_LastOwnerId = 0;    // the id Register gave last
		bool m_Running = false;
	};

}
