#pragma once

#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Testing/FeatureTestRunner.h"

#include <map>

namespace Engine {

	class EditorMethodContext;
	class EditorContext;

	// One automation request owns this adapter. Scratch preflight never publishes a session; Acquire transitions
	// to exclusive runner ownership. The runner must cancel/unpublish before destroying this adapter.
	class EditorFeatureTestHost final : public IFeatureTestHost
	{
	public:
		explicit EditorFeatureTestHost(EditorMethodContext& context);
		~EditorFeatureTestHost() override;
		[[nodiscard]] Status Preflight();
		[[nodiscard]] Status Acquire();
		[[nodiscard]] const ProjectSettings& GetProjectSettings() const override;
		[[nodiscard]] Result<AssetHandle> ResolveTestScript(std::string_view path) override;
		[[nodiscard]] Result<std::vector<std::string>> ExpandReplayPaths(std::span<const std::string> globs) override;
		[[nodiscard]] Result<AssetRef<ReplayData>> LoadReplay(std::string_view path) override;
		[[nodiscard]] Result<ReplayHeader> DescribeReplayHeader(const PlaySession& session) const override;
		[[nodiscard]] Result<Scope<PlaySession>> CreateSuiteSession(const TestSuiteSettings& suite,
			TestSuiteSettings::Mode mode, IPlaySessionTestHook& hook, IScriptTestHost& testHost) override;
		[[nodiscard]] Result<Scope<PlaySession>> CreateReplaySession(const ReplayHeader& header, TestSuiteSettings::Mode mode) override;
		void SetActiveTestSession(PlaySession* session) override;
		[[nodiscard]] AudioEngine* GetAudioEngine() const override;
		[[nodiscard]] Status CaptureScreenshot(PlaySession& session, std::string_view name) override;
		[[nodiscard]] Status ReloadScript(PlaySession& session, AssetHandle script) override;
		[[nodiscard]] Result<TestCoverageReport> GetCoverage() const override;
		[[nodiscard]] Result<Json> Evaluate(PlaySession& session, const ReplayBytecodeExpectation& expectation) override;
	private:
		EditorMethodContext& m_Context; // request context outlives its pending operation and this adapter
		EditorContext& m_Editor;        // owns controller and assets; outlives server/requests
		ProjectSettings m_Project{};
		ScriptApiRegistry& m_Api; // engine-owned frozen registry, outlives every test VM
		ScriptApiCoverage m_CoverageStart{};
		std::map<uint64_t, ReplayHeader> m_Headers{};
		bool m_Acquired = false;
	};

}
