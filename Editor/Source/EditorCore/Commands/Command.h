#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// Commands and undo (Architecture §12.3). Every mutation of the project or the open scene, by a panel or by an agent, is
// one Command executed through EditorContext::Execute, the single mutation entry point (§1.3 principle 3, §12.1).

namespace Engine {

	class EditorContext;
	class Scene;

	// Who issued a command (§12.3). The undo history shows Agent commands with the prefix "[agent] " (§12.2, §13.4).
	enum class CommandOrigin : uint8_t
	{
		User,
		Agent
	};

	// "User" or "Agent".
	[[nodiscard]] constexpr std::string_view CommandOriginToString(CommandOrigin origin)
	{
		return origin == CommandOrigin::Agent ? std::string_view("Agent") : std::string_view("User");
	}

	// One undoable mutation (Architecture §12.3, frozen by the M4 contract).
	//
	// Contract of every command:
	//   - Execute is atomic: it fully applies the change and returns success, or returns an error and leaves the editor
	//     exactly as it was. It is called for the first execution (EditorContext::Execute) and again for every redo, always
	//     from the state Undo restored, so Execute -> Undo -> Execute yields the state of the first Execute (§12.3 property
	//     test). A command may be constructed in its applied state (SceneEditCommand, built after the edit happened); its
	//     first Execute then changes nothing.
	//   - Undo restores the exact prior state (byte-identical canonical JSON), and is only called after a successful
	//     Execute. Like Execute it is atomic: it restores the prior state and returns success, or returns an error and
	//     leaves the command applied. An in-memory restore (a scene snapshot) cannot fail, because the state it restores
	//     was valid when it was captured, so such a failure is a bug and asserted; Undo returns an error only for external
	//     failures, the file writes of ProjectSettingsCommand and, from M6, of the asset commands (§12.3).
	//   - Commands never keep raw pointers into the scene or a component reference across calls (§4.7): they refer to
	//     entities by UUID and keep JSON snapshots.
	// Main thread only, like the editor state they change. Not copyable; histories own them through Scope.
	class Command
	{
	public:
		Command() = default;
		virtual ~Command() = default;

		Command(const Command&) = delete;
		Command& operator=(const Command&) = delete;

		// Applies the change (see the class comment). Errors: whatever makes the change impossible; nothing changed then.
		[[nodiscard]] virtual Status Execute(EditorContext& context) = 0;

		// Restores the state from before Execute (see the class comment). Errors: an external failure (a file write); the
		// command stays applied then.
		[[nodiscard]] virtual Status Undo(EditorContext& context) = 0;

		// The undo label without the origin prefix: "Create Entity 'Board'", "Set Project Settings".
		[[nodiscard]] virtual std::string_view GetLabel() const = 0;

		// Continuous edits (gizmo drags, slider scrubs, §12.3): two consecutive commands with the same origin and the same
		// non-empty merge key may merge. A key names the command type and what it edits ("SceneEdit:Transform:<uuid>"), so
		// equal keys imply the same command type. Empty (the default): never merges.
		[[nodiscard]] virtual std::string_view GetMergeKey() const { return {}; }

		// Asked on the newest history entry right after `next` executed, only when both have the same origin and the same
		// non-empty merge key (so `next` has this command's type); returning true means this command now also covers `next`
		// (Undo restores the state before both), and the history drops `next`. The default merges nothing.
		[[nodiscard]] virtual bool MergeWith(const Command& /*next*/) { return false; }

		// Whether the command changes the open scene, which decides the scene's dirty flag (§12.3 save point). Project
		// settings commands write through to the .eproj and do not make the scene dirty.
		[[nodiscard]] virtual bool ChangesScene() const { return true; }

		// An estimate of the bytes the command holds (snapshots, settings JSON), for the history's 256 MB bound (§12.3).
		[[nodiscard]] virtual size_t GetMemorySize() const = 0;

		// Brings `scene`, a scratch copy of the open scene, from the state before this command to the state after it (`after`
		// true) or back (`after` false), without touching the editor or this command. scene.diff {against: "revision"}
		// rebuilds an earlier revision this way, replaying held history entries on a copy (ADR 0008 decision 29). `scene`
		// must be in the opposite state. A command that does not change the scene (ChangesScene false) changes nothing, which
		// is the default; a scene-changing command that does not override it fails with Unsupported. Errors: Validation when
		// a snapshot does not apply (the copy is unusable then), Unsupported as above.
		[[nodiscard]] virtual Status ReplayOnSceneCopy(Scene& /*scene*/, bool /*after*/) const
		{
			if (!ChangesScene())
				return {};
			return MakeError(ErrorCode::Unsupported, "'{}' cannot be replayed on a scene copy", GetLabel());
		}

		[[nodiscard]] CommandOrigin GetOrigin() const { return m_Origin; }
		// Set by EditorContext::Execute from the request being served (Agent during automation requests).
		void SetOrigin(CommandOrigin origin) { m_Origin = origin; }
	private:
		CommandOrigin m_Origin = CommandOrigin::User;
	};

}
