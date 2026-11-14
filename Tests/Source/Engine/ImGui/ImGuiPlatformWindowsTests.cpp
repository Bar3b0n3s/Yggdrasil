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
#include "Engine/Graphics/RenderContext.h"
#include "Engine/ImGui/ImGuiLayer.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Engine/Platform/Process.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"
#include "Support/TrimCounter.h"
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
// decision 32 has the owners that present prove it; the out-of-memory rule of a window's target through the injected
// oom-rendertarget faults, and of the default swapchain target through the oom-swapchain faults in a windowed child; and the
// rules M24's review added: a creation that fails otherwise waits for the window's next size, and Render's error codes tell a
// target's failure (Gpu) from a recording's (OutOfMemory).

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
				// Dear ImGui sets every platform window's title, without a check that the platform side can.
				platform.Platform_SetWindowTitle = [](ImGuiViewport* /*viewport*/, const char* /*title*/) {};
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

		// The test's frame pacer as an application's frame owner keeps it: registered with the device's recreation registry, so
		// a recovery releases its event queries and creates it again on the recovered device, as Application's frame does.
		class RegisteredPacer
		{
		public:
			explicit RegisteredPacer(GraphicsDevice& device)
				: m_Device(&device)
			{
				m_Pacer.emplace(device, 1);
				GpuRecreationHooks hooks;
				hooks.Name = "ImGuiPlatformWindowsTests frame";
				hooks.Release = [this]()
				{
					m_Pacer.reset();
				};
				hooks.Recreate = [this]() -> Status
				{
					m_Pacer.emplace(*m_Device, 1);
					return {};
				};
				m_Registration = device.GetRecreation().Register(std::move(hooks));
			}

			RegisteredPacer(const RegisteredPacer&) = delete;
			RegisteredPacer& operator=(const RegisteredPacer&) = delete;

			// The pacer; between a recovery's Release and its Recreate there is none.
			[[nodiscard]] FramePacer& Get() { return *m_Pacer; }
		private:
			GraphicsDevice* m_Device = nullptr; // documented back-reference: the fixture's device outlives the pacer
			std::optional<FramePacer> m_Pacer{};
			GpuRecreationRegistration m_Registration{}; // declared last, so it is dropped first
		};

	}

	TEST_SUITE("ImGui")
	{
		TEST_CASE("ImGuiPlatformWindows: a device loss recreates every platform window's target and draws the same image"
			* doctest::test_suite(Test::GpuSuite))
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
			RegisteredPacer frames(device);

			// One frame: a window placed beyond the main viewport's 256 x 128 gets a platform window of its own, drawn into its
			// target, which is read back.
			const auto frame = [&frames, &renderer, &windows, &device, &targets]() -> Result<std::vector<std::byte>>
			{
				FramePacer& pacer = frames.Get();
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
			* doctest::test_suite(Test::GpuSuite))
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

		// M24's review: a target creation that fails otherwise than out of memory (a surface the device cannot present to) leaves
		// its window without a target until its next size change too, so it is not tried again every frame, each failed swapchain
		// waiting for the device to go idle; Render reports it once per size.
		TEST_CASE("ImGuiPlatformWindows: a target creation that fails otherwise is tried once a size, and its error is reported once"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			FakePlatformViewports platform;
			Result<Scope<ImGuiRenderer>> renderer = ImGuiRenderer::Create(device, gpu.GetPipelines(), { .FramesInFlight = 1 });
			REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
			uint32_t creations = 0;
			ImGuiPlatformWindowsSpecification specification;
			specification.FramesInFlight = 1;
			specification.CreateTarget = [&creations](GraphicsDevice& /*owner*/, const ImGuiPlatformWindowTargetRequest& /*request*/) -> Result<Scope<IImGuiPlatformWindowTarget>>
			{
				++creations;
				return MakeError(ErrorCode::Unsupported, "the graphics queue family cannot present to the window's surface");
			};
			Result<Scope<ImGuiPlatformWindows>> windows = ImGuiPlatformWindows::Create(device, **renderer, specification);
			REQUIRE_MESSAGE(windows.has_value(), windows.error().ToString());
			FramePacer pacer(device, 1);

			// The frame's errors, one entry per frame that failed.
			std::vector<ErrorCode> failures;
			const auto frame = [&pacer, &renderer, &windows, &device, &failures](float width)
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
				if (!rendered.has_value())
					failures.push_back(rendered.error().GetCode());
			};
			for (int settling = 0; settling < 4; ++settling)
				frame(160.0f);
			// One creation and one error at the window's size, then none until it changes.
			CHECK(creations == 1);
			CHECK(failures == std::vector<ErrorCode>{ ErrorCode::Unsupported });
			CHECK((*windows)->GetWindowCount() == 0u);
			frame(200.0f);
			frame(200.0f);
			CHECK(creations == 2);
			CHECK(failures == std::vector<ErrorCode>{ ErrorCode::Unsupported, ErrorCode::Unsupported });
			CHECK_FALSE(device.IsLost());
			windows->reset();
			(*renderer)->DestroyTextures();
		}

		// M24's review: the application reports Render's errors by their codes (Application.cpp): a platform window's target
		// failure, as a swapchain's, is Gpu, which ends the process as the main window's swapchain failure does (a GPU error),
		// while a recording's failure to create what it records with (here ImGui's pipeline for the window's target format,
		// under the injected oom-pipeline-persistent fault) is OutOfMemory, as the main UI's is.
		TEST_CASE("ImGuiPlatformWindows: a target's failure is a Gpu error and a recording's creation failure is out of memory"
			* doctest::test_suite(Test::GpuSuite))
		{
			// A target whose acquire fails as a swapchain recreation that is not out of memory does.
			class FailingTarget final : public IImGuiPlatformWindowTarget
			{
			public:
				Result<nvrhi::IFramebuffer*> Acquire(uint32_t /*frameSlot*/) override
				{
					return MakeError(ErrorCode::Gpu, "the platform window's swapchain cannot be created again: VK_ERROR_INITIALIZATION_FAILED");
				}
				void QueueFrameSemaphores() override {}
				void Present() override {}
				void Resize(glm::uvec2 /*size*/) override {}
				void ReleaseGpuObjects() override {}
				Status RecreateGpuObjects() override { return {}; }
			};

			for (const bool recording : { false, true })
			{
				CAPTURE(recording);
				Test::HeadlessGpuFixture gpu({ .InjectFault = recording ? GpuFault::OomPipelinePersistent : GpuFault::None });
				ENGINE_REQUIRE_GPU(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				FakePlatformViewports platform;
				Result<Scope<ImGuiRenderer>> renderer = ImGuiRenderer::Create(device, gpu.GetPipelines(), { .FramesInFlight = 1 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				ImGuiPlatformWindowsSpecification specification;
				specification.FramesInFlight = 1;
				specification.CreateTarget = [recording](GraphicsDevice& owner, const ImGuiPlatformWindowTargetRequest& request) -> Result<Scope<IImGuiPlatformWindowTarget>>
				{
					if (!recording)
						return Scope<IImGuiPlatformWindowTarget>(CreateScope<FailingTarget>());
					Scope<OffscreenWindowTarget> target = CreateScope<OffscreenWindowTarget>(owner, request.Size);
					ENGINE_TRY(target->Create());
					return Scope<IImGuiPlatformWindowTarget>(std::move(target));
				};
				Result<Scope<ImGuiPlatformWindows>> windows = ImGuiPlatformWindows::Create(device, **renderer, specification);
				REQUIRE_MESSAGE(windows.has_value(), windows.error().ToString());
				FramePacer pacer(device, 1);
				// The first garbage collection arms the fault, so ImGui's pipeline for the window's format is created after it.
				device.RunGarbageCollection();

				std::vector<ErrorCode> failures;
				for (int frame = 0; frame < 4; ++frame)
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
					if (!rendered.has_value())
						failures.push_back(rendered.error().GetCode());
				}
				REQUIRE_FALSE(failures.empty());
				const ErrorCode expected = recording ? ErrorCode::OutOfMemory : ErrorCode::Gpu;
				CHECK(std::ranges::all_of(failures, [expected](ErrorCode code)
				{
					return code == expected;
				}));
				CHECK_FALSE(device.IsLost());
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

		// Runs only in a windowed child process (Support/WindowedChild.h), M24's review (decision 19, M18's main-window pattern,
		// ADR 0029 decisions 42 and 44): the default target's out-of-memory paths, a swapchain on the platform window's GLFW
		// window. Under oom-swapchain and oom-swapchain-persistent, armed by the first garbage collection, the window is undocked
		// either after the arming, so its swapchain's creation runs out of memory (Swapchain::CreateForNativeWindow's trim and
		// retry), or from the first frame, so its first resize's recreation does (the Acquire's OutOfMemory that RenderWindow
		// answers). Once, the retry after one trim succeeds and the window keeps its target; persistently, the window has no
		// target from then on, each new size is tried once more (one trim each) and fails alone, while the main window presents
		// every frame. Every run ends with exit code 0 and no GPU message (ExpectNoGpuErrors).
		TEST_CASE("ImGuiPlatformWindows: a windowed platform window whose swapchain runs out of memory at its creation or resize draws nothing until its next size"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			if (!Test::ProbeGpuForProcess())
				return;
			class SwapchainOutOfMemoryApplication final : public Application
			{
			public:
				SwapchainOutOfMemoryApplication(ApplicationSpecification specification, uint64_t undockFrame)
					: Application(std::move(specification)), m_UndockFrame(undockFrame)
				{
				}
			public:
				std::vector<size_t> Windows; // GetWindowCount at the update of each frame
				uint32_t Trims = 0;
				uint32_t MainFramesAfterUndock = 0;
				bool Supported = false;
			protected:
				Status OnInitialize() override
				{
					m_TrimCounter = CreateScope<Test::TrimCounter>(GetContext().GetGraphicsDevice()->GetRecreation());
					return {};
				}

				void OnUpdate(const FrameTime& frame) override
				{
					m_Frame = frame.FrameIndex;
					const ImGuiPlatformWindows* windows = GetImGuiLayer()->GetPlatformWindows();
					Windows.push_back(windows != nullptr ? windows->GetWindowCount() : 0);
				}

				void OnImGuiRender() override
				{
					if (m_Frame < m_UndockFrame)
						return;
					// Undocked beyond the main window, at a width that changes in frames 7 and 11.
					const float width = m_Frame >= 11 ? 260.0f : m_Frame >= 7 ? 230.0f
																			  : 200.0f;
					const ImGuiViewport* main = ImGui::GetMainViewport();
					ImGui::SetNextWindowPos(ImVec2(main->Pos.x + main->Size.x + 64.0f, main->Pos.y));
					ImGui::SetNextWindowSize(ImVec2(width, 120.0f));
					ImGui::Begin("Undocked");
					ImGui::TextUnformatted("A platform window");
					ImGui::End();
				}

				void OnRender(RenderContext& context) override
				{
					if (context.FrameIndex >= m_UndockFrame)
						++MainFramesAfterUndock;
				}

				void OnShutdown() override
				{
					Supported = GetImGuiLayer()->GetPlatformWindowSupport().has_value();
					if (m_TrimCounter)
						Trims = m_TrimCounter->GetTrims();
					m_TrimCounter.reset();
				}
			private:
				uint64_t m_UndockFrame = 0;
				uint64_t m_Frame = 0;
				Scope<Test::TrimCounter> m_TrimCounter;
			};

			for (const GpuFault fault : { GpuFault::OomSwapchain, GpuFault::OomSwapchainPersistent })
			{
				for (const uint64_t undockFrame : { uint64_t{ 3 }, uint64_t{ 0 } })
				{
					const bool persistent = fault == GpuFault::OomSwapchainPersistent;
					const bool atCreation = undockFrame != 0;
					CAPTURE(GpuFaultToString(fault));
					CAPTURE(atCreation);
					ApplicationSpecification specification;
					specification.Name = "ImGuiPlatformWindowsTests";
					specification.Window = WindowMode::Windowed;
					specification.WindowSettings = { .Title = "Platform window out of memory", .Width = 320, .Height = 240 };
					specification.Clock = ClockKind::Manual;
					specification.MaxFrames = 16;
					specification.WorkerCount = 0;
					specification.EnableImGui = true;
					specification.ImGuiPlatformWindows = true;
					// No test touches the machine's audio device (§15.1 T1).
					specification.Audio = AudioEngineSpecification{};
					specification.Renderer = RendererMode::Vulkan;
					specification.Graphics.Validation = true;
					specification.Graphics.SynchronizationValidation = true;
					specification.Graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
					specification.Graphics.InjectFault = fault;
					specification.ExpectNoGpuErrors = true;
					SwapchainOutOfMemoryApplication application(std::move(specification), undockFrame);
					CHECK(application.Run() == ExitCode::Success);
					CHECK(application.Supported);
					REQUIRE(application.Windows.size() == 16);
					// The main window presents every frame from the undocking on, whatever the platform window's swapchain met.
					CHECK(application.MainFramesAfterUndock == 16 - undockFrame);
					if (!atCreation)
						REQUIRE(application.Windows[6] == 1); // drawn before the arming, so its first resize meets the fault
					if (!persistent)
					{
						// The first swapchain creation after the arming, the window's or its resize's, fails once and is retried
						// after the trim; the window keeps a target at every size.
						CHECK(application.Trims == 1);
						CHECK(application.Windows[6] == 1);
						CHECK(application.Windows[10] == 1);
						CHECK(application.Windows[15] == 1);
					}
					else
					{
						// Each size the window takes after the arming is tried once (one trim each) and fails alone: at the
						// creation, its 200 and 230 and 260 widths, or at the resize, its 230 and 260 widths.
						CHECK(application.Trims == (atCreation ? 3u : 2u));
						if (atCreation)
							CHECK(application.Windows[6] == 0);
						CHECK(application.Windows[10] == 0);
						CHECK(application.Windows[15] == 0);
					}
				}
			}
		}

		TEST_CASE("ImGuiPlatformWindows: a platform window's swapchain out of memory at its creation or resize is retried after a trim, or the window waits for its next size"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M24's review (decision 19): the default target's half of "an out-of-memory target is trimmed and retried once, and
			// otherwise only its window draws nothing until its next resize", which the headless case's offscreen targets
			// cannot reach.
			std::vector<std::string> arguments = {
				"--windowed-child=ImGuiPlatformWindows: a windowed platform window whose swapchain runs out of memory at its creation or resize draws nothing until its next size",
			};
			const std::vector<std::string> gpuArguments = Test::GetGpuTestsChildArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const Result<ProcessResult> child = Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), std::chrono::seconds(120));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stdout: ", child->StandardOutput);
			INFO("child stderr: ", child->StandardError);
			CHECK(child->ExitCode == 0);
		}

		TEST_CASE("ImGuiPlatformWindows: platform windows recover their swapchains after an injected loss and present without a GPU message"
			* doctest::test_suite(Test::GpuSuite))
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
