#include "EditorPCH.h"
#include "EditorCore/Graph/GraphEditorTarget.h"

#include "EditorCore/Automation/GraphMethods.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/AssetPipeline/Importers/GraphImporter.h"
#include "Engine/Core/VfsPath.h"

#include <charconv>
#include <format>

// What the graph editor opens (Docs/Decisions/0035-m24-contract.md decision 15, W24.2's stream A).

namespace Engine {

	namespace Utils {

		// The node of the document `read` holds with id `node`, else 0.
		static uint32_t KeepDocumentNode(const GraphReadResult& read, uint32_t node)
		{
			return node != 0 && FindGraphNode(read.Document, node) != nullptr ? node : 0;
		}

		// The id of the node "/Nodes/<i>" names (the pointer may go on into the node), or 0.
		static uint32_t FindPointerNode(const GraphReadResult& read, std::string_view pointer)
		{
			constexpr std::string_view Prefix = "/Nodes/";
			if (!pointer.starts_with(Prefix))
				return 0;
			const std::string_view rest = pointer.substr(Prefix.size());
			size_t index = 0;
			const auto [end, error] = std::from_chars(rest.data(), rest.data() + rest.size(), index);
			if (error != std::errc() || end == rest.data() || (end != rest.data() + rest.size() && *end != '/'))
				return 0;
			return index < read.Document.Nodes.size() ? read.Document.Nodes[index].Id : 0;
		}

	}

	uint32_t ResolveGraphEditorTarget(const GraphEditorTarget& target, const GraphReadResult& read)
	{
		if (const uint32_t node = Utils::KeepDocumentNode(read, target.Node); node != 0)
			return node;
		if (target.Line != 0 && target.Line <= read.LineNodes.size())
		{
			if (const uint32_t node = Utils::KeepDocumentNode(read, read.LineNodes[target.Line - 1]); node != 0)
				return node;
		}
		if (!target.JsonPointer.empty())
		{
			for (const GraphDiagnostic& diagnostic : read.Diagnostics)
			{
				if (diagnostic.JsonPointer != target.JsonPointer)
					continue;
				if (const uint32_t node = Utils::KeepDocumentNode(read, diagnostic.Node); node != 0)
					return node;
			}
			if (const uint32_t node = Utils::FindPointerNode(read, target.JsonPointer); node != 0)
				return node;
		}
		if (!target.Finding.empty())
		{
			// GraphImporter writes a graph issue as "<pointer>: <message>", or the message alone without a pointer.
			for (const GraphDiagnostic& diagnostic : read.Diagnostics)
			{
				if (diagnostic.Code != GraphInvalidCode)
					continue;
				const std::string written = diagnostic.JsonPointer.empty() ? diagnostic.Message : std::format("{}: {}", diagnostic.JsonPointer, diagnostic.Message);
				if (written != target.Finding)
					continue;
				if (const uint32_t node = Utils::KeepDocumentNode(read, diagnostic.Node); node != 0)
					return node;
			}
		}
		return 0;
	}

	bool IsGraphAsset(const AssetRegistry& registry, AssetHandle handle)
	{
		const AssetRecord* record = registry.Find(handle);
		return record != nullptr && record->Metadata.Type == AssetType::Script && record->Metadata.Importer == GraphImporter::Id;
	}

	std::optional<AssetHandle> FindGraphAsset(const AssetRegistry& registry, std::string_view path)
	{
		const Result<VfsPath> source = VfsPath::Create("project", path);
		if (!source)
			return std::nullopt;
		const AssetRecord* record = registry.FindBySourcePath(*source);
		if (record == nullptr || !IsGraphAsset(registry, record->Metadata.Handle))
			return std::nullopt;
		return record->Metadata.Handle;
	}

}
