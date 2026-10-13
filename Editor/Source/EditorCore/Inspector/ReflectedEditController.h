#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/Value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	class EditorContext;

	enum class InspectorTargetKind : uint8_t
	{
		Component,
		AssetProperties,
		AssetImportSettings,
		ProjectSettings
	};

	struct InspectorEditTarget
	{
		InspectorTargetKind Kind = InspectorTargetKind::Component;
		std::vector<UUID> Entities{}; // component edits; all validated before any write
		AssetHandle Asset{};          // native/import settings edits
		std::string Component{};
		std::string FieldPath{};                // reflected path, including nested array index or map key
		SceneTarget Target = SceneTarget::Edit; // panels explicitly use Play for transient runtime component edits
	};

	// CPU half of registry-driven drawers. Owns copied values/UUIDs only, no field-storage pointer across a frame.
	// Main thread, context outlives the controller. Begin/Preview/Commit model activation/scrub/release; preview is owned
	// editor state, not a scene mutation. Commit routes through SceneEdit, AssetEditCommand or ProjectSettingsCommand.
	// Map add/remove/rename and array edits are full validated Value replacements; duplicate map keys are invalid.
	// Variant resolution uses FieldInfo::Resolver plus the actual owner/key and schema provider. An unresolved Variant
	// stays visible and read-only as raw JSON with its diagnostic; never silently reset or replaced by a guessed type.
	class ReflectedEditController
	{
	public:
		explicit ReflectedEditController(EditorContext& context);
		~ReflectedEditController();
		ReflectedEditController(const ReflectedEditController&) = delete;
		ReflectedEditController& operator=(const ReflectedEditController&) = delete;
		// InvalidState already editing/no target; PermissionDenied read-only; NotFound missing field/entity/asset;
		// Unsupported unresolved Variant or read-only field. Multi-edit requires equal field schemas.
		[[nodiscard]] Status Begin(const InspectorEditTarget& target);
		// Validate candidate with FieldInfo metadata/resolver against the owned snapshots, then run the real setters on an
		// isolated copy of the edited entities and their ancestors (never the whole scene, so a drag frame costs the same in
		// any scene). Validation never changes preview or target. Commit repeats the setter check on a whole-scene copy.
		// `components`: for a Vec2, Vec3, Vec4 or Bool3 field (also as a script Variant), the components the gesture
		// changed (bit i is component i); every target keeps its own other components. 0 replaces the whole value.
		[[nodiscard]] Status Preview(const Value& value, uint32_t components = 0);
		// Candidate copy; InvalidState idle.
		[[nodiscard]] Result<Value> GetPreview() const;
		// Exactly one undo command in Edit; runtime component edits use SceneEdit's transient path, index 0.
		// Conflict if the target changed since Begin, nothing then written: the scene document or its revision (the project
		// settings for a project-settings target, so a settings change alone never rejects a scene edit), the asset version,
		// the play session or its scene's revision, or the edited values themselves.
		[[nodiscard]] Result<uint64_t> Commit();
		void Cancel();
		[[nodiscard]] bool IsEditing() const;
	private:
		struct State;
		Scope<State> m_State;
	};

}
