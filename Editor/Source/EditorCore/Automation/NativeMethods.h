#pragma once

#include "EditorCore/Native/NativeBuild.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Result.h"
#include "Engine/Native/NativeApi.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The native.* methods (Docs/Decisions/0030-m19-contract.md decision 32): what an agent needs to build a game with a native
// module through automation alone (Architecture §1.1 G4): its sources (native.read, native.write, recorded in provenance like
// every authored file, decision 35), its build and load (native.build, native.load, through NativeModuleHost), its
// components on entities (native.getComponents, native.setComponents) and its state (native.status). Editor only: the
// Runtime links its module and builds nothing (its scene's native components are read with scene.get). Every one of them is
// an MCP tool. None takes, sets or writes the user's "Allow native code" preference (decision 29): native.status only reports
// it, and while it is off native.build and native.load are refused with PermissionDenied, whatever the request.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class PendingOperation;
	class TypeRegistry;

	// Registry struct "NativeTypeSummary": one type of the loaded module.
	struct NativeTypeSummary
	{
		std::string Name{};                              // the module's name ("Spinner")
		std::string RegistryName{};                      // a component's name in files and automation ("Native.Spinner"); a resource's Name
		NativeTypeRole Role = NativeTypeRole::Component; // registry enum "NativeTypeRole"
		std::string Description{};
		VariantValue Schema{}; // GetNativeTypeSchema: the JSON Schema of its canonical JSON
	};

	// Registry struct "NativeSystemSummary": one system of the loaded module, in run order.
	struct NativeSystemSummary
	{
		std::string Name{};
		std::string Description{};
	};

	// Registry struct "NativeComponentDiagnostic": a native component's finding (a NATIVE_* code of Native/NativeModule.h).
	struct NativeComponentDiagnostic
	{
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning;
		std::string Code{};
		std::string Message{};
		EntitySummary Entity{};
		std::string Pointer{}; // below the entity: "/Components/Native.Spinner/Speed"
	};

	// native.status takes NoParams.
	struct NativeStatusResult
	{
		bool Allowed = false;               // the user's "Allow native code" preference; no method sets it
		bool Supported = false;             // this platform builds, loads and exports native modules (Windows)
		std::vector<std::string> Sources{}; // every source below Native/, project-relative, sorted; empty: no native module
		std::string SourceHash{};           // the sources' hash now (ComputeNativeSourceHash); empty without sources
		bool Building = false;
		// The newest successful build of the editor's configuration (project-relative library path; empty when none) and
		// whether it was built from the sources as they are now.
		std::string LatestBuild{};
		bool LatestBuildCurrent = false;
		// The last build this editor ran for the project, with its diagnostics; LastBuildRan false before the first.
		bool LastBuildRan = false;
		bool LastBuildSucceeded = false;
		std::vector<NativeBuildDiagnostic> LastBuildDiagnostics{};
		bool Loaded = false;
		bool LoadedCurrent = false;                  // the loaded module was built from the sources as they are now
		std::vector<NativeTypeSummary> Components{}; // the loaded module's, in registration order; empty when none is loaded
		std::vector<NativeTypeSummary> Resources{};
		std::vector<NativeSystemSummary> Systems{};
		// The play session's resources ({"<Name>": <value>, ...}, NativeWorld::GetResources) while a Play session runs the
		// module; null otherwise.
		VariantValue PlayResources{};
	};

	struct NativeReadParams
	{
		std::string Path{}; // a source below Native/: "Native/Spin.cpp" (project-relative or project://)
	};

	struct NativeReadResult
	{
		std::string Path{};   // canonical project-relative path
		std::string Source{}; // the exact UTF-8 bytes
	};

	struct NativeWriteParams
	{
		std::string Path{};   // a .cpp or .h file below Native/, created with its folders when missing
		std::string Source{}; // the exact UTF-8 text; empty is written as is
	};

	struct NativeWriteResult
	{
		std::string Path{};
		bool Created = false; // the file did not exist before
		uint32_t UndoIndex = 0;
	};

	// native.build takes NoParams (the editor's own configuration, decision 30) and resolves with:
	struct NativeBuildResult
	{
		bool Succeeded = false;
		std::string Library{}; // the built library, project-relative (below Library/Native/Builds/); empty when it failed
		std::string SourceHash{};
		std::vector<NativeBuildDiagnostic> Diagnostics{}; // registry struct "NativeBuildDiagnostic"
		std::string Log{};                                // the build's whole output, project-relative
		float Seconds = 0.0f;
	};

	// native.load takes NoParams.
	struct NativeLoadResult
	{
		bool Reloaded = false;      // a loaded module was replaced (hot reload)
		bool LayoutChanged = false; // its types' layout changed (always true for a first load)
		bool SceneChanged = false;  // the open scene's native components were migrated: the scene is now modified
		bool UndoCleared = false;   // a load whose layout changed (a first load included) cleared the open scene's undo history
		uint32_t ComponentCount = 0;
		std::vector<NativeComponentDiagnostic> Diagnostics{};
		std::vector<NativeTypeSummary> Components{};
		std::vector<NativeTypeSummary> Resources{};
		std::vector<NativeSystemSummary> Systems{};
	};

	struct NativeGetComponentsParams
	{
		std::string Entity{}; // an EntityRef param (§13.4): id, unique id prefix or path
		SceneTarget Target = SceneTarget::Edit;
	};

	struct NativeGetComponentsResult
	{
		EntitySummary Entity{};
		// The entity's native components the loaded module registers, by registry name ("Native.Spinner"), as the scene
		// holds them: canonical (every field written) once the editor canonicalized or wrote them, but as stored for those it
		// did not (a scene Scene.Load loaded during play, a prefab instance member from a prefab saved under an older layout;
		// decisions 27 and 28).
		std::map<std::string, VariantValue> Components{};
		// Its other "Native." components, as written (all of them while no module is loaded).
		std::map<std::string, VariantValue> Unregistered{};
	};

	struct NativeSetComponentsParams
	{
		std::string Entity{};
		// {"Native.<Name>": <merge patch of its canonical JSON> | null}: a component the entity lacks is added from its type's
		// defaults; null removes it (PatchNativeComponents).
		std::map<std::string, VariantValue> Components{};
		SceneTarget Target = SceneTarget::Edit;
	};

	struct NativeSetComponentsResult
	{
		EntitySummary Entity{};
		// The entity's native components after the change, as the scene holds them: the patched ones canonical, the others as
		// NativeGetComponentsResult::Components reads them.
		std::map<std::string, VariantValue> Components{};
		uint32_t UndoIndex = 0; // 0 for the play scene (transient) and for a dry run
	};

	namespace Automation {

		// The handlers (decision 32; stream N-D). Paths below Native/ resolve like script.read's below Assets/ (ResolveProjectPath,
		// then the Native/ confinement: InvalidArgument at /path for another folder, an extension other than .cpp or .h, or a
		// path that escapes). native.write goes through AssetEditCommand (one undo step) and EditorContext's write path, so
		// provenance records it (decision 35); it is refused while a build runs, an export's Runtime build included
		// (NativeModuleHost::IsRuntimeBuildRunning), with InvalidState. native.build and native.load are NativeModuleHost's
		// StartBuild/PollBuild and Load, with their errors; a client's disconnect cancels a build.
		// native.setComponents is one SceneEdit (one undo step) for the edit scene, a transient change for the play scene
		// (target Play), and needs a loaded module (InvalidState otherwise); its errors are PatchNativeComponents' located
		// below /components. In the edit scene it refuses a member of a prefab instance (an entity with a PrefabLink) with
		// InvalidState at /entity: no prefab override records native components, so the next rebuild of the instance from
		// its prefab (a prefab change, scene.open) would restore the prefab's and lose the edit silently; the hint says to
		// set them on the entity before prefab.create, or to prefab.unpack the instance (decision 27). The play scene,
		// which no prefab rebuilds, takes them. native.getComponents reads ReadNativeComponents. Entity params resolve as
		// entity.get's do.
		[[nodiscard]] Result<NativeStatusResult> NativeStatus(EditorMethodContext& context, const NoParams& params);
		[[nodiscard]] Result<NativeReadResult> NativeRead(EditorMethodContext& context, const NativeReadParams& params);
		[[nodiscard]] Result<NativeWriteResult> NativeWrite(EditorMethodContext& context, const NativeWriteParams& params);
		[[nodiscard]] Result<Scope<PendingOperation>> NativeBuild(EditorMethodContext& context, const NoParams& params);
		[[nodiscard]] Result<NativeLoadResult> NativeLoad(EditorMethodContext& context, const NoParams& params);
		[[nodiscard]] Result<NativeGetComponentsResult> NativeGetComponents(EditorMethodContext& context, const NativeGetComponentsParams& params);
		[[nodiscard]] Result<NativeSetComponentsResult> NativeSetComponents(EditorMethodContext& context, const NativeSetComponentsParams& params);

	}

	// Registers the enum "NativeTypeRole" and the structs above ("NativeBuildDiagnostic" included).
	void RegisterNativeMethodTypes(TypeRegistry& registry);
	// The seven editor-only methods, every one a tool. status, read and getComponents: pure reads, AllowedInBatch. write and
	// setComponents: Mutates, SupportsDryRun, AllowedInBatch. build: pending (NativeBuildBoundSeconds and a minute,
	// NativeBuild.h). load: immediate, neither a batch op nor a dry run.
	void RegisterNativeMethods(MethodRegistry& methods);

}
