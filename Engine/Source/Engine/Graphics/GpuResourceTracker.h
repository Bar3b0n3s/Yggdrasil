#pragma once

#include "Engine/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

// Live GPU object counts per type (Architecture §8.14 item 5). GPU tests assert that the live counts return to their
// baseline after a scene unloads and are zero when the device is destroyed ("GpuResourceTracker: live counts return to
// zero"); application teardown asserts the same (§4.1).
//
// NVRHI destroys its objects itself, when the last handle drops (a submitted command list holds a reference to every
// object it used until NVRHI's runGarbageCollection retires it), and offers no hook for observing that. The tracker
// therefore keeps one reference to every object the GraphicsDevice creation wrappers made and, in Sweep, releases the
// objects it is the last owner of and counts them as destroyed. An object thus lives until the first Sweep after its
// last other reference dropped. GraphicsDevice::RunGarbageCollection sweeps once per frame right after NVRHI's own
// garbage collection, so the objects the retired command lists held are released in the same frame, and the delay adds
// no frame to NVRHI's deferred destruction. Objects NVRHI does not own (the host-copied images of HostImageUpload) are
// counted with RecordCreated and RecordDestroyed instead.
//
// The tracker's reference must not change what owners observe: a pool or cache that hands out copies of a handle and
// asks whether anyone still uses it (RenderTargetPool, HostImageUpload, M6's GpuResourceCache) asks HasOtherReferences,
// which discounts the tracker's own reference and answers the same in every configuration. Reading the reference count
// directly would see one reference more in Debug and Release than in Dist.
//
// Active in Debug and Release builds; in Dist every member except HasOtherReferences is a no-op and every count is 0
// (§8.14: "non-Dist"). Thread safety: main thread only, like every NVRHI call (§4.11).
//
// The lifetime log (M18, R-GPUCRASH; Docs/Decisions/0029-m18-contract.md, W18.1 stream C): a tracker built with a log
// directory (the environment variable ENGINE_GPU_LIFETIME_LOG, GpuLifetimeLogVariable, for the device's own tracker) appends
// one line per event to <directory>/GpuLifetime-<process id>.log, shared by every tracker of the process:
//     <sequence> <microseconds since the Unix epoch> tracker=<tracker address> <event> [<type> <object address> native=<handle>]
// with the events open (pid=<process id>), create (Track), destroy (a Sweep released the object; logged before the release),
// host-create and host-destroy (RecordCreated and RecordDestroyed, without an object), leak (each object ReleaseAll gives up)
// and close (the tracker's destruction, after its device's). The sequence counts the tracker's lines from 1; native is the
// Vulkan handle of the object's type (VkImage, VkBuffer, VkSampler, VkShaderModule, VkDescriptorSetLayout, VkDescriptorSet,
// VkPipeline; 0 where the type has none, a framebuffer and a command list included, or NVRHI reports none). Each line is
// flushed to the operating system before the call returns, so a crash leaves every earlier event in the file: the
// hardening stage's debug-heap GPU step sets the variable to reproduce the GPU-suite crash on a driver thread
// (Docs/Backlog.md). A directory that cannot be created or a file that cannot be opened logs one warning and leaves the log
// off; the tracker counts as before either way. Not in Dist.

namespace Engine {

	// The environment variable that turns the device trackers' lifetime log on: the directory of the log files.
	inline constexpr std::string_view GpuLifetimeLogVariable = "ENGINE_GPU_LIFETIME_LOG";

	// The object types counted. Values index the count arrays; append new types at the end.
	enum class GpuResourceType : uint8_t
	{
		Texture,
		StagingTexture,
		GpuBuffer, // not Buffer: an enumerator named like the alias Engine::Buffer (Core/Buffer.h) trips GCC's -Wshadow
		Sampler,
		Shader,
		InputLayout,
		BindingLayout,
		BindingSet,
		Framebuffer,
		GraphicsPipeline,
		ComputePipeline,
		CommandList,
		EventQuery,
		TimerQuery,
		HostImage // a VkImage plus memory created by HostImageUpload's host-copy path, wrapped by createHandleForNativeTexture
	};

	inline constexpr size_t GpuResourceTypeCount = 15;
	static_assert(static_cast<size_t>(GpuResourceType::HostImage) + 1 == GpuResourceTypeCount, "GpuResourceTypeCount is stale");

	// The counts of one type.
	struct GpuResourceCounts
	{
		uint64_t Created = 0;
		uint64_t Destroyed = 0;

		[[nodiscard]] uint64_t GetLive() const { return Created - Destroyed; }
	};

