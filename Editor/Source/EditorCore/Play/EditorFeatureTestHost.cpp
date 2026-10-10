#include "EditorPCH.h"
#include "EditorCore/Play/EditorFeatureTestHost.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/AssetMethodSupport.h"
#include "EditorCore/Automation/ScriptMethods.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Export/Private/ExportPaths.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/App/EngineContext.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Automation/Methods/ScreenshotMethods.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/ScriptEngine.h"

#include <algorithm>
#include <format>

namespace Engine {

	EditorFeatureTestHost::EditorFeatureTestHost(EditorMethodContext& context)
		: m_Context(context), m_Editor(context.GetEditor()), m_Api(m_Editor.GetEngine().GetScriptApiRegistry())
	{
	}

	EditorFeatureTestHost::~EditorFeatureTestHost()
	{
		if (m_Acquired)
			m_Editor.GetPlay().ReleaseTestRun(*this);
	}

	Status EditorFeatureTestHost::Preflight()
	{
		ENGINE_TRY(m_Editor.GetPlay().CheckTestAdmission(m_Context.GetRequest().Client));
		m_Project = m_Editor.GetProject().GetSettings();
		ENGINE_TRY(Utils::RefreshAssets(m_Editor));
		if (AudioEngine* audio = GetAudioEngine(); audio && audio->GetSpecification().Decoding != AudioDecoding::Deterministic)
			return MakeError(ErrorCode::InvalidState, "test runs require deterministic audio decoding at engine construction");
		EditorScriptService* scripts = m_Editor.GetScriptService();
		if (scripts == nullptr)
			return MakeError(ErrorCode::InvalidState, "script diagnostics are unavailable");
		ENGINE_TRY_ASSIGN(const EditorScriptCheckResult checked, scripts->Check({}));
		if (std::ranges::any_of(checked.Diagnostics, [](const ScriptDiagnostic& diagnostic)
		{
			return diagnostic.Severity == DiagnosticSeverity::Error;
		}))
		{
			ENGINE_TRY_ASSIGN(Json findings, m_Context.SerializeResult(ScriptCheckResult{ .Paths = checked.Paths, .Diagnostics = checked.Diagnostics, .Passed = false }));
			m_Context.SetErrorData("scriptCheck", std::move(findings));
			return MakeError(ErrorCode::Validation, "script type errors block test discovery");
		}
		ENGINE_TRY_ASSIGN(const std::vector<std::string> replays, ExpandReplayPaths(m_Project.Testing.Replays));
		for (const std::string& path : replays)
		{
			ENGINE_TRY_ASSIGN(const AssetRef<ReplayData> replay, LoadReplay(path));
			ENGINE_TRY_ASSIGN(auto scene, CreateReplaySession(replay->Header, TestSuiteSettings::Mode::Editor));
			static_cast<void>(scene);
		}
		return {};
	}

	Status EditorFeatureTestHost::Acquire()
	{
		ENGINE_TRY_ASSIGN(m_CoverageStart, m_Api.GetCoverage(RunModes::Editor));
		ENGINE_TRY(m_Editor.GetPlay().AcquireTestRun(*this, m_Context.GetRequest().Client));
		m_Acquired = true;
		m_Headers.clear();
		return {};
	}

	const ProjectSettings& EditorFeatureTestHost::GetProjectSettings() const
	{
		return m_Project;
	}

	Result<AssetHandle> EditorFeatureTestHost::ResolveTestScript(std::string_view path)
	{
		ENGINE_TRY_ASSIGN(const VfsPath canonical, Utils::ResolveAssetsPath(m_Context, path, "/script", ".luau"));
		const auto handle = m_Editor.GetAssets().Resolve(canonical.GetPath());
		if (!handle.has_value())
			return MakeError(ErrorCode::NotFound, "test script '{}' is missing", path);
		ENGINE_TRY_ASSIGN(const AssetRef<Asset> loaded, m_Editor.GetAssets().Load(*handle));
		const auto script = AssetCast<ScriptData>(loaded);
		if (!script || script->Kind != ScriptKind::TestSuite)
			return MakeError(ErrorCode::Validation, "'{}' is not a Test.Suite script", path);
		return *handle;
	}

