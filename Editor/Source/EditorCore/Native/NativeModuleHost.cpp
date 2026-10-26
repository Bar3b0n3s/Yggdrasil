#include "EditorPCH.h"
#include "EditorCore/Native/NativeModuleHost.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/EditorPreferences.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Native/NativeModule.h"
#include "Engine/Platform/SharedLibrary.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdint>
#include <format>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#if !defined(ENGINE_REPO_ROOT)
	#error "ENGINE_REPO_ROOT must be defined for the editor (Dependencies.lua, ApplyFirstPartySettings): module builds compile against its NativeApi.h"
#endif

namespace Engine {

	namespace Utils {

		// The functions ENGINE_NATIVE_MODULE exports from a module's shared library (NativeApi.h).
		constexpr const char* NativeModuleApiVersionSymbol = "EngineNativeModuleApiVersion";
		constexpr const char* NativeModuleRegisterSymbol = "EngineNativeModuleRegister";
		// Where the editor's module builds go below the project (decision 31), each in a numbered directory of at least this
		// many digits.
		constexpr std::string_view NativeBuildsDirectory = "Library/Native/Builds";
		constexpr size_t NativeBuildSequenceDigits = 8;
		// The one way the preference changes (decision 29).
		constexpr std::string_view NativePermissionHint = "the user turns on Allow native code in the editor's Native panel; no automation method can";

	}

	namespace {

		// The platform's loader (CreateNativeLibraryLoader): the library through SharedLibrary, then the two functions
		// ENGINE_NATIVE_MODULE exports. The SharedLibrary is the keepalive of everything registered from it.
		class SharedLibraryNativeLoader final : public INativeLibraryLoader
		{
		public:
			[[nodiscard]] Result<NativeLoadedLibrary> Load(const std::filesystem::path& library) override
			{
				ENGINE_TRY_ASSIGN(Scope<SharedLibrary> loaded, SharedLibrary::Load(library));
				Result<void*> version = loaded->FindSymbol(Utils::NativeModuleApiVersionSymbol);
				Result<void*> registration = loaded->FindSymbol(Utils::NativeModuleRegisterSymbol);
				if (!version.has_value() || !registration.has_value())
				{
					Error error = !version.has_value() ? std::move(version).error() : std::move(registration).error();
					return std::unexpected(std::move(error).WithHint(
						"a native module's library exports EngineNativeModuleApiVersion and EngineNativeModuleRegister: one of its sources names its registration "
						"function with ENGINE_NATIVE_MODULE, and native.build builds it"));
				}
				// The exported functions' own types (NativeApi.h's ENGINE_NATIVE_MODULE); a code address and a data address
				// have the same size on every platform the editor runs on.
				using ApiVersionFunction = uint32_t (*)();
				const auto readVersion = std::bit_cast<ApiVersionFunction>(*version);
				NativeLoadedLibrary result;
				result.Entry.ApiVersion = readVersion();
				result.Entry.Register = std::bit_cast<NativeModuleRegisterFunction>(*registration);
				result.Keepalive = Ref<const SharedLibrary>(std::move(loaded));
				return result;
			}
		};

		// An export's Runtime build (StartRuntimeBuild), counted in `running` from its start until it resolves or its owner
		// destroys it, so native.write refuses while one runs (decision 32). Main thread only, like the host, which outlives
		// every job it starts (the exporter's EditorContext owns the host; ~NativeModuleHost asserts it).
		class CountedNativeBuildJob final : public INativeBuildJob
		{
		public:
			CountedNativeBuildJob(Scope<INativeBuildJob> job, uint32_t& running)
				: m_Job(std::move(job)), m_Running(&running)
			{
				++*m_Running;
			}

			~CountedNativeBuildJob() override
			{
				Release();
			}

			CountedNativeBuildJob(const CountedNativeBuildJob&) = delete;
			CountedNativeBuildJob& operator=(const CountedNativeBuildJob&) = delete;
			CountedNativeBuildJob(CountedNativeBuildJob&&) = delete;
			CountedNativeBuildJob& operator=(CountedNativeBuildJob&&) = delete;

			[[nodiscard]] std::optional<Result<NativeBuildOutcome>> Poll() override
			{
				std::optional<Result<NativeBuildOutcome>> polled = m_Job->Poll();
				if (polled.has_value())
					Release();
				return polled;
			}

