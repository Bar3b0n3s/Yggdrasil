#pragma once

#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// The diagnostics delta every response carries (Architecture §13.4): "_meta": {revision, dirty, undoLabel?, tick?,
// playState, diagnostics: {newErrors, newWarnings, newScriptErrors, logCursor, firstNew: [<= 3 entries]}}, so an agent
// learns about problems without polling.

namespace Engine {

	class RingBufferSink;

	// What the host reports about its state at the end of a call (IMethodHost::GetMetaState).
	struct MetaState
	{
		uint64_t Revision = 0;          // the host's revision (EditorContext::GetRevision: never repeats across scenes)
		bool Dirty = false;             // the open scene has unsaved changes
		std::string UndoLabel{};        // the label edit.undo would undo ("[agent] Create Entity 'Board'"); empty: omitted
		std::optional<uint64_t> Tick{}; // the play session's tick (M7); omitted when not playing
		std::string PlayState = "Edit"; // "Edit", "Play", "Simulate" or "Paused"
	};

	// The most new log entries quoted in diagnostics.firstNew.
	inline constexpr size_t MaxFirstNewDiagnostics = 3;

	// Builds "_meta" per client from the log's ring buffer (§4.4): each client has its own cursor, so the counts are the
	// entries logged since that client's previous response. Main thread only (the ring itself is thread-safe).
	class MetaBuilder
	{
	public:
		// `log` is a documented back-reference (in production Log::GetRingBuffer()) that outlives the builder.
		explicit MetaBuilder(const RingBufferSink& log);
		~MetaBuilder();

		MetaBuilder(const MetaBuilder&) = delete;
		MetaBuilder& operator=(const MetaBuilder&) = delete;

		// Starts the client's cursor at the log's next sequence number: only entries logged after this count. Asserts an
		// unknown client.
		void AddClient(ClientId client);
		// Forgets the client; unknown clients are ignored.
		void RemoveClient(ClientId client);

		// The "_meta" object for `client` (asserted known) from `state` and the log entries since its cursor:
		//   newErrors       entries at Error or Critical of the Engine and App loggers
		//   newWarnings     entries at Warn of the Engine and App loggers
		//   newScriptErrors entries at Error or Critical of the Script logger
		//   logCursor       the log's next sequence number as a decimal string: the "cursor" log.read continues from
		//   firstNew        the first MaxFirstNewDiagnostics of those entries (Warn and above, every logger), each
		//                   {seq, level, logger, message, file?, line?, entity?}
		// Entries overwritten before they were seen are counted in "dropped" when non-zero. Advances the cursor to logCursor.
		[[nodiscard]] Json Build(ClientId client, const MetaState& state);
	private:
		struct State; // the log back-reference and each client's cursor (MetaBuilder.cpp)
	private:
		Scope<State> m_State;
	};

}
