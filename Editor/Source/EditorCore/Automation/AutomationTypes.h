#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>
#include <string>

// Reflected types shared by several editor method domains (the param-struct conventions of
// Engine/Automation/Protocol/MethodRegistry.h). Registered once by RegisterAutomationCommonTypes, before every domain's
// types (RegisterEditorMethodTypes).
//
// 64-bit counters: the registry has no 64-bit integer FieldType, so revisions (EditorContext::GetRevision), undo indexes
// and ticks travel as uint32_t in param and result structs (JSON consumers lose precision above 2^53 anyway). A value
// above UINT32_MAX is reported as UINT32_MAX (ADR 0008 decision 8). Log and event cursors are decimal strings of the full
// 64-bit sequence instead (convention 9 of MethodRegistry.h), and ifRevision, transcriptLine and "_meta" are read or
// written outside the registry and keep 64 bits.

namespace Engine {

	class TypeRegistry;

	// Registry enum "SceneTarget" (§13.4 "Target"): which scene a request reads or changes. A param struct declares the
	// member "target"; when absent (MethodContext::HasParam), reads use the play scene while playing and mutations the edit
	// scene. Mutations of the play scene must say "play"; they are transient (no undo). There is no play scene before M7,
	// so "play" is InvalidState "not playing" in M4.
	enum class SceneTarget : uint8_t
	{
		Edit,
		Play
	};

	// Registry struct "EntitySummary" (§13.4: canonical ids plus readable names and paths): {id, name, path}.
	struct EntitySummary
	{
		std::string Id{}; // 16 lowercase hex digits
		std::string Name{};
		std::string Path{}; // Scene::GetEntityPath, which FindEntityByPath resolves back to the entity
	};

	// Registry struct "NoParams": the params of methods that take none (session.info, project.save, component.list ...).
	// It has no fields, so any member is an unknown-member InvalidParams.
	struct NoParams
	{
	};

	// Saturating conversion for the counters of the file comment.
	[[nodiscard]] constexpr uint32_t ToAutomationCounter(uint64_t value)
	{
		return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
	}

	// Registers SceneTarget, EntitySummary and NoParams, plus the engine enums automation reports by name: CommandOrigin
	// ("CommandOrigin"), DiagnosticSeverity ("DiagnosticSeverity"), LogLevel ("LogLevel"), LogChannel ("LogChannel") and
	// EngineEventType ("EngineEventType").
	void RegisterAutomationCommonTypes(TypeRegistry& registry);

}
