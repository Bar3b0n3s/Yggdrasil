#include "EditorPCH.h"
#include "Editor/Private/EditorHostClose.h"

#include "EditorCore/Autosave/Autosave.h"
#include "EditorCore/EditorActions.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Scene.h"

#include <imgui.h>

#include <format>

namespace Engine {

	namespace Utils {

		constexpr const char* EditorClosePromptTitle = "Unsaved changes";

	}

	EditorHostClose::EditorHostClose(EditorContext& editor, EditorActions& actions, Autosave& saves)
		: m_Editor(editor), m_Actions(actions), m_Saves(saves)
	{
	}

	bool EditorHostClose::NeedsConfirmation() const
	{
		return m_Editor.HasProject() && m_Editor.IsAnySceneDirty() && !m_Editor.IsReadOnly() && !m_Editor.IsDryRun();
	}

	EditorClosePrompt EditorHostClose::MakePrompt() const
	{
		EditorClosePrompt prompt;
		prompt.CanSave = true;
		for (const OpenSceneId id : m_Editor.GetOpenScenes())
		{
			const Result<OpenSceneInfo> scene = m_Editor.DescribeOpenScene(id);
			if (!scene || !scene->Dirty)
				continue;
			prompt.SceneName += std::format("{}{}", prompt.SceneNames.empty() ? "" : ", ", scene->Name);
			prompt.SceneNames.push_back(scene->Name);
			prompt.CanSave = prompt.CanSave && scene->Path.has_value();
		}
		return prompt;
	}

	void EditorHostClose::Fail(const Error& error)
	{
		m_Pending.reset();
		if (m_Prompt)
		{
			m_Prompt->Pending = false;
			m_Prompt->Failure = error;
		}
	}

	void EditorHostClose::Dismiss()
	{
		if (m_SaveTicket)
		{
			static_cast<void>(m_Actions.Cancel(*m_SaveTicket));
			static_cast<void>(m_Actions.TakeResult(*m_SaveTicket));
			m_SaveTicket.reset();
		}
		m_Pending.reset();
		m_Prompt.reset();
	}

	void EditorHostClose::WriteExitRecovery(std::string_view sceneNames)
	{
		// The final recovery holds every committed scene the user is discarding; the editor closes whatever it reports.
		const Result<AutosaveWriteResult> written = m_Saves.Save(AutosaveReason::Exit);
		if (!written)
			ENGINE_WARN("The editor closes without a final recovery of '{}': {}", sceneNames, written.error());
	}

	bool EditorHostClose::RequestClose(bool canPrompt)
	{
		if (!canPrompt)
		{
			// Nobody could ever answer a held close, so the editor closes now, as after Don't save.
			const bool unsaved = NeedsConfirmation();
			const std::string sceneName = unsaved ? MakePrompt().SceneName : std::string();
			Dismiss();
			if (unsaved)
			{
				ENGINE_WARN("The editor closes with unsaved changes to '{}': there is no UI to ask whether to save them, so a final "
							"recovery keeps them for the next launch",
					sceneName);
				WriteExitRecovery(sceneName);
			}
			return false;
		}
		if (m_Prompt)
			return true;
		if (!NeedsConfirmation())
			return false;
		m_Prompt = MakePrompt();
		return true;
	}

	std::optional<EditorClosePrompt> EditorHostClose::GetPrompt() const
	{
		return m_Prompt;
	}

	Status EditorHostClose::Choose(EditorCloseChoice choice)
	{
		if (!m_Prompt)
			return MakeError(ErrorCode::NotFound, "No window close is waiting for a choice");
		if (choice == EditorCloseChoice::Cancel)
		{
			Dismiss();
			return {};
		}
		if (choice != EditorCloseChoice::Save && choice != EditorCloseChoice::Discard)
			return MakeError(ErrorCode::InvalidArgument, "Unknown close choice");
		if (m_Pending)
			return MakeError(ErrorCode::InvalidState, "A close choice is already pending");
		if (choice == EditorCloseChoice::Save)
		{
			if (!m_Prompt->CanSave)
			{
				Status refused = MakeError(ErrorCode::InvalidState, "A scene with unsaved changes has never been saved: choose Don't save to keep it recoverable, or Cancel");
				m_Prompt->Failure = refused.error();
				return refused;
			}
			// The active scene alone through scene.save, as before M20; several dirty scenes through project.save, which
			// writes every dirty open scene.
			const bool onlyActive = m_Editor.HasScene() && m_Editor.IsSceneDirty() && m_Prompt->SceneNames.size() == 1;
			const Result<uint64_t> ticket = m_Actions.Submit(onlyActive ? "scene.save" : "project.save", Json::object());
			if (!ticket)
			{
				m_Prompt->Failure = ticket.error();
				return std::unexpected(ticket.error());
			}
			m_SaveTicket = *ticket;
		}
		m_Pending = choice;
		m_Prompt->Pending = true;
		m_Prompt->Failure.reset();
		return {};
	}

	bool EditorHostClose::Pump()
	{
		if (!m_Prompt)
			return false;
		if (m_SaveTicket)
		{
			const Result<std::optional<Result<Json>>> taken = m_Actions.TakeResult(*m_SaveTicket);
			if (taken && !taken->has_value())
				return false;
			m_SaveTicket.reset();
			if (!taken)
			{
				Fail(taken.error());
				return false;
			}
			if (!**taken)
			{
				Fail((**taken).error());
				return false;
			}
			m_Pending.reset();
			m_Prompt->Pending = false;
		}
		if (m_Pending == EditorCloseChoice::Discard)
		{
			WriteExitRecovery(m_Prompt->SceneName);
			m_Pending.reset();
			m_Prompt.reset();
			return true;
		}
		// Saved, here or meanwhile by another path: nothing unsaved holds the close any longer.
		if (!NeedsConfirmation())
		{
			m_Prompt.reset();
			return true;
		}
		return false;
	}

	void EditorHostClose::Draw()
	{
		// Never displaces another modal (the recovery offer): the prompt appears once that one is dismissed.
		if (m_Prompt && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
			ImGui::OpenPopup(Utils::EditorClosePromptTitle);
		if (!ImGui::BeginPopupModal(Utils::EditorClosePromptTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
			return;
		if (!m_Prompt)
		{
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		const EditorClosePrompt prompt = *m_Prompt;
		ImGui::TextWrapped("Save changes to %s before closing?", prompt.SceneName.c_str());
		if (!prompt.CanSave)
			ImGui::TextWrapped("%s", prompt.SceneNames.size() == 1 ? "This scene has never been saved." : "A scene with unsaved changes has never been saved.");
		if (prompt.Failure)
			ImGui::TextWrapped("%s", prompt.Failure->GetMessageText().c_str());
		std::optional<EditorCloseChoice> choice;
		ImGui::BeginDisabled(prompt.Pending || !prompt.CanSave);
		if (ImGui::Button("Save"))
			choice = EditorCloseChoice::Save;
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(prompt.Pending);
		if (ImGui::Button("Don't save"))
			choice = EditorCloseChoice::Discard;
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			choice = EditorCloseChoice::Cancel;
		// A refused choice stays visible in the prompt's Failure.
		if (choice)
			static_cast<void>(Choose(*choice));
		ImGui::EndPopup();
	}

}
