#include "EnginePCH.h"
#include "Engine/Scene/StructuralValidator.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		constexpr size_t NoEntity = std::numeric_limits<size_t>::max();

		// The JSON key of PrefabLinkComponent::InstanceRoot (its registered field name).
		constexpr std::string_view InstanceRootKey = "InstanceRoot";

		// Where a finding sits within its document position: the document's own keys ("Root") come first, then for each
		// entity its ID, its Parent and its components in key order. SCENE_NONCANONICAL_ORDER about the whole array is
		// Order at the document position, after "Root".
		enum class FindingRank : uint8_t
		{
			Document,
			Order,
			ID,
			Parent,
			Component
		};

		// One structural defect (or the non-canonical-order warning), kept until the pass ends so that the findings can be
		// reported in document order.
		struct Finding
		{
			size_t Position = 0; // 0 for the document's own keys, entity index + 1 otherwise
			FindingRank Rank = FindingRank::Document;
			size_t ComponentIndex = 0; // key position within the entity's "Components"
			DiagnosticSeverity Severity = DiagnosticSeverity::Error;
			bool Repairable = true;
			std::string_view Code;
			std::string Pointer;
			std::string Message;
			UUID Entity;         // the entity's ID as written in the file
			UUID RepairedEntity; // the entity's ID in the repaired document
			std::string Repair;  // what Repair mode does about it
			Json Removed;        // the dropped component's JSON, or null
		};

		// One element of "Entities" while the graph is checked; Object receives the repairs.
		struct EntityNode
		{
			Json Object;
			std::string Pointer;
			UUID WrittenID; // as written; the invalid UUID when missing or malformed
			UUID ID;        // the final ID: WrittenID, or a fresh one in Repair mode
			size_t Parent = NoEntity;
			size_t IDFinding = NoEntity; // the finding that asks for a fresh ID
		};

		// One run of StructuralValidator::Validate.
		class DocumentValidator
		{
		public:
			DocumentValidator(const Json& document, DocumentKind kind, const TypeRegistry& registry, const LoadOptions& options)
				: m_Document(document), m_Registry(registry), m_Options(options), m_Kind(kind)
			{
			}

			[[nodiscard]] Result<Json> Run(LoadReport& report);
		private:
			[[nodiscard]] Status ReadEntities();
			void CheckIDs();
			[[nodiscard]] Status AssignFreshIDs();
			[[nodiscard]] Status CheckParents();
			void CutCycles();
			void ComputeCanonicalOrder();
			void CheckOrder();
			void CheckPrefabRoot();
			[[nodiscard]] Status CheckComponents();
			// The reason why the PrefabLink `link` does not point at an instance root, or an empty string when it does.
			[[nodiscard]] std::string FindPrefabLinkProblem(const JsonReader& link, std::string_view instanceName, std::string& pointer) const;
			[[nodiscard]] Result<Json> Finish(LoadReport& report);

			Finding& AddFinding(size_t entityIndex, FindingRank rank, std::string_view code, std::string pointer, std::string message);
		private:
			const Json& m_Document;
			const TypeRegistry& m_Registry;
			const LoadOptions& m_Options;
			DocumentKind m_Kind = DocumentKind::Scene;
			bool m_HasEntities = false;
			std::vector<EntityNode> m_Nodes;
			std::unordered_map<UUID, size_t> m_FirstHolders; // lookup only: written ID -> first entity holding it
			std::vector<size_t> m_Order;                     // canonical order of m_Nodes
			std::vector<Finding> m_Findings;
		};

		Result<Json> DocumentValidator::Run(LoadReport& report)
		{
			ENGINE_TRY(ReadEntities());
			CheckIDs();
			ENGINE_TRY(AssignFreshIDs());
			ENGINE_TRY(CheckParents());
			CutCycles();
			ComputeCanonicalOrder();
			CheckOrder();
			if (m_Kind == DocumentKind::Prefab)
				CheckPrefabRoot();
			ENGINE_TRY(CheckComponents());
			return Finish(report);
		}

		Status DocumentValidator::ReadEntities()
		{
			const JsonReader root(m_Document);
			ENGINE_TRY(root.ExpectType(JsonType::Object));
			const std::optional<JsonReader> entities = root.FindMember("Entities");
			if (!entities)
				return {};

			m_HasEntities = true;
			ENGINE_TRY_ASSIGN(const size_t count, entities->GetArraySize());
			m_Nodes.resize(count);
			for (size_t index = 0; index < count; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader entity, entities->GetElement(index));
				ENGINE_TRY(entity.ExpectType(JsonType::Object));
				m_Nodes[index].Object = entity.GetValue();
				m_Nodes[index].Pointer = entity.GetPointer();
			}
			return {};
		}

		void DocumentValidator::CheckIDs()
		{
			for (size_t index = 0; index < m_Nodes.size(); ++index)
			{
				EntityNode& node = m_Nodes[index];
				const JsonReader entity(node.Object, node.Pointer);
				const std::optional<JsonReader> id = entity.FindMember("ID");
				std::string problem;
				std::string pointer = JsonReader::AppendPointer(node.Pointer, "ID");
				if (!id)
				{
					problem = "the entity has no \"ID\"";
					pointer = node.Pointer;
				}
				else if (Result<UUID> value = id->ReadUUID(); !value)
				{
					problem = std::format("malformed ID: {}", value.error().GetMessageText());
				}
				else if (!value->IsValid())
				{
					problem = "the zero ID is not a valid entity ID";
				}
				else if (const auto [holder, inserted] = m_FirstHolders.try_emplace(*value, index); !inserted)
				{
					node.WrittenID = *value;
					Finding& finding = AddFinding(index, FindingRank::ID, SceneDuplicateIdCode, std::move(pointer),
						std::format("entity ID {} is already used by {}", *value, m_Nodes[holder->second].Pointer));
					finding.Entity = *value;
					node.IDFinding = m_Findings.size() - 1;
					continue;
				}
				else
				{
					node.WrittenID = *value;
					node.ID = *value;
					continue;
				}

				AddFinding(index, FindingRank::ID, SceneInvalidIdCode, std::move(pointer), std::move(problem));
				node.IDFinding = m_Findings.size() - 1;
			}
		}

		Status DocumentValidator::AssignFreshIDs()
		{
			if (m_Options.Mode != LoadMode::Repair)
				return {};

			UUIDGenerator* generator = m_Options.RepairIdGenerator;
			ENGINE_CORE_ASSERT(generator != nullptr, "Repair loading needs LoadOptions::RepairIdGenerator");
			if (generator == nullptr)
				return MakeError(ErrorCode::InvalidArgument, "repair loading needs a repair ID generator");

			// Fresh IDs are drawn in file order and never collide with an ID written anywhere in the document, so repairs
			// are a pure function of the document and the generator state.
			std::unordered_set<UUID> drawn; // lookup only
			for (EntityNode& node : m_Nodes)
			{
				if (node.IDFinding == NoEntity)
					continue;
				UUID fresh = generator->Next();
				while (m_FirstHolders.contains(fresh) || drawn.contains(fresh))
					fresh = generator->Next();
				drawn.insert(fresh);
				node.ID = fresh;
				node.Object["ID"] = fresh.ToString();

				Finding& finding = m_Findings[node.IDFinding];
				finding.RepairedEntity = fresh;
				if (finding.Code == SceneDuplicateIdCode)
					finding.Repair = std::format("gave the later duplicate of {} the new ID {}", node.WrittenID, fresh);
				else
					finding.Repair = std::format("gave the entity the new ID {}", fresh);
			}
			return {};
		}

		Status DocumentValidator::CheckParents()
		{
			for (size_t index = 0; index < m_Nodes.size(); ++index)
			{
				EntityNode& node = m_Nodes[index];
				const JsonReader entity(node.Object, node.Pointer);
				const std::optional<JsonReader> parent = entity.FindMember("Parent");
				if (!parent || parent->IsNull())
					continue;

				// A malformed Parent is not one of the repairable defects of §6: the document is rejected in both modes.
				ENGINE_TRY_ASSIGN(const UUID parentID, parent->ReadUUID());
				if (!parentID.IsValid())
					continue; // "0000000000000000" is the invalid UUID, which names no entity: a root

				const auto holder = m_FirstHolders.find(parentID);
				if (holder == m_FirstHolders.end())
				{
					Finding& finding = AddFinding(index, FindingRank::Parent, SceneDanglingParentCode, parent->GetPointer(),
						std::format("parent {} names no entity of the document", parentID));
					finding.Repair = "reparented the orphan to the root";
					node.Object["Parent"] = nullptr;
					continue;
				}
				node.Parent = holder->second;
			}
			return {};
		}

		void DocumentValidator::CutCycles()
		{
			enum class VisitState : uint8_t
			{
				Unvisited,
				OnPath,
				Done
			};

			std::vector<VisitState> states(m_Nodes.size(), VisitState::Unvisited);
			std::vector<size_t> path;
			for (size_t start = 0; start < m_Nodes.size(); ++start)
			{
				if (states[start] != VisitState::Unvisited)
					continue;

				path.clear();
				size_t current = start;
				while (current != NoEntity && states[current] == VisitState::Unvisited)
				{
					states[current] = VisitState::OnPath;
					path.push_back(current);
					current = m_Nodes[current].Parent;
				}

				if (current != NoEntity && states[current] == VisitState::OnPath)
				{
					// The cycle is the tail of the path from `current`. Its first member in file order becomes a root.
					const auto cycleBegin = std::find(path.begin(), path.end(), current);
					const size_t cut = *std::min_element(cycleBegin, path.end());
					std::string members;
					for (auto member = cycleBegin; member != path.end(); ++member)
					{
						if (!members.empty())
							members += " -> ";
						members += m_Nodes[*member].ID.ToString();
					}

					EntityNode& node = m_Nodes[cut];
					Finding& finding = AddFinding(cut, FindingRank::Parent, SceneParentCycleCode, JsonReader::AppendPointer(node.Pointer, "Parent"),
						std::format("the parents of {} -> {} form a cycle", members, m_Nodes[current].ID));
					finding.Repair = "cut the cycle: the entity became a root";
					node.Object["Parent"] = nullptr;
					node.Parent = NoEntity;
				}

				for (const size_t member : path)
					states[member] = VisitState::Done;
			}
		}

		void DocumentValidator::ComputeCanonicalOrder()
		{
			std::vector<std::vector<size_t>> children(m_Nodes.size());
			std::vector<size_t> roots;
			for (size_t index = 0; index < m_Nodes.size(); ++index)
			{
				const size_t parent = m_Nodes[index].Parent;
				if (parent == NoEntity)
					roots.push_back(index);
				else
					children[parent].push_back(index);
			}

			// Roots in file order, then depth-first through the children in file order (§5.1), without recursion.
			m_Order.reserve(m_Nodes.size());
			std::vector<std::pair<size_t, size_t>> stack; // entity, next child position
			for (const size_t root : roots)
			{
				m_Order.push_back(root);
				stack.emplace_back(root, 0);
				while (!stack.empty())
				{
					const size_t current = stack.back().first;
					const size_t next = stack.back().second;
					if (next == children[current].size())
					{
						stack.pop_back();
						continue;
					}
					stack.back().second = next + 1;
					const size_t child = children[current][next];
					m_Order.push_back(child);
					stack.emplace_back(child, 0);
				}
			}
			ENGINE_CORE_ASSERT(m_Order.size() == m_Nodes.size(), "Every entity is reachable once cycles are cut");
		}

		void DocumentValidator::CheckOrder()
		{
			bool reported = false;
			for (size_t index = 0; index < m_Nodes.size(); ++index)
			{
				const size_t parent = m_Nodes[index].Parent;
				if (parent == NoEntity || parent < index)
					continue;
				Finding& finding = AddFinding(index, FindingRank::Parent, SceneNonCanonicalOrderCode,
					JsonReader::AppendPointer(m_Nodes[index].Pointer, "Parent"),
					std::format("the entity is listed before its parent {}; it is loaded in canonical order", m_Nodes[parent].Pointer));
				finding.Severity = DiagnosticSeverity::Warning;
				reported = true;
			}
			if (reported)
				return;

			for (size_t position = 0; position < m_Order.size(); ++position)
			{
				if (m_Order[position] == position)
					continue;
				Finding& finding = AddFinding(NoEntity, FindingRank::Order, SceneNonCanonicalOrderCode, "/Entities",
					std::format("the entities are not in canonical order (the first misplaced one is {}); they are loaded in canonical order",
						m_Nodes[position].Pointer));
				finding.Severity = DiagnosticSeverity::Warning;
				return;
			}
		}

		void DocumentValidator::CheckPrefabRoot()
		{
			const JsonReader root(m_Document);
			const std::optional<JsonReader> member = root.FindMember("Root");
			std::string problem;
			std::string pointer = "/Root";
			if (!member)
			{
				problem = "the prefab has no \"Root\"";
				pointer.clear();
			}
			else if (const Result<UUID> id = member->ReadUUID(); !id || !id->IsValid())
			{
				problem = "\"Root\" must name an entity by its ID";
			}
			else if (const auto holder = m_FirstHolders.find(*id); holder == m_FirstHolders.end())
			{
				problem = std::format("\"Root\" {} names no entity of the prefab", *id);
			}
			else
			{
				const size_t rootCount = static_cast<size_t>(std::count_if(m_Nodes.begin(), m_Nodes.end(), [](const EntityNode& node)
				{
					return node.Parent == NoEntity;
				}));
				if (m_Nodes[holder->second].Parent == NoEntity && rootCount == 1)
					return;
				problem = std::format("\"Root\" {} is not the only root entity of the prefab ({} roots)", *id, rootCount);
			}

			Finding& finding = AddFinding(NoEntity, FindingRank::Document, PrefabInvalidRootCode, std::move(pointer), std::move(problem));
			finding.Repairable = false;
		}

		Status DocumentValidator::CheckComponents()
		{
			const ComponentInfo* linkInfo = m_Registry.FindComponent<PrefabLinkComponent>();
			const ComponentInfo* instanceInfo = m_Registry.FindComponent<PrefabInstanceComponent>();
			std::unordered_map<const ComponentInfo*, size_t> uniqueHolders; // lookup only

			// Canonical order, so that the first holder of a unique component is the first in canonical order.
			for (const size_t index : m_Order)
			{
				EntityNode& node = m_Nodes[index];
				std::vector<std::string> dropped;
				{
					const JsonReader entity(node.Object, node.Pointer);
					const std::optional<JsonReader> components = entity.FindMember("Components");
					if (!components)
						continue;
					ENGINE_TRY(components->ExpectType(JsonType::Object));
					ENGINE_TRY_ASSIGN(const std::vector<std::string> names, components->GetMemberNames());

					for (size_t position = 0; position < names.size(); ++position)
					{
						const std::string& name = names[position];
						const ComponentInfo* info = m_Registry.FindComponent(name);
						if (info == nullptr)
							continue; // unknown: preserved by the serializer
						ENGINE_TRY_ASSIGN(const JsonReader component, components->GetMember(name));

						std::string_view code;
						std::string pointer = component.GetPointer();
						std::string message;
						std::string repair;
						Json removed;
						if (info->HasFlag(ComponentFlags::EntityLevel) || !info->HasFlag(ComponentFlags::Serializable))
						{
							code = SceneMisplacedComponentCode;
							message = info->HasFlag(ComponentFlags::EntityLevel)
								? std::format("'{}' is an entity key, not an entry of \"Components\"", name)
								: std::format("'{}' is not serialized; it cannot appear in \"Components\"", name);
							repair = std::format("dropped the misplaced '{}' component", name);
							removed = component.GetValue();
						}
						else if (m_Kind == DocumentKind::Prefab && (info == linkInfo || info == instanceInfo))
						{
							code = PrefabNestedInstanceCode;
							message = std::format("'{}' marks a nested prefab instance, which a prefab never contains", name);
							repair = std::format("flattened the nested instance: dropped its '{}' component", name);
							removed = component.GetValue();
						}
						else if (m_Kind == DocumentKind::Scene && info->HasFlag(ComponentFlags::UniquePerScene))
						{
							const auto [holder, inserted] = uniqueHolders.try_emplace(info, index);
							if (inserted)
								continue;
							code = SceneDuplicateUniqueComponentCode;
							message = std::format("'{}' is unique per scene and {} already has one", name, m_Nodes[holder->second].Pointer);
							repair = std::format("dropped the extra '{}' component", name);
							removed = component.GetValue();
						}
						else if (info == linkInfo && instanceInfo != nullptr && component.IsObject())
						{
							message = FindPrefabLinkProblem(component, instanceInfo->GetName(), pointer);
							if (message.empty())
								continue;
							code = SceneInconsistentPrefabLinkCode;
							repair = std::format("unpacked the member: removed its '{}' component", name);
						}
						else
						{
							continue;
						}

						Finding& finding = AddFinding(index, FindingRank::Component, code, std::move(pointer), std::move(message));
						finding.ComponentIndex = position;
						finding.Repair = std::move(repair);
						finding.Removed = std::move(removed);
						dropped.push_back(name);
					}
				}

				if (dropped.empty())
					continue;
				Json& components = node.Object["Components"];
				for (const std::string& name : dropped)
					components.erase(name);
			}
			return {};
		}

		std::string DocumentValidator::FindPrefabLinkProblem(const JsonReader& link, std::string_view instanceName, std::string& pointer) const
		{
			const std::optional<JsonReader> root = link.FindMember(InstanceRootKey);
			if (!root)
				return std::format("the member has no \"{}\"", InstanceRootKey);

			pointer = root->GetPointer();
			const Result<UUID> id = root->ReadUUID();
			if (!id || !id->IsValid())
				return std::format("\"{}\" must name the instance root by its ID", InstanceRootKey);

			const auto holder = m_FirstHolders.find(*id);
			if (holder == m_FirstHolders.end())
				return std::format("\"{}\" {} names no entity of the document", InstanceRootKey, *id);

			const Json& rootObject = m_Nodes[holder->second].Object;
			const auto components = rootObject.find("Components");
			if (components != rootObject.end() && components->is_object() && components->contains(std::string(instanceName)))
				return {};
			return std::format("\"{}\" {} is not a prefab instance root (it has no '{}' component)", InstanceRootKey, *id, instanceName);
		}

		Finding& DocumentValidator::AddFinding(size_t entityIndex, FindingRank rank, std::string_view code, std::string pointer, std::string message)
		{
			Finding& finding = m_Findings.emplace_back();
			finding.Position = entityIndex == NoEntity ? 0 : entityIndex + 1;
			finding.Rank = rank;
			finding.Code = code;
			finding.Pointer = std::move(pointer);
			finding.Message = std::move(message);
			if (entityIndex != NoEntity)
			{
				finding.Entity = m_Nodes[entityIndex].WrittenID;
				finding.RepairedEntity = m_Nodes[entityIndex].ID;
			}
			return finding;
		}

		Result<Json> DocumentValidator::Finish(LoadReport& report)
		{
			std::stable_sort(m_Findings.begin(), m_Findings.end(), [](const Finding& left, const Finding& right)
			{
				if (left.Position != right.Position)
					return left.Position < right.Position;
				if (left.Rank != right.Rank)
					return left.Rank < right.Rank;
				return left.ComponentIndex < right.ComponentIndex;
			});

			const bool repair = m_Options.Mode == LoadMode::Repair;
			const auto fails = [repair](const Finding& finding)
			{
				return finding.Severity == DiagnosticSeverity::Error && (!repair || !finding.Repairable);
			};

			std::vector<ErrorIssue> issues;
			const Finding* first = nullptr;
			for (const Finding& finding : m_Findings)
			{
				if (!fails(finding))
					continue;
				if (first == nullptr)
					first = &finding;
				issues.push_back(ErrorIssue{ finding.Pointer, std::format("{}: {}", finding.Code, finding.Message), {}, {} });
			}

			if (first != nullptr)
			{
				for (const Finding& finding : m_Findings)
				{
					if (finding.Severity == DiagnosticSeverity::Warning || fails(finding))
						report.Diagnostics.push_back(
							LoadDiagnostic{ finding.Severity, std::string(finding.Code), finding.Message, finding.Pointer, finding.Entity });
				}

				const std::string subject = m_Kind == DocumentKind::Scene ? "scene" : "prefab";
				std::string message = issues.front().Message;
				if (issues.size() > 1)
					message = std::format("{} structural defects in the {}; the first is {}", issues.size(), subject, issues.front().Message);
				ErrorLocation location;
				location.File = m_Options.SourcePath;
				location.JsonPointer = first->Pointer;
				location.Entity = first->Entity;
				return std::unexpected(
					Error(ErrorCode::Validation, std::move(message)).WithLocation(std::move(location)).WithIssues(std::move(issues)));
			}

			for (Finding& finding : m_Findings)
			{
				if (finding.Severity == DiagnosticSeverity::Warning)
				{
					report.Diagnostics.push_back(
						LoadDiagnostic{ finding.Severity, std::string(finding.Code), finding.Message, finding.Pointer, finding.Entity });
					continue;
				}
				report.Diagnostics.push_back(LoadDiagnostic{ DiagnosticSeverity::Warning, std::string(finding.Code),
					std::format("{} (repaired: {})", finding.Message, finding.Repair), finding.Pointer, finding.Entity });
				report.Repairs.push_back(LoadRepair{ std::string(finding.Code), finding.Repair, finding.Pointer, finding.RepairedEntity,
					VariantValue(std::move(finding.Removed)) });
			}

			// The document with its members in their order and "Entities" replaced by the repaired entities in canonical order.
			Json result = Json::object();
			for (auto member = m_Document.begin(); member != m_Document.end(); ++member)
			{
				if (member.key() != "Entities")
				{
					result[member.key()] = *member;
					continue;
				}
				Json entities = Json::array();
				for (const size_t index : m_Order)
					entities.push_back(std::move(m_Nodes[index].Object));
				result[member.key()] = std::move(entities);
			}
			ENGINE_CORE_ASSERT(m_HasEntities == result.contains("Entities"), "The result keeps the document's members");
			return result;
		}

	}

	Result<Json> StructuralValidator::Validate(const Json& document, DocumentKind kind, const TypeRegistry& registry, const LoadOptions& options,
		LoadReport& report)
	{
		DocumentValidator validator(document, kind, registry, options);
		Result<Json> result = validator.Run(report);
		if (!result && !options.SourcePath.empty() && result.error().GetLocation().File.empty())
		{
			ErrorLocation location;
			location.File = options.SourcePath;
			return std::unexpected(std::move(result).error().WithLocation(std::move(location)));
		}
		return result;
	}

}
