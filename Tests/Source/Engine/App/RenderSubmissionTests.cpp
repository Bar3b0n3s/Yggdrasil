#include "TestsPCH.h"

#include "Engine/App/Application.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/Platform/Process.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"
#include "Support/WindowedChild.h"

#include <imgui.h>

#include <cstring>
#include <vector>

namespace Engine {

	namespace {

		ApplicationSpecification SubmissionSpecification(bool windowed)
		{
			ApplicationSpecification specification;
			specification.Name = "RenderSubmissionTests";
			specification.Window = windowed ? WindowMode::Windowed : WindowMode::Headless;
			specification.WindowSettings = { .Title = "Render submission test", .Width = 320, .Height = 240 };
			specification.Clock = ClockKind::Manual;
			specification.MaxFrames = 8;
			specification.WorkerCount = 0;
			specification.ThrottleHeadless = false;
			specification.EnableImGui = true;
			specification.Audio = AudioEngineSpecification{};
			specification.Graphics.Validation = true;
			specification.Graphics.SynchronizationValidation = true;
			specification.Graphics.MaxApiVersion = Test::GetTestOptions().VulkanApi;
			specification.ExpectNoGpuErrors = true;
			return specification;
		}

		class SubmissionApplication final : public Application
		{
		public:
			explicit SubmissionApplication(bool windowed = false)
				: Application(SubmissionSpecification(windowed)), m_Windowed(windowed)
			{
			}
			bool SkipMinimizedFrames = false;
			std::vector<uint64_t> Rendered;
			std::vector<uint64_t> Submitted;
			uint64_t Steps = 0;
			uint64_t Updates = 0;
			uint64_t UiFrames = 0;
			bool ForcedWhileMinimized = false;
		protected:
			Status OnInitialize() override
			{
				GraphicsDevice& device = *GetContext().GetGraphicsDevice();
				ENGINE_TRY_ASSIGN(m_Source, device.CreateBuffer(nvrhi::BufferDesc().setByteSize(sizeof(uint32_t)).setInitialState(nvrhi::ResourceStates::CopyDest).setKeepInitialState(true).setDebugName("SubmissionSource")));
				ENGINE_TRY_ASSIGN(m_Copy, device.CreateCommandList());
				for (uint32_t index = 0; index < 8; ++index)
				{
					ENGINE_TRY_ASSIGN(auto buffer, device.CreateBuffer(nvrhi::BufferDesc().setByteSize(sizeof(uint32_t)).setCpuAccess(nvrhi::CpuAccessMode::Read).setDebugName("SubmissionReadback")));
					m_Readbacks.push_back(std::move(buffer));
				}
				return {};
			}
			void OnFixedStep(const SimStep&) override { ++Steps; }
			void OnUpdate(const FrameTime& frame) override
			{
				++Updates;
				Window& window = *GetContext().GetWindow();
				if (m_Windowed && frame.FrameIndex == 3)
				{
					window.Minimize();
					REQUIRE(Test::WaitUntilMinimized(window, true));
				}
				if (m_Windowed && !SkipMinimizedFrames && frame.FrameIndex == 4)
					REQUIRE(RequestOffscreenUiFrame().has_value());
				if (m_Windowed && frame.FrameIndex == 6)
				{
					window.Restore();
					REQUIRE(Test::WaitUntilMinimized(window, false));
				}
				if (SkipMinimizedFrames && frame.FrameIndex == 4)
				{
					// An independent capture-like list has no application notification or rendered-frame identity.
					m_Copy->open();
					m_Copy->copyBuffer(m_Readbacks[4], 0, m_Source, 0, sizeof(uint32_t));
					m_Copy->close();
					static_cast<void>(GetContext().GetGraphicsDevice()->ExecuteCommandList(*m_Copy));
				}
			}
			void OnRender(RenderContext& context) override
			{
				Rendered.push_back(context.FrameIndex);
				m_CurrentFrame = context.FrameIndex;
				m_BeforeSubmission = context.Device->GetLastSubmissionID();
				m_UiBefore = UiFrames;
				const uint32_t value = static_cast<uint32_t>(context.FrameIndex + 1);
				context.CommandList->writeBuffer(m_Source, &value, sizeof(value));
				if (m_Windowed && context.FrameIndex == 4)
				{
					ForcedWhileMinimized = GetContext().GetWindow()->IsMinimized();
					CHECK(context.Width == 320);
					CHECK(context.Height == 240);
				}
			}
			void OnImGuiRender() override
			{
				++UiFrames;
				CHECK(ImGui::GetIO().DisplaySize.x > 0.0f);
				CHECK(ImGui::GetIO().DisplaySize.y > 0.0f);
				ImGui::Begin("Fresh frame");
				ImGui::TextUnformatted(std::to_string(m_CurrentFrame).c_str());
				ImGui::End();
			}
			void OnRenderSubmitted(uint64_t frameIndex, uint64_t submissionId) override
			{
				GraphicsDevice& device = *GetContext().GetGraphicsDevice();
				CHECK(frameIndex == m_CurrentFrame);
				CHECK(UiFrames == m_UiBefore + 1);
				CHECK(ImGui::GetDrawData() != nullptr);
				CHECK(submissionId > m_BeforeSubmission);
				CHECK(device.GetLastSubmissionID() == submissionId);
				Submitted.push_back(frameIndex);
				m_Copy->open();
				m_Copy->copyBuffer(m_Readbacks[frameIndex], 0, m_Source, 0, sizeof(uint32_t));
				m_Copy->close();
				CHECK(device.ExecuteCommandList(*m_Copy) > submissionId);
			}
			void OnShutdown() override
			{
				GraphicsDevice& device = *GetContext().GetGraphicsDevice();
				device.WaitForIdle();
				for (uint64_t frame : Submitted)
				{
					const void* bytes = device.GetNvrhiDevice()->mapBuffer(m_Readbacks[frame], nvrhi::CpuAccessMode::Read);
					REQUIRE(bytes != nullptr);
					uint32_t value = 0;
					std::memcpy(&value, bytes, sizeof(value));
					device.GetNvrhiDevice()->unmapBuffer(m_Readbacks[frame]);
					CHECK(value == frame + 1);
				}
				m_Readbacks.clear();
				m_Source = nullptr;
				m_Copy = nullptr;
			}
		private:
			bool m_Windowed = false;
			uint64_t m_CurrentFrame = 0;
			uint64_t m_BeforeSubmission = 0;
			uint64_t m_UiBefore = 0;
			nvrhi::BufferHandle m_Source{};
			std::vector<nvrhi::BufferHandle> m_Readbacks;
			nvrhi::CommandListHandle m_Copy{};
		};

	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("Application: submitted hook follows the scene and UI list and covers queued pick copies")
		{
			if (!Test::ProbeGpuForProcess())
				return;
			SubmissionApplication application;
			CHECK(application.Run() == ExitCode::Success);
			CHECK(application.Submitted == application.Rendered);
			CHECK(application.Submitted.size() == 8);
			CHECK(application.UiFrames == 8);
		}

