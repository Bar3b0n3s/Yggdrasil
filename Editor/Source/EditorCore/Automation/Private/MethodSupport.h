#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Scene/LoadReport.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What the editor method domains share inside EditorCore/Automation (the param-struct conventions of
// Engine/Automation/Protocol/MethodRegistry.h made concrete): located errors, the summaries several
// results carry, and the scene file operations of scene.*, project.* and session.shutdown, so every domain saves, loads
// and reports a scene the same way.

namespace Engine {

	class AutomationMethodContext;
	class ComponentInfo;
	class ConstEntity;
	class EditorContext;
	class Entity;
	class Scene;
	class TypeInfo;
	class UUIDGenerator;
	struct ProjectSummary;
	struct SceneSummary;

	namespace Utils {

		// `error` with its issues replaced by `issues`; code, message, contexts, location and hint are kept.
		[[nodiscard]] Error ReplaceIssues(const Error& error, std::vector<ErrorIssue> issues);

		// `error` relocated below `prefix` (a JSON pointer such as "/components/RigidBody"): the location's pointer and every
		// issue's pointer are prefixed; an unset location pointer becomes `prefix` itself.
		[[nodiscard]] Error PrefixPointers(const Error& error, std::string_view prefix);

		// `error` located at the param `pointer`: the location's pointer is set to it, and issues without a pointer (located
		// at the root of whatever produced them) are moved there too.
		[[nodiscard]] Error LocateAtParam(const Error& error, std::string_view pointer);

		// An error of `code` located at the param `pointer`, with an optional hint.
		[[nodiscard]] Error MakeParamError(ErrorCode code, std::string_view pointer, std::string message, std::string hint = {});

		// The project-relative text of a project:// path ("Assets/Scenes/Main.scene").
		[[nodiscard]] std::string ToProjectRelative(const VfsPath& path);

		// The 16 lowercase hex digits of a valid UUID, or "" for the invalid one.
		[[nodiscard]] std::string FormatOptionalUUID(UUID id);

		// The open project as project.* results report it (asserted: a project is open).
		[[nodiscard]] ProjectSummary MakeProjectSummary(const EditorContext& editor);

		// The open scene as scene.* results report it (asserted: a scene is open).
		[[nodiscard]] SceneSummary MakeSceneSummary(const EditorContext& editor);

		// Writes the open scene's canonical document to `path` through EditorContext::WriteProjectFile (provenance; it creates
		// the directory), and marks the scene saved there. Errors: InvalidState without an open scene; PermissionDenied for a
		// read-only editor; Validation for a scene that cannot be serialized; the write errors.
		[[nodiscard]] Status SaveOpenScene(EditorContext& editor, const VfsPath& path);

		// The path the open scene is saved to by a call that does not name one (scene.save {}, project.save, session.shutdown
		// {save}, scene.open {save}): its own path. Errors: InvalidState for a scene that was never saved, with a hint naming
		// scene.save {path}.
		[[nodiscard]] Result<VfsPath> GetOwnScenePath(const EditorContext& editor);

		// What scene.open, scene.new and session.shutdown do with a dirty open scene before they replace or leave it (§13.5:
		// exactly one of save and discardChanges is required). `save` writes it to its own path; `discard` drops it;
		// `discardFlag` names the param that discards ("discardChanges", "force") in the error. Errors: InvalidState for a
		// dirty scene with neither; the save's errors. No effect for a clean scene or without one.
		[[nodiscard]] Status ResolveDirtyScene(EditorContext& editor, bool save, bool discard, std::string_view discardFlag);

		// InvalidState unless the open scene is clean or `save` or `discard` is given (the check of ResolveDirtyScene without
		// its effect), so a call can refuse before it changes anything.
		[[nodiscard]] Status CheckDirtyScene(const EditorContext& editor, bool save, bool discard, std::string_view discardFlag);

		// Reads `path` into `scene` (empty) with `mode`: options.SourcePath is the project-relative path, Repair loads draw
		// their fresh ids from `repairIds`. Errors: the VFS read errors (an operating-system access failure as Io,
		// ToEditorFileError) and those of SceneSerializer::LoadFromFile.
		[[nodiscard]] Status LoadSceneFile(const EditorContext& editor, Scene& scene, const VfsPath& path, LoadMode mode, UUIDGenerator* repairIds,
			LoadReport& report);

		// The one log line of a load diagnostic, of a scene file and a project file alike, which _meta.diagnostics.firstNew
		// hands to agents (§13.4): "'<file>' <pointer>: <message> (<code>)" (CodeStyle §11 quotes paths), with "(root)" for
		// an empty pointer and without " (<code>)" when there is no code.
		[[nodiscard]] std::string FormatLoadDiagnostic(std::string_view file, std::string_view pointer, std::string_view message, std::string_view code);

		// Logs FormatLoadDiagnostic at Error for an Error severity, at Warn otherwise.
		void LogLoadDiagnostic(std::string_view file, DiagnosticSeverity severity, std::string_view pointer, std::string_view message, std::string_view code);

		// Logs every diagnostic of a scene load (LogLoadDiagnostic), so a load warning reaches the "_meta" diagnostics delta
		// of the response (§13.4).
		void LogLoadDiagnostics(std::string_view file, const LoadReport& report);

		// Rewrites every enum spelling inside `value`, a JSON value of `type`, to its canonical name where it matches one
		// ignoring ASCII case (§13.4 "Enum values are parsed case-insensitively"): struct members, array and map elements and
		// nested structs; Variant values and unrecognized spellings are left alone (the strict read reports those). Used for
		// documents the registry reads as a whole (project.setSettings's merge patch).
		void CanonicalizeEnumSpellings(Json& value, const TypeInfo& type);

		// The components of `entity` that results report (serializable and not entity-level, Hidden ones included because
		// they are readable), in registry order, which is the order scene files use.
		[[nodiscard]] std::vector<const ComponentInfo*> GetEntityComponents(ConstEntity entity);

		// The decimal 64-bit cursor of log.read and events.read: "" is 0 (the oldest held entry), digits are the sequence
		// number, and "end" is nullopt (read nothing; the caller answers with the next sequence number). Errors:
		// InvalidArgument at "/cursor" for anything else.
		[[nodiscard]] Result<std::optional<uint64_t>> ParseSequenceCursor(std::string_view cursor);

		// The entity cap of play sessions (§5.7, M7): OK when `scene` is not the play session's scene, else
		// PlaySession::CheckEntityCapacity(additional) (InvalidState "entity limit <N> reached"). entity.create and
		// entity.duplicate call it before they create entities in a "play" target.
		[[nodiscard]] Status CheckPlayEntityCapacity(const AutomationMethodContext& context, const Scene& scene, size_t additional);

		// The number of entities in the subtrees of `roots` (valid, asserted; disjoint subtrees, as ResolveEntityRoots gives).
		[[nodiscard]] size_t CountSubtreeEntities(std::span<const Entity> roots);

	}

}
