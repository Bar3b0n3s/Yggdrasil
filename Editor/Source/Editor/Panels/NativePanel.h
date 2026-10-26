#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class NativeModuleTypes;
	struct EditorPanelContext;

	// The game's native C++ module (Docs/Decisions/0030-m19-contract.md decisions 29 to 31; NativeModuleHost): the user's
	// "Allow native code" preference, the module's sources and builds, Build and Load, the loaded module's types and the
	// selected entity's native components, read-only. Closed by default; opened from the Window menu.
	//
	// The checkbox is the preference's only writer: it queues the change (NativeModuleHost::QueuePreferenceChange, no I/O),
	// which the host saves into user://Editor.json at the next frame boundary; no automation method can change it. Build and
	// Load queue native.build and native.load (EditorActions), the methods agents call, so every other action here has its
	// method (Roadmap rule 5); native.getComponents reads what the components section shows. The status (sources, builds,
	// whether they are current) reads files, so it is read when the panel first shows a project, after a build or load it
	// queued resolved, when the loaded module changes and on Refresh, never every frame. Main thread inside ImGui; it keeps
	// no entity or component pointer across frames, and observes the module it last read only weakly, so a module loaded
	// later at a freed module's address is never taken for it.
	class NativePanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
	private:
		// What the panel shows of NativeModuleHost::GetStatus, copied so the panel holds none of the module's types.
		struct StatusSnapshot
		{
			std::vector<std::string> Sources{};
			std::string SourceHash{};
			std::optional<std::string> LatestBuild{}; // the newest build's directory name; nullopt without one
			bool LatestBuildCurrent = false;
			bool Building = false;
			std::optional<bool> LastBuildSucceeded{};
			std::vector<std::string> LastBuildDiagnostics{};
			bool LoadedCurrent = false;
			std::string Error{}; // why the status could not be read; empty when it was
		};

		void RefreshStatus(EditorPanelContext& context);
		void DrawPreference(EditorPanelContext& context);
		void DrawModule(EditorPanelContext& context, const NativeModuleTypes* loaded);
		void DrawSelection(EditorPanelContext& context, const NativeModuleTypes* loaded);
		// Takes the results of the build and load this panel queued; true when one resolved.
		[[nodiscard]] bool TakeResults(EditorPanelContext& context);
		void Submit(EditorPanelContext& context, std::string_view method, std::optional<uint64_t>& ticket);

		std::optional<StatusSnapshot> m_Status{};
		std::filesystem::path m_StatusProject{};                 // the project the snapshot describes
		std::weak_ptr<const NativeModuleTypes> m_StatusModule{}; // the loaded module the snapshot saw, never owned
		bool m_StatusHadModule = false;                          // a module was loaded when the snapshot was read
		std::optional<bool> m_RequestedAllowed{};                // the checkbox's last change, until the host's frame applies it
		uint64_t m_RequestedFrame = 0;                           // the UI frames completed when it was made (EditorUiState::GetCompletedFrame)
		bool m_PreferenceFailed = false;                         // a frame passed without the host applying it (the Console names why)
		std::optional<uint64_t> m_BuildTicket{};
		std::optional<uint64_t> m_LoadTicket{};
		std::string m_ActionMessage{}; // the last Build or Load outcome
		bool m_ActionFailed = false;
	};

}
