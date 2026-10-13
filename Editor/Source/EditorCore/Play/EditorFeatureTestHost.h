#pragma once

#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Testing/FeatureTestRunner.h"

#include <map>

namespace Engine {

	class EditorMethodContext;
	class EditorContext;

	// One automation request owns this adapter. Scratch preflight never publishes a session; Acquire transitions
	// to exclusive runner ownership. The runner must cancel/unpublish before destroying this adapter. The script API
	// coverage of a run is the delta from CaptureCoverageBaseline, which the owner calls immediately before
	// FeatureTestRunner::Begin (Docs/Decisions/0022-m14-contract.md decision 4), so the suite collection of Begin counts.
	class EditorFeatureTestHost final : public IFeatureTestHost
	{
	public:
		explicit EditorFeatureTestHost(EditorMethodContext& context);
		~EditorFeatureTestHost() override;
		[[nodiscard]] Status Preflight();
		// The ScriptApiRegistry snapshot GetCoverage subtracts (MakeScriptApiCoverageCounters); call it immediately before
		// FeatureTestRunner::Begin. Errors: those of ScriptApiRegistry::GetCoverage.
		[[nodiscard]] Status CaptureCoverageBaseline();
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
		// The editor's EditorAssetManager, which every suite and replay session loads through (gate 9's load observer).
		[[nodiscard]] AssetManager* GetAssetManager() const override;
		[[nodiscard]] Result<Json> Evaluate(PlaySession& session, const ReplayBytecodeExpectation& expectation) override;
	private:
		// The test-run cursor baseline (Normal) for the next suite or replay session of an acquired run.
		void ResetTestCursor();
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
