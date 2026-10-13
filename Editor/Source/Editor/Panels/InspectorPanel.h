#pragma once

#include "EditorCore/Inspector/ReflectedEditController.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/Value.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

	class StructInfo;
	struct EditorPanelContext;

	// Registry-driven components/categories, multi-edit, add/remove, prefab override highlighting and per-field revert.
	// Uses ReflectedEditController/DrawReflectedValue including Map/Variant; asset selection shows native properties and
	// import settings. Begin on activation, commit on release, cancel ESC. Read-only controls remain visible and disabled.
	// A field released in the same frame another one activates is never dropped: its commit runs at the next safe point,
	// then the newer gesture begins against the committed state. Only the active field's release queues a commit.
	// A multi-edit of a vector changes only the components the gesture touched. Selected ids missing from the target
	// scene are skipped. Runtime (Play) content is tinted with the play accent. A failed action shows a dismissible
	// error until the selection changes or a later action succeeds. Asset properties are parsed once per asset version,
	// edit or refresh interval, never every frame; a read or parse failure is shown inline. Replacing the inspected owner
	// (the project binding, the edit scene document or the play session and its scene) discards a pending gesture or typed
	// name; edits, undo/redo and project-settings changes keep them.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class InspectorPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		// A gesture that started while another still owned the controller (released, or ending in the same frame).
		struct PendingEdit
		{
			InspectorEditTarget Target{};
			Value Candidate{};
			uint32_t Components = 0; // ReflectedDrawerResult::ChangedComponents, accumulated
			bool Mixed = false;
			bool Changed = false;
			bool Committed = false;
		};
		// The selected asset's parsed native properties and import-settings schema, refreshed only when its key changes.
		struct AssetProperties
		{
			AssetHandle Asset{};
			uint64_t ProjectGeneration = 0;
			uint64_t Version = 0;
			uint64_t HistorySequence = 0;
			size_t HistoryUndoCount = 0;
			uint64_t Revision = 0;
			double RefreshAt = 0.0;               // ImGui time of the next re-read (external edits)
			const StructInfo* Type = nullptr;     // native properties schema; immutable registry data
			const StructInfo* Settings = nullptr; // import settings schema; immutable registry data
			Json Values{};
			std::string Error{};
		};

		void ReportFailure(const Error& error);
		[[nodiscard]] Status ApplyQueuedEdit(EditorPanelContext& context);
		// Cancels the active gesture and any pending one (selection, owner or project changes, Escape, structural actions).
		void CancelEdit(EditorPanelContext& context);
		// Ends only the active gesture (it finished without a change, or its value failed validation).
		void EndActiveEdit(EditorPanelContext& context);
		// Begins `pending` as the active gesture: Begin, its latest candidate as the preview, and its release.
		void BeginPendingEdit(EditorPanelContext& context, PendingEdit pending);
		[[nodiscard]] Status CommitActiveEdit(EditorPanelContext& context);
		[[nodiscard]] const AssetProperties& ReadAssetProperties(EditorPanelContext& context, AssetHandle asset);
		std::optional<InspectorEditTarget> m_ActiveTarget{};
		std::optional<PendingEdit> m_Pending{};
		uint32_t m_ActiveComponents = 0;
		bool m_ActiveMixed = false;
		std::string m_OwnerIdentity{};
		std::string m_Error{};
		std::vector<UUID> m_ErrorSelection{};
		AssetHandle m_ErrorAsset{};
		std::vector<uint64_t> m_Tickets{};
		std::string m_ComponentSearch{};
		std::optional<AssetProperties> m_AssetProperties{};
		bool m_CommitQueued = false;
		friend class EditorLayer;
	};

}
