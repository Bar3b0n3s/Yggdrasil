#pragma once

#include "Engine/Automation/Protocol/Dispatcher.h"
#include "Engine/Core/Base.h"

// The automation servers' answer to a Vulkan error inside a method (Architecture §4.6 item 2, §8.14): the frame-boundary
// catch of App/FrameLoop.cpp covers the frame's own work, and this handler gives a server the same ending for the methods it
// runs (Docs/Decisions/0009-m5-decisions.md decision 33). It lives in App because only App and Graphics may name vulkan.hpp's
// error category; the Runtime, whose project may not include third-party headers, passes it to its RuntimeAutomationServer.

namespace Engine {

	class EngineContext;

	// The SystemErrorHandler (Automation/Protocol/Dispatcher.h) of an application with a device: a std::system_error of
	// vulkan.hpp's error category (vk::SystemError thrown out of NVRHI) ends the process through RaiseVulkanError
	// (Graphics/GraphicsDevice.h) with `context`'s device, which never returns; any other system error returns, and the
	// Dispatcher handles it like any exception. `context` is a documented back-reference that outlives the handler.
	[[nodiscard]] SystemErrorHandler MakeVulkanSystemErrorHandler(EngineContext& context);

}
