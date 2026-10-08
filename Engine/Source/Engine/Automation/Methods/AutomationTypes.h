#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Core/Base.h"

#include <cstdint>
#include <string>

// Reflected types shared by the automation method domains of the Editor and the Runtime (the param-struct conventions of
// Engine/Automation/Protocol/MethodRegistry.h; Architecture §3 "Automation/Methods", §13.4). They moved here from
// EditorCore/Automation/AutomationTypes.h with the M7 contract (Docs/Decisions/0008-m4-decisions.md decision 26,
// Docs/Decisions/0012-m7-decisions.md decision 12), unchanged in name and wire format, because the handlers the two hosts
// share (AutomationMethodContext) use them. RegisterAutomationSharedTypes registers them before every domain's types.
//
// 64-bit counters: the registry has no 64-bit integer FieldType, so revisions, undo indexes and ticks travel as uint32_t
// in param and result structs (JSON consumers lose precision above 2^53 anyway). A value above UINT32_MAX is reported as
// UINT32_MAX (ADR 0008 decision 8). Log and event cursors are decimal strings of the full 64-bit sequence instead
// (convention 9 of MethodRegistry.h), and ifRevision, transcriptLine and "_meta" are read or written outside the registry
// and keep 64 bits. State hashes are 16 lowercase hex digits (XXH64), never JSON numbers (§4.8).

namespace Engine {

	class TypeRegistry;

	// Registry enum "SceneTarget" (§13.4 "Target"): which scene a request reads or changes. A param struct declares the
	// member "target"; when absent (MethodContext::HasParam), reads use the play scene while playing and mutations the edit
	// scene. Mutations of the play scene must say "play"; they are transient (no undo). The Runtime has only a play scene:
	// "edit" is InvalidState there.
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

	// Registry struct "AssetSummary" (§7.1: "Responses always expand references to {id, path, type}"): an asset as every
	// asset-related result names it (M6). Registered with the registry enum "AssetType" (Engine/Asset/AssetType.h, every
	// name including "None") by RegisterAutomationSharedTypes, before the domains that use them.
	struct AssetSummary
	{
		std::string Id{};   // 16 lowercase hex digits
		std::string Path{}; // the readable reference (AssetManager::GetReferencePath): "Assets/Models/Track.glb#mesh:0:Straight"
		AssetType Type = AssetType::None;
	};

	// Registry struct "NoParams": the params of methods that take none (session.info, play.state, project.save ...). It has no
	// fields, so any member is an unknown-member InvalidParams.
	struct NoParams
	{
	};

	// Saturating conversion for the counters of the file comment.
	[[nodiscard]] constexpr uint32_t ToAutomationCounter(uint64_t value)
	{
		return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
	}

	// Registers SceneTarget, EntitySummary, AssetSummary and NoParams, plus the engine enums automation reports by name:
	// DiagnosticSeverity ("DiagnosticSeverity"), LogLevel ("LogLevel"), LogChannel ("LogChannel"), EngineEventType
	// ("EngineEventType") and AssetType ("AssetType"). Called once per registry, before every domain's types: by the
	// editor's RegisterAutomationCommonTypes (EditorCore, which adds the editor's own CommandOrigin) and by the Runtime's
	// RegisterSharedMethodTypes caller (RuntimeAutomationServer.h).
	void RegisterAutomationSharedTypes(TypeRegistry& registry);

}