			void Cancel() override
			{
				m_Job->Cancel();
			}
		private:
			void Release()
			{
				if (!m_Counted)
					return;
				m_Counted = false;
				--*m_Running;
			}
		private:
			Scope<INativeBuildJob> m_Job;
			uint32_t* m_Running = nullptr; // documented back-reference: the host's count, the host outliving the job
			bool m_Counted = true;
		};

	}

	namespace Utils {

		// One directory below Library/Native/Builds/: its sequence and, when it holds a readable record, the build.
		struct NativeBuildEntry
		{
			uint64_t Sequence = 0;
			std::filesystem::path Directory{};
			std::optional<NativeBuildRecord> Record{};
		};

		[[nodiscard]] static std::filesystem::path GetNativeBuildsDirectory(const std::filesystem::path& projectRoot)
		{
			return projectRoot / FileSystem::PathFromUtf8(NativeBuildsDirectory);
		}

		// The sequence a build directory's name spells: decimal digits only; nullopt for any other name.
		[[nodiscard]] static std::optional<uint64_t> ParseNativeBuildSequence(std::string_view name)
		{
			if (name.empty() || !std::ranges::all_of(name, [](char character)
			{
				return character >= '0' && character <= '9';
			}))
				return std::nullopt;
			uint64_t sequence = 0;
			const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), sequence);
			if (error != std::errc() || end != name.data() + name.size())
				return std::nullopt;
			return sequence;
		}

		// Every numbered build directory of the project, oldest first. A directory without a readable record (a build that
		// runs, failed to start or was cancelled) has no Record. Errors: Io when the builds directory cannot be listed.
		[[nodiscard]] static Result<std::vector<NativeBuildEntry>> ListNativeBuilds(const std::filesystem::path& projectRoot)
		{
			std::vector<NativeBuildEntry> builds;
			const std::filesystem::path root = GetNativeBuildsDirectory(projectRoot);
			std::error_code error;
			if (!std::filesystem::is_directory(root, error) || error)
				return builds;
			ENGINE_TRY_ASSIGN(const std::vector<std::filesystem::path> entries, FileSystem::ListDirectory(root));
			for (const std::filesystem::path& entry : entries)
			{
				const std::optional<uint64_t> sequence = ParseNativeBuildSequence(FileSystem::PathToUtf8(entry.filename()));
				if (!sequence.has_value() || !std::filesystem::is_directory(entry, error) || error)
					continue;
				NativeBuildEntry build{ .Sequence = *sequence, .Directory = entry, .Record = std::nullopt };
				const std::filesystem::path recordPath = entry / FileSystem::PathFromUtf8(NativeBuildRecordFileName);
				if (FileSystem::Exists(recordPath))
				{
					if (Result<std::string> text = FileSystem::ReadText(recordPath); text.has_value())
					{
						if (Result<NativeBuildRecord> record = ParseNativeBuildRecord(*text); record.has_value())
							build.Record = std::move(*record);
					}
				}
				builds.push_back(std::move(build));
			}
			std::ranges::sort(builds, {}, &NativeBuildEntry::Sequence);
			return builds;
		}

		// A successful module build of `configuration` (the builds the editor loads).
		[[nodiscard]] static bool IsLoadableBuild(const NativeBuildEntry& build, ExportConfiguration configuration)
		{
			return build.Record.has_value() && build.Record->Succeeded && build.Record->Kind == NativeBuildKind::Module
				&& build.Record->Configuration == configuration && !build.Record->Output.empty();
		}

		// The newest loadable build of `configuration`, and with `sourceHash` the newest one built from those sources; null
		// when there is none.
		[[nodiscard]] static const NativeBuildEntry* FindNewestBuild(std::span<const NativeBuildEntry> builds, ExportConfiguration configuration,
			std::optional<std::string_view> sourceHash)
		{
			for (auto build = builds.rbegin(); build != builds.rend(); ++build)
			{
				if (IsLoadableBuild(*build, configuration) && (!sourceHash.has_value() || build->Record->SourceHash == *sourceHash))
					return &*build;
			}
			return nullptr;
		}

		[[nodiscard]] static NativeBuildInfo MakeBuildInfo(const NativeBuildEntry& build, std::string_view sourceHash)
		{
			return NativeBuildInfo{ .Directory = build.Directory,
				.Library = build.Directory / FileSystem::PathFromUtf8(build.Record->Output),
				.SourceHash = build.Record->SourceHash,
				.Configuration = build.Record->Configuration,
				.Current = !sourceHash.empty() && build.Record->SourceHash == sourceHash };
		}

		// The "Native." components of `scene`, registered or not.
		[[nodiscard]] static uint32_t CountNativeComponents(const Scene& scene)
		{
			uint32_t count = 0;
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const ConstEntity entity = scene.FindEntityByID(id);
				const UnknownComponentsComponent* preserved = entity.IsValid() ? entity.TryGetComponent<UnknownComponentsComponent>() : nullptr;
				if (preserved == nullptr)
					continue;
				count += static_cast<uint32_t>(std::ranges::count_if(preserved->Components, [](const UnknownComponentData& data)
				{
					return IsNativeComponentName(data.Name);
				}));
			}
			return count;
		}

		// Whether `diagnostics` hold an error.
		[[nodiscard]] static bool HasErrorDiagnostic(std::span<const LoadDiagnostic> diagnostics)
		{
			return std::ranges::any_of(diagnostics, [](const LoadDiagnostic& diagnostic)
			{
				return diagnostic.Severity == DiagnosticSeverity::Error;
			});
		}

		// A canonicalization's errors in the open scene `sceneName` as one Validation error, each with its entity and pointer.
		[[nodiscard]] static Error MakeNativeMigrationError(std::string_view sceneName, std::span<const LoadDiagnostic> diagnostics, bool reload)
		{
			std::vector<ErrorIssue> issues;
			for (const LoadDiagnostic& diagnostic : diagnostics)
			{
				if (diagnostic.Severity != DiagnosticSeverity::Error)
					continue;
				issues.push_back(ErrorIssue{ .JsonPointer = diagnostic.JsonPointer,
					.Message = std::format("entity {} {}: {} ({})", diagnostic.Entity.ToString(), diagnostic.JsonPointer, diagnostic.Message, diagnostic.Code),
					.Hint = {},
					.Suggestions = {} });
			}
			std::string first = issues.empty() ? std::string("a native component does not convert") : issues.front().Message;
			return Error(ErrorCode::Validation, std::format("the native components of the open scene '{}' do not take the module's types: {}", sceneName, first))
				.WithIssues(std::move(issues))
				.WithHint(reload ? "correct or remove the components named (native.setComponents), or change the module so it accepts them; the loaded module stays loaded"
								 : "correct or remove the components named, or change the module so it accepts them; no module was loaded");
		}

		// Writes `allowed` as AllowNativeCode into user://Editor.json, keeping every other member (decision 29).
		[[nodiscard]] static Status WriteAllowNativeCode(VirtualFileSystem& vfs, bool allowed)
		{
			ENGINE_TRY_ASSIGN(EditorPreferences preferences, ReadEditorPreferences(vfs));
			preferences.AllowNativeCode = allowed;
			return WriteEditorPreferences(vfs, preferences);
		}

	}

	struct NativeModuleHost::State
	{
		NativeModuleHostSpecification Specification{};
		bool Allowed = false;                                    // the user's preference as last read or applied
		std::optional<bool> QueuedPreference{};                  // the Native panel's change, applied by Update
		Scope<INativeBuildJob> Build{};                          // the module build StartBuild started, until it resolved
		uint32_t RuntimeBuilds = 0;                              // the exports' Runtime builds that run (CountedNativeBuildJob)
		std::optional<Result<NativeBuildOutcome>> BuildResult{}; // how that build resolved, for PollBuild after it did
		std::optional<NativeBuildOutcome> LastBuild{};
		Ref<const NativeModuleTypes> Loaded{};
		std::filesystem::path LoadedDirectory{}; // the loaded build's directory, which no clean-up removes
		std::string LoadedSourceHash{};

		[[nodiscard]] Status CheckAllowed(std::string_view action) const
		{
			if (Allowed)
				return {};
			return std::unexpected(Error(ErrorCode::PermissionDenied, std::format("cannot {} the native module: the user's Allow native code preference is off", action))
					.WithHint(std::string(Utils::NativePermissionHint)));
		}

		// The newest build of the editor's configuration whose source hash is the sources' hash now; nullopt when there is
		// none (no sources, no build, or only stale ones).
		[[nodiscard]] Result<std::optional<Utils::NativeBuildEntry>> FindCurrentBuild(const std::filesystem::path& projectRoot) const
		{
			ENGINE_TRY_ASSIGN(const std::vector<std::string> sources, ListNativeSources(projectRoot));
			if (sources.empty())
				return std::optional<Utils::NativeBuildEntry>();
			ENGINE_TRY_ASSIGN(const std::string hash, ComputeNativeSourceHash(projectRoot, Specification.EditorConfiguration, Specification.ApiHeader));
			ENGINE_TRY_ASSIGN(const std::vector<Utils::NativeBuildEntry> builds, Utils::ListNativeBuilds(projectRoot));
			const Utils::NativeBuildEntry* build = Utils::FindNewestBuild(builds, Specification.EditorConfiguration, std::string_view(hash));
			return build != nullptr ? std::optional<Utils::NativeBuildEntry>(*build) : std::optional<Utils::NativeBuildEntry>();
		}

		// Loads `build` and replaces the loaded module once every step succeeded (decision 31; the header's file comment).
		[[nodiscard]] Result<NativeLoadReport> LoadBuild(EditorContext& editor, const Utils::NativeBuildEntry& build)
		{
			const std::filesystem::path library = build.Directory / FileSystem::PathFromUtf8(build.Record->Output);
			Result<NativeLoadedLibrary> loaded = Specification.Loader->Load(library);
			if (!loaded.has_value())
				return std::unexpected(std::move(loaded).error().WithContext(std::format("loading the native module '{}'", FileSystem::PathToUtf8(library))));
			Result<Ref<const NativeModuleTypes>> registered = BuildNativeModuleTypes(loaded->Entry, std::move(loaded->Keepalive));
			if (!registered.has_value())
				return std::unexpected(std::move(registered).error().WithContext(std::format("registering the native module '{}'", FileSystem::PathToUtf8(library))));
			Ref<const NativeModuleTypes> types = std::move(*registered);

			NativeLoadReport report;
			report.Reloaded = Loaded != nullptr;
			report.LayoutChanged = Loaded == nullptr || Loaded->GetLayoutHash() != types->GetLayoutHash();
			// M20 (Docs/Decisions/0031-m20-contract.md, "Merge with M19"): every open scene, in tab order.
			const std::vector<OpenSceneId> scenes = editor.GetOpenScenes();
			if (report.LayoutChanged)
			{
				// The migration by field name (decision 27), checked in every open scene before any changes: an error leaves
				// every open scene as it was, so the load fails here with the previous module still loaded.
				for (const OpenSceneId id : scenes)
				{
					const Scene* scene = std::as_const(editor).FindScene(id);
					ENGINE_ASSERT(scene != nullptr, "an open scene of EditorContext::GetOpenScenes has no scene");
					const NativeSceneReport checked = CheckNativeComponents(*scene, types.get(), false);
					if (Utils::HasErrorDiagnostic(checked.Diagnostics))
						return std::unexpected(Utils::MakeNativeMigrationError(scene->GetName(), checked.Diagnostics, report.Reloaded));
				}
				for (const OpenSceneId id : scenes)
				{
					ENGINE_TRY_ASSIGN(NativeSceneReport migrated, CanonicalizeNativeComponents(*editor.FindScene(id), types.get(), false));
					report.Scenes.push_back(NativeSceneMigration{ .Scene = id, .Report = std::move(migrated) });
				}
			}
			else
			{
				// Equal layout hashes mean equal canonical JSON for every value: nothing is rewritten.
				for (const OpenSceneId id : scenes)
				{
					const Scene* scene = std::as_const(editor).FindScene(id);
					ENGINE_ASSERT(scene != nullptr, "an open scene of EditorContext::GetOpenScenes has no scene");
					NativeSceneReport counted;
					counted.ComponentCount = Utils::CountNativeComponents(*scene);
					report.Scenes.push_back(NativeSceneMigration{ .Scene = id, .Report = std::move(counted) });
				}
			}
			for (const NativeSceneMigration& migration : report.Scenes)
			{
				report.Scene.Changed = report.Scene.Changed || migration.Report.Changed;
				report.Scene.ComponentCount += migration.Report.ComponentCount;
				report.Scene.Diagnostics.insert(report.Scene.Diagnostics.end(), migration.Report.Diagnostics.begin(), migration.Report.Diagnostics.end());
			}

			Ref<const NativeModuleTypes> previous = std::exchange(Loaded, std::move(types));
			LoadedDirectory = build.Directory;
			LoadedSourceHash = build.Record->SourceHash;
			// Any load whose layout changed, a first load included, clears the undo history of every open scene: its entity
			// snapshots hold the previous layout's values or, recorded without a module, the values as written, which undo
			// would put back whole.
			report.UndoCleared = !scenes.empty() && report.LayoutChanged;
			for (const NativeSceneMigration& migration : report.Scenes)
			{
				if (migration.Report.Changed || report.UndoCleared)
					editor.ApplySceneMigration(migration.Scene, migration.Report.Changed, report.UndoCleared);
			}
			ENGINE_INFO("{} the native module of '{}' from '{}': {} components, {} resources, {} systems{}{}", report.Reloaded ? "Reloaded" : "Loaded",
				editor.GetProject().GetSettings().Name, FileSystem::PathToUtf8(library), Loaded->GetComponents().size(), Loaded->GetResources().size(),
				Loaded->GetSystems().size(), report.Reloaded && !report.LayoutChanged ? ", layout unchanged" : "",
				report.Scene.Changed ? std::format(", the {} native components of the open scenes migrated", report.Scene.ComponentCount) : std::string());
			for (const NativeSceneMigration& migration : report.Scenes)
			{
				const Scene* scene = std::as_const(editor).FindScene(migration.Scene);
				for (const LoadDiagnostic& diagnostic : migration.Report.Diagnostics)
				{
					ENGINE_WARN("Native module migration: scene '{}', entity {} {}: {} ({})", scene != nullptr ? scene->GetName() : std::string(),
						diagnostic.Entity.ToString(), diagnostic.JsonPointer, diagnostic.Message, diagnostic.Code);
				}
			}
			// The previous types last: their library unloads once nothing else holds them (a play session cannot run now).
			previous.reset();
			RemoveOldBuilds(editor);
			return report;
		}

		// Releases the loaded module; its components stay in the scene as written.
		void Unload(std::string_view reason)
		{
			if (Loaded == nullptr)
				return;
			Loaded.reset();
			LoadedDirectory.clear();
			LoadedSourceHash.clear();
			ENGINE_INFO("Unloaded the native module: {}", reason);
		}

		// Resolves a finished module build (PollBuild, CancelBuild): LastBuild, the log and the clean-up.
		void FinishBuild(EditorContext* editor, const Result<NativeBuildOutcome>& result)
		{
			if (result.has_value())
			{
				LastBuild = *result;
				if (result->Succeeded)
				{
					ENGINE_INFO("Native module built in {:.1f} s into '{}'", result->Seconds, FileSystem::PathToUtf8(result->Output));
					if (editor != nullptr)
						RemoveOldBuilds(*editor);
					return;
				}
				const auto first = std::ranges::find(result->Diagnostics, DiagnosticSeverity::Error, &NativeBuildDiagnostic::Severity);
				ENGINE_WARN("The native module build failed{}", first == result->Diagnostics.end() ? std::string() : std::format(": {} {}:{}: {}", first->Code, first->File, first->Line, first->Message));
				return;
			}
			if (result.error().GetCode() == ErrorCode::Cancelled)
			{
				ENGINE_INFO("The native module build was cancelled");
				return;
			}
			// A build that could not run or whose record cannot be read is a failed build with that one diagnostic.
			NativeBuildOutcome failed;
			failed.Diagnostics.push_back(NativeBuildDiagnostic{ .Severity = DiagnosticSeverity::Error,
				.Code = "NATIVE_BUILD_ERROR",
				.File = {},
				.Line = 0,
				.Column = 0,
				.Message = result.error().ToString() });
			LastBuild = std::move(failed);
			ENGINE_ERROR("The native module build failed: {}", result.error());
		}

		// Removes the editor configuration's builds older than its newest successful one, and the directories without a
		// readable record older than it, except the loaded build (decision 31). A failure to remove one is logged.
		void RemoveOldBuilds(EditorContext& editor) const
		{
			if (!editor.HasProject() || editor.IsReadOnly())
				return;
			Result<std::vector<Utils::NativeBuildEntry>> builds = Utils::ListNativeBuilds(editor.GetProject().GetRoot());
			if (!builds.has_value())
			{
				ENGINE_WARN("Cannot list the native module builds to remove the old ones: {}", builds.error());
				return;
			}
			const Utils::NativeBuildEntry* newest = Utils::FindNewestBuild(*builds, Specification.EditorConfiguration, std::nullopt);
			if (newest == nullptr)
				return;
			for (const Utils::NativeBuildEntry& build : *builds)
			{
				if (build.Sequence >= newest->Sequence || build.Directory == LoadedDirectory)
					continue;
				if (build.Record.has_value() && build.Record->Configuration != Specification.EditorConfiguration)
					continue;
				if (Status removed = FileSystem::Remove(build.Directory); !removed.has_value())
					ENGINE_WARN("Cannot remove the old native module build '{}': {}", FileSystem::PathToUtf8(build.Directory), removed.error());
			}
		}
	};

	Scope<INativeLibraryLoader> CreateNativeLibraryLoader()
	{
		return CreateScope<SharedLibraryNativeLoader>();
	}

	NativeModuleHostSpecification MakeEditorNativeModuleHostSpecification(Ref<INativeBuildBackend> build, Ref<INativeLibraryLoader> loader)
	{
		NativeModuleHostSpecification specification;
		specification.Build = build != nullptr ? std::move(build) : Ref<INativeBuildBackend>(CreateNativeBuildBackend());
		specification.Loader = loader != nullptr ? std::move(loader) : Ref<INativeLibraryLoader>(CreateNativeLibraryLoader());
#if defined(ENGINE_DEBUG)
		specification.EditorConfiguration = ExportConfiguration::Debug;
#else
		specification.EditorConfiguration = ExportConfiguration::Release;
#endif
		specification.ApiHeader = std::filesystem::path(ENGINE_REPO_ROOT) / "Engine" / "Source" / "Engine" / "Native" / "NativeApi.h";
		return specification;
	}

	NativeModuleHost::NativeModuleHost(NativeModuleHostSpecification specification)
		: m_State(CreateScope<State>())
	{
		ENGINE_ASSERT(specification.Build != nullptr && specification.Loader != nullptr, "the native module host needs a build backend and a library loader");
		m_State->Specification = std::move(specification);
	}

	NativeModuleHost::~NativeModuleHost()
	{
		CancelBuild();
		ENGINE_ASSERT(m_State->RuntimeBuilds == 0, "{} Runtime build(s) of an export outlive the native module host that started them", m_State->RuntimeBuilds);
	}

	bool NativeModuleHost::IsAllowed() const
	{
		return m_State->Allowed;
	}

	Status NativeModuleHost::ReadPreference(const EditorContext& editor)
	{
		const VirtualFileSystem& vfs = editor.GetVfs();
		// An editor without user data (no user:// mount) has no preference: native code stays off.
		if (!vfs.IsMounted("user"))
		{
			m_State->Allowed = false;
			return {};
		}
		Result<EditorPreferences> preferences = ReadEditorPreferences(vfs);
		if (!preferences.has_value())
		{
			m_State->Allowed = false;
			return std::unexpected(std::move(preferences).error().WithContext("reading the Allow native code preference from user://Editor.json; native code stays off"));
		}
		m_State->Allowed = preferences->AllowNativeCode;
		return {};
	}

	void NativeModuleHost::QueuePreferenceChange(bool allowed)
	{
		m_State->QueuedPreference = allowed;
	}

	void NativeModuleHost::Update(EditorContext& editor)
	{
		State& state = *m_State;
		if (const std::optional<bool> queued = std::exchange(state.QueuedPreference, std::nullopt); queued.has_value())
		{
			if (Status written = Utils::WriteAllowNativeCode(editor.GetVfs(), *queued); !written.has_value())
			{
				ENGINE_ERROR("The Allow native code preference stays {}: it could not be saved: {}", state.Allowed ? "on" : "off", written.error());
			}
			else if (state.Allowed != *queued)
			{
				state.Allowed = *queued;
				ENGINE_INFO("Allow native code turned {}", state.Allowed ? "on" : "off");
			}
		}
		// Off: the module goes at the first frame boundary without a play session (decision 29).
		if (!state.Allowed && state.Loaded != nullptr && !editor.GetPlay().IsPlaying() && !editor.GetPlay().IsTestRunActive())
			state.Unload("Allow native code is off");
	}

	Status NativeModuleHost::OnProjectOpened(EditorContext& editor)
	{
		State& state = *m_State;
		ENGINE_ASSERT(editor.HasProject(), "NativeModuleHost::OnProjectOpened without an open project");
		const std::filesystem::path& root = editor.GetProject().GetRoot();
		if (!HasNativeModuleDirectory(root) || !SharedLibrary::CheckSupported().has_value())
			return {};
		if (!state.Allowed)
		{
			ENGINE_INFO("'{}' has a native module, which stays unloaded while the Allow native code preference is off", editor.GetProject().GetSettings().Name);
			return {};
		}
		if (editor.GetPlay().IsPlaying())
			return MakeError(ErrorCode::InvalidState, "the native module of '{}' cannot load while a play session runs", editor.GetProject().GetSettings().Name);
		ENGINE_TRY_ASSIGN(const std::optional<Utils::NativeBuildEntry> build, state.FindCurrentBuild(root));
		if (!build.has_value())
		{
			ENGINE_INFO("'{}' opens without its native module: no build of its sources as they are now (native.build then native.load load it)",
				editor.GetProject().GetSettings().Name);
			return {};
		}
		ENGINE_TRY(state.LoadBuild(editor, *build));
		return {};
	}

	void NativeModuleHost::OnProjectClosed()
	{
		CancelBuild();
		m_State->LastBuild.reset();
		m_State->Unload("the project closed");
	}

	Result<NativeModuleStatus> NativeModuleHost::GetStatus(const EditorContext& editor) const
	{
		const State& state = *m_State;
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "the native module's status needs an open project");
		const std::filesystem::path& root = editor.GetProject().GetRoot();
		NativeModuleStatus status;
		status.Allowed = state.Allowed;
		status.Supported = SharedLibrary::CheckSupported().has_value();
		ENGINE_TRY_ASSIGN(status.Sources, ListNativeSources(root));
		if (!status.Sources.empty())
		{
			ENGINE_TRY_ASSIGN(status.SourceHash, ComputeNativeSourceHash(root, state.Specification.EditorConfiguration, state.Specification.ApiHeader));
		}
		ENGINE_TRY_ASSIGN(const std::vector<Utils::NativeBuildEntry> builds, Utils::ListNativeBuilds(root));
		if (const Utils::NativeBuildEntry* newest = Utils::FindNewestBuild(builds, state.Specification.EditorConfiguration, std::nullopt); newest != nullptr)
			status.LatestBuild = Utils::MakeBuildInfo(*newest, status.SourceHash);
		status.Building = state.Build != nullptr;
		status.LastBuild = state.LastBuild;
		status.Loaded = state.Loaded;
		status.LoadedSourceHash = state.LoadedSourceHash;
		return status;
	}

	Status NativeModuleHost::StartBuild(EditorContext& editor)
	{
		State& state = *m_State;
		ENGINE_TRY(SharedLibrary::CheckSupported());
		ENGINE_TRY(state.CheckAllowed("build"));
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "cannot build the native module: no project is open");
		if (editor.IsReadOnly())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "cannot build the native module: the project is open read-only, so nothing is written below it")
					.WithHint("open the project without --read-only, or close the editor that holds it"));
		}
		if (state.Build != nullptr)
			return std::unexpected(Error(ErrorCode::InvalidState, "a build of the native module already runs").WithHint("wait for it to resolve (native.build)"));
		const std::filesystem::path& root = editor.GetProject().GetRoot();
		ENGINE_TRY_ASSIGN(const std::vector<std::string> sources, ListNativeSources(root));
		if (sources.empty())
		{
			return std::unexpected(Error(ErrorCode::NotFound, std::format("'{}' has no native module source (a .cpp or .h file below Native/)", editor.GetProject().GetSettings().Name))
					.WithHint("write the module's sources with native.write, one of them naming its registration function with ENGINE_NATIVE_MODULE"));
		}

		// A new numbered directory: Windows does not let a loaded library be replaced, so every build gets its own.
		ENGINE_TRY_ASSIGN(const std::vector<Utils::NativeBuildEntry> builds, Utils::ListNativeBuilds(root));
		if (!builds.empty() && builds.back().Sequence == std::numeric_limits<uint64_t>::max())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, std::format("'{}' holds a build numbered {}, after which no build can be numbered", FileSystem::PathToUtf8(builds.back().Directory), builds.back().Sequence))
					.WithHint("remove the project's Library/Native/Builds folder while the editor is closed; builds are made again by native.build"));
		}
		const uint64_t sequence = builds.empty() ? 1 : builds.back().Sequence + 1;
		const std::filesystem::path directory = Utils::GetNativeBuildsDirectory(root)
			/ FileSystem::PathFromUtf8(std::format("{:0{}}", sequence, Utils::NativeBuildSequenceDigits));
		ENGINE_TRY(FileSystem::CreateDirectories(directory));
		const NativeBuildRequest request{ .ProjectRoot = root, .Kind = NativeBuildKind::Module, .Configuration = state.Specification.EditorConfiguration, .OutputDirectory = directory };
		Result<Scope<INativeBuildJob>> job = state.Specification.Build->Start(request);
		if (!job.has_value())
		{
			if (Status removed = FileSystem::RemoveEmptyDirectory(directory); !removed.has_value())
				ENGINE_WARN("Cannot remove the directory of a native module build that did not start, '{}': {}", FileSystem::PathToUtf8(directory), removed.error());
			return std::unexpected(std::move(job).error().WithContext("starting the native module build"));
		}
		state.Build = std::move(*job);
		state.BuildResult.reset();
		return {};
	}

	std::optional<Result<NativeBuildOutcome>> NativeModuleHost::PollBuild(EditorContext& editor)
	{
		State& state = *m_State;
		if (state.Build == nullptr)
		{
			if (state.BuildResult.has_value())
				return state.BuildResult;
			return Result<NativeBuildOutcome>(MakeError(ErrorCode::InvalidState, "no native module build was started"));
		}
		std::optional<Result<NativeBuildOutcome>> polled = state.Build->Poll();
		if (!polled.has_value())
			return std::nullopt;
		state.Build.reset();
		state.FinishBuild(&editor, *polled);
		state.BuildResult = polled;
		return polled;
	}

	void NativeModuleHost::CancelBuild()
	{
		State& state = *m_State;
		if (state.Build == nullptr)
			return;
		state.Build->Cancel();
		std::optional<Result<NativeBuildOutcome>> polled = state.Build->Poll();
		state.Build.reset();
		if (!polled.has_value())
			polled = Result<NativeBuildOutcome>(MakeError(ErrorCode::Cancelled, "the native module build was cancelled"));
		state.FinishBuild(nullptr, *polled);
		state.BuildResult = std::move(polled);
	}

	Result<Scope<INativeBuildJob>> NativeModuleHost::StartRuntimeBuild(EditorContext& editor, ExportConfiguration configuration,
		const std::filesystem::path& outputDirectory)
	{
		State& state = *m_State;
		ENGINE_TRY(SharedLibrary::CheckSupported());
		ENGINE_TRY(state.CheckAllowed("build"));
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "cannot build the Runtime with the native module: no project is open");
		const std::filesystem::path& root = editor.GetProject().GetRoot();
		ENGINE_TRY_ASSIGN(const std::vector<std::string> sources, ListNativeSources(root));
		if (sources.empty())
		{
			return std::unexpected(Error(ErrorCode::NotFound, std::format("'{}' has no native module source (a .cpp or .h file below Native/)", editor.GetProject().GetSettings().Name))
					.WithHint("write the module's sources with native.write, one of them naming its registration function with ENGINE_NATIVE_MODULE"));
		}
		const NativeBuildRequest request{ .ProjectRoot = root, .Kind = NativeBuildKind::Runtime, .Configuration = configuration, .OutputDirectory = outputDirectory };
		Result<Scope<INativeBuildJob>> job = state.Specification.Build->Start(request);
		if (!job.has_value())
			return std::unexpected(std::move(job).error().WithContext(std::format("starting the {} Runtime build with the native module", ExportConfigurationToString(configuration))));
		return Scope<INativeBuildJob>(CreateScope<CountedNativeBuildJob>(std::move(*job), state.RuntimeBuilds));
	}

	bool NativeModuleHost::IsRuntimeBuildRunning() const
	{
		return m_State->RuntimeBuilds > 0;
	}

	Result<NativeLoadReport> NativeModuleHost::Load(EditorContext& editor)
	{
		State& state = *m_State;
		ENGINE_TRY(SharedLibrary::CheckSupported());
		ENGINE_TRY(state.CheckAllowed("load"));
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "cannot load the native module: no project is open");
		if (editor.GetPlay().IsPlaying() || editor.GetPlay().IsTestRunActive())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "cannot load the native module while a play session runs: it loads only at a frame boundary in Edit mode")
					.WithHint("stop the play session (play.stop), then load"));
		}
		if (state.Build != nullptr)
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "cannot load the native module while its build runs")
					.WithHint("wait for native.build to resolve, then load the build it made"));
		}
		const std::filesystem::path& root = editor.GetProject().GetRoot();
		ENGINE_TRY_ASSIGN(const std::optional<Utils::NativeBuildEntry> build, state.FindCurrentBuild(root));
		if (!build.has_value())
		{
			return std::unexpected(Error(ErrorCode::InvalidState,
				std::format("'{}' has no successful {} build of its native module from its sources as they are now", editor.GetProject().GetSettings().Name,
					ExportConfigurationToString(state.Specification.EditorConfiguration)))
					.WithHint("build it with native.build, then load it"));
		}
		return state.LoadBuild(editor, *build);
	}

	Ref<const NativeModuleTypes> NativeModuleHost::GetLoadedTypes() const
	{
		return m_State->Loaded;
	}

}
