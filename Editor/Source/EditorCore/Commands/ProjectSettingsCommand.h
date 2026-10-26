#pragma once

#include "EditorCore/Commands/Command.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/ValidationContext.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	// Changes the project settings (§12.3 "settings JSON before/after"): project.setSettings and the ProjectSettingsPanel
	// (M10). Execute and Undo apply the canonical settings document of their side through EditorContext::ApplyProjectSettings,
	// which writes the .eproj at once (with provenance), so the file always equals the settings the editor runs with; the
	// open scene's dirty flag is unaffected (ChangesScene is false).
	class ProjectSettingsCommand final : public Command
	{
	public:
		// `before` and `after` (non-null, asserted) are canonical ProjectSerializer documents ("Format": "Project", "Version",
		// the settings), shared immutable data (§4.7).
		ProjectSettingsCommand(std::string label, Ref<const Json> before, Ref<const Json> after);

		// The command for an RFC 7386 merge patch of the current settings (§13.5 project.setSettings {patch}): reads the
		// current settings' document, applies `patch` with StructInfo::ApplyMergePatch of "ProjectSettings" (null resets a
		// field, Input.Actions keys merge and delete, Variant values replace, enums canonical), validates the result and
		// returns the command, not yet executed. A member the settings do not have (REFLECTION_UNKNOWN_FIELD) is dropped, a
		// null one included (which the merge itself would erase unread); a Variant value that does not resolve or match its
		// schema (REFLECTION_VARIANT_UNRESOLVED, REFLECTION_VARIANT_MISMATCH; no settings Variant has a schema yet) is kept as
		// written. Each comes with a warning of the command (GetWarnings), or, in a --strict editor
		// (EditorContextSpecification::StrictUnknowns), is refused as an error, as the strict reader refuses it in the .eproj.
		// Errors: InvalidState without an open project; Validation (located, with issues relative to the settings root, such
		// as "/Simulation/FixedHz" or "/Window/Titel") for a patch that is not an object, yields invalid settings or, in a
		// --strict editor, has a member the settings do not have.
		[[nodiscard]] static Result<Scope<ProjectSettingsCommand>> CreateFromPatch(const EditorContext& context, const Json& patch,
			std::string label);

		// Applies the After document. Errors: those of EditorContext::ApplyProjectSettings (the write); nothing changed then.
		[[nodiscard]] Status Execute(EditorContext& context) override;
		// Applies the Before document. Errors: those of EditorContext::ApplyProjectSettings (the write, which can fail on a
		// full disk or a locked file); the command stays applied then (Command.h).
		[[nodiscard]] Status Undo(EditorContext& context) override;
		[[nodiscard]] std::string_view GetLabel() const override { return m_Label; }
		[[nodiscard]] bool ChangesScene() const override { return false; }
		[[nodiscard]] size_t GetMemorySize() const override;

		[[nodiscard]] const Json& GetBefore() const { return *m_Before; }
		[[nodiscard]] const Json& GetAfter() const { return *m_After; }
		// The warnings of the patch CreateFromPatch read (above), located relative to the settings root, in the order the read
		// met them: what was dropped or kept as written. Empty for a command built from documents, and in a --strict editor.
		[[nodiscard]] std::span<const ValidationIssue> GetWarnings() const { return m_Warnings; }
	private:
		std::string m_Label;
		Ref<const Json> m_Before;
		Ref<const Json> m_After;
		std::vector<ValidationIssue> m_Warnings;
	};

}
