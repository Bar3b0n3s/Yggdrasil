#include "TestsPCH.h"

#include "Engine/ImGui/ImGuiPlatformWindows.h"

#include "Engine/App/Application.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/GraphicsSpecification.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/ImGui/ImGuiLayer.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Engine/Platform/Process.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"
#include "Support/WindowedChild.h"

#include <glm/vec2.hpp>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Dear ImGui's platform windows (M24, C24.1, E-VIEWPORTS; Docs/Decisions/0035-m24-contract.md decisions 18 and 19, stream V):
// the recreation protocol through the shared loss test, with offscreen targets standing in for the windows' swapchains on the
// headless device, and the presenting half (swapchains on real windows, Recover(window)) in a windowed child, as ADR 0029
// decision 32 has the owners that present prove it; and the out-of-memory rule of a window's target through the injected
// oom-rendertarget faults. Skipped until stream V implements them.

namespace Engine {

	namespace {

		// A platform side without OS windows, the GLFW backend's stand-in on the headless device: every viewport keeps the
		// position and size Dear ImGui gives it, one monitor holds them all. As a platform backend's initialization does, it
		// gives the main viewport a platform handle (this object, a sentinel nothing dereferences), which Dear ImGui's frame
		// checks require once the renderer side has viewports.
		class FakePlatformViewports
		{
		public:
			FakePlatformViewports()
				: m_Context(ImGui::CreateContext())
			{
				ImGuiIO& io = ImGui::GetIO();
				io.DisplaySize = ImVec2(256.0f, 128.0f);
				io.DeltaTime = 1.0f / 60.0f;
				io.IniFilename = nullptr;
				io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
				io.BackendFlags |= ImGuiBackendFlags_PlatformHasViewports;
				ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
				platform.Platform_CreateWindow = [](ImGuiViewport* /*viewport*/) {};
				platform.Platform_DestroyWindow = [](ImGuiViewport* /*viewport*/) {};
				platform.Platform_ShowWindow = [](ImGuiViewport* /*viewport*/) {};
				platform.Platform_SetWindowPos = [](ImGuiViewport* /*viewport*/, ImVec2 /*position*/) {};
				platform.Platform_GetWindowPos = [](ImGuiViewport* viewport)
				{
					return viewport->Pos;
				};
				platform.Platform_SetWindowSize = [](ImGuiViewport* /*viewport*/, ImVec2 /*size*/) {};
				platform.Platform_GetWindowSize = [](ImGuiViewport* viewport)
				{
					return viewport->Size;
				};
				ImGuiPlatformMonitor monitor;
				monitor.MainPos = monitor.WorkPos = ImVec2(0.0f, 0.0f);
				monitor.MainSize = monitor.WorkSize = ImVec2(1024.0f, 512.0f);
				platform.Monitors.push_back(monitor);
				ImGui::GetMainViewport()->PlatformHandle = this;
			}

			~FakePlatformViewports()
			{
				ImGui::DestroyPlatformWindows();
				ImGui::DestroyContext(m_Context);
			}

			FakePlatformViewports(const FakePlatformViewports&) = delete;
			FakePlatformViewports& operator=(const FakePlatformViewports&) = delete;
		private:
			ImGuiContext* m_Context = nullptr;
		};

		// One platform window's target on the headless device: an offscreen target of the window's size, read back by the test.
		class OffscreenWindowTarget final : public IImGuiPlatformWindowTarget
		{
		public:
			OffscreenWindowTarget(GraphicsDevice& device, glm::uvec2 size)
				: m_Device(&device), m_Size(size)
			{
			}

			[[nodiscard]] Status Create()
			{
				ENGINE_TRY_ASSIGN(OffscreenTarget target, OffscreenTarget::Create(*m_Device, { .Width = m_Size.x, .Height = m_Size.y }));
				m_Target.emplace(std::move(target));
				return {};
			}

			Result<nvrhi::IFramebuffer*> Acquire(uint32_t /*frameSlot*/) override
			{
				if (!m_Target.has_value())
					return static_cast<nvrhi::IFramebuffer*>(nullptr);
				return m_Target->GetFramebuffer();
			}

			void QueueFrameSemaphores() override {}
			void Present() override {}
			void Resize(glm::uvec2 size) override { m_Size = size; }
			void ReleaseGpuObjects() override { m_Target.reset(); }
			Status RecreateGpuObjects() override { return Create(); }

			[[nodiscard]] nvrhi::ITexture* GetColorTexture() const { return m_Target.has_value() ? m_Target->GetColorTexture() : nullptr; }
		private:
			GraphicsDevice* m_Device = nullptr; // documented back-reference: the fixture's device outlives the target
			glm::uvec2 m_Size = glm::uvec2(0);
			std::optional<OffscreenTarget> m_Target{};
		};

	}

