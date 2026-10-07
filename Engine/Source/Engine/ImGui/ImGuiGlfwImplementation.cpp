// Dear ImGui's GLFW platform backend (Architecture §2.2, §8.11; Vendor/imgui/VENDOR.md "GLFW platform backend"): the
// vendored backends/imgui_impl_glfw.cpp compiled in its own translation unit of the Engine project, without the
// precompiled header (Engine/premake5.lua), with GLFW's include path from UseGLFW. It defines the GLFW_EXPOSE_NATIVE_*
// macros it needs itself. ImGuiLayer is its only caller (ImGui_ImplGlfw_InitForVulkan, ImGui_ImplGlfw_NewFrame,
// ImGui_ImplGlfw_Shutdown), on native platforms. On GLFW's null platform the backend cannot initialize (on Windows it
// asserts that the window has an HWND to subclass), so ImGuiLayer acts as the platform backend there itself
// (ImGui/ImGuiLayer.cpp); this file is compiled and linked the same way on every platform.
//
// The Vulkan headers come first. Without VULKAN_H_ the backend declares stand-ins for the Vulkan types it names,
// among them its own one-enumerator `enum VkResult`, which would be a second, different definition of ::VkResult in the
// Engine library next to the one of vulkan_core.h (a one-definition-rule violation that link-time optimization may
// diagnose or miscompile). With vulkan.h included the backend uses the real types (Docs/Decisions/0009-m5-decisions.md).

#include <vulkan/vulkan.h>

#include <backends/imgui_impl_glfw.cpp>
