#include "EditorPCH.h"
#include "EditorCore/Automation/NativeMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/Error.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <format>
#include <string_view>

namespace Engine {

	namespace Utils {

		// The refusal of a native.* handler until stream N-D lands it.
		[[nodiscard]] static std::unexpected<Error> MakeNativeMethodStubError(std::string_view method)
		{
			return std::unexpected(Error(ErrorCode::Unsupported, std::format("{} (Docs/Decisions/0030-m19-contract.md decision 32, stream N-D)", method)));
		}

	}

	namespace Automation {

		Result<NativeStatusResult> NativeStatus(EditorMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return Utils::MakeNativeMethodStubError("native.status");
		}

		Result<NativeReadResult> NativeRead(EditorMethodContext& /*context*/, const NativeReadParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return Utils::MakeNativeMethodStubError("native.read");
		}

		Result<NativeWriteResult> NativeWrite(EditorMethodContext& /*context*/, const NativeWriteParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return Utils::MakeNativeMethodStubError("native.write");
		}

		Result<Scope<PendingOperation>> NativeBuild(EditorMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return Utils::MakeNativeMethodStubError("native.build");
		}

		Result<NativeLoadResult> NativeLoad(EditorMethodContext& /*context*/, const NoParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return Utils::MakeNativeMethodStubError("native.load");
		}

		Result<NativeGetComponentsResult> NativeGetComponents(EditorMethodContext& /*context*/, const NativeGetComponentsParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return Utils::MakeNativeMethodStubError("native.getComponents");
		}

		Result<NativeSetComponentsResult> NativeSetComponents(EditorMethodContext& /*context*/, const NativeSetComponentsParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return Utils::MakeNativeMethodStubError("native.setComponents");
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
			.Field("undoCleared", &NativeLoadResult::UndoCleared, "A reload whose layout changed cleared the undo history, whose snapshots hold the previous layout's values.")
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
