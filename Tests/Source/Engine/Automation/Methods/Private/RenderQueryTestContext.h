#pragma once

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/SceneMethods.h"
#include "Engine/Automation/Methods/SessionMethods.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Methods/StatsMethods.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

namespace Engine::Test {

	// A request-local host for testing the shared handlers through the real registry. References point to the fixture's
	// scene/assets/settings and outlive this context. The statistics are injected observations, never renderer stand-ins.
	class RenderQueryTestContext final : public AutomationMethodContext
	{
	public:
		RenderQueryTestContext(Scene& scene, AssetManager& assets, const ProjectSettings& settings, MethodRequest request)
			: AutomationMethodContext(TypeKeyOf<RenderQueryTestContext>(), std::move(request)), m_Scene(&scene), m_Assets(&assets), m_Settings(&settings)
		{
		}
		StatsGetResult Statistics{};
		bool HasStatistics = true;
		bool HasAssets = true;
		bool HasSettings = true;
		bool Runtime = false;
		mutable uint32_t StatisticsReads = 0;
		std::vector<UUID> Selection{};
		[[nodiscard]] Scope<MethodContext> CreateNested(MethodRequest request) const override
		{
			return CreateScope<RenderQueryTestContext>(*m_Scene, *m_Assets, *m_Settings, std::move(request));
		}
		[[nodiscard]] Result<Scene*> ResolveTargetScene(SceneTarget target, bool given, bool) const override
		{
			if (given && ((Runtime && target == SceneTarget::Edit) || (!Runtime && target == SceneTarget::Play)))
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidState, "/target", "target scene is unavailable"));
			return m_Scene;
		}
		[[nodiscard]] Result<Entity> ResolveEntity(Scene& scene, std::string_view reference, std::string_view pointer) const override
		{
			return Utils::ResolveEntityReference(scene, reference, pointer);
		}
		[[nodiscard]] EntitySummary MakeEntitySummary(ConstEntity entity) const override { return Utils::SummarizeEntity(entity); }
		[[nodiscard]] PlaySession* GetPlaySession() const override { return nullptr; }
		[[nodiscard]] Status StartPlay(const PlayStartOptions&) override { return MakeError(ErrorCode::Unsupported, "no play service"); }
		[[nodiscard]] Status StopPlay() override { return MakeError(ErrorCode::Unsupported, "no play service"); }
		[[nodiscard]] std::string GetClientName(ClientId) const override { return {}; }
		[[nodiscard]] EventLog& GetEventLog() const override { return m_Events; }
		[[nodiscard]] Result<Image> CaptureView(const RenderSnapshot&, const ViewportScreenshotRequest&) override { return MakeError(ErrorCode::Unsupported, "no renderer"); }
		[[nodiscard]] std::optional<ExplicitRenderCamera> GetSceneViewCamera() const override { return std::nullopt; }
		[[nodiscard]] Result<std::string> WriteOutputFile(std::string_view, std::span<const std::byte>) override { return MakeError(ErrorCode::Unsupported, "no output"); }
		[[nodiscard]] SessionHostDescription DescribeSession() const override { return { .Renderer = "none" }; }
		[[nodiscard]] Result<SessionShutdownResult> Shutdown(const SessionShutdownParams&) override { return MakeError(ErrorCode::Unsupported, "no shutdown"); }
		[[nodiscard]] SceneSummary MakeSceneSummary(const Scene&) const override { return {}; }
		[[nodiscard]] AssetManager* GetAssets() const override { return HasAssets ? m_Assets : nullptr; }
		[[nodiscard]] std::chrono::steady_clock::time_point GetWallClockTime() const override { return {}; }
		[[nodiscard]] AudioEngine* GetAudioEngine() const override { return nullptr; }
		[[nodiscard]] const ProjectSettings* GetProjectSettings() const override { return HasSettings ? m_Settings : nullptr; }
		[[nodiscard]] Result<StatsGetResult> GetHostStatistics() const override
		{
			++StatisticsReads;
			if (!HasStatistics)
				return AutomationMethodContext::GetHostStatistics();
			return Statistics;
		}
		[[nodiscard]] std::vector<UUID> GetSelectedEntities() const override { return Selection; }
	private:
		Scene* m_Scene = nullptr;
		AssetManager* m_Assets = nullptr;
		const ProjectSettings* m_Settings = nullptr;
		mutable EventLog m_Events{};
	};

}