		TEST_CASE("Application: skipped frames do not publish submissions or consume another view's picks")
		{
			std::vector<std::string> arguments{ "--windowed-child=Application: skipped native frames preserve submitted identities" };
			const auto gpuArguments = Test::GetGpuTestsChildArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const auto child = Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), std::chrono::seconds(60));
			REQUIRE(child.has_value());
			INFO(child->StandardOutput, child->StandardError);
			CHECK(child->ExitCode == 0);
		}

		TEST_CASE("Application: a requested minimized UI frame renders offscreen without swapchain acquisition")
		{
			std::vector<std::string> arguments{ "--windowed-child=Application: forced UI renders while the native window stays minimized" };
			const auto gpuArguments = Test::GetGpuTestsChildArguments();
			arguments.insert(arguments.end(), gpuArguments.begin(), gpuArguments.end());
			const auto child = Process::Run(Test::MakeTestsChildSpecification(std::move(arguments)), std::chrono::seconds(60));
			REQUIRE(child.has_value());
			INFO(child->StandardOutput, child->StandardError);
			CHECK(child->ExitCode == 0);
		}
	}

	TEST_CASE("Application: skipped native frames preserve submitted identities"
		* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
	{
		if (!Test::ProbeGpuForProcess())
			return;
		SubmissionApplication application(true);
		application.SkipMinimizedFrames = true;
		CHECK(application.Run() == ExitCode::Success);
		CHECK(std::ranges::find(application.Submitted, 2u) != application.Submitted.end());
		CHECK(std::ranges::find(application.Submitted, 7u) != application.Submitted.end());
		for (uint64_t frame : { 3u, 4u, 5u })
			CHECK(std::ranges::find(application.Submitted, frame) == application.Submitted.end());
		CHECK(application.Submitted == application.Rendered);
		CHECK(application.Steps == 8);
	}

	TEST_CASE("Application: forced UI renders while the native window stays minimized"
		* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
	{
		if (!Test::ProbeGpuForProcess())
			return;
		SubmissionApplication application(true);
		CHECK(application.Run() == ExitCode::Success);
		CHECK(application.ForcedWhileMinimized);
		CHECK(std::ranges::find(application.Submitted, 4u) != application.Submitted.end());
		CHECK(std::ranges::find(application.Submitted, 3u) == application.Submitted.end());
		CHECK(std::ranges::find(application.Submitted, 5u) == application.Submitted.end());
		CHECK(application.Submitted == application.Rendered);
		CHECK(application.UiFrames == application.Rendered.size());
		CHECK(application.Updates == 8);
		CHECK(application.Steps == 8);
	}

}
