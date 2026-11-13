#pragma once

#include "EditorCore/Automation/GraphMethods.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The graph editor's live values (M24, C-VS; Docs/Decisions/0035-m24-contract.md decision 14; W24.2's stream B): the polling
// that the graph canvas (Editor/Panels/GraphEditorPanel) and the inspector's graph slots call once a frame while a play
// session runs, and the text they draw over a node or beside a member. A view submits graph.liveValues through EditorActions,
// the editor's own client of the automation methods, so each read runs at the editor's next safe point, between frames, as
// ScriptEngine::ReadInstanceValues, which runs no Luau and allocates nothing in the VM: showing live values never changes the
// session's state, its state hash included. UI-free, so the polling and the text are tested headless; the panels draw what a
// view holds and nothing else.

namespace Engine {

	class EditorActions;
	class EditorContext;

	// The instance a view shows: an entity of the play scene and the behaviour name of its graph slot (graph.liveValues' params).
	struct GraphLiveTarget
	{
		UUID Entity{};
		std::string Script{}; // the slot's behaviour name; empty: the entity's first slot holding a graph

		bool operator==(const GraphLiveTarget&) const = default;
	};

	// The instance the canvas shows for the graph asset `graph`, whose behaviour is `behaviour` (its document's Name): the
	// editor's selected entity of the play scene when exactly one is selected and a script slot of it holds the graph, else the
	// first entity in the play scene's canonical order with such a slot; nullopt without a play session that runs behaviours
	// (a simulation runs none) or without such an entity.
	[[nodiscard]] std::optional<GraphLiveTarget> FindGraphLiveTarget(const EditorContext& editor, AssetHandle graph, std::string_view behaviour);

	// One live value as a short line of text: a number as Luau writes it in its shortest form ("2", "0.25"), a boolean, a string
	// quoted and cut after MaxGraphLiveTextBytes bytes at a UTF-8 boundary ("\"door\"", ending in "..." when cut), a vector,
	// Color or Quat by its components ("(1, 0.5, -2)"), an Entity or AssetRef by its id, and a value without a JSON spelling by
	// its type name ("table", "Transform", "number" for a non-finite one). Pure.
	[[nodiscard]] std::string FormatGraphLiveValue(std::string_view type, const VariantValue& value);

	// The most bytes of a string FormatGraphLiveValue shows.
	inline constexpr size_t MaxGraphLiveTextBytes = 64;

	// One panel's live values. Main thread; holds data only (no entity, component, asset or VM reference), so a view is a plain
	// member of its panel.
	class GraphLiveView
	{
	public:
		// Once a frame, while the panel shows `target`'s graph (nullopt: there is nothing to show). Takes a completed read,
		// then submits the next one when none is pending, so each frame's read arrives at the next frame and a read is never
		// queued twice. A changed target or play session forgets the values and abandons a pending read. Nothing is submitted,
		// and the values are forgotten, without a target or without a play session that runs behaviours; a test run's session
		// is read too (graph.liveValues is among the reads a test run admits).
		void Update(EditorContext& editor, EditorActions& actions, const std::optional<GraphLiveTarget>& target);
		// Stops polling: abandons a pending read and forgets the values (the panel closed, or its graph or project changed).
		void Reset(EditorActions& actions);

		// The last read's values; nullopt before the first read of the current target and session, and after a failed read.
		[[nodiscard]] const std::optional<GraphLiveValuesResult>& GetValues() const { return m_Values; }
		// The last read's failure (its error's text), empty after a successful one.
		[[nodiscard]] const std::string& GetError() const { return m_Error; }
		// The current target; nullopt while the view polls nothing.
		[[nodiscard]] const std::optional<GraphLiveTarget>& GetTarget() const { return m_Target; }
		// The last values of `node`'s outputs, by pin; empty without values.
		[[nodiscard]] std::span<const GraphLiveValue> GetNodeValues(uint32_t node) const;
		// The retained script errors that `node`'s code raised, in the read's order.
		[[nodiscard]] std::vector<GraphLiveError> GetNodeErrors(uint32_t node) const;
		// The overlay hook of the canvas: the lines it draws under `node`, "<Pin> = <value>" per output value (FormatGraphLiveValue),
		// then "Error: <message>" per error its code raised, with " (x<count>)" for a repeated one; empty without values.
		[[nodiscard]] std::vector<std::string> GetNodeLines(uint32_t node) const;
		// The members panel's and the inspector's line of a member: "<Name> = <value>".
		[[nodiscard]] static std::string FormatMemberLine(const GraphLiveMember& member);
	private:
		void Forget();

		std::optional<GraphLiveTarget> m_Target{};
		uint64_t m_Session = 0; // the serial of the play session the values and the pending read are of
		uint64_t m_Ticket = 0;  // the pending graph.liveValues, 0 when none
		std::optional<GraphLiveValuesResult> m_Values{};
		std::string m_Error{};
	};

}
