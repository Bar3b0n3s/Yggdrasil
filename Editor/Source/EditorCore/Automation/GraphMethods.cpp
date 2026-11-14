#include "EditorPCH.h"
#include "EditorCore/Automation/GraphMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/AssetMethodSupport.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Graph/GraphLiveCache.h"
#include "EditorCore/Private/EditorFileError.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/Graph/GraphTranspiler.h"
#include "Engine/AssetPipeline/Importers/GraphImporter.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

// graph.* (Docs/Decisions/0035-m24-contract.md decisions 12 to 14): the wire format and the registration are the contract's;
// stream G-C implements the edit handlers and W24.2's stream B graph.liveValues; RegisterMethods.cpp calls the registration
// functions.

namespace Engine {

	namespace {

		// How far right of the rightmost node graph.addNode places a node given no position: one node width of the canvas.
		constexpr float GraphNodeSpacing = 240.0f;

		// The prefixes of a graph's member kinds (GraphNodeLibrary::Resolve's Self.Get.<Name> and Self.Set.<Name>).
		constexpr std::string_view SelfGetPrefix = "Self.Get.";
		constexpr std::string_view SelfSetPrefix = "Self.Set.";

		constexpr std::string_view ListKindsHint = "graph.nodes lists the node kinds and their pins";

		// A graph source read for a method: its asset, its file and its document as the file holds it.
		struct LoadedGraph
		{
			AssetHandle Handle{};
			VfsPath Source{};
			std::string Path{}; // project-relative ("Assets/Scripts/Door.graph")
			std::string Text{}; // the file's bytes
			GraphDocument Document{};
		};

		Buffer TextToBuffer(std::string_view text)
		{
			const std::span<const std::byte> bytes = AsBytes(text);
			return Buffer(bytes.begin(), bytes.end());
		}

		// `token` escaped as one JSON pointer reference token (RFC 6901: '~' as "~0", '/' as "~1").
		std::string EscapePointerToken(std::string_view token)
		{
			std::string escaped;
			for (const char character : token)
			{
				if (character == '~')
					escaped += "~0";
				else if (character == '/')
					escaped += "~1";
				else
					escaped.push_back(character);
			}
			return escaped;
		}

		// A JSON pointer into a .graph document, split at its first two tokens: "/Nodes/5/Values/speed" is the section
		// "Nodes", the index 5 and the rest "/Values/speed".
		struct DocumentPointer
		{
			std::string_view Section{};
			std::optional<size_t> Index{};
			std::string_view Rest{};
		};

		DocumentPointer SplitDocumentPointer(std::string_view pointer)
		{
			DocumentPointer split;
			if (!pointer.starts_with('/'))
				return split;
			std::string_view rest = pointer.substr(1);
			size_t slash = rest.find('/');
			split.Section = rest.substr(0, slash);
			if (slash == std::string_view::npos)
				return split;
			rest = rest.substr(slash + 1);
			slash = rest.find('/');
			const std::string_view index = rest.substr(0, slash);
			size_t value = 0;
			const auto [end, error] = std::from_chars(index.data(), index.data() + index.size(), value);
			if (error == std::errc() && end == index.data() + index.size() && !index.empty())
				split.Index = value;
			split.Rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
			return split;
		}

		// The id of the node a document pointer names ("/Nodes/<i>..."), or 0.
		uint32_t FindNodeAtPointer(const GraphDocument& graph, std::string_view pointer)
		{
			const DocumentPointer split = SplitDocumentPointer(pointer);
			if (split.Section != "Nodes" || !split.Index.has_value() || *split.Index >= graph.Nodes.size())
				return 0;
			return graph.Nodes[*split.Index].Id;
		}

		// The pin a pointer of a node's literal names ("/Values/<pin>" of DocumentPointer::Rest), or empty.
		std::string_view FindValuePin(std::string_view rest)
		{
			constexpr std::string_view Values = "/Values/";
			if (!rest.starts_with(Values))
				return {};
			const std::string_view pin = rest.substr(Values.size());
			return pin.substr(0, pin.find('/'));
		}

		GraphNode* FindMutableNode(GraphDocument& graph, uint32_t id)
		{
			const auto found = std::ranges::lower_bound(graph.Nodes, id, {}, &GraphNode::Id);
			return found != graph.Nodes.end() && found->Id == id ? &*found : nullptr;
		}

		// The member a Self.Get or Self.Set kind names, or empty for any other kind.
		std::string_view GetMemberOfKind(std::string_view kind)
		{
			for (const std::string_view prefix : { SelfGetPrefix, SelfSetPrefix })
				if (kind.starts_with(prefix))
					return kind.substr(prefix.size());
			return {};
		}

		bool HasMember(const GraphDocument& graph, std::string_view name)
		{
			return std::ranges::any_of(graph.Fields, [name](const GraphField& field)
			{
				return field.Name == name;
			}) || std::ranges::any_of(graph.Variables, [name](const GraphVariable& variable)
			{
				return variable.Name == name;
			});
		}

		std::vector<std::string> GetMemberNames(const GraphDocument& graph)
		{
			std::vector<std::string> names;
			for (const GraphField& field : graph.Fields)
				names.push_back(field.Name);
			for (const GraphVariable& variable : graph.Variables)
				names.push_back(variable.Name);
			return names;
		}

		// The ids of the nodes of `graph` whose kind is Self.Get.<name> or Self.Set.<name>.
		std::vector<uint32_t> FindMemberNodes(const GraphDocument& graph, std::string_view name)
		{
			std::vector<uint32_t> nodes;
			for (const GraphNode& node : graph.Nodes)
				if (GetMemberOfKind(node.Kind) == name)
					nodes.push_back(node.Id);
			return nodes;
		}

		std::string JoinIds(std::span<const uint32_t> ids)
		{
			std::string text;
			for (const uint32_t id : ids)
				text += text.empty() ? std::format("{}", id) : std::format(", {}", id);
			return text;
		}

		// An InvalidArgument error at `pointer` for a name that is none of `candidates`, with its suggestions.
		Error MakeUnknownNameError(std::string_view pointer, std::string message, std::string_view name, std::span<const std::string> candidates,
			std::string hint)
		{
			std::vector<std::string> suggestions = FuzzySuggest(name, candidates);
			ErrorIssue issue{ .JsonPointer = std::string(pointer), .Message = message, .Hint = MakeDidYouMeanHint(suggestions), .Suggestions = std::move(suggestions) };
			return Utils::ReplaceIssues(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, std::move(message), std::move(hint)), { std::move(issue) });
		}

