#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Result.h"

#include <functional>
#include <string_view>

// GPU out-of-memory after startup (M18, C18.4; Docs/Decisions/0029-m18-contract.md decisions 42 to 45; Architecture §8.14
// item 7). At startup (the device, the swapchain, the startup pipelines and targets, the hosts' startup objects) an
// out-of-memory creation is FatalError(OutOfMemory): nothing has rendered. After startup a request that creates GPU objects
// (a resize of the frame, a view's new size, a capture, a debug-view switch, a view's per-frame Render) trims what can be
// rebuilt and tries once more, and if that fails too only the request fails, with ErrorCode::OutOfMemory, while the
// process goes on. A request creates its new objects aside and replaces the current ones only when every creation
// succeeded, so a failed attempt leaves its owner as it was: the retry starts from the same state, and a request that
// failed keeps the previous objects. A target is never shrunk to fit, because headless must equal windowed.

namespace Engine {

	class GraphicsDevice;

	// M18 (C18.4, decision 42): whether `error` is an out-of-memory creation on `device`: a Gpu error (a GraphicsDevice
	// wrapper's null result, an injected oom-* fault, a Vulkan out-of-memory result) while the device is not lost. On a lost
	// device a Gpu error is the loss (decision 28), which the frame boundary recovers, never out-of-memory.
	[[nodiscard]] bool IsGpuOutOfMemory(const Error& error, const GraphicsDevice& device);

	// M18 (C18.4, decision 42): the out-of-memory rule after startup. Runs `attempt`; when it fails with an out-of-memory
	// creation, logs one Warn line naming `request` and the error, trims (GraphicsDevice::TrimUnused) and runs `attempt`
	// once more. A failure is out-of-memory when IsGpuOutOfMemory holds for it and, when `isOutOfMemory` is set, that
	// classification also names it out-of-memory: a request whose creations can also fail with Gpu for another reason
	// passes one, so that failure is returned as Gpu, never trimmed for or turned into OutOfMemory (the swapchain's
	// recreation: only vkCreateSwapchainKHR's out-of-memory results, an injected oom-swapchain fault and a failed image
	// wrapping are out-of-memory, never VK_ERROR_INITIALIZATION_FAILED or a failed surface query; Swapchain.h). A second
	// out-of-memory failure is returned as ErrorCode::OutOfMemory ("<request>: out of GPU memory after trimming what could be
	// rebuilt and retrying once: <the retry's error>"), with the retry's error's location; any other result of either
	// attempt is returned unchanged (success, an error that is not out-of-memory, and a Gpu error on a lost device, which is
	// the loss), without a trim. While the device's recreation registry runs (GpuRecreationRegistry::IsRunning: a
	// recovery's Release or Recreate), `attempt` runs once and its result is returned unchanged, without a trim: an
	// out-of-memory creation in a recovery's Recreate is a second startup's, which fails the recovery (decision 42), and
	// the registry cannot trim while it runs. `attempt` must leave its owner as it was when it fails (it creates aside and
	// replaces on success), and must run between recordings, as TrimUnused requires: never while a command list records
	// with what an owner may trim, nor while its caller holds a GpuResourceCache mirror reference. Main thread. At most one
	// trim per call: a request inside another that failed alone returned OutOfMemory, which is not a Gpu error, so the
	// outer request returns it without trimming for it again; an inner request that succeeded after its own trim does not
	// keep the outer one from trimming for a later failure of its own.
	[[nodiscard]] Status RetryAfterTrim(GraphicsDevice& device, std::string_view request, const std::function<Status()>& attempt,
		const std::function<bool(const Error&)>& isOutOfMemory = {});

}