	class GpuResourceTracker
	{
	public:
		// The lifetime log goes to the directory ENGINE_GPU_LIFETIME_LOG names (see the file comment); off when it is unset or
		// empty.
		GpuResourceTracker();
		// The lifetime log goes to `lifetimeLogDirectory`, created when missing; off for an empty path.
		explicit GpuResourceTracker(const std::filesystem::path& lifetimeLogDirectory);
		~GpuResourceTracker();

		GpuResourceTracker(const GpuResourceTracker&) = delete;
		GpuResourceTracker& operator=(const GpuResourceTracker&) = delete;

		// Counts `resource` as created and keeps a reference to it until a Sweep finds the tracker its last owner. A null
		// resource is a programmer error (asserted; creation wrappers only track non-null results).
		void Track(GpuResourceType type, nvrhi::IResource* resource);

		// Counts objects whose lifetime the caller manages (GpuResourceType::HostImage).
		void RecordCreated(GpuResourceType type);
		void RecordDestroyed(GpuResourceType type);

		// Releases every tracked object whose only remaining reference is the tracker's and counts it as destroyed, and
		// repeats until a pass releases nothing: objects reference each other (a framebuffer its textures, a binding set its
		// resources, a pipeline its shaders and layouts), so releasing one can make the tracker the last owner of another.
		// After NVRHI's runGarbageCollection on an idle device, one Sweep therefore leaves exactly the objects someone
		// besides the tracker still holds.
		void Sweep();

		// Whether anyone besides the caller and the tracker references `resource`: its reference count, minus the tracker's
		// own reference while the tracker holds one (Debug and Release, for an object a creation wrapper made), exceeds
		// `callerReferences`, the references the caller itself holds (usually 1, its own handle). References held by
		// submitted command lists that NVRHI has not retired yet count as other references. The answer is the same in every
		// configuration, Dist included. The way pools and caches decide that a resource they own is free (see the file
		// comment). `resource` must be alive (the caller holds it).
		[[nodiscard]] bool HasOtherReferences(nvrhi::IResource& resource, uint32_t callerReferences) const;

		// Releases every reference the tracker still holds without counting it as destroyed; the device calls it after its
		// final Sweep and before NVRHI's device is destroyed, so a leaked object is reported (live count above zero) rather
		// than kept alive by the tracker.
		void ReleaseAll();

		[[nodiscard]] GpuResourceCounts GetCounts(GpuResourceType type) const;
		[[nodiscard]] uint64_t GetLiveCount(GpuResourceType type) const;
		// The sum of GetLiveCount over every type.
		[[nodiscard]] uint64_t GetTotalLiveCount() const;

		// "Texture: 3, Buffer: 1" for every type with a live count above zero, in GpuResourceType order; "none" when all are
		// zero. For assertion and test failure messages.
		[[nodiscard]] std::string DescribeLiveCounts() const;

		// The file the lifetime log appends to; empty when the log is off.
		[[nodiscard]] std::filesystem::path GetLifetimeLogPath() const;

		// Whether the tracker counts and holds references: false in Dist builds.
		[[nodiscard]] static constexpr bool IsEnabled()
		{
#if defined(ENGINE_DIST)
			return false;
#else
			return true;
#endif
		}
	private:
		// One object a creation wrapper made, with the tracker's reference to it.
		struct TrackedResource
		{
			GpuResourceType Type = GpuResourceType::Texture;
			nvrhi::RefCountPtr<nvrhi::IResource> Resource{};
		};
		// The open lifetime log (GpuResourceTracker.cpp).
		struct LifetimeLog;
	private:
		// Appends one event line to the lifetime log when it is on.
		void LogEvent(std::string_view event, GpuResourceType type, nvrhi::IResource* resource);
		void LogEvent(std::string_view event);
	private:
		std::array<GpuResourceCounts, GpuResourceTypeCount> m_Counts{};
		// Keyed by the object, so HasOtherReferences finds whether the tracker holds it. A Sweep never releases in the map's
		// order: each pass collects the objects it releases and releases them in address order, so the lifetime log lists
		// them in a fixed order (each object is destroyed once its last reference drops, whatever the order).
		std::unordered_map<nvrhi::IResource*, TrackedResource> m_Tracked;
		Scope<LifetimeLog> m_LifetimeLog{}; // null while the log is off
	};

	// The enumerator name ("Texture", "HostImage", ...).
	[[nodiscard]] std::string_view GpuResourceTypeToString(GpuResourceType type);

}