		// The source of the graph `handle`. Errors at `pointer`, as asset.setProperties' reference: InvalidArgument for a
		// built-in, a sub-asset or an asset whose importer is not Graph (the hint naming script.read for a Luau script), and for a
		// dependency file's record (a guard: the reference resolver resolves assets only, so a dependency file's path or handle
		// is already NotFound there); NotFound for a handle the registry does not hold.
		Result<VfsPath> FindGraphSource(const EditorAssetManager& assets, AssetHandle handle, std::string_view pointer)
		{
			if (assets.GetBuiltins().Find(handle) != nullptr || IsBuiltinAssetHandle(handle))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
					std::format("'{}' is a built-in asset, not a graph", assets.GetReferencePath(handle)), "built-in assets cannot change; graph.create makes a graph"));
			}
			const AssetRegistry& registry = assets.GetRegistry();
			if (const std::optional<AssetRegistry::Location> location = registry.Locate(handle); location.has_value() && !location->SubAssetKey.empty())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
					std::format("'{}' is a sub-asset of '{}', not a graph", assets.GetReferencePath(handle), Utils::ToProjectRelative(location->Record->SourcePath)),
					"pass a .graph asset; graph.create makes one"));
			}
			const AssetRecord* record = registry.Find(handle);
			if (record == nullptr)
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, pointer, std::format("no asset has the handle {}", handle.ToString())));
			const std::string path = Utils::ToProjectRelative(record->SourcePath);
			if (record->Metadata.Kind == AssetMetaKind::Dependency)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' is a dependency file, not a graph", path),
					"pass a .graph asset; graph.create makes one"));
			}
			if (record->Metadata.Importer != GraphImporter::Id)
			{
				const bool script = record->Metadata.Type == AssetType::Script;
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
					std::format("'{}' is a {} asset of the {} importer, not a graph", path, AssetTypeToString(record->Metadata.Type), record->Metadata.Importer),
					script ? "a Luau script is read with script.read and written with script.write" : "pass a .graph asset; graph.create makes one"));
			}
			return record->SourcePath;
		}

		// `text`, the bytes of the graph source at the project path `relative`, read strictly as its importer reads it: a member
		// the file does not define would not survive a rewrite. Errors: ImportFailed naming the file for a source that does not
		// read, with the reader's location and issues.
		Result<GraphDocument> ReadGraphText(EditorContext& editor, std::string_view text, const std::string& relative)
		{
			GraphLoadReport report;
			Result<GraphDocument> read = GraphFromText(text, editor.GetTypeRegistry(), report, true);
			if (!read)
			{
				ErrorLocation location = read.error().GetLocation();
				location.File = relative;
				return std::unexpected(Error(ErrorCode::ImportFailed, std::format("'{}' cannot be read as a graph: {}", relative, read.error().GetMessageText()))
						.WithLocation(std::move(location))
						.WithIssues(read.error().GetIssues())
						.WithHint("project.validate reports what is wrong with the file; fix the file, then edit the graph again"));
			}
			return read;
		}

		// The graph `reference` names (the "graph" param), read strictly (ReadGraphText). Errors: FindGraphSource's and the
		// reference's at /graph; the read errors; ReadGraphText's.
		Result<LoadedGraph> LoadGraph(EditorMethodContext& context, std::string_view reference)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const AssetHandle handle, Utils::ResolveAssetParam(context, reference, "/graph"));
			ENGINE_TRY_ASSIGN(VfsPath source, FindGraphSource(editor.GetAssets(), handle, "/graph"));
			Result<std::string> text = editor.GetVfs().ReadText(source);
			if (!text)
				return std::unexpected(Utils::ToEditorFileError(std::move(text).error()));
			std::string relative = Utils::ToProjectRelative(source);
			ENGINE_TRY_ASSIGN(GraphDocument document, ReadGraphText(editor, *text, relative));
			return LoadedGraph{ .Handle = handle, .Source = std::move(source), .Path = std::move(relative), .Text = std::move(*text), .Document = std::move(document) };
		}

		// graph.liveValues' read of the graph `handle` (a script slot's), through the editor's GraphLiveCache (decision 14,
		// W24.2's review): the method runs once a frame for every graph the Graph Editor and the inspector show, so the document
		// is read strictly again only when the graph changed since the last call: its source's bytes, its path or its asset
		// version. The source's bytes are read every call, since an edit during a lockstep or test session writes the file but
		// defers its reimport. Errors: the read errors; ReadGraphText's.
		Result<GraphLiveCacheEntry*> LoadLiveGraph(EditorContext& editor, AssetHandle handle, const VfsPath& source)
		{
			Result<std::string> text = editor.GetVfs().ReadText(source);
			if (!text)
				return std::unexpected(Utils::ToEditorFileError(std::move(text).error()));
			GraphLiveCacheKey key{ .Graph = handle, .Path = Utils::ToProjectRelative(source), .SourceHash = XXH64(*text), .Version = editor.GetAssets().GetVersion(handle) };
			GraphLiveCache& cache = editor.GetGraphLiveCache();
			if (GraphLiveCacheEntry* kept = cache.Find(key))
				return kept;
			ENGINE_TRY_ASSIGN(GraphDocument document, ReadGraphText(editor, *text, key.Path));
			return &cache.Insert(std::move(key), std::move(document));
		}

		// The node library of the editor's frozen script API registry, the same library the importer builds from the editor's
		// load-time script API (decision 13), built once per editor (EditorContext::GetGraphNodeLibrary).
		Result<const GraphNodeLibrary*> GetLibrary(EditorContext& editor)
		{
			return editor.GetGraphNodeLibrary();
		}

		// Every kind a node of `graph` may have: the library's and the graph's member kinds.
		std::vector<std::string> GetKindNames(const GraphNodeLibrary& library, const GraphDocument& graph)
		{
			std::vector<std::string> kinds;
			for (const GraphNodeDescription& node : library.GetNodes())
				kinds.push_back(node.Kind);
			for (const GraphNodeDescription& node : library.GetMemberNodes(graph))
				kinds.push_back(node.Kind);
			return kinds;
		}

		Error MakeUnknownKindError(std::string_view pointer, std::string_view kind, std::span<const std::string> kinds)
		{
			return MakeUnknownNameError(pointer, std::format("'{}' is not a node kind of the node library or of the graph's members", kind), kind, kinds,
				std::string(ListKindsHint));
		}

		// The description of `node`'s kind. Errors at `pointer`: InvalidArgument for a kind the library does not resolve (a
		// kind the script API lost since the node was added, or a hand edit).
		Result<GraphNodeDescription> ResolveNodeKind(const GraphNodeLibrary& library, const GraphDocument& graph, const GraphNode& node, std::string_view pointer)
		{
			Result<GraphNodeDescription> resolved = library.Resolve(node.Kind, graph);
			if (!resolved)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
					std::format("node {} is of the kind '{}', which the node library does not hold", node.Id, node.Kind),
					"graph.read reports the node; remove it with graph.removeNodes"));
			}
			return resolved;
		}

		const GraphPinDescription* FindPin(const GraphNodeDescription& node, std::string_view name)
		{
			const auto found = std::ranges::find(node.Pins, name, &GraphPinDescription::Name);
			return found != node.Pins.end() ? &*found : nullptr;
		}

		bool IsDataInput(const GraphPinDescription& pin)
		{
			return pin.Direction == GraphPinDirection::Input && pin.Type != GraphPinType::Exec;
		}

		// The names of `node`'s pins of `direction` (data inputs only with `dataOnly`).
		std::vector<std::string> GetPinNames(const GraphNodeDescription& node, GraphPinDirection direction, bool dataOnly)
		{
			std::vector<std::string> names;
			for (const GraphPinDescription& pin : node.Pins)
				if (pin.Direction == direction && (!dataOnly || pin.Type != GraphPinType::Exec))
					names.push_back(pin.Name);
			return names;
		}

		std::string DescribePins(std::span<const std::string> names)
		{
			if (names.empty())
				return "none";
			std::string text;
			for (const std::string& name : names)
				text += text.empty() ? std::format("'{}'", name) : std::format(", '{}'", name);
			return text;
		}

		// Success when `pin` is a data input of `node` (the node `id`, of kind `description`); InvalidArgument at `pointer`
		// otherwise, with the data inputs' names as suggestions.
		Status CheckDataInput(const GraphNodeDescription& description, uint32_t id, std::string_view pin, std::string_view pointer)
		{
			const GraphPinDescription* found = FindPin(description, pin);
			if (found != nullptr && IsDataInput(*found))
				return {};
			const std::vector<std::string> inputs = GetPinNames(description, GraphPinDirection::Input, true);
			const std::string what = found == nullptr ? std::string("no pin") : std::string(found->Direction == GraphPinDirection::Output ? "an output pin" : "an exec pin");
			return std::unexpected(MakeUnknownNameError(pointer,
				std::format("'{}' is {} of node {} ({}), not a data input that takes a literal", pin, found == nullptr ? "not a pin" : what, id, description.Kind),
				pin, inputs, std::format("the data inputs of {} are {}", description.Kind, DescribePins(inputs))));
		}

		// "Number", "Enum AudioGroup", "Asset?": a pin's type as refusals name it.
		std::string DescribePinType(const GraphPinDescription& pin)
		{
			std::string text(GraphPinTypeToString(pin.Type));
			if (!pin.TypeName.empty())
				text += std::format(" {}", pin.TypeName);
			if (pin.Optional)
				text.push_back('?');
			return text;
		}

		// Success when `position` is a canvas place a node may take (finite, each component within MaxGraphPosition);
		// InvalidArgument at `pointer` otherwise.
		Status CheckPosition(glm::vec2 position, std::string_view pointer)
		{
			const auto inside = [](float component)
			{
				return std::isfinite(component) && std::abs(component) <= MaxGraphPosition;
			};
			if (inside(position.x) && inside(position.y))
				return {};
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
				std::format("the position ({}, {}) is outside the canvas: each component is finite and within {}", position.x, position.y, MaxGraphPosition)));
		}

		// Where graph.addNode puts a node given no position: one node width right of the rightmost node, at the top of the
		// graph's bounds; (0, 0) for the first node.
		glm::vec2 GetDefaultNodePosition(const GraphDocument& graph)
		{
			if (graph.Nodes.empty())
				return glm::vec2(0.0f);
			float right = -MaxGraphPosition;
			float top = MaxGraphPosition;
			for (const GraphNode& node : graph.Nodes)
			{
				right = std::max(right, node.Position.x);
				top = std::min(top, node.Position.y);
			}
			return glm::vec2(std::min(right + GraphNodeSpacing, MaxGraphPosition), top);
		}

		// The ValidateGraph issues that block an edit (every one that is not Incomplete), counted by node and message.
		using IssueCounts = std::map<std::pair<uint32_t, std::string>, size_t>;

		IssueCounts CountBlockingIssues(std::span<const GraphIssue> issues)
		{
			IssueCounts counts;
			for (const GraphIssue& issue : issues)
				if (!issue.Incomplete)
					++counts[{ issue.Node, issue.Message }];
			return counts;
		}

		// The refusal of an edit: InvalidArgument at the first issue's pointer, carrying every issue.
		Error RefuseEdit(std::vector<ErrorIssue> issues)
		{
			const std::string pointer = issues.front().JsonPointer;
			std::string message = issues.front().Message;
			std::string hint = issues.front().Hint;
			return Utils::ReplaceIssues(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, std::move(message), std::move(hint)), std::move(issues));
		}

		// Refuses an edit of `before` whose result `after` (canonical) breaks a ValidateGraphStructure rule or, with
		// `library`, adds a ValidateGraph issue that is not Incomplete (an incomplete graph is accepted on the way to a finished
		// one, and a graph that already broke a rule, a kind the script API lost or a hand edit, may still be edited: only an
		// issue the edit adds refuses it). `locate(documentPointer, node)` gives each issue's pointer in the params. `before` is
		// validated only when `after` has a blocking issue, to tell the issues the edit adds from those it kept.
		template<typename Locate>
		Status CheckEdit(const GraphDocument& before, const GraphDocument& after, const GraphNodeLibrary* library, const Locate& locate)
		{
			if (const Status structure = ValidateGraphStructure(after); !structure)
			{
				std::vector<ErrorIssue> issues;
				for (const ErrorIssue& issue : structure.error().GetIssues())
				{
					issues.push_back({ .JsonPointer = locate(issue.JsonPointer, FindNodeAtPointer(after, issue.JsonPointer)),
						.Message = issue.Message,
						.Hint = issue.Hint,
						.Suggestions = issue.Suggestions });
				}
				if (issues.empty())
					issues.push_back({ .JsonPointer = locate(std::string_view(), 0), .Message = structure.error().GetMessageText(), .Hint = {}, .Suggestions = {} });
				return std::unexpected(RefuseEdit(std::move(issues)));
			}
			if (library == nullptr)
				return {};
			const std::vector<GraphIssue> afterIssues = ValidateGraph(after, *library);
			if (std::ranges::all_of(afterIssues, &GraphIssue::Incomplete))
				return {};
			IssueCounts existing = CountBlockingIssues(ValidateGraph(before, *library));
			std::vector<ErrorIssue> added;
			for (const GraphIssue& issue : afterIssues)
			{
				if (issue.Incomplete)
					continue;
				if (const auto found = existing.find({ issue.Node, issue.Message }); found != existing.end() && found->second > 0)
				{
					--found->second;
					continue;
				}
				added.push_back({ .JsonPointer = locate(issue.JsonPointer, issue.Node), .Message = issue.Message, .Hint = {}, .Suggestions = {} });
			}
			if (added.empty())
				return {};
			return std::unexpected(RefuseEdit(std::move(added)));
		}

		// Writes `after` (canonical) over the graph's source as one AssetEditCommand labelled `label`: one undo step,
		// written through EditorContext's write path (provenance, the dry-run overlay, the reimport, deferred while a
		// lockstep, recording, replay or test session runs). Writes nothing and returns 0 when the document did not change.
		Result<uint32_t> WriteGraph(EditorContext& editor, const LoadedGraph& graph, const GraphDocument& after, std::string label)
		{
			if (after == graph.Document)
				return 0u;
			ENGINE_TRY_ASSIGN(const std::string text, GraphToText(after, editor.GetTypeRegistry()));
			if (text == graph.Text)
				return 0u;
			ENGINE_TRY_ASSIGN(Scope<AssetEditCommand> command, AssetEditCommand::CreateForWrite(editor, graph.Source, AsBytes(text), std::move(label)));
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(std::move(command)));
			return ToAutomationCounter(undoIndex);
		}

		// Success when the list param `pointer` holds 1 (0 with `allowEmpty`) to MaxGraphMethodItems items.
		Status CheckItemCount(size_t count, std::string_view pointer, bool allowEmpty)
		{
			if ((count > 0 || allowEmpty) && count <= MaxGraphMethodItems)
				return {};
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
				std::format("{} takes {} to {} items, not {}", pointer, allowEmpty ? 0 : 1, MaxGraphMethodItems, count), "split a larger edit into several calls"));
		}

		// A field or variable given to graph.setMembers, checked alone by ValidateGraphStructure (its name, default, element,
		// asset type, enum values and options), each issue located below `pointer` ("/fields/2"). `member` is a one-member
		// document's "/Fields/0" or "/Variables/0".
		Status CheckMemberAlone(const GraphDocument& probe, std::string_view member, std::string_view pointer)
		{
			const Status structure = ValidateGraphStructure(probe);
			if (structure)
				return {};
			std::vector<ErrorIssue> issues;
			for (const ErrorIssue& issue : structure.error().GetIssues())
			{
				std::string located(pointer);
				if (issue.JsonPointer.starts_with(member))
					located += issue.JsonPointer.substr(member.size());
				issues.push_back({ .JsonPointer = std::move(located), .Message = issue.Message, .Hint = issue.Hint, .Suggestions = issue.Suggestions });
			}
			if (issues.empty())
				issues.push_back({ .JsonPointer = std::string(pointer), .Message = structure.error().GetMessageText(), .Hint = {}, .Suggestions = {} });
			return std::unexpected(RefuseEdit(std::move(issues)));
		}

		template<typename Member>
		bool ChangesKind(const Member& before, const Member& after)
		{
			return before.Kind != after.Kind || before.Element != after.Element || before.AssetType != after.AssetType || before.EnumValues != after.EnumValues;
		}

		// The script slot of `entity` graph.liveValues reads (decision 14): the first whose asset is a graph, or with `behaviour`
		// the first whose graph names that behaviour, with the graph's current document (LoadLiveGraph's kept read). Errors:
		// NotFound at /script for an entity with no graph slot, or none of that behaviour (its hint lists the entity's graph
		// behaviours, or says that none of its graphs' sources reads); the chosen graph's read errors (ImportFailed for a source
		// that does not read).
		struct LiveGraphSlot
		{
			uint32_t Slot = 0;
			GraphLiveCacheEntry* Graph = nullptr; // the editor's GraphLiveCache's entry, valid for the call
		};

		Result<LiveGraphSlot> FindLiveGraphSlot(EditorMethodContext& context, const Scene& scene, ConstEntity entity, std::string_view behaviour)
		{
			EditorContext& editor = context.GetEditor();
			const EditorAssetManager& assets = editor.GetAssets();
			const AssetRegistry& registry = assets.GetRegistry();
			const ScriptComponent* component = entity.TryGetComponent<ScriptComponent>();
			bool graphs = false;
			std::string names;
			for (size_t slot = 0; component != nullptr && slot < component->Scripts.size(); ++slot)
			{
				const AssetHandle handle = component->Scripts[slot].Script.GetHandle();
				const AssetRecord* record = handle.IsValid() ? registry.Find(handle) : nullptr;
				if (record == nullptr || record->Metadata.Importer != GraphImporter::Id)
					continue;
				graphs = true;
				Result<GraphLiveCacheEntry*> graph = FindGraphSource(assets, handle, "/script").and_then([&editor, handle](const VfsPath& source)
				{
					return LoadLiveGraph(editor, handle, source);
				});
				if (behaviour.empty())
				{
					if (!graph)
						return std::unexpected(std::move(graph).error());
					return LiveGraphSlot{ .Slot = static_cast<uint32_t>(slot), .Graph = *graph };
				}
				if (!graph)
					continue; // a source that does not read names no behaviour now
				if ((*graph)->Document.Name == behaviour)
					return LiveGraphSlot{ .Slot = static_cast<uint32_t>(slot), .Graph = *graph };
				names += std::format("{}{} (slot {})", names.empty() ? "" : ", ", (*graph)->Document.Name, slot);
			}
			const std::string path = scene.GetEntityPath(entity);
			if (!graphs)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/script", std::format("entity '{}' has no script slot holding a graph", path),
					"graph.liveValues reads a graph behaviour; script.eval reads a Luau one"));
			}
			return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/script",
				std::format("entity '{}' has no graph slot whose behaviour is named '{}'", path, behaviour),
				names.empty() ? std::string("none of its graphs' sources reads; graph.read names what is wrong") : "its graph behaviours: " + names));
		}

	}

	namespace Automation {

		Result<GraphCreateResult> GraphCreate(EditorMethodContext& context, const GraphCreateParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const VfsPath path, Utils::ResolveAssetsPath(context, params.Path, "/path", GraphFileExtension));
			std::string name = params.Name.empty() ? std::string(path.GetStem()) : params.Name;
			if (!IsGraphIdentifier(name))
			{
				const std::string what = params.Name.empty() ? std::format("the file name '{}'", name) : std::format("'{}'", name);
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/name",
					std::format("{} is not a graph's name: a Luau identifier of at most {} bytes that is not a reserved word and does not start with \"__\"", what, MaxGraphNameBytes),
					params.Name.empty() ? "pass name, the behaviour's Script.Define name" : "the name is the behaviour's Script.Define name"));
			}
			ENGINE_TRY(Utils::RefreshAssets(editor));
			VirtualFileSystem& vfs = editor.GetVfs();
			const std::string relative = Utils::ToProjectRelative(path);
			ENGINE_TRY_ASSIGN(const VfsPath metaPath, GetMetaPath(path));
			for (const VfsPath& taken : { path, metaPath })
			{
				if (vfs.Exists(taken))
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/path", std::format("'{}' already exists", Utils::ToProjectRelative(taken)),
						"choose another path, or edit the existing graph with the graph methods"));
				}
			}
			GraphDocument document;
			document.Name = name;
			ENGINE_TRY_ASSIGN(const std::string text, GraphToText(document, editor.GetTypeRegistry()));
			ENGINE_TRY_ASSIGN(const AssetMetadata metadata, editor.GetAssets().CreateMetadata(path));
			const std::string metaText = SerializeAssetMetadata(metadata);
			std::vector<AssetFileEdit> edits;
			Utils::AddMissingDirectoryEdits(vfs, path.GetParent(), edits);
			edits.push_back(AssetFileEdit{ .Path = path, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = TextToBuffer(text) });
			edits.push_back(AssetFileEdit{ .Path = metaPath, .Kind = AssetFileKind::File, .Before = std::nullopt, .After = TextToBuffer(metaText) });
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, editor.Execute(CreateScope<AssetEditCommand>("Create Graph", std::move(edits))));
			return GraphCreateResult{ .Graph = AssetSummary{ .Id = metadata.Handle.ToString(), .Path = relative, .Type = metadata.Type },
				.Name = std::move(name),
				.UndoIndex = ToAutomationCounter(undoIndex) };
		}

		Result<GraphReadResult> GraphRead(EditorMethodContext& context, const GraphReadParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(LoadedGraph graph, LoadGraph(context, params.Graph));
			ENGINE_TRY_ASSIGN(const GraphNodeLibrary* library, GetLibrary(editor));
			EditorAssetManager& assets = editor.GetAssets();
			GraphReadResult result;
			result.Graph = Utils::MakeAssetSummary(assets, graph.Handle);

			// The graph's own issues, in document order, then the statements no event reaches (only for a graph whose kinds,
			// links and values hold, so its flows are what the transpiler would follow).
			const std::vector<GraphIssue> issues = ValidateGraph(graph.Document, *library);
			bool blocking = false;
			for (const GraphIssue& issue : issues)
			{
				result.Diagnostics.push_back({ .Severity = DiagnosticSeverity::Error,
					.Code = std::string(GraphInvalidCode),
					.Message = issue.Message,
					.Node = issue.Node,
					.JsonPointer = issue.JsonPointer,
					.Line = 0,
					.Incomplete = issue.Incomplete });
				blocking = blocking || !issue.Incomplete;
			}
			if (!blocking)
			{
				for (const uint32_t id : FindUnreachableGraphNodes(graph.Document, *library))
				{
					const auto node = std::ranges::lower_bound(graph.Document.Nodes, id, {}, &GraphNode::Id);
					const size_t index = static_cast<size_t>(node - graph.Document.Nodes.begin());
					const std::string kind = node != graph.Document.Nodes.end() ? node->Kind : std::string();
					result.Diagnostics.push_back({ .Severity = DiagnosticSeverity::Warning,
						.Code = std::string(GraphUnreachableNodeCode),
						.Message = std::format("node {} ({}) is a statement that no event's flow reaches, so it never runs", id, kind),
						.Node = id,
						.JsonPointer = std::format("/Nodes/{}", index),
						.Line = 0,
						.Incomplete = false });
				}
			}

			// Only a valid, complete graph has a text (decision 7), whose line map names each script finding's node.
			std::optional<GraphTranspileResult> transpiled;
			if (issues.empty())
			{
				Result<GraphTranspileResult> text = TranspileGraph(graph.Document, *library, graph.Path);
				if (text)
					transpiled = std::move(*text);
			}

			// The latest import's script check of the text (decision 13), once the imports the edits scheduled have run, and
			// only while it is of the file's current bytes: a session that defers reimports (§7.5 race rule 4) leaves it
			// behind until it ends. Its GRAPH_INVALID findings are the issues above.
			assets.WaitIdle();
			if (const Result<ScriptImportCheck> check = assets.GetScriptCheck(graph.Handle); check && check->Performed && check->SourceHash == XXH64(graph.Text))
			{
				std::vector<ScriptDiagnostic> findings;
				for (const ScriptDiagnostic& finding : check->Diagnostics)
					if (finding.Code != GraphInvalidCode)
						findings.push_back(finding);
				std::ranges::stable_sort(findings, {}, &ScriptDiagnostic::Line);
				for (const ScriptDiagnostic& finding : findings)
				{
					result.Diagnostics.push_back({ .Severity = finding.Severity,
						.Code = finding.Code,
						.Message = finding.Message,
						.Node = transpiled.has_value() ? FindGraphNodeAtLine(*transpiled, finding.Line) : 0,
						.JsonPointer = {},
						.Line = finding.Line,
						.Incomplete = false });
				}
			}
			if (params.Luau && transpiled.has_value())
			{
				result.Luau = std::move(transpiled->Luau);
				result.LineNodes = std::move(transpiled->LineNodes);
			}
			result.Document = std::move(graph.Document);
			return result;
		}

		Result<GraphNodesResult> GraphNodes(EditorMethodContext& context, const GraphNodesParams& params)
		{
			ENGINE_TRY(CheckItemCount(params.Kinds.size(), "/kinds", true));
			ENGINE_TRY_ASSIGN(const GraphNodeLibrary* library, GetLibrary(context.GetEditor()));
			const std::span<const GraphNodeDescription> common = library->GetNodes();
			std::vector<GraphNodeDescription> nodes(common.begin(), common.end());
			if (!params.Graph.empty())
			{
				ENGINE_TRY_ASSIGN(const LoadedGraph graph, LoadGraph(context, params.Graph));
				std::vector<GraphNodeDescription> members = library->GetMemberNodes(graph.Document);
				nodes.insert(nodes.end(), std::make_move_iterator(members.begin()), std::make_move_iterator(members.end()));
				std::ranges::sort(nodes, {}, &GraphNodeDescription::Kind);
			}
			std::set<std::string_view> wanted;
			for (size_t index = 0; index < params.Kinds.size(); ++index)
			{
				const std::string& kind = params.Kinds[index];
				if (std::ranges::find(nodes, kind, &GraphNodeDescription::Kind) == nodes.end())
				{
					std::vector<std::string> kinds;
					for (const GraphNodeDescription& node : nodes)
						kinds.push_back(node.Kind);
					return std::unexpected(MakeUnknownKindError(std::format("/kinds/{}", index), kind, kinds));
				}
				wanted.insert(kind);
			}
			GraphNodesResult result;
			for (GraphNodeDescription& node : nodes)
				if (node.Kind.starts_with(params.Prefix) && (wanted.empty() || wanted.contains(node.Kind)))
					result.Nodes.push_back(std::move(node));
			return result;
		}

		Result<GraphAddNodeResult> GraphAddNode(EditorMethodContext& context, const GraphAddNodeParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const LoadedGraph graph, LoadGraph(context, params.Graph));
			ENGINE_TRY_ASSIGN(const GraphNodeLibrary* library, GetLibrary(editor));
			const Result<GraphNodeDescription> description = library->Resolve(params.Kind, graph.Document);
			if (!description)
				return std::unexpected(MakeUnknownKindError("/kind", params.Kind, GetKindNames(*library, graph.Document)));
			glm::vec2 position = GetDefaultNodePosition(graph.Document);
			if (context.HasParam("position"))
			{
				ENGINE_TRY(CheckPosition(params.Position, "/position"));
				position = params.Position;
			}
			if (graph.Document.Nodes.size() >= MaxGraphNodes)
				return MakeError(ErrorCode::Validation, "'{}' holds {} nodes, the most a graph holds", graph.Path, MaxGraphNodes);
			const uint32_t id = GetNextGraphNodeId(graph.Document);
			if (id == 0)
				return MakeError(ErrorCode::Validation, "'{}' has no node id left: its highest id is {}", graph.Path, MaxGraphNodeId);

			GraphNode node{ .Id = id, .Kind = params.Kind, .Position = position, .Values = {} };
			for (const auto& [pin, value] : params.Values)
			{
				ENGINE_TRY(CheckDataInput(*description, id, pin, std::format("/values/{}", EscapePointerToken(pin))));
				// A null literal is no literal, as graph.setValues' null removes one.
				if (!value.Get().is_null())
					node.Values.emplace(pin, value);
			}
			GraphDocument after = graph.Document;
			after.Nodes.push_back(std::move(node));
			CanonicalizeGraph(after);
			ENGINE_TRY(CheckEdit(graph.Document, after, library, [id](std::string_view pointer, uint32_t issueNode)
			{
				const std::string_view rest = SplitDocumentPointer(pointer).Rest;
				if (issueNode != id)
					return std::string("/kind");
				if (const std::string_view pin = FindValuePin(rest); !pin.empty())
					return std::format("/values/{}", pin);
				if (rest.starts_with("/Values"))
					return std::string("/values");
				return std::string(rest.starts_with("/Position") ? "/position" : "/kind");
			}));
			ENGINE_TRY_ASSIGN(const uint32_t undoIndex, WriteGraph(editor, graph, after, "Add Graph Node"));
			return GraphAddNodeResult{ .Graph = Utils::MakeAssetSummary(editor.GetAssets(), graph.Handle), .Node = id, .UndoIndex = undoIndex };
		}

		Result<GraphRemoveNodesResult> GraphRemoveNodes(EditorMethodContext& context, const GraphRemoveNodesParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY(CheckItemCount(params.Nodes.size(), "/nodes", false));
			ENGINE_TRY_ASSIGN(const LoadedGraph graph, LoadGraph(context, params.Graph));
			ENGINE_TRY_ASSIGN(const GraphNodeLibrary* library, GetLibrary(editor));
			std::set<uint32_t> removed;
			for (size_t index = 0; index < params.Nodes.size(); ++index)
			{
				const uint32_t id = params.Nodes[index];
				const std::string pointer = std::format("/nodes/{}", index);
				if (FindGraphNode(graph.Document, id) == nullptr)
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' has no node {}", graph.Path, id), "graph.read lists the nodes"));
				if (!removed.insert(id).second)
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("node {} is listed twice", id)));
			}
			GraphDocument after = graph.Document;
			std::erase_if(after.Nodes, [&removed](const GraphNode& node)
			{
				return removed.contains(node.Id);
			});
			GraphRemoveNodesResult result;
			std::erase_if(after.Links, [&removed, &result](const GraphLink& link)
			{
				const bool touches = removed.contains(link.From.Node) || removed.contains(link.To.Node);
				if (touches)
					result.RemovedLinks.push_back(link);
				return touches;
			});
			// Removing nodes leaves unfinished inputs (Incomplete), which are accepted; but removing one of the exec links into a
			// merge (a node several links enter, its flow a function of its own) lays its flow out again at the depth of the
			// link that is left, which may nest it too deep, so the graph is checked against the library.
			ENGINE_TRY(CheckEdit(graph.Document, after, library, [](std::string_view, uint32_t)
			{
				return std::string("/nodes");
			}));
			ENGINE_TRY_ASSIGN(result.UndoIndex, WriteGraph(editor, graph, after, "Remove Graph Nodes"));
			result.Graph = Utils::MakeAssetSummary(editor.GetAssets(), graph.Handle);
			return result;
		}

		Result<GraphEditResult> GraphMoveNodes(EditorMethodContext& context, const GraphMoveNodesParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY(CheckItemCount(params.Positions.size(), "/positions", false));
			ENGINE_TRY_ASSIGN(const LoadedGraph graph, LoadGraph(context, params.Graph));
			GraphDocument after = graph.Document;
			for (size_t index = 0; index < params.Positions.size(); ++index)
			{
				const GraphNodePosition& moved = params.Positions[index];
				GraphNode* node = FindMutableNode(after, moved.Node);
				if (node == nullptr)
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("/positions/{}/node", index),
						std::format("'{}' has no node {}", graph.Path, moved.Node), "graph.read lists the nodes"));
				}
				ENGINE_TRY(CheckPosition(moved.Position, std::format("/positions/{}/position", index)));
				node->Position = moved.Position;
			}
			// Layout only: the transpiled text never reads a position.
			ENGINE_TRY(CheckEdit(graph.Document, after, nullptr, [](std::string_view, uint32_t)
			{
				return std::string("/positions");
			}));
			ENGINE_TRY_ASSIGN(const uint32_t undoIndex, WriteGraph(editor, graph, after, "Move Graph Nodes"));
			return GraphEditResult{ .Graph = Utils::MakeAssetSummary(editor.GetAssets(), graph.Handle), .UndoIndex = undoIndex };
		}

		Result<GraphConnectResult> GraphConnect(EditorMethodContext& context, const GraphConnectParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const LoadedGraph graph, LoadGraph(context, params.Graph));
			ENGINE_TRY_ASSIGN(const GraphNodeLibrary* library, GetLibrary(editor));
			const GraphDocument& before = graph.Document;
			const GraphNode* fromNode = FindGraphNode(before, params.From.Node);
			if (fromNode == nullptr)
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/from/Node", std::format("'{}' has no node {}", graph.Path, params.From.Node), "graph.read lists the nodes"));
			const GraphNode* toNode = FindGraphNode(before, params.To.Node);
			if (toNode == nullptr)
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/to/Node", std::format("'{}' has no node {}", graph.Path, params.To.Node), "graph.read lists the nodes"));
			if (fromNode->Id == toNode->Id)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/to/Node", std::format("a link joins two different nodes, not node {} to itself", toNode->Id),
					"link the output to another node's input"));
			}
			ENGINE_TRY_ASSIGN(const GraphNodeDescription fromKind, ResolveNodeKind(*library, before, *fromNode, "/from/Node"));
			ENGINE_TRY_ASSIGN(const GraphNodeDescription toKind, ResolveNodeKind(*library, before, *toNode, "/to/Node"));
			const GraphPinDescription* fromPin = FindPin(fromKind, params.From.Pin);
			if (fromPin == nullptr || fromPin->Direction != GraphPinDirection::Output)
			{
				const std::vector<std::string> outputs = GetPinNames(fromKind, GraphPinDirection::Output, false);
				return std::unexpected(MakeUnknownNameError("/from/Pin",
					std::format("'{}' is {} of node {} ({}): a link starts at an output pin", params.From.Pin, fromPin == nullptr ? "not a pin" : "an input pin", fromNode->Id, fromKind.Kind),
					params.From.Pin, outputs, std::format("the output pins of {} are {}", fromKind.Kind, DescribePins(outputs))));
			}
			const GraphPinDescription* toPin = FindPin(toKind, params.To.Pin);
			if (toPin == nullptr || toPin->Direction != GraphPinDirection::Input)
			{
				const std::vector<std::string> inputs = GetPinNames(toKind, GraphPinDirection::Input, false);
				return std::unexpected(MakeUnknownNameError("/to/Pin",
					std::format("'{}' is {} of node {} ({}): a link ends at an input pin", params.To.Pin, toPin == nullptr ? "not a pin" : "an output pin", toNode->Id, toKind.Kind),
					params.To.Pin, inputs, std::format("the input pins of {} are {}", toKind.Kind, DescribePins(inputs))));
			}
			if (!AreGraphPinsCompatible(*fromPin, *toPin))
			{
				const bool optional = fromPin->Optional && !toPin->Optional && toPin->Type != GraphPinType::Exec && toPin->Type != GraphPinType::Any;
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/to",
					std::format("pin '{}' of node {} gives {} and pin '{}' of node {} takes {}, which do not link", fromPin->Name, fromNode->Id, DescribePinType(*fromPin),
						toPin->Name, toNode->Id, DescribePinType(*toPin)),
					optional ? "the output may give nil, which the input does not take: link it to a Flow.IfNotNil node's Value and use its Result on its True flow"
							 : std::string(ListKindsHint)));
			}

			GraphConnectResult result;
			const GraphLink link{ .From = params.From, .To = params.To };
			GraphDocument after = before;
			if (std::ranges::find(after.Links, link) == after.Links.end())
			{
				// A data input and an exec output hold one link: the new one replaces theirs.
				const bool dataInput = toPin->Type != GraphPinType::Exec;
				const bool execOutput = fromPin->Type == GraphPinType::Exec;
				std::erase_if(after.Links, [&params, &result, dataInput, execOutput](const GraphLink& existing)
				{
					const bool replaced = (dataInput && existing.To == params.To) || (execOutput && existing.From == params.From);
					if (replaced)
						result.Replaced.push_back(existing);
					return replaced;
				});
				after.Links.push_back(link);
				CanonicalizeGraph(after);
			}
			// What is left to refuse is a cycle, which only the whole graph shows.
			ENGINE_TRY(CheckEdit(before, after, library, [](std::string_view, uint32_t)
			{
				return std::string("/to");
			}));
			ENGINE_TRY_ASSIGN(result.UndoIndex, WriteGraph(editor, graph, after, "Connect Graph Pins"));
			result.Graph = Utils::MakeAssetSummary(editor.GetAssets(), graph.Handle);
			return result;
		}

		Result<GraphEditResult> GraphDisconnect(EditorMethodContext& context, const GraphDisconnectParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const LoadedGraph graph, LoadGraph(context, params.Graph));
			ENGINE_TRY_ASSIGN(const GraphNodeLibrary* library, GetLibrary(editor));
			GraphDocument after = graph.Document;
			const GraphLink link{ .From = params.From, .To = params.To };
			if (std::erase(after.Links, link) == 0)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/to",
					std::format("'{}' has no link from pin '{}' of node {} to pin '{}' of node {}", graph.Path, params.From.Pin, params.From.Node, params.To.Pin, params.To.Node),
					"graph.read lists the links"));
			}
			// Removing a link leaves an unfinished input (Incomplete), which is accepted; but removing one of the exec links into
			// a merge lays its flow out again at the depth of the link that is left, which may nest it too deep, so the graph is
			// checked against the library.
			ENGINE_TRY(CheckEdit(graph.Document, after, library, [](std::string_view, uint32_t)
			{
				return std::string("/to");
			}));
			ENGINE_TRY_ASSIGN(const uint32_t undoIndex, WriteGraph(editor, graph, after, "Disconnect Graph Pins"));
			return GraphEditResult{ .Graph = Utils::MakeAssetSummary(editor.GetAssets(), graph.Handle), .UndoIndex = undoIndex };
		}

		Result<GraphEditResult> GraphSetValues(EditorMethodContext& context, const GraphSetValuesParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY(CheckItemCount(params.Values.size(), "/values", false));
			ENGINE_TRY_ASSIGN(const LoadedGraph graph, LoadGraph(context, params.Graph));
			ENGINE_TRY_ASSIGN(const GraphNodeLibrary* library, GetLibrary(editor));
			GraphDocument after = graph.Document;
			// The last edit of each pin, and of each node, which a refusal of the result is located at.
			std::map<std::pair<uint32_t, std::string>, size_t> pinEdits;
			std::map<uint32_t, size_t> nodeEdits;
			for (size_t index = 0; index < params.Values.size(); ++index)
			{
				const GraphValueEdit& edit = params.Values[index];
				GraphNode* node = FindMutableNode(after, edit.Node);
				if (node == nullptr)
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("/values/{}/node", index),
						std::format("'{}' has no node {}", graph.Path, edit.Node), "graph.read lists the nodes"));
				}
				ENGINE_TRY_ASSIGN(const GraphNodeDescription description, ResolveNodeKind(*library, graph.Document, *node, std::format("/values/{}/node", index)));
				ENGINE_TRY(CheckDataInput(description, node->Id, edit.Pin, std::format("/values/{}/pin", index)));
				if (edit.Value.Get().is_null())
					node->Values.erase(edit.Pin);
				else
					node->Values.insert_or_assign(edit.Pin, edit.Value);
				pinEdits[{ edit.Node, edit.Pin }] = index;
				nodeEdits[edit.Node] = index;
			}
			ENGINE_TRY(CheckEdit(graph.Document, after, library, [&pinEdits, &nodeEdits](std::string_view pointer, uint32_t issueNode)
			{
				const std::string_view pin = FindValuePin(SplitDocumentPointer(pointer).Rest);
				if (const auto edit = pinEdits.find({ issueNode, std::string(pin) }); !pin.empty() && edit != pinEdits.end())
					return std::format("/values/{}/value", edit->second);
				if (const auto edit = nodeEdits.find(issueNode); edit != nodeEdits.end())
					return std::format("/values/{}", edit->second);
				return std::string("/values");
			}));
			ENGINE_TRY_ASSIGN(const uint32_t undoIndex, WriteGraph(editor, graph, after, "Set Graph Values"));
			return GraphEditResult{ .Graph = Utils::MakeAssetSummary(editor.GetAssets(), graph.Handle), .UndoIndex = undoIndex };
		}

		Result<GraphSetMembersResult> GraphSetMembers(EditorMethodContext& context, const GraphSetMembersParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY(CheckItemCount(params.Rename.size(), "/rename", true));
			ENGINE_TRY(CheckItemCount(params.Remove.size(), "/remove", true));
			ENGINE_TRY(CheckItemCount(params.Fields.size(), "/fields", true));
			ENGINE_TRY(CheckItemCount(params.Variables.size(), "/variables", true));
			if (!params.Name.empty() && !IsGraphIdentifier(params.Name))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/name",
					std::format("'{}' is not a graph's name: a Luau identifier of at most {} bytes that is not a reserved word and does not start with \"__\"", params.Name, MaxGraphNameBytes)));
			}
			ENGINE_TRY_ASSIGN(const LoadedGraph graph, LoadGraph(context, params.Graph));
			GraphDocument after = graph.Document;
			GraphSetMembersResult result;
			if (!params.Name.empty())
				after.Name = params.Name;

			for (size_t index = 0; index < params.Rename.size(); ++index)
			{
				const GraphMemberRename& rename = params.Rename[index];
				if (!HasMember(after, rename.From))
				{
					return std::unexpected(MakeUnknownNameError(std::format("/rename/{}/from", index), std::format("'{}' has no field or variable '{}'", graph.Path, rename.From),
						rename.From, GetMemberNames(after), "graph.read lists the fields and variables"));
				}
				if (rename.To == rename.From)
					continue;
				const std::string pointer = std::format("/rename/{}/to", index);
				if (!IsGraphIdentifier(rename.To) || IsReservedScriptFieldName(rename.To))
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
						std::format("'{}' is not a member's name: a Luau identifier of at most {} bytes that is not a reserved word, a reserved script field name or a name "
									"starting with \"__\"",
							rename.To, MaxGraphNameBytes)));
				}
				if (HasMember(after, rename.To))
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' already has a field or variable '{}'", graph.Path, rename.To)));
				for (GraphField& field : after.Fields)
					if (field.Name == rename.From)
						field.Name = rename.To;
				for (GraphVariable& variable : after.Variables)
					if (variable.Name == rename.From)
						variable.Name = rename.To;
				// Its Self.Get and Self.Set nodes follow, keeping their ids, values and links.
				for (GraphNode& node : after.Nodes)
				{
					if (GetMemberOfKind(node.Kind) != rename.From)
						continue;
					node.Kind = std::format("{}{}", node.Kind.starts_with(SelfGetPrefix) ? SelfGetPrefix : SelfSetPrefix, rename.To);
					++result.RenamedNodes;
				}
			}

			for (size_t index = 0; index < params.Remove.size(); ++index)
			{
				const std::string& name = params.Remove[index];
				const std::string pointer = std::format("/remove/{}", index);
				if (!HasMember(after, name))
				{
					return std::unexpected(MakeUnknownNameError(pointer, std::format("'{}' has no field or variable '{}'", graph.Path, name), name, GetMemberNames(after),
						"graph.read lists the fields and variables"));
				}
				if (const std::vector<uint32_t> users = FindMemberNodes(after, name); !users.empty())
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer,
						std::format("'{}' is used by node{} {}", name, users.size() == 1 ? "" : "s", JoinIds(users)),
						std::format("remove node{} {} with graph.removeNodes first, or rename the member instead", users.size() == 1 ? "" : "s", JoinIds(users))));
				}
				std::erase_if(after.Fields, [&name](const GraphField& field)
				{
					return field.Name == name;
				});
				std::erase_if(after.Variables, [&name](const GraphVariable& variable)
				{
					return variable.Name == name;
				});
			}

			// Each member given is checked alone (decision 4's member rules, located in the params), then added or replaces
			// the member of its name; a kind change is refused below when a node's link or literal needs the old kind.
			std::map<std::string, std::string> kindChanges; // member name -> the param pointer of its new Kind
			for (size_t index = 0; index < params.Fields.size(); ++index)
			{
				const GraphField& field = params.Fields[index];
				const std::string pointer = std::format("/fields/{}", index);
				ENGINE_TRY(CheckMemberAlone(GraphDocument{ .Name = "Graph", .Fields = { field }, .Variables = {}, .Nodes = {}, .Links = {} }, "/Fields/0", pointer));
				if (std::ranges::find(after.Variables, field.Name, &GraphVariable::Name) != after.Variables.end())
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer + "/Name", std::format("'{}' is a variable, not a field", field.Name),
						"a member becomes a field by its removal and addition: remove it and give the field in one graph.setMembers call"));
				}
				if (const auto existing = std::ranges::find(after.Fields, field.Name, &GraphField::Name); existing != after.Fields.end())
				{
					if (ChangesKind(*existing, field))
						kindChanges[field.Name] = pointer + "/Kind";
					*existing = field;
				}
				else
				{
					after.Fields.push_back(field);
				}
			}
			for (size_t index = 0; index < params.Variables.size(); ++index)
			{
				const GraphVariable& variable = params.Variables[index];
				const std::string pointer = std::format("/variables/{}", index);
				ENGINE_TRY(CheckMemberAlone(GraphDocument{ .Name = "Graph", .Fields = {}, .Variables = { variable }, .Nodes = {}, .Links = {} }, "/Variables/0", pointer));
				if (std::ranges::find(after.Fields, variable.Name, &GraphField::Name) != after.Fields.end())
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, pointer + "/Name", std::format("'{}' is a field, not a variable", variable.Name),
						"a member becomes a variable by its removal and addition: remove it and give the variable in one graph.setMembers call"));
				}
				if (const auto existing = std::ranges::find(after.Variables, variable.Name, &GraphVariable::Name); existing != after.Variables.end())
				{
					if (ChangesKind(*existing, variable))
						kindChanges[variable.Name] = pointer + "/Kind";
					*existing = variable;
				}
				else
				{
					after.Variables.push_back(variable);
				}
			}
			CanonicalizeGraph(after);
			if (after == graph.Document)
			{
				result.Graph = Utils::MakeAssetSummary(editor.GetAssets(), graph.Handle);
				return result;
			}

			ENGINE_TRY_ASSIGN(const GraphNodeLibrary* library, GetLibrary(editor));
			ENGINE_TRY(CheckEdit(graph.Document, after, library, [&params, &after, &kindChanges](std::string_view pointer, uint32_t issueNode)
			{
				const DocumentPointer split = SplitDocumentPointer(pointer);
				if (split.Section == "Name")
					return std::string("/name");
				if (split.Section == "Fields" || split.Section == "Variables")
				{
					// The member the issue concerns, located at the param that gave or renamed it.
					const bool isField = split.Section == "Fields";
					const std::string_view list = isField ? "/fields" : "/variables";
					const size_t count = isField ? after.Fields.size() : after.Variables.size();
					if (!split.Index.has_value() || *split.Index >= count)
						return std::string(list);
					const std::string& name = isField ? after.Fields[*split.Index].Name : after.Variables[*split.Index].Name;
					const size_t given = isField ? params.Fields.size() : params.Variables.size();
					for (size_t index = given; index-- > 0;)
					{
						const std::string& candidate = isField ? params.Fields[index].Name : params.Variables[index].Name;
						if (candidate == name)
							return std::format("{}/{}{}", list, index, split.Rest);
					}
					for (size_t index = params.Rename.size(); index-- > 0;)
						if (params.Rename[index].To == name)
							return std::format("/rename/{}/to", index);
					return std::string(list);
				}
				// A ValidateGraph issue: at the Kind of the member whose node it concerns, else of the first member whose kind
				// changed, since only a kind change makes a link or a literal of the graph's nodes wrong here.
				if (const GraphNode* node = FindGraphNode(after, issueNode); node != nullptr)
					if (const auto change = kindChanges.find(std::string(GetMemberOfKind(node->Kind))); change != kindChanges.end())
						return change->second;
				return kindChanges.empty() ? std::string() : kindChanges.begin()->second;
			}));
			ENGINE_TRY_ASSIGN(result.UndoIndex, WriteGraph(editor, graph, after, "Set Graph Members"));
			result.Graph = Utils::MakeAssetSummary(editor.GetAssets(), graph.Handle);
			return result;
		}

		Result<GraphLiveValuesResult> GraphLiveValues(EditorMethodContext& context, const GraphLiveValuesParams& params)
		{
			PlaySession* session = context.GetPlaySession();
			if (session == nullptr)
				return std::unexpected(Error(ErrorCode::InvalidState, "graph.liveValues reads a play session, and none is running").WithHint("start one with play.start"));
			ScriptEngine* scripts = session->GetScripts();
			if (scripts == nullptr)
			{
				return std::unexpected(Error(ErrorCode::InvalidState, "graph.liveValues reads a playing session's behaviours, and a simulation runs none")
						.WithHint("play.start with mode Play runs the behaviours"));
			}
			Scene& scene = session->GetScene();
			ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(scene, params.Entity, "/entity"));
			ENGINE_TRY_ASSIGN(const LiveGraphSlot slot, FindLiveGraphSlot(context, scene, entity, params.Script));
			GraphLiveCacheEntry& graph = *slot.Graph;
			Result<ScriptInstanceValues> values = scripts->ReadInstanceValues(entity.GetUUID(), slot.Slot);
			if (!values)
				return std::unexpected(std::move(values).error().WithContext(std::format("while reading the live values of '{}' in slot {}", graph.Key.Path, slot.Slot)));

			GraphLiveValuesResult result;
			result.Graph = Utils::MakeAssetSummary(context.GetEditor().GetAssets(), graph.Key.Graph);
			result.Entity = context.MakeEntitySummary(entity);
			result.Slot = slot.Slot;
			result.Tick = ToAutomationCounter(session->GetTick());
			result.Truncated = values->Truncated;

			// The nodes' last values: the live-value table's keys that are the transpiler's (GetGraphProbeKey), by (node, pin).
			for (ScriptInstanceValue& live : values->LiveValues)
			{
				if (std::optional<GraphPinRef> pin = ParseGraphProbeKey(live.Key); pin.has_value())
					result.Values.push_back({ .Node = pin->Node, .Pin = std::move(pin->Pin), .Type = std::move(live.TypeName), .Value = std::move(live.Value) });
			}
			std::ranges::sort(result.Values, [](const GraphLiveValue& left, const GraphLiveValue& right)
			{
				return std::tie(left.Node, left.Pin) < std::tie(right.Node, right.Pin);
			});

			// The graph's fields and variables by name, each with its instance value ("nil" while the instance holds none).
			for (const GraphField& field : graph.Document.Fields)
				result.Members.push_back({ .Name = field.Name, .Type = "nil", .Value = {} });
			for (const GraphVariable& variable : graph.Document.Variables)
				result.Members.push_back({ .Name = variable.Name, .Type = "nil", .Value = {} });
			std::ranges::sort(result.Members, {}, &GraphLiveMember::Name);
			for (GraphLiveMember& member : result.Members)
			{
				const auto found = std::ranges::lower_bound(values->Members, member.Name, {}, &ScriptInstanceValue::Key);
				if (found != values->Members.end() && found->Key == member.Name)
				{
					member.Type = found->TypeName;
					member.Value = found->Value;
				}
			}

			// The instance's retained errors raised by its graph's code (their script is the graph's path), each line mapped to
			// its node through the graph's current text (decision 8); transpiled only when there is an error to map, and then
			// kept with the graph's read, its line map only, until the graph changes.
			for (const ScriptError& error : scripts->GetErrors().GetErrors())
			{
				if (error.Entity != entity.GetUUID() || error.Slot != slot.Slot || error.Script != graph.Key.Path)
					continue;
				if (!graph.LineMap.has_value())
				{
					ENGINE_TRY_ASSIGN(const GraphNodeLibrary* library, GetLibrary(context.GetEditor()));
					Result<GraphTranspileResult> transpiled = TranspileGraph(graph.Document, *library, graph.Key.Path);
					graph.LineMap = GraphTranspileResult{ .Luau = {}, .LineNodes = transpiled ? std::move(transpiled->LineNodes) : std::vector<uint32_t>{}, .Probes = {} };
				}
				result.Errors.push_back({ .Node = FindGraphNodeAtLine(*graph.LineMap, error.Line), .Line = error.Line, .Message = error.Message, .Count = ToAutomationCounter(error.Count) });
			}
			return result;
		}

	}

	void RegisterGraphMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<GraphPinType>("GraphPinType", "What a graph pin carries.")
			.Entry(GraphPinType::Exec, "Exec", "Control flow, no value.")
			.Entry(GraphPinType::Any, "Any", "Any value (Luau's any): links to every data pin.")
			.Entry(GraphPinType::Bool, "Bool", "A boolean.")
			.Entry(GraphPinType::Number, "Number", "A number.")
			.Entry(GraphPinType::String, "String", "A string.")
			.Entry(GraphPinType::Vector, "Vector", "A native three-component vector.")
			.Entry(GraphPinType::Color, "Color", "A linear RGBA Color.")
			.Entry(GraphPinType::Quat, "Quat", "A quaternion.")
			.Entry(GraphPinType::Entity, "Entity", "An Entity.")
			.Entry(GraphPinType::Asset, "Asset", "An AssetRef; typeName names its AssetType when the member restricts it.")
			.Entry(GraphPinType::Enum, "Enum", "One choice of a registered enum, a string; typeName names the enum.")
			.Entry(GraphPinType::Object, "Object", "A value of another named type (a component, a TaskHandle, a record such as CollisionContact); typeName names it.");
		registry.Enum<GraphPinDirection>("GraphPinDirection", "Whether a graph pin takes or gives its value.")
			.Entry(GraphPinDirection::Input, "Input", "The pin takes a value (or, for Exec, is where the flow enters).")
			.Entry(GraphPinDirection::Output, "Output", "The pin gives a value (or, for Exec, is where the flow continues).");
		registry.Enum<GraphNodeCategory>("GraphNodeCategory", "Where the graph editor's node menu lists a node kind.")
			.Entry(GraphNodeCategory::Event, "Event", "A behaviour callback: where a flow starts (Event.*).")
			.Entry(GraphNodeCategory::Flow, "Flow", "Branches, sequences, loops and delays (Flow.*).")
			.Entry(GraphNodeCategory::Member, "Member", "The behaviour's own entity, fields and variables (Self.*).")
			.Entry(GraphNodeCategory::Operator, "Operator", "Arithmetic, comparison, logic, vector and text operators.")
			.Entry(GraphNodeCategory::Call, "Call", "A script API function, method or constructor (Call.*).")
			.Entry(GraphNodeCategory::Property, "Property", "A script API property or component field (Get.*, Set.*).")
			.Entry(GraphNodeCategory::Constant, "Constant", "A script API constant (Const.*).");
		registry.Struct<GraphPinDescription>("GraphPinInfo", "One pin of a graph node kind.")
			.Field("name", &GraphPinDescription::Name, "The pin's name, which links and values use.")
			.Field("direction", &GraphPinDescription::Direction, "Input or Output.")
			.Field("type", &GraphPinDescription::Type, "What the pin carries.")
			.Field("typeName", &GraphPinDescription::TypeName, "Enum and Object: the type's name; Asset: its AssetType, or empty; otherwise empty.")
			.Field("optional", &GraphPinDescription::Optional, "An input that may stay unlinked and unset, passing nil; an output that may give nil, which feeds only an optional or Any input (Flow.IfNotNil checks it).")
			.Field("default", &GraphPinDescription::Default, "The literal an unlinked, unset required input takes; null when it must be linked or set.");
		registry.Struct<GraphNodeDescription>("GraphNodeInfo", "One graph node kind of the node library.")
			.Field("kind", &GraphNodeDescription::Kind, "The kind a node names (\"Flow.Branch\", \"Call.Input.GetAxis\").")
			.Field("title", &GraphNodeDescription::Title, "The graph editor's caption.")
			.Field("category", &GraphNodeDescription::Category, "Where the node menu lists it.")
			.Field("description", &GraphNodeDescription::Description, "What the node does: its script API member's description, or the built-in's.")
			.Field("pure", &GraphNodeDescription::Pure, "Whether it has no exec pins and is evaluated where each statement needs its outputs.")
			.Field("pins", &GraphNodeDescription::Pins, "Its pins: the exec input, the data inputs, the exec outputs, the data outputs.");

		registry.Struct<GraphCreateParams>("GraphCreateParams", "The params of graph.create.")
			.Field("path", &GraphCreateParams::Path, "The new graph's \"Assets/....graph\" path.")
			.Field("name", &GraphCreateParams::Name, "The behaviour's name (a Luau identifier); empty takes the file's stem.");
		registry.Struct<GraphCreateResult>("GraphCreateResult", "The graph graph.create made.")
			.Field("graph", &GraphCreateResult::Graph, "The new graph, a Script asset.")
			.Field("name", &GraphCreateResult::Name, "The behaviour's name.")
			.Field("undoIndex", &GraphCreateResult::UndoIndex, "The creation's undo step; 0 in a dry run or a batch.");
		registry.Struct<GraphDiagnostic>("GraphDiagnostic", "One finding of a graph, mapped to its node.")
			.Field("severity", &GraphDiagnostic::Severity, "Error or Warning.")
			.Field("code", &GraphDiagnostic::Code, "GRAPH_INVALID, GRAPH_UNREACHABLE_NODE, SCRIPT_TYPE_ERROR or SCRIPT_COMPILE_ERROR.")
			.Field("message", &GraphDiagnostic::Message, "What is wrong.")
			.Field("node", &GraphDiagnostic::Node, "The node it concerns; 0 for the graph's name or members.")
			.Field("jsonPointer", &GraphDiagnostic::JsonPointer, "Where in the .graph document; empty for a finding of the transpiled text.")
			.Field("line", &GraphDiagnostic::Line, "The transpiled text's line of a script check's finding; 0 otherwise.")
			.Field("incomplete", &GraphDiagnostic::Incomplete, "A finding of an unfinished graph the edits accept (a required input not linked yet).");
		registry.Struct<GraphReadParams>("GraphReadParams", "The params of graph.read.")
			.Field("graph", &GraphReadParams::Graph, "The graph: its id, its \"Assets/...\" path or another asset reference.")
			.Field("luau", &GraphReadParams::Luau, "Also return the transpiled Luau text and its line map.");
		registry.Struct<GraphReadResult>("GraphReadResult", "A graph as its file holds it, with its findings.")
			.Field("graph", &GraphReadResult::Graph, "The graph.")
			.Field("document", &GraphReadResult::Document, "The graph document, in the .graph file's spelling.")
			.Field("diagnostics", &GraphReadResult::Diagnostics, "Its findings, each mapped to its node.")
			.Field("luau", &GraphReadResult::Luau, "With luau: the transpiled text; empty while the graph does not transpile.")
			.Field("lineNodes", &GraphReadResult::LineNodes, "With luau: the node of each line of the text (0 for none).");
		registry.Struct<GraphNodesParams>("GraphNodesParams", "The params of graph.nodes.")
			.Field("graph", &GraphNodesParams::Graph, "A graph whose Self.Get and Self.Set kinds to add; empty for none.")
			.Field("prefix", &GraphNodesParams::Prefix, "Only the kinds that start with it (\"Call.Input.\", \"Flow.\").")
			.Field("kinds", &GraphNodesParams::Kinds, "Only these kinds.");
		registry.Struct<GraphNodesResult>("GraphNodesResult", "Node kinds of the node library.")
			.Field("nodes", &GraphNodesResult::Nodes, "The kinds, sorted by kind.");
		registry.Struct<GraphAddNodeParams>("GraphAddNodeParams", "The params of graph.addNode.")
			.Field("graph", &GraphAddNodeParams::Graph, "The graph.")
			.Field("kind", &GraphAddNodeParams::Kind, "The node's kind (graph.nodes lists them).")
			.Field("position", &GraphAddNodeParams::Position, "The node's place on the canvas; absent: right of the rightmost node.")
			.Field("values", &GraphAddNodeParams::Values, "Literals of the node's inputs, by pin name.");
		registry.Struct<GraphAddNodeResult>("GraphAddNodeResult", "The node graph.addNode added.")
			.Field("graph", &GraphAddNodeResult::Graph, "The graph.")
			.Field("node", &GraphAddNodeResult::Node, "The new node's id.")
			.Field("undoIndex", &GraphAddNodeResult::UndoIndex, "The edit's undo step; 0 in a dry run or a batch.");
		registry.Struct<GraphRemoveNodesParams>("GraphRemoveNodesParams", "The params of graph.removeNodes.")
			.Field("graph", &GraphRemoveNodesParams::Graph, "The graph.")
			.Field("nodes", &GraphRemoveNodesParams::Nodes, "The ids of the nodes to remove.");
		registry.Struct<GraphRemoveNodesResult>("GraphRemoveNodesResult", "The outcome of a graph.removeNodes.")
			.Field("graph", &GraphRemoveNodesResult::Graph, "The graph.")
			.Field("removedLinks", &GraphRemoveNodesResult::RemovedLinks, "The links removed with the nodes.")
			.Field("undoIndex", &GraphRemoveNodesResult::UndoIndex, "The edit's undo step; 0 in a dry run or a batch.");
		registry.Struct<GraphNodePosition>("GraphNodePosition", "A node's new place on the canvas.")
			.Field("node", &GraphNodePosition::Node, "The node's id.")
			.Field("position", &GraphNodePosition::Position, "Its top-left corner in canvas units.");
		registry.Struct<GraphMoveNodesParams>("GraphMoveNodesParams", "The params of graph.moveNodes.")
			.Field("graph", &GraphMoveNodesParams::Graph, "The graph.")
			.Field("positions", &GraphMoveNodesParams::Positions, "The nodes' new places.");
		registry.Struct<GraphEditResult>("GraphEditResult", "The outcome of a graph edit.")
			.Field("graph", &GraphEditResult::Graph, "The graph.")
			.Field("undoIndex", &GraphEditResult::UndoIndex, "The edit's undo step; 0 in a dry run, in a batch or when nothing changed.");
		registry.Struct<GraphConnectParams>("GraphConnectParams", "The params of graph.connect.")
			.Field("graph", &GraphConnectParams::Graph, "The graph.")
			.Field("from", &GraphConnectParams::From, "An output pin: {Node, Pin}.")
			.Field("to", &GraphConnectParams::To, "An input pin: {Node, Pin}.");
		registry.Struct<GraphConnectResult>("GraphConnectResult", "The outcome of a graph.connect.")
			.Field("graph", &GraphConnectResult::Graph, "The graph.")
			.Field("replaced", &GraphConnectResult::Replaced, "The link the new one replaced, if any.")
			.Field("undoIndex", &GraphConnectResult::UndoIndex, "The edit's undo step; 0 in a dry run or a batch.");
		registry.Struct<GraphDisconnectParams>("GraphDisconnectParams", "The params of graph.disconnect.")
			.Field("graph", &GraphDisconnectParams::Graph, "The graph.")
			.Field("from", &GraphDisconnectParams::From, "The link's output pin.")
			.Field("to", &GraphDisconnectParams::To, "The link's input pin.");
		registry.Struct<GraphValueEdit>("GraphValueEdit", "One input literal to set.")
			.Field("node", &GraphValueEdit::Node, "The node's id.")
			.Field("pin", &GraphValueEdit::Pin, "The data input's name.")
			.Field("value", &GraphValueEdit::Value, "The literal in its pin type's JSON spelling; null removes it.");
		registry.Struct<GraphSetValuesParams>("GraphSetValuesParams", "The params of graph.setValues.")
			.Field("graph", &GraphSetValuesParams::Graph, "The graph.")
			.Field("values", &GraphSetValuesParams::Values, "The literals, in order.");
		registry.Struct<GraphMemberRename>("GraphMemberRename", "One field or variable to rename.")
			.Field("from", &GraphMemberRename::From, "Its name.")
			.Field("to", &GraphMemberRename::To, "Its new name.");
		registry.Struct<GraphSetMembersParams>("GraphSetMembersParams", "The params of graph.setMembers.")
			.Field("graph", &GraphSetMembersParams::Graph, "The graph.")
			.Field("name", &GraphSetMembersParams::Name, "The behaviour's new name; empty keeps it.")
			.Field("rename", &GraphSetMembersParams::Rename, "Fields and variables to rename, with their Self nodes.")
			.Field("remove", &GraphSetMembersParams::Remove, "Fields and variables to remove; refused while a node uses one.")
			.Field("fields", &GraphSetMembersParams::Fields, "Fields to add or replace, by name.")
			.Field("variables", &GraphSetMembersParams::Variables, "Variables to add or replace, by name.");
		registry.Struct<GraphSetMembersResult>("GraphSetMembersResult", "The outcome of a graph.setMembers.")
			.Field("graph", &GraphSetMembersResult::Graph, "The graph.")
			.Field("renamedNodes", &GraphSetMembersResult::RenamedNodes, "The Self.Get and Self.Set nodes the renames rewrote.")
			.Field("undoIndex", &GraphSetMembersResult::UndoIndex, "The edit's undo step; 0 in a dry run, in a batch or when nothing changed.");

		registry.Struct<GraphLiveValuesParams>("GraphLiveValuesParams", "The params of graph.liveValues.")
			.Field("entity", &GraphLiveValuesParams::Entity, "An entity of the play scene: its id, a unique id prefix or its path.")
			.Field("script", &GraphLiveValuesParams::Script,
				"The slot's behaviour name, matched against the graph's current Name (of two such slots, the first); empty: the entity's first slot "
				"holding a graph.");
		registry.Struct<GraphLiveValue>("GraphLiveValue", "A node output's last value.")
			.Field("node", &GraphLiveValue::Node, "The node's id.")
			.Field("pin", &GraphLiveValue::Pin, "The output pin.")
			.Field("type", &GraphLiveValue::Type, "The value's Luau or engine type name.")
			.Field("value", &GraphLiveValue::Value,
				"The value in reflection's JSON spelling; null for a table, a function, a proxy, a non-finite number or a string that is not UTF-8, "
				"whose type still names it.");
		registry.Struct<GraphLiveMember>("GraphLiveMember", "A field's or variable's value.")
			.Field("name", &GraphLiveMember::Name, "The member's name.")
			.Field("type", &GraphLiveMember::Type, "The value's Luau or engine type name.")
			.Field("value", &GraphLiveMember::Value,
				"The value in reflection's JSON spelling; null while the instance holds none, and for a table, a function, a proxy, a non-finite "
				"number or a string that is not UTF-8, whose type still names it.");
		registry.Struct<GraphLiveError>("GraphLiveError", "A retained script error of the instance.")
			.Field("node", &GraphLiveError::Node, "The node whose code raised it; 0 when its line maps to none.")
			.Field("line", &GraphLiveError::Line, "Its line of the transpiled text.")
			.Field("message", &GraphLiveError::Message, "The error's message.")
			.Field("count", &GraphLiveError::Count, "How many times it was raised.");
		registry.Struct<GraphLiveValuesResult>("GraphLiveValuesResult", "A graph behaviour's live values in play.")
			.Field("graph", &GraphLiveValuesResult::Graph, "The graph the slot holds.")
			.Field("entity", &GraphLiveValuesResult::Entity, "The entity.")
			.Field("slot", &GraphLiveValuesResult::Slot, "The slot's index in its Script list.")
			.Field("tick", &GraphLiveValuesResult::Tick, "The play session's tick.")
			.Field("values", &GraphLiveValuesResult::Values, "The nodes' last output values, by node and pin.")
			.Field("members", &GraphLiveValuesResult::Members, "The graph's fields and variables, by name.")
			.Field("errors", &GraphLiveValuesResult::Errors, "The instance's retained script errors raised by the graph's own code, each mapped to its node.")
			.Field("truncated", &GraphLiveValuesResult::Truncated, "Whether a list was cut at its limit.");
	}

	void RegisterGraphMethods(MethodRegistry& methods)
	{
		const Json graph = "Assets/Scripts/Door.graph";
		methods.Add(
			{
				.Name = "graph.create",
				.Description = "Creates an empty visual-scripting graph, a behaviour transpiled to Luau at import, as one undoable edit.",
				.RequiredParams = { "path" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Create the Door behaviour.", .Params = Json{ { "path", graph }, { "name", "Door" } } } },
			},
			&Automation::GraphCreate);
		methods.Add(
			{
				.Name = "graph.read",
				.Description = "Reads a graph's document and its findings mapped to nodes, and optionally its transpiled Luau with each line's node.",
				.RequiredParams = { "graph" },
				.ExposeAsTool = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the Door graph and its Luau.", .Params = Json{ { "graph", graph }, { "luau", true } } } },
			},
			&Automation::GraphRead);
		methods.Add(
			{
				.Name = "graph.nodes",
				.Description = "Lists the node library's kinds with their pins and types, optionally filtered by a kind prefix, with a graph's own "
							   "Self.Get and Self.Set kinds.",
				.ExposeAsTool = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "List the flow nodes.", .Params = Json{ { "prefix", "Flow." } } } },
			},
			&Automation::GraphNodes);
		methods.Add(
			{
				.Name = "graph.addNode",
				.Description = "Adds a node of a kind to a graph, with literals for its inputs, as one undoable edit; returns its id.",
				.RequiredParams = { "graph", "kind" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Add a start event.", .Params = Json{ { "graph", graph }, { "kind", "Event.OnStart" } } } },
			},
			&Automation::GraphAddNode);
		methods.Add(
			{
				.Name = "graph.removeNodes",
				.Description = "Removes nodes and their links from a graph as one undoable edit.",
				.RequiredParams = { "graph", "nodes" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Remove node 3.", .Params = Json{ { "graph", graph }, { "nodes", Json::array({ 3 }) } } } },
			},
			&Automation::GraphRemoveNodes);
		methods.Add(
			{
				.Name = "graph.moveNodes",
				.Description = "Moves nodes on a graph's canvas (layout only) as one undoable edit.",
				.RequiredParams = { "graph", "positions" },
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Move node 3.",
					.Params = Json{ { "graph", graph }, { "positions", Json::array({ { { "node", 3 }, { "position", Json::array({ 240, 0 }) } } }) } } } },
			},
			&Automation::GraphMoveNodes);
		const Json from = { { "Node", 1 }, { "Pin", "Then" } };
		const Json to = { { "Node", 2 }, { "Pin", "In" } };
		methods.Add(
			{
				.Name = "graph.connect",
				.Description = "Links an output pin to a compatible input pin of a graph as one undoable edit; a data input or exec output already "
							   "linked loses its old link.",
				.RequiredParams = { "graph", "from", "to" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Continue the start event at node 2.", .Params = Json{ { "graph", graph }, { "from", from }, { "to", to } } } },
			},
			&Automation::GraphConnect);
		methods.Add(
			{
				.Name = "graph.disconnect",
				.Description = "Removes a link of a graph as one undoable edit.",
				.RequiredParams = { "graph", "from", "to" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Remove the start event's link.", .Params = Json{ { "graph", graph }, { "from", from }, { "to", to } } } },
			},
			&Automation::GraphDisconnect);
		methods.Add(
			{
				.Name = "graph.setValues",
				.Description = "Sets or removes literals of graph nodes' inputs as one undoable edit.",
				.RequiredParams = { "graph", "values" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Log a message.",
					.Params = Json{ { "graph", graph }, { "values", Json::array({ { { "node", 2 }, { "pin", "Values" }, { "value", "Opened" } } }) } } } },
			},
			&Automation::GraphSetValues);
		methods.Add(
			{
				.Name = "graph.setMembers",
				.Description = "Renames the behaviour and renames, removes, adds or replaces its fields and variables as one undoable edit.",
				.RequiredParams = { "graph" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Add a Speed field.",
					.Params = Json{ { "graph", graph },
						{ "fields", Json::array({ { { "Name", "Speed" }, { "Kind", "Number" }, { "Default", 2 }, { "Element", "Number" }, { "AssetType", "" }, { "EnumValues", Json::array() }, { "Options", Json{ { "Min", 0 }, { "Max", 10 } } } } }) } } } },
			},
			&Automation::GraphSetMembers);
	}

	void RegisterGraphLiveValueMethods(MethodRegistry& methods)
	{
		methods.Add(
			{
				.Name = "graph.liveValues",
				.Description = "Reads, during play (a test run's included) and without changing the session, a graph behaviour's last node values, its "
							   "fields and variables and the script errors its code raised, mapped to nodes.",
				.RequiredParams = { "entity" },
				.ExposeAsTool = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the door's live values.", .Params = Json{ { "entity", "/Door" } } } },
			},
			&Automation::GraphLiveValues);
	}

}