	Result<std::vector<std::string>> EditorFeatureTestHost::ExpandReplayPaths(std::span<const std::string> globs)
	{
		std::vector<std::string> result;
		ENGINE_TRY_ASSIGN(const VfsPath root, VfsPath::Create("project", "Assets"));
		ENGINE_TRY_ASSIGN(const std::vector<VfsEntry> entries, m_Editor.GetVfs().List(root, true));
		for (const std::string& glob : globs)
		{
			// Validate confinement separately from wildcard syntax; VfsPath forbids wildcards as native file names.
			std::string checked = glob;
			std::replace(checked.begin(), checked.end(), '*', 'x');
			std::replace(checked.begin(), checked.end(), '?', 'x');
			ENGINE_TRY(Utils::ResolveAssetsPath(m_Context, checked, "/replays", ".replay"));
			std::string_view pattern = glob;
			if (pattern.starts_with("project://"))
				pattern.remove_prefix(10);
			bool matched = false;
			for (const VfsEntry& entry : entries)
				if (!entry.Info.IsDirectory && entry.Path.GetExtension() == ".replay" && Utils::MatchesExportGlob(pattern, entry.Path.GetPath()))
				{
					matched = true;
					result.emplace_back(entry.Path.GetPath());
				}
			if (!matched && glob.find_first_of("*?") == std::string::npos)
				return MakeError(ErrorCode::NotFound, "replay '{}' is missing", glob);
		}
		std::ranges::sort(result);
		result.erase(std::unique(result.begin(), result.end()), result.end());
		return result;
	}

	Result<AssetRef<ReplayData>> EditorFeatureTestHost::LoadReplay(std::string_view path)
	{
		return m_Context.LoadReplay(path);
	}

	Result<ReplayHeader> EditorFeatureTestHost::DescribeReplayHeader(const PlaySession& session) const
	{
		const auto found = m_Headers.find(session.GetSerial());
		if (found == m_Headers.end() || !found->second.Scene.Handle.IsValid())
			return MakeError(ErrorCode::InvalidState, "recording requires a suite with a saved scene asset");
		return found->second;
	}

	Result<Scope<PlaySession>> EditorFeatureTestHost::CreateSuiteSession(const TestSuiteSettings& suite,
		TestSuiteSettings::Mode mode, IPlaySessionTestHook& hook, IScriptTestHost& testHost)
	{
		if (mode != TestSuiteSettings::Mode::Editor)
			return MakeError(ErrorCode::InvalidArgument, "the editor adapter serves Editor suites only");
		ProjectSettings project = m_Project;
		if (suite.Overrides.CallbackBudgetMs != 0)
			project.Scripting.CallbackBudgetMs = suite.Overrides.CallbackBudgetMs;
		if (suite.Overrides.MemoryLimitMB != 0)
			project.Scripting.MemoryLimitMB = suite.Overrides.MemoryLimitMB;
		if (suite.Overrides.PauseOnError != TestSuiteOverrides::PauseOnErrorOverride::Inherit)
			project.Scripting.PauseOnError = suite.Overrides.PauseOnError == TestSuiteOverrides::PauseOnErrorOverride::Pause;
		PlayStartOptions options;
		options.ScenePath = suite.Scene;
		options.Parameters = suite.Parameters;
		options.Lockstep = suite.Clock.empty();
		ENGINE_TRY_ASSIGN(auto prepared, m_Editor.GetPlay().PrepareSession(options, std::move(project), suite.Scene.empty(), &hook, &testHost, true, !m_Acquired, &m_Api));
		m_Headers[prepared.Session->GetSerial()] = std::move(prepared.Header);
		return std::move(prepared.Session);
	}

	Result<Scope<PlaySession>> EditorFeatureTestHost::CreateReplaySession(const ReplayHeader& header, TestSuiteSettings::Mode mode)
	{
		if (mode != TestSuiteSettings::Mode::Editor)
			return MakeError(ErrorCode::InvalidArgument, "the editor adapter serves Editor replays only");
		const AssetRecord* scene = m_Editor.GetAssets().GetRegistry().Find(header.Scene.Handle);
		if (!scene || scene->Metadata.Type != AssetType::Scene)
			return MakeError(ErrorCode::NotFound, "the replay's scene asset is missing");
		PlayStartOptions options;
		options.ScenePath = std::string(scene->SourcePath.GetPath());
		options.Parameters = header.Parameters;
		options.Seed = header.Seed;
		options.Lockstep = true;
		ProjectSettings project = m_Project;
		project.Simulation.FixedHz = header.FixedHz;
		ENGINE_TRY_ASSIGN(auto prepared, m_Editor.GetPlay().PrepareSession(options, std::move(project), false, nullptr, nullptr, true, !m_Acquired, &m_Api));
		m_Headers[prepared.Session->GetSerial()] = std::move(prepared.Header);
		return std::move(prepared.Session);
	}

	void EditorFeatureTestHost::SetActiveTestSession(PlaySession* session)
	{
		if (m_Acquired)
			m_Editor.GetPlay().PublishTestSession(*this, session);
	}

	AudioEngine* EditorFeatureTestHost::GetAudioEngine() const
	{
		return m_Editor.GetEngine().GetAudioEngine();
	}

