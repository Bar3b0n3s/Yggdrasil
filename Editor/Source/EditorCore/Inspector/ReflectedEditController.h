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
		// Validate candidate with FieldInfo metadata/resolver. Validation never changes preview or target.
		[[nodiscard]] Status Preview(const Value& value);
		// Candidate copy; InvalidState idle.
		[[nodiscard]] Result<Value> GetPreview() const;
		// Exactly one undo command in Edit; runtime component edits use SceneEdit's transient path, index 0.
		// Conflict if target revision/version/session changed since Begin, nothing then written.
		[[nodiscard]] Result<uint64_t> Commit();
		void Cancel();
		[[nodiscard]] bool IsEditing() const;
	};

}
