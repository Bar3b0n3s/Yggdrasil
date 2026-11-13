#pragma once

#include "EditorCore/Graph/GraphEditorTarget.h"
#include "EditorCore/Graph/GraphLiveView.h"
#include "EditorCore/Inspector/PrefabInstanceView.h"
#include "EditorCore/Inspector/ReflectedEditController.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/Value.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	class FieldInfo;
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
	// M21 (Docs/Decisions/0032-m21-contract.md decision 14; Backlog §2's sprite sheet inspector): a selected sprite sheet
	// shows its texture, frames and clips. A field's gesture shows its candidate while it lasts and, on release, submits
	// asset.setProperties {asset, values: {<field>: <whole value>}} (an array replaces the whole array, RFC 7386), the
	// method's own action, so its texture check and its undo step are asset.setProperties'; Escape discards the gesture.
	// A selected tilemap (decision 26) shows its palette, cell size and chunk size the same way, each field's release
	// submitting asset.setProperties, and the counts of its chunks and painted cells; its cells are painted with the scene
	// view's tile brush or tilemap.paint, never edited here. M21's milestone review: an array field of more than
	// InlineAssetElements elements (a large sheet's frames or clips, a large tilemap palette) is a clipped list of its
	// elements, one line each (ImGuiListClipper, so only the visible rows are drawn, as the tile brush lists a palette),
	// and the fields of its one selected element (picked in the list or by its index); each of that element's gestures,
	// Add element (the element type's default, appended) and Remove element (the selected one) submits asset.setProperties
	// with the whole array, so no frame draws or converts every element. Smaller arrays draw every element as before.
	// M24 (Docs/Decisions/0035-m24-contract.md decision 15): a script slot holding a graph shows Open graph, which asks the Graph
	// Editor (EditorLayer's) to open it (TakeGraphRequest), and every slot shared by the selection offers New graph..., the graph
	// editor's CreateGraphInSlot: one edit.batch of graph.create and the slot's entity.update per selected entity
	// (MakeScriptSlotGraph), so creating the graph and assigning it is one undo step; the new graph then opens.
	// M24 (decision 14): during play, a script slot holding a graph shows its fields' and variables' live values for the one
	// selected entity, read through graph.liveValues once a frame (GraphLiveView).
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class InspectorPanel
	{
	public:
		// M21's milestone review: the most elements an asset's array field draws inline; a longer one is listed (above).
		static constexpr size_t InlineAssetElements = 64;

		[[nodiscard]] Status Draw(EditorPanelContext& context);
		// M24: the graph Open graph, or a finished New graph, asked the Graph Editor to open since the last call, once.
		[[nodiscard]] std::optional<GraphEditorTarget> TakeGraphRequest() { return std::exchange(m_GraphRequest, std::nullopt); }
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
			std::string Summary{}; // M21: a tilemap's counts, taken when it is read
		};
		// M21: the field of a selected sprite sheet or tilemap a gesture is changing, with the value it shows until its
		// release submits it.
		struct AssetFieldEdit
		{
			AssetHandle Asset{};
			std::string Field{};
			Value Candidate{};
			// M21's milestone review: for a listed array, the index of the element the gesture edits (Candidate is that
			// element's value); nullopt for a whole field.
			std::optional<size_t> Element{};
		};
		// M24: one graph slot's live values; the views that a frame did not draw stop at the next Draw.
		struct GraphSlotLiveView
		{
			GraphLiveView View{};
			bool Drawn = false;
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
		// M21: the selected sprite sheet `asset`'s fields, read in `properties`; `escape` discards a gesture.
		void DrawSpriteSheet(EditorPanelContext& context, AssetHandle asset, const AssetProperties& properties, bool escape);
		// M21 (decision 26): the selected tilemap `asset`'s palette, cell size and chunk size, read in `properties`.
		void DrawTilemap(EditorPanelContext& context, AssetHandle asset, const AssetProperties& properties, bool escape);
		// M21: the fields of a native asset's `properties` (those its values hold), each gesture's release submitting
		// asset.setProperties; the fields' ids are keyed by `idPrefix`, their labels by the registry type `typeName`.
		void DrawAssetFields(EditorPanelContext& context, AssetHandle asset, const AssetProperties& properties, bool escape,
			std::string_view idPrefix, std::string_view typeName);
		// M21's milestone review: the array `field` of `properties`, holding `elements` (more than InlineAssetElements), as a
		// clipped list with its selected element's fields (see the class comment); ids keyed by `path`, labels by `typeName`.
		void DrawListedAssetArray(EditorPanelContext& context, AssetHandle asset, const AssetProperties& properties, const FieldInfo& field,
			const Json& elements, bool escape, const std::string& path, std::string_view typeName);
		// M24: the live values of the play entity `entity`'s script slot `slot` when it holds a graph (`script`), polled once a
		// frame while drawn; nothing for a slot of another script.
		void DrawGraphSlotLiveValues(EditorPanelContext& context, UUID entity, size_t slot, AssetHandle script);
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
		std::optional<AssetFieldEdit> m_AssetFieldEdit{};
		// M21's milestone review: the selected element of each listed array field (by field name) of m_ListedAsset.
		AssetHandle m_ListedAsset{};
		std::map<std::string, size_t, std::less<>> m_ListedElements{};
		std::optional<PrefabViewCache> m_PrefabView{};
		UUID m_ApplyInstance{}; // the instance the level picker belongs to
		UUID m_ApplyLevel{};    // the picked level's root; reset to the outermost for another instance
		bool m_CommitQueued = false;
		// M24: TakeGraphRequest's; the New graph... batch in flight, its slot and its name.
		std::optional<GraphEditorTarget> m_GraphRequest{};
		uint64_t m_NewGraphTicket = 0;
		size_t m_NewGraphSlot = 0;
		std::string m_NewGraphName{};
		std::map<size_t, GraphSlotLiveView> m_GraphLiveViews{}; // M24: by slot index, of the one selected play entity
		friend class EditorLayer;
	};

}
