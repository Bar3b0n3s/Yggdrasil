#include "EditorPCH.h"
#include "EditorCore/Automation/NativeMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Native/NativeModuleHost.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/UUID.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Native/NativeComponents.h"
#include "Engine/Native/NativeModule.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/NameComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/NativeWorld.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		// The extensions of a native source (decision 26; ListNativeSources).
		constexpr std::array<std::string_view, 2> NativeSourceExtensions = { ".cpp", ".h" };

		// A path param naming a native source (decision 32): a .cpp or .h file below Native/, resolved like script.read's
		// paths below Assets/ (EditorMethodContext::ResolveProjectPath, then the Native/ confinement). Errors: those of
		// ResolveProjectPath (InvalidState without a project, InvalidArgument for another scheme or an escaping path);
		// InvalidArgument at `pointer` for another folder or another extension.
		[[nodiscard]] static Result<VfsPath> ResolveNativeSourcePath(const EditorMethodContext& context, std::string_view path, std::string_view pointer)
		{
			ENGINE_TRY_ASSIGN(VfsPath resolved, context.ResolveProjectPath(path, pointer));
			ENGINE_TRY_ASSIGN(const VfsPath native, VfsPath::Create("project", NativeSourceDirectoryName));
			if (!resolved.IsUnder(native) || resolved == native)
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' is not below the Native folder", path),
					"a game's native module is the .cpp and .h files below Native/, such as \"Native/Spinner.cpp\""));
			}
			const std::string_view extension = resolved.GetExtension();
			if (std::ranges::find(NativeSourceExtensions, extension) == NativeSourceExtensions.end())
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' is not a .cpp or .h source", path),
					"a native module holds only .cpp and .h files (decision 35's lint refuses any other file below Native/)"));
			}
			return resolved;
		}

		// A file of a build as results name it: project-relative with forward slashes ("Library/Native/Builds/00000001/
		// NativeModule.dll"), or its absolute path when it is not below the project; empty for an empty path.
		[[nodiscard]] static std::string ToProjectRelativeFile(const EditorContext& editor, const std::filesystem::path& file)
		{
			if (file.empty())
				return {};
			if (!editor.HasProject())
				return FileSystem::PathToUtf8(file);
			const std::filesystem::path relative = file.lexically_normal().lexically_relative(editor.GetProject().GetRoot().lexically_normal());
			if (relative.empty() || *relative.begin() == "..")
				return FileSystem::PathToUtf8(file);
			return FileSystem::PathToUtf8(relative);
		}

		[[nodiscard]] static NativeTypeSummary MakeNativeTypeSummary(const NativeTypeInfo& type)
		{
			return NativeTypeSummary{ .Name = type.Name,
				.RegistryName = type.RegistryName,
				.Role = type.Role,
				.Description = type.Description,
				.Schema = VariantValue(GetNativeTypeSchema(type)) };
		}

		// The loaded module's types, systems and resources as native.status and native.load list them.
		static void SummarizeNativeModule(const NativeModuleTypes& types, std::vector<NativeTypeSummary>& components, std::vector<NativeTypeSummary>& resources,
			std::vector<NativeSystemSummary>& systems)
		{
			for (const NativeTypeInfo& type : types.GetComponents())
				components.push_back(MakeNativeTypeSummary(type));
			for (const NativeTypeInfo& type : types.GetResources())
				resources.push_back(MakeNativeTypeSummary(type));
			for (const NativeSystemInfo& system : types.GetSystems())
				systems.push_back(NativeSystemSummary{ .Name = system.Name, .Description = system.Description });
		}

		// A canonicalization's finding (NativeSceneReport::Diagnostics) as native.load reports it, its entity summarized in the
		// open scene.
		[[nodiscard]] static NativeComponentDiagnostic MakeNativeComponentDiagnostic(const EditorMethodContext& context, const LoadDiagnostic& diagnostic)
		{
			NativeComponentDiagnostic result{ .Severity = diagnostic.Severity,
				.Code = diagnostic.Code,
				.Message = diagnostic.Message,
				.Entity = EntitySummary{ .Id = FormatOptionalUUID(diagnostic.Entity), .Name = {}, .Path = {} },
				.Pointer = diagnostic.JsonPointer };
			const EditorContext& editor = context.GetEditor();
			if (editor.HasScene())
			{
				if (const ConstEntity entity = editor.GetScene().FindEntityByID(diagnostic.Entity); entity.IsValid())
					result.Entity = context.MakeEntitySummary(entity);
			}
			return result;
		}

		[[nodiscard]] static std::map<std::string, VariantValue> ToComponentMap(std::vector<std::pair<std::string, VariantValue>> components)
		{
			std::map<std::string, VariantValue> result;
			for (auto& [name, value] : components)
				result.emplace(std::move(name), std::move(value));
			return result;
		}

		[[nodiscard]] static NativeBuildResult MakeNativeBuildResult(const EditorContext& editor, const NativeBuildOutcome& outcome)
		{
			return NativeBuildResult{ .Succeeded = outcome.Succeeded,
				.Library = outcome.Succeeded ? ToProjectRelativeFile(editor, outcome.Output) : std::string(),
				.SourceHash = outcome.SourceHash,
				.Diagnostics = outcome.Diagnostics,
				.Log = ToProjectRelativeFile(editor, outcome.Log),
				.Seconds = static_cast<float>(outcome.Seconds) };
		}

		// Rewrites, in native.setComponents' patch, every Entity value of a component `types` registers that is a string but
		// not a 16-hex id to the id `resolver` gives it (decision 32, as decision 7 resolves EntityRef values in component
		// data); a 16-hex id of either case is written in its canonical lowercase form, and null and the other JSON types are
		// left for PatchNativeComponents to accept or refuse. A member names a field as NativeValueFromJson reads it: by the
		// field's name, or by its RenamedFrom while the field's own name is absent. Errors: the resolver's (NotFound,
		// InvalidArgument), located at /components/<name>/<member>.
		[[nodiscard]] static Status ResolveNativeEntityValues(Json& patch, const NativeModuleTypes& types, IEntityReferenceResolver& resolver)
		{
			for (auto component = patch.begin(); component != patch.end(); ++component)
			{
				const NativeTypeInfo* type = types.FindComponent(component.key());
				if (type == nullptr || !component.value().is_object())
					continue;
				Json& value = component.value();
				for (const NativeFieldInfo& field : type->Fields)
				{
					if (field.Kind != NativeFieldKind::Entity)
						continue;
					std::string member = field.Name;
					if (!value.contains(member) && !field.RenamedFrom.empty() && value.contains(field.RenamedFrom))
						member = field.RenamedFrom;
					const auto found = value.find(member);
					if (found == value.end() || !found->is_string())
						continue;
					const Result<std::string> text = JsonReader(*found).ReadString();
					if (!text)
						continue;
					if (const std::optional<UUID> id = UUID::FromString(*text); id.has_value())
					{
						*found = id->ToString();
						continue;
					}
					const std::string pointer = JsonReader::AppendPointer(JsonReader::AppendPointer("/components", component.key()), member);
					ENGINE_TRY_ASSIGN(const UUID resolved, resolver.ResolveEntityReference(*text, pointer));
					*found = resolved.ToString();
				}
			}
			return {};
		}

	}

	namespace {

		// native.build's operation: the host's build, polled once per frame until it resolves; a disconnect cancels it.
		class NativeBuildOperation final : public PendingOperation
		{
		public:
			explicit NativeBuildOperation(EditorContext& editor)
				: m_Editor(&editor)
			{
			}

			~NativeBuildOperation() override
			{
				if (!m_Resolved)
					m_Editor->GetNativeModule().CancelBuild();
			}

			NativeBuildOperation(const NativeBuildOperation&) = delete;
			NativeBuildOperation& operator=(const NativeBuildOperation&) = delete;

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				std::optional<Result<NativeBuildOutcome>> outcome = m_Editor->GetNativeModule().PollBuild(*m_Editor);
				if (!outcome.has_value())
					return std::nullopt;
				m_Resolved = true;
				if (!outcome->has_value())
					return Result<Json>(std::unexpected(std::move(*outcome).error()));
				return context.SerializeResult(Utils::MakeNativeBuildResult(*m_Editor, **outcome));
			}

			void Cancel(MethodContext& /*context*/) override
			{
				if (m_Resolved)
					return;
				m_Resolved = true;
				m_Editor->GetNativeModule().CancelBuild();
			}

			[[nodiscard]] std::string GetPhase() const override { return "Native:build"; }
		private:
			EditorContext* m_Editor = nullptr; // documented back-reference: the editor outlives its requests
			bool m_Resolved = false;
		};

	}

	namespace Automation {

		Result<NativeStatusResult> NativeStatus(EditorMethodContext& context, const NoParams& /*params*/)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const NativeModuleStatus status, editor.GetNativeModule().GetStatus(editor));
			NativeStatusResult result;
			result.Allowed = status.Allowed;
			result.Supported = status.Supported;
			result.Sources = status.Sources;
			result.SourceHash = status.SourceHash;
			result.Building = status.Building;
			if (status.LatestBuild.has_value())
			{
				result.LatestBuild = Utils::ToProjectRelativeFile(editor, status.LatestBuild->Library);
				result.LatestBuildCurrent = status.LatestBuild->Current;
			}
			if (status.LastBuild.has_value())
			{
				result.LastBuildRan = true;
				result.LastBuildSucceeded = status.LastBuild->Succeeded;
				result.LastBuildDiagnostics = status.LastBuild->Diagnostics;
			}
			if (status.Loaded != nullptr)
			{
				result.Loaded = true;
				result.LoadedCurrent = !status.SourceHash.empty() && status.LoadedSourceHash == status.SourceHash;
				Utils::SummarizeNativeModule(*status.Loaded, result.Components, result.Resources, result.Systems);
			}
			if (const PlaySession* session = context.GetPlaySession(); session != nullptr)
			{
				if (const NativeWorld* world = session->GetNative(); world != nullptr)
					result.PlayResources = VariantValue(world->GetResources());
			}
			return result;
		}

		Result<NativeReadResult> NativeRead(EditorMethodContext& context, const NativeReadParams& params)
		{
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolveNativeSourcePath(context, params.Path, "/path"));
			Result<std::string> source = context.GetEditor().GetVfs().ReadText(path);
			if (!source)
				return std::unexpected(Utils::LocateAtParam(source.error(), "/path"));
			return NativeReadResult{ .Path = Utils::ToProjectRelative(path), .Source = std::move(*source) };
		}

		Result<NativeWriteResult> NativeWrite(EditorMethodContext& context, const NativeWriteParams& params)
		{
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolveNativeSourcePath(context, params.Path, "/path"));
			// A read-only editor refuses the method before it runs (a Mutates method), and request params are valid UTF-8.
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const NativeModuleStatus status, editor.GetNativeModule().GetStatus(editor));
			if (status.Building)
			{
				return std::unexpected(Error(ErrorCode::InvalidState, std::format("'{}' cannot be written while the native module builds", Utils::ToProjectRelative(path)))
						.WithHint("wait for native.build to resolve (native.status reports building), then write the source"));
			}

			// The missing folders, outermost first, then the file: one AssetEditCommand, so one undo step removes them all.
			VirtualFileSystem& vfs = editor.GetVfs();
			std::vector<AssetFileEdit> edits;
			std::vector<VfsPath> missingDirectories;
			for (VfsPath directory = path.GetParent(); !directory.IsRoot(); directory = directory.GetParent())
			{
				const Result<FileInfo> info = vfs.GetInfo(directory);
				if (info)
				{
					if (!info->IsDirectory)
						return std::unexpected(Utils::MakeParamError(ErrorCode::AlreadyExists, "/path", std::format("'{}' is a file", Utils::ToProjectRelative(directory))));
					break;
				}
				if (info.error().GetCode() != ErrorCode::NotFound)
					return std::unexpected(info.error());
				missingDirectories.push_back(directory);
			}
			for (auto directory = missingDirectories.rbegin(); directory != missingDirectories.rend(); ++directory)
				edits.push_back(AssetFileEdit{ .Path = *directory, .Kind = AssetFileKind::Directory, .Before = std::nullopt, .After = Buffer() });
			const Result<Buffer> previous = vfs.ReadFile(path);
			if (!previous && previous.error().GetCode() != ErrorCode::NotFound)
				return std::unexpected(Utils::LocateAtParam(previous.error(), "/path"));
			NativeWriteResult result{ .Path = Utils::ToProjectRelative(path), .Created = !previous.has_value(), .UndoIndex = 0 };
			if (previous.has_value() && AsStringView(*previous) == params.Source)
				return result;
			const std::span<const std::byte> bytes = AsBytes(params.Source);
			edits.push_back(AssetFileEdit{ .Path = path,
				.Kind = AssetFileKind::File,
				.Before = previous ? std::optional<Buffer>(*previous) : std::nullopt,
				.After = Buffer(bytes.begin(), bytes.end()) });
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(CreateScope<AssetEditCommand>(std::format("Write '{}'", result.Path), std::move(edits))));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

		Result<Scope<PendingOperation>> NativeBuild(EditorMethodContext& context, const NoParams& /*params*/)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY(editor.GetNativeModule().StartBuild(editor));
			return Scope<PendingOperation>(CreateScope<NativeBuildOperation>(editor));
		}

		Result<NativeLoadResult> NativeLoad(EditorMethodContext& context, const NoParams& /*params*/)
		{
			EditorContext& editor = context.GetEditor();
			NativeModuleHost& host = editor.GetNativeModule();
			ENGINE_TRY_ASSIGN(const NativeLoadReport report, host.Load(editor));
			NativeLoadResult result;
			result.Reloaded = report.Reloaded;
			result.LayoutChanged = report.LayoutChanged;
			result.SceneChanged = report.Scene.Changed;
			result.UndoCleared = report.UndoCleared;
			result.ComponentCount = report.Scene.ComponentCount;
			result.Diagnostics.reserve(report.Scene.Diagnostics.size());
			for (const LoadDiagnostic& diagnostic : report.Scene.Diagnostics)
				result.Diagnostics.push_back(Utils::MakeNativeComponentDiagnostic(context, diagnostic));
			if (const Ref<const NativeModuleTypes> types = host.GetLoadedTypes(); types != nullptr)
				Utils::SummarizeNativeModule(*types, result.Components, result.Resources, result.Systems);
			return result;
		}

		Result<NativeGetComponentsResult> NativeGetComponents(EditorMethodContext& context, const NativeGetComponentsParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));
			ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(*scene, params.Entity, "/entity"));
			const Ref<const NativeModuleTypes> types = context.GetEditor().GetNativeModule().GetLoadedTypes();
			NativeEntityComponents components = ReadNativeComponents(entity, types.get());
			return NativeGetComponentsResult{ .Entity = context.MakeEntitySummary(entity),
				.Components = Utils::ToComponentMap(std::move(components.Registered)),
				.Unregistered = Utils::ToComponentMap(std::move(components.Unregistered)) };
		}

		Result<NativeSetComponentsResult> NativeSetComponents(EditorMethodContext& context, const NativeSetComponentsParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), true));
			ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(*scene, params.Entity, "/entity"));
			EditorContext& editor = context.GetEditor();
			const Ref<const NativeModuleTypes> types = editor.GetNativeModule().GetLoadedTypes();
			if (types == nullptr)
			{
				return std::unexpected(Error(ErrorCode::InvalidState, "no native module is loaded, so native components cannot be set")
						.WithHint("build and load the game's module with native.build, then native.load (native.status reports why it is not loaded)"));
			}
			// No prefab override records a native component, so the next rebuild of an instance from its prefab would restore
			// the prefab's and lose the edit (decision 27); the play scene, which no prefab rebuilds, takes them.
			const bool editScene = editor.HasScene() && scene == &editor.GetScene();
			if (editScene && entity.HasComponent<PrefabLinkComponent>())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidState, "/entity",
					std::format("'{}' is a member of a prefab instance, whose native components come from its prefab", scene->GetEntityPath(entity)),
					"set native components on the entity before prefab.create, or prefab.unpack the instance first"));
			}

			Json patch = Json::object();
			for (const auto& [name, value] : params.Components)
				patch[name] = value.Get();
			IEntityReferenceResolver* resolver = context.GetEntityReferenceResolver();
			Utils::SceneEntityReferenceResolver sceneResolver(*scene);
			ENGINE_TRY(Utils::ResolveNativeEntityValues(patch, *types, resolver != nullptr ? *resolver : sceneResolver));

			const UUID id = entity.GetUUID();
			SceneEdit edit(editor, std::format("Set Native Components of '{}'", entity.GetName()));
			// The change tracker does not see an entity's preserved components, which hold its native data (Scene::
			// IsTrackedComponent counts registered components only), so the entity is recorded in the edit first: an unchanged
			// patch of a registered component takes its snapshot, as the first change of any component would, and counts in
			// the scene's revision. Undo and redo then restore the entity's whole JSON, its native components included.
			entity.Patch<NameComponent>([](NameComponent& /*name*/) {});
			ENGINE_TRY_ASSIGN(const bool changed, PatchNativeComponents(entity, *types, patch, "/components"));
			uint64_t undoIndex = 0;
			if (changed)
			{
				ENGINE_TRY_ASSIGN(undoIndex, edit.Commit());
			}
			else
			{
				edit.Cancel();
			}

			const Entity updated = scene->FindEntityByID(id);
			return NativeSetComponentsResult{ .Entity = context.MakeEntitySummary(updated),
				.Components = Utils::ToComponentMap(ReadNativeComponents(updated, types.get()).Registered),
				.UndoIndex = ToAutomationCounter(undoIndex) };
		}

	}

	void RegisterNativeMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<NativeTypeRole>("NativeTypeRole", "What a type of a game's native module is.")
			.Entry(NativeTypeRole::Component, "Component", "Data on entities, saved in scenes and prefabs as Native.<Name>.")
			.Entry(NativeTypeRole::Resource, "Resource", "One value per play session, part of the state hash, never saved.");
		registry.Struct<NativeTypeSummary>("NativeTypeSummary", "One type of the loaded native module.")
			.Field("name", &NativeTypeSummary::Name, "The name the module registers.")
			.Field("registryName", &NativeTypeSummary::RegistryName, "A component's name in files and automation (Native.<Name>); a resource's name.")
			.Field("role", &NativeTypeSummary::Role, "Component or Resource.")
			.Field("description", &NativeTypeSummary::Description, "What the module says it is.")
			.Field("schema", &NativeTypeSummary::Schema, "The JSON Schema of its value: every field with its kind, bounds, unit and enum names.");
		registry.Struct<NativeSystemSummary>("NativeSystemSummary", "One system of the loaded native module.")
			.Field("name", &NativeSystemSummary::Name, "The name the module registers.")
			.Field("description", &NativeSystemSummary::Description, "What the module says it does.");
		registry.Struct<NativeBuildDiagnostic>("NativeBuildDiagnostic", "One finding of a native build: the lint, the compiler or the linker.")
			.Field("severity", &NativeBuildDiagnostic::Severity, "Error or Warning.")
			.Field("code", &NativeBuildDiagnostic::Code,
				"NATIVE_LINT_ERROR, NATIVE_COMPILE_ERROR, NATIVE_COMPILE_WARNING (an error, since warnings are errors), NATIVE_LINK_ERROR or NATIVE_BUILD_ERROR.")
			.Field("file", &NativeBuildDiagnostic::File, "The source it is in, project-relative (Native/Spin.cpp); empty without a location.")
			.Field("line", &NativeBuildDiagnostic::Line, "One-based line; zero without a location.")
			.Field("column", &NativeBuildDiagnostic::Column, "One-based column; zero without one.")
			.Field("message", &NativeBuildDiagnostic::Message, "What the tool reported.");
		registry.Struct<NativeComponentDiagnostic>("NativeComponentDiagnostic", "A finding about a native component of the open scene.")
			.Field("severity", &NativeComponentDiagnostic::Severity, "Error or Warning.")
			.Field("code", &NativeComponentDiagnostic::Code,
				"NATIVE_COMPONENT_UNREGISTERED, NATIVE_COMPONENT_INVALID, NATIVE_FIELD_DROPPED or NATIVE_FIELD_RESET.")
			.Field("message", &NativeComponentDiagnostic::Message, "What was found and what was done.")
			.Field("entity", &NativeComponentDiagnostic::Entity, "The entity that holds the component.")
			.Field("pointer", &NativeComponentDiagnostic::Pointer, "The JSON pointer below the entity: /Components/Native.<Name>[/<Field>].");
		registry.Struct<NativeStatusResult>("NativeStatusResult", "The open project's native module: the user's permission, its sources, builds and loaded types.")
			.Field("allowed", &NativeStatusResult::Allowed,
				"The user's Allow native code preference, which only the user changes, in the editor's Native panel; while it is off native.build and native.load are refused.")
			.Field("supported", &NativeStatusResult::Supported, "Whether this platform builds, loads and exports native modules (Windows).")
			.Field("sources", &NativeStatusResult::Sources, "Every .cpp and .h file below Native/, sorted; empty for a game without a native module.")
			.Field("sourceHash", &NativeStatusResult::SourceHash, "The hash of the sources as they are now, which a current build records.")
			.Field("building", &NativeStatusResult::Building, "A native.build runs.")
			.Field("latestBuild", &NativeStatusResult::LatestBuild, "The newest successful build's library, project-relative; empty when there is none.")
			.Field("latestBuildCurrent", &NativeStatusResult::LatestBuildCurrent, "The newest build was made from the sources as they are now, so native.load takes it.")
			.Field("lastBuildRan", &NativeStatusResult::LastBuildRan, "This editor ran a build for the project.")
			.Field("lastBuildSucceeded", &NativeStatusResult::LastBuildSucceeded, "That build succeeded.")
			.Field("lastBuildDiagnostics", &NativeStatusResult::LastBuildDiagnostics, "That build's findings.")
			.Field("loaded", &NativeStatusResult::Loaded, "A module is loaded, so play sessions run its systems.")
			.Field("loadedCurrent", &NativeStatusResult::LoadedCurrent, "The loaded module was built from the sources as they are now.")
			.Field("components", &NativeStatusResult::Components, "The loaded module's component types, in registration order.")
			.Field("resources", &NativeStatusResult::Resources, "The loaded module's resource types, in registration order.")
			.Field("systems", &NativeStatusResult::Systems, "The loaded module's systems, in the order they run each tick.")
			.Field("playResources", &NativeStatusResult::PlayResources,
				"The running Play session's resource values by name; null when no Play session runs the module.");
		registry.Struct<NativeReadParams>("NativeReadParams", "Read a source of the native module.")
			.Field("path", &NativeReadParams::Path, "A .cpp or .h file below Native/.");
		registry.Struct<NativeReadResult>("NativeReadResult", "A native source's exact text.")
			.Field("path", &NativeReadResult::Path, "Canonical project-relative path.")
			.Field("source", &NativeReadResult::Source, "Exact UTF-8 text.");
		registry.Struct<NativeWriteParams>("NativeWriteParams", "Create or replace a source of the native module.")
			.Field("path", &NativeWriteParams::Path, "A .cpp or .h file below Native/; missing folders are created.")
			.Field("source", &NativeWriteParams::Source, "Exact UTF-8 text; empty is written as is.");
		registry.Struct<NativeWriteResult>("NativeWriteResult", "The written source and its undo step.")
			.Field("path", &NativeWriteResult::Path, "Canonical project-relative path.")
			.Field("created", &NativeWriteResult::Created, "The file did not exist before.")
			.Field("undoIndex", &NativeWriteResult::UndoIndex, "Undo step; zero for unchanged bytes or a dry run.");
		registry.Struct<NativeBuildResult>("NativeBuildResult", "A build of the native module in the editor's configuration.")
			.Field("succeeded", &NativeBuildResult::Succeeded, "It compiled and linked; native.load can then load it.")
			.Field("library", &NativeBuildResult::Library, "The built library, project-relative below Library/Native/Builds/; empty when it failed.")
			.Field("sourceHash", &NativeBuildResult::SourceHash, "The hash of the sources it was built from.")
			.Field("diagnostics", &NativeBuildResult::Diagnostics, "The lint's, compiler's and linker's findings, in the order found.")
			.Field("log", &NativeBuildResult::Log, "The build's whole output, project-relative.")
			.Field("seconds", &NativeBuildResult::Seconds, "How long it took (wall-clock time, for information).", { .Unit = "s" });
		registry.Struct<NativeLoadResult>("NativeLoadResult", "The native module loaded, or reloaded in place of the previous one.")
			.Field("reloaded", &NativeLoadResult::Reloaded, "A loaded module was replaced (hot reload).")
			.Field("layoutChanged", &NativeLoadResult::LayoutChanged, "Its types' layout changed; true for a first load.")
			.Field("sceneChanged", &NativeLoadResult::SceneChanged, "The open scene's native components were migrated to the new layout; the scene is now modified.")
			.Field("undoCleared", &NativeLoadResult::UndoCleared, "A load whose layout changed, a first load included, cleared the open scene's undo history, whose snapshots hold the previous layout's values or, recorded without a module, the values as written.")
			.Field("componentCount", &NativeLoadResult::ComponentCount, "Native components in the open scene.")
			.Field("diagnostics", &NativeLoadResult::Diagnostics, "What the migration reported per component and field.")
			.Field("components", &NativeLoadResult::Components, "The module's component types, in registration order.")
			.Field("resources", &NativeLoadResult::Resources, "The module's resource types, in registration order.")
			.Field("systems", &NativeLoadResult::Systems, "The module's systems, in run order.");
		registry.Struct<NativeGetComponentsParams>("NativeGetComponentsParams", "Read an entity's native components.")
			.Field("entity", &NativeGetComponentsParams::Entity, "The entity: its id, a unique id prefix or its path.")
			.Field("target", &NativeGetComponentsParams::Target, "The scene to read; omitted: the play scene while playing, else the edit scene.");
		registry.Struct<NativeGetComponentsResult>("NativeGetComponentsResult", "An entity's native components.")
			.Field("entity", &NativeGetComponentsResult::Entity, "The entity.")
			.Field("components", &NativeGetComponentsResult::Components, "Its components the loaded module registers, by registry name, every field written.")
			.Field("unregistered", &NativeGetComponentsResult::Unregistered,
				"Its other Native.<Name> components, as written: all of them while no module is loaded.");
		registry.Struct<NativeSetComponentsParams>("NativeSetComponentsParams", "Add, change or remove an entity's native components.")
			.Field("entity", &NativeSetComponentsParams::Entity, "The entity: its id, a unique id prefix or its path.")
			.Field("components", &NativeSetComponentsParams::Components,
				"By registry name (Native.<Name>): a merge patch of the component's fields, a missing component starting from its defaults, or null to remove it.")
			.Field("target", &NativeSetComponentsParams::Target, "The scene to change; play changes are transient.");
		registry.Struct<NativeSetComponentsResult>("NativeSetComponentsResult", "The entity's native components after the change.")
			.Field("entity", &NativeSetComponentsResult::Entity, "The entity.")
			.Field("components", &NativeSetComponentsResult::Components, "Its native components the loaded module registers, every field written.")
			.Field("undoIndex", &NativeSetComponentsResult::UndoIndex, "Undo step; zero for the play scene and for a dry run.");
	}

	void RegisterNativeMethods(MethodRegistry& methods)
	{
		const Json setExample = Json{ { "entity", "/Spinner" }, { "components", Json{ { "Native.Spinner", Json{ { "DegreesPerSecond", 180 } } } } } };
		methods.Add({ .Name = "native.status",
						.Description = "Reports the open project's native module: whether the user allows native code, its sources, its builds, the loaded module's types and systems, and in play its resources.",
						.ExposeAsTool = true,
						.AllowedInBatch = true,
						.Examples = { { .Description = "Read the native module's state.", .Params = Json::object() } } },
			&Automation::NativeStatus);
		methods.Add({ .Name = "native.read",
						.Description = "Reads a source file of the native module (below Native/) exactly.",
						.RequiredParams = { "path" },
						.ExposeAsTool = true,
						.AllowedInBatch = true,
						.Examples = { { .Description = "Read a source.", .Params = Json{ { "path", "Native/Spinner.cpp" } } } } },
			&Automation::NativeRead);
		methods.Add({ .Name = "native.write",
						.Description = "Writes a .cpp or .h source of the native module below Native/ as one undoable edit, recorded in provenance; it builds nothing (native.build).",
						.RequiredParams = { "path", "source" },
						.ExposeAsTool = true,
						.Mutates = true,
						.SupportsDryRun = true,
						.AllowedInBatch = true,
						.Examples = { { .Description = "Write a source.", .Params = Json{ { "path", "Native/Spinner.cpp" }, { "source", "#include \"Engine/Native/NativeApi.h\"\n" } } } } },
			&Automation::NativeWrite);
		methods.AddPending<EditorMethodContext, NoParams, NativeBuildResult>(
			{ .Name = "native.build",
				.Description = "Builds the native module in the editor's configuration (Windows; needs the user's Allow native code preference); resolves with the build's diagnostics. native.load then loads it.",
				.ExposeAsTool = true,
				.TimeoutSeconds = 900,
				.Examples = { { .Description = "Build the native module.", .Params = Json::object() } } },
			&Automation::NativeBuild);
		methods.Add({ .Name = "native.load",
						.Description = "Loads the newest current build of the native module, or hot-reloads it, migrating the open scene's native components to a changed layout (Windows; needs the user's Allow native code preference; not while playing).",
						.ExposeAsTool = true,
						.Examples = { { .Description = "Load the built module.", .Params = Json::object() } } },
			&Automation::NativeLoad);
		methods.Add({ .Name = "native.getComponents",
						.Description = "Reads an entity's native components (Native.<Name>) as the loaded module writes them, and those it does not register as written.",
						.RequiredParams = { "entity" },
						.ExposeAsTool = true,
						.AllowedInBatch = true,
						.Examples = { { .Description = "Read the spinner's native components.", .Params = Json{ { "entity", "/Spinner" } } } } },
			&Automation::NativeGetComponents);
		methods.Add({ .Name = "native.setComponents",
						.Description = "Adds, changes (a merge patch) or removes (null) an entity's native components through the loaded module, as one undoable edit.",
						.RequiredParams = { "entity", "components" },
						.ExposeAsTool = true,
						.Mutates = true,
						.SupportsDryRun = true,
						.AllowedInBatch = true,
						.Examples = { { .Description = "Make the spinner turn faster.", .Params = setExample } } },
			&Automation::NativeSetComponents);
	}

}
