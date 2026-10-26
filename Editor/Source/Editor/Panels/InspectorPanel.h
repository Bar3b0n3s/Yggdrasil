#pragma once

#include "EditorCore/Inspector/PrefabInstanceView.h"
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
#include <string_view>
#include <vector>

namespace Engine {

	class StructInfo;
	struct EditorPanelContext;

	// Registry-driven components/categories, multi-edit, add/remove, prefab override highlighting and per-field revert.
	// One selected entity of a prefab instance in the edit scene shows its instance (Docs/Decisions/0031-m20-contract.md
	// decisions 7 to 9): Apply with the levels it offers (prefab.apply {instance, level}: the entity's innermost instance,
	// the outermost level first and selected by default) and the list of the overrides it would write, RemoveEntity rows
	// included, each with its Revert (prefab.revert {overrides}); a member of a nested instance highlights and reverts the
	// overrides its outermost instance records for it (PrefabInstantiator::GetOverridePath). The Script component shows
	// its slots in order (decision 17), each with Up, Down and Remove and its per-slot overrides (decision 19), and Add
	// script appends an empty slot: one edit.batch of entity.update per change (MakeScriptSlotChange), applied by index
	// to every selected entity; a slot moves or goes only when every selected entity has it and the slot it swaps with.
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
		// The inspected entity's prefab instance (DescribePrefabInstance), re-read when the entity, the inspected owner, the
		// editor revision or the outermost prefab's version changes, so an override list is never rebuilt every frame.
		struct PrefabViewCache
		{
			UUID Entity{};
			std::string Owner{};
			uint64_t Revision = 0;
			uint64_t PrefabVersion = 0;
			std::optional<PrefabInstanceView> View{};
			std::string Error{};
		};

		void ReportFailure(const Error& error);
		void QueueAction(EditorPanelContext& context, std::string_view method, const Json& params);
		[[nodiscard]] const PrefabViewCache& ReadPrefabView(EditorPanelContext& context, Entity entity);
		// Apply with its level picker and the override list of `view` (one selected entity of an instance, edit scene).
		void DrawPrefabInstance(EditorPanelContext& context, const PrefabInstanceView& view);
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
		std::optional<PrefabViewCache> m_PrefabView{};
		UUID m_ApplyInstance{}; // the instance the level picker belongs to
		UUID m_ApplyLevel{};    // the picked level's root; reset to the outermost for another instance
		bool m_CommitQueued = false;
		friend class EditorLayer;
	};

}