	TEST_SUITE("ImGui")
	{
		TEST_CASE("ImGuiPlatformWindows: a device loss recreates every platform window's target and draws the same image"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			FakePlatformViewports platform;
			Result<Scope<ImGuiRenderer>> renderer = ImGuiRenderer::Create(device, gpu.GetPipelines(), { .FramesInFlight = 1 });
			REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
			std::vector<OffscreenWindowTarget*> targets;
			ImGuiPlatformWindowsSpecification specification;
			specification.FramesInFlight = 1;
			specification.CreateTarget = [&targets](GraphicsDevice& owner, const ImGuiPlatformWindowTargetRequest& request) -> Result<Scope<IImGuiPlatformWindowTarget>>
			{
				Scope<OffscreenWindowTarget> target = CreateScope<OffscreenWindowTarget>(owner, request.Size);
				ENGINE_TRY(target->Create());
				targets.push_back(target.get());
				return Scope<IImGuiPlatformWindowTarget>(std::move(target));
			};
			Result<Scope<ImGuiPlatformWindows>> windows = ImGuiPlatformWindows::Create(device, **renderer, specification);
			REQUIRE_MESSAGE(windows.has_value(), windows.error().ToString());
			FramePacer pacer(device, 1);

			// One frame: a window placed beyond the main viewport's 256 x 128 gets a platform window of its own, drawn into its
			// target, which is read back.
			const auto frame = [&pacer, &renderer, &windows, &device, &targets]() -> Result<std::vector<std::byte>>
			{
				pacer.BeginFrame();
				(*renderer)->BeginFrame(pacer.GetFrameSlot());
				ImGui::NewFrame();
				ImGui::SetNextWindowPos(ImVec2(400.0f, 32.0f));
				ImGui::SetNextWindowSize(ImVec2(160.0f, 96.0f));
				ImGui::Begin("Undocked");
				ImGui::TextUnformatted("A platform window");
				ImGui::End();
				ImGui::Render();
				ImGui::UpdatePlatformWindows();
				const Status rendered = (*windows)->Render(pacer.GetFrameSlot());
				pacer.EndFrame(device.GetLastSubmissionID());
				device.RunGarbageCollection();
				ENGINE_TRY(rendered);
				if ((*windows)->GetWindowCount() != 1 || targets.empty() || targets.back()->GetColorTexture() == nullptr)
					return MakeError(ErrorCode::InvalidState, "the undocked window has no platform window");
				Readback readback(device);
				ENGINE_TRY_ASSIGN(const Image image, readback.ReadTexture(*targets.back()->GetColorTexture()));
				return image.Pixels;
			};
			// Dear ImGui settles a new window over its first frames.
			for (int settling = 0; settling < 3; ++settling)
				REQUIRE(frame().has_value());

			Test::DeviceLossTest test;
			test.Owners = { "ImGuiRenderer", "ImGuiPlatformWindows" };
			test.Use = frame;
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK_FALSE(report->Before.empty());
			windows->reset();
			(*renderer)->DestroyTextures();
		}

		TEST_CASE("ImGuiPlatformWindows: an out-of-memory target is trimmed and retried once, and otherwise only its window draws nothing until its next resize"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// Decision 19 (M18's rule after startup, ADR 0029 decisions 42 and 44): a platform window's target is a request. Under
			// oom-rendertarget its first creation fails and the retry after the trim succeeds; under oom-rendertarget-persistent
			// both fail, so the window has no target and draws nothing while Render and the frame go on, and its next size change
			// tries again.
			for (const GpuFault fault : { GpuFault::OomRenderTarget, GpuFault::OomRenderTargetPersistent })
			{
				CAPTURE(GpuFaultToString(fault));
				const bool persistent = fault == GpuFault::OomRenderTargetPersistent;
				Test::HeadlessGpuFixture gpu({ .InjectFault = fault });
				ENGINE_REQUIRE_GPU(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				FakePlatformViewports platform;
				Result<Scope<ImGuiRenderer>> renderer = ImGuiRenderer::Create(device, gpu.GetPipelines(), { .FramesInFlight = 1 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				uint32_t creations = 0;
				ImGuiPlatformWindowsSpecification specification;
				specification.FramesInFlight = 1;
				specification.CreateTarget = [&creations](GraphicsDevice& owner, const ImGuiPlatformWindowTargetRequest& request) -> Result<Scope<IImGuiPlatformWindowTarget>>
				{
					++creations;
					Scope<OffscreenWindowTarget> target = CreateScope<OffscreenWindowTarget>(owner, request.Size);
					ENGINE_TRY(target->Create());
					return Scope<IImGuiPlatformWindowTarget>(std::move(target));
				};
				Result<Scope<ImGuiPlatformWindows>> windows = ImGuiPlatformWindows::Create(device, **renderer, specification);
				REQUIRE_MESSAGE(windows.has_value(), windows.error().ToString());
				FramePacer pacer(device, 1);
				// The first garbage collection arms the fault, so the window's first target is the first render target after it.
				device.RunGarbageCollection();

				const auto frame = [&pacer, &renderer, &windows, &device](float width) -> Status
				{
					pacer.BeginFrame();
					(*renderer)->BeginFrame(pacer.GetFrameSlot());
					ImGui::NewFrame();
					ImGui::SetNextWindowPos(ImVec2(400.0f, 32.0f));
					ImGui::SetNextWindowSize(ImVec2(width, 96.0f));
					ImGui::Begin("Undocked");
					ImGui::TextUnformatted("A platform window");
					ImGui::End();
					ImGui::Render();
					ImGui::UpdatePlatformWindows();
					const Status rendered = (*windows)->Render(pacer.GetFrameSlot());
					pacer.EndFrame(device.GetLastSubmissionID());
					device.RunGarbageCollection();
					return rendered;
				};
				// Dear ImGui settles a new window over its first frames; no frame fails, whatever the target's creation met.
				for (int settling = 0; settling < 3; ++settling)
				{
					const Status rendered = frame(160.0f);
					REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
				}
				CHECK(creations == 2); // the creation that ran out of memory and its retry after the trim
				CHECK((*windows)->GetWindowCount() == (persistent ? 0u : 1u));
				CHECK_FALSE(device.IsLost());
				if (persistent)
				{
					// The same size is not tried again; a new size is a new request, which fails alone again.
					REQUIRE(frame(160.0f).has_value());
					CHECK(creations == 2);
					REQUIRE(frame(200.0f).has_value());
					CHECK(creations == 4);
					CHECK((*windows)->GetWindowCount() == 0u);
				}
				windows->reset();
				(*renderer)->DestroyTextures();
			}
		}

		// Runs only in a windowed child process (Support/WindowedChild.h): a windowed application with platform windows on
		// (ApplicationSpecification::ImGuiPlatformWindows) draws a window beyond its main window, so Dear ImGui opens a platform
		// window for it on the real GLFW backend, loses its device in the update of frame 3, and presents both windows again
		// after the recovery: each platform window's swapchain and surface released before the old VkInstance is destroyed and
		// created on the new one. ExpectNoGpuErrors fails the run on any GPU message of the device's whole life,
		// synchronization validation's included.
		TEST_CASE("ImGuiPlatformWindows: a windowed application with a platform window loses its device and presents both windows again"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			class UndockingApplication final : public Application
			{
			public:
				explicit UndockingApplication(ApplicationSpecification specification)
					: Application(std::move(specification))
				{
				}
			public:
				size_t MostWindows = 0;
				size_t WindowsAtEnd = 0;
				uint32_t RecoveryCount = 0;
				bool Supported = false;
			protected:
				void OnUpdate(const FrameTime& frame) override
				{
					if (frame.FrameIndex == 3)
						GetContext().GetGraphicsDevice()->GetDiagnostics().SetDeviceLost(true);
				}

				void OnImGuiRender() override
				{
					const ImGuiViewport* main = ImGui::GetMainViewport();
					ImGui::SetNextWindowPos(ImVec2(main->Pos.x + main->Size.x + 64.0f, main->Pos.y));
					ImGui::SetNextWindowSize(ImVec2(200.0f, 120.0f));
					ImGui::Begin("Undocked");
					ImGui::TextUnformatted("A platform window");
					ImGui::End();
					if (ImGuiPlatformWindows* windows = GetImGuiLayer()->GetPlatformWindows())
						MostWindows = std::max(MostWindows, windows->GetWindowCount());
				}

				void OnShutdown() override
				{
					Supported = GetImGuiLayer()->GetPlatformWindowSupport().has_value();
					if (ImGuiPlatformWindows* windows = GetImGuiLayer()->GetPlatformWindows())
						WindowsAtEnd = windows->GetWindowCount();
					RecoveryCount = GetContext().GetGraphicsDevice()->GetRecoveryCount();
				}
			};

			ApplicationSpecification specification;
			specification.Name = "ImGuiPlatformWindowsTests";
			specification.Window = WindowMode::Windowed;
			specification.WindowSettings = { .Title = "Platform windows test", .Width = 320, .Height = 240 };
			specification.Clock = ClockKind::Manual;
			specification.MaxFrames = 8;
			specification.WorkerCount = 0;
			specification.EnableImGui = true;
			specification.ImGuiPlatformWindows = true;
			// No test touches the machine's audio device (§15.1 T1).
			specification.Audio = AudioEngineSpecification{};
			specification.Renderer = RendererMode::Vulkan;
			specification.Graphics.Validation = true;
			specification.Graphics.SynchronizationValidation = true;
			specification.Graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
			specification.ExpectNoGpuErrors = true;
			UndockingApplication application(std::move(specification));
			CHECK(application.Run() == ExitCode::Success);
			CHECK(application.Supported);
			CHECK(application.MostWindows == 1);
			CHECK(application.WindowsAtEnd == 1);
			CHECK(application.RecoveryCount == 1);
		}

		TEST_CASE("ImGuiPlatformWindows: platform windows recover their swapchains after an injected loss and present without a GPU message"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M24 acceptance (E-VIEWPORTS): "Platform windows stay clean under synchronization validation and survive an injected
			// device loss", for the presenting half that the shared loss test's headless device cannot reach.
			std::vector<std::string> arguments = {
				"--windowed-child=ImGuiPlatformWindows: a windowed application with a platform window loses its device and presents both windows again",
			};
			const std::vector<std::string> gpuArguments = Test::GetGpuTestsChildArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const Result<ProcessResult> child = Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), std::chrono::seconds(60));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stdout: ", child->StandardOutput);
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode == 0);
		}
	}

}
