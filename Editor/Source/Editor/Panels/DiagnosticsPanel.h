#pragma once

#include "EditorCore/Automation/ProjectMethods.h"
#include "EditorCore/Graph/GraphEditorTarget.h"
#include "Engine/Core/Result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace Engine {

	struct EditorPanelContext;

	// Asset/validation/type diagnostics with stable IDs and severity filters. Auto-fix submits project.validate
	// for the chosen IDs as one undo step. Latest import checks and the active play session's script errors update live.
	// M24 (Docs/Decisions/0035-m24-contract.md decision 15, C24.1's review finding 16): a finding of a graph (a Script asset of the
	// Graph importer: an import check's finding, a play session's script error or a project.validate finding of a .graph source)
	// opens the Graph Editor at its node instead of the external editor at the .graph file: the panel records the request with
	// what locates the node (its text line, JSON pointer or GRAPH_INVALID message; GraphEditorTarget), and EditorLayer's Graph
	// Editor selects and frames the node graph.read gives for it.
	// Main thread inside ImGui. Retains only UI presentation state, no entity/component/asset pointers across frames.
	class DiagnosticsPanel
	{
	public:
		[[nodiscard]] Status Draw(EditorPanelContext& context);
		// The graph finding Open in Graph Editor asked the Graph Editor to show since the last call, once.
		[[nodiscard]] std::optional<GraphEditorTarget> TakeGraphRequest() { return std::exchange(m_GraphRequest, std::nullopt); }
	private:
		ProjectValidateResult m_Report{};
		std::array<char, 256> m_Search{};
		std::set<std::string> m_Selected{};
		std::filesystem::path m_Project{};
		uint64_t m_Ticket = 0;
		uint64_t m_SelectionTicket = 0;
		uint64_t m_ReportRevision = 0;
		int m_Severity = 0;
		bool m_HasReport = false;
		bool m_Refresh = true;
		bool m_Fixing = false;
		std::string m_Error{};
		std::optional<GraphEditorTarget> m_GraphRequest{}; // TakeGraphRequest's
	};

}