	Status EditorFeatureTestHost::CaptureScreenshot(PlaySession& session, std::string_view name)
	{
		ENGINE_TRY(VfsPath::Create("project", name));
		RenderExtractionRequest request;
		request.Width = m_Project.Window.Width;
		request.Height = m_Project.Window.Height;
		ENGINE_TRY_ASSIGN(const RenderSnapshot snapshot, session.ExtractView(request));
		ViewportScreenshotRequest capture;
		capture.Width = request.Width;
		capture.Height = request.Height;
		ENGINE_TRY_ASSIGN(const Image image, m_Context.CaptureView(snapshot, capture));
		ENGINE_TRY_ASSIGN(const Buffer png, EncodePng(image));
		ENGINE_TRY(m_Context.WriteOutputFile("png", png));
		return {};
	}

	Status EditorFeatureTestHost::ReloadScript(PlaySession& session, AssetHandle script)
	{
		if (!m_Acquired || m_Editor.GetPlay().GetSession() != &session)
			return MakeError(ErrorCode::InvalidState, "only the active test session may reload scripts");
		if (!session.GetScripts())
			return MakeError(ErrorCode::InvalidState, "no script VM is active");
		ENGINE_TRY(m_Editor.ReimportScriptForTest(script));
		ENGINE_TRY(session.GetScripts()->Reload(script, true));
		return {};
	}

	Result<Json> EditorFeatureTestHost::Evaluate(PlaySession& session, const ReplayBytecodeExpectation& expectation)
	{
		if (!session.GetScripts())
			return MakeError(ErrorCode::InvalidState, "no replay VM is active");
		ENGINE_TRY_ASSIGN(const ScriptEvaluation evaluated, session.GetScripts()->ExecuteBytecode(expectation.Script));
		return evaluated.Value.Get();
	}

	Result<TestCoverageReport> EditorFeatureTestHost::GetCoverage() const
	{
		ENGINE_TRY_ASSIGN(const ScriptApiCoverage coverage, m_Api.GetCoverage(RunModes::Editor));
		ENGINE_TRY_ASSIGN(const std::string definitions, m_Api.GenerateDefinitions());
		TestCoverageReport report;
		report.RegistryFingerprint = std::format("{:016x}", XXH64(definitions));
		for (ScriptApiCounter counter : coverage.Members)
		{
			const auto before = std::ranges::find_if(m_CoverageStart.Members, [&counter](const ScriptApiCounter& item)
			{
				return item.Owner == counter.Owner && item.Member == counter.Member && item.Kind == counter.Kind;
			});
			if (before != m_CoverageStart.Members.end())
			{
				counter.Calls -= std::min(counter.Calls, before->Calls);
				counter.Reads -= std::min(counter.Reads, before->Reads);
				counter.Writes -= std::min(counter.Writes, before->Writes);
				for (auto& value : counter.EnumValues)
				{
					const auto old = std::ranges::find_if(before->EnumValues, [&value](const ScriptEnumCounter& item)
					{
						return item.ArgumentIndex == value.ArgumentIndex && item.EnumName == value.EnumName && item.ValueName == value.ValueName;
					});
					if (old != before->EnumValues.end())
						value.Count -= std::min(value.Count, old->Count);
				}
			}
			std::vector<TestSuiteSettings::Mode> modes;
			if (HasFlag(counter.Modes, RunModes::Editor))
				modes.push_back(TestSuiteSettings::Mode::Editor);
			if (HasFlag(counter.Modes, RunModes::Release))
				modes.push_back(TestSuiteSettings::Mode::Release);
			if (HasFlag(counter.Modes, RunModes::Dist))
				modes.push_back(TestSuiteSettings::Mode::Dist);
			const std::string id = std::format("{}.{}", counter.Owner, counter.Member);
			if (counter.RequiresRead)
				report.Counters.push_back({ .Id = id + "/read", .Kind = "read", .Modes = modes, .Count = ToAutomationCounter(counter.Reads) });
			if (counter.RequiresWrite)
				report.Counters.push_back({ .Id = id + "/write", .Kind = "write", .Modes = modes, .Count = ToAutomationCounter(counter.Writes) });
			if (!counter.RequiresRead && !counter.RequiresWrite && counter.Kind != ScriptApiMemberKind::Constant)
				report.Counters.push_back({ .Id = id, .Kind = counter.Kind == ScriptApiMemberKind::Callback ? "callback" : "call", .Modes = modes, .Count = ToAutomationCounter(counter.Calls) });
			for (const ScriptEnumCounter& value : counter.EnumValues)
				report.Counters.push_back({ .Id = std::format("{}/argument{}/{}/{}", id, value.ArgumentIndex, value.EnumName, value.ValueName), .Kind = "enum", .Modes = modes, .Count = ToAutomationCounter(value.Count) });
		}
		std::ranges::sort(report.Counters, {}, &TestCoverageCounter::Id);
		return report;
	}

}
