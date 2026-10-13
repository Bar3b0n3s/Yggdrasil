#include "EnginePCH.h"
#include "Engine/Testing/CoverageGates.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/SourceFormats.h"
#include "Engine/Core/Error.h"
#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace Engine {

	namespace {

		// The kinds of the counter table (CoverageGates.h), each with its gate and its id prefix.
		struct CoverageKindInfo
		{
			std::string_view Kind{};
			CoverageGate Gate = CoverageGate::Components;
			std::string_view Prefix{};
		};

		constexpr std::array<CoverageKindInfo, 14> CoverageKinds = { {
			{ .Kind = CoverageKindComponent, .Gate = CoverageGate::Components, .Prefix = "component:" },
			{ .Kind = CoverageKindField, .Gate = CoverageGate::Components, .Prefix = "field:" },
			{ .Kind = CoverageKindEnumValue, .Gate = CoverageGate::EnumValues, .Prefix = "enum:" },
			{ .Kind = CoverageKindFunction, .Gate = CoverageGate::ScriptApi, .Prefix = "api:" },
			{ .Kind = CoverageKindMethod, .Gate = CoverageGate::ScriptApi, .Prefix = "api:" },
			{ .Kind = CoverageKindOperator, .Gate = CoverageGate::ScriptApi, .Prefix = "api:" },
			{ .Kind = CoverageKindConstructor, .Gate = CoverageGate::ScriptApi, .Prefix = "api:" },
			{ .Kind = CoverageKindPropertyRead, .Gate = CoverageGate::ScriptApi, .Prefix = "api:" },
			{ .Kind = CoverageKindPropertyWrite, .Gate = CoverageGate::ScriptApi, .Prefix = "api:" },
			{ .Kind = CoverageKindArgumentValue, .Gate = CoverageGate::ScriptApi, .Prefix = "api:" },
			{ .Kind = CoverageKindCallback, .Gate = CoverageGate::Callbacks, .Prefix = "callback:" },
			{ .Kind = CoverageKindProxyRead, .Gate = CoverageGate::ProxyFields, .Prefix = "proxy:" },
			{ .Kind = CoverageKindProxyWrite, .Gate = CoverageGate::ProxyFields, .Prefix = "proxy:" },
			{ .Kind = CoverageKindFormat, .Gate = CoverageGate::Formats, .Prefix = "format:" },
		} };

		constexpr std::array<std::pair<CoverageGate, std::string_view>, 10> CoverageGateNames = { {
			{ CoverageGate::Components, "1" },
			{ CoverageGate::EnumValues, "1b" },
			{ CoverageGate::ScriptApi, "2" },
			{ CoverageGate::Callbacks, "3" },
			{ CoverageGate::ProxyFields, "4" },
			{ CoverageGate::AutomationMethods, "5" },
			{ CoverageGate::ValidationCodes, "6" },
			{ CoverageGate::Descriptions, "7" },
			{ CoverageGate::ProjectSettings, "8" },
			{ CoverageGate::Formats, "9" },
		} };

		// The Missing entry of a gate that has no counter available in the evaluated mode (never a vacuous pass).
		constexpr std::string_view NoCountersEntry = "<no counters>";

		constexpr std::array<TestSuiteSettings::Mode, 3> AllTestModes = { TestSuiteSettings::Mode::Editor, TestSuiteSettings::Mode::Release,
			TestSuiteSettings::Mode::Dist };

		const CoverageKindInfo* FindCoverageKind(std::string_view kind)
		{
			const auto found = std::ranges::find(CoverageKinds, kind, &CoverageKindInfo::Kind);
			return found == CoverageKinds.end() ? nullptr : &*found;
		}

		uint32_t SaturateCount(uint64_t value)
		{
			return static_cast<uint32_t>(std::min<uint64_t>(value, std::numeric_limits<uint32_t>::max()));
		}

		bool ContainsMode(const std::vector<TestSuiteSettings::Mode>& modes, TestSuiteSettings::Mode mode)
		{
			return std::ranges::find(modes, mode) != modes.end();
		}

		// The run modes a counter of `kind` must list: components, enum values and formats exist in all three (ADR 0022
		// decision 2); every other entry at least in the Editor.
		bool RequiresAllModes(std::string_view kind)
		{
			return kind == CoverageKindComponent || kind == CoverageKindEnumValue || kind == CoverageKindFormat;
		}

		Status ValidateCounter(const TestCoverageCounter& counter)
		{
			const CoverageKindInfo* kind = FindCoverageKind(counter.Kind);
			if (kind == nullptr)
				return MakeError(ErrorCode::Validation, "coverage counter '{}' has the unknown kind '{}'", counter.Id, counter.Kind);
			if (!counter.Id.starts_with(kind->Prefix) || counter.Id.size() == kind->Prefix.size())
				return MakeError(ErrorCode::Validation, "coverage counter '{}' of kind '{}' must start with '{}' and name its entry", counter.Id, counter.Kind, kind->Prefix);
			if (counter.Modes.empty())
				return MakeError(ErrorCode::Validation, "coverage counter '{}' lists no run mode", counter.Id);
			RunModes listed = RunModes::None;
			for (const TestSuiteSettings::Mode mode : counter.Modes)
			{
				if (HasFlag(listed, ToRunModes(mode)))
					return MakeError(ErrorCode::Validation, "coverage counter '{}' lists a run mode twice", counter.Id);
				listed |= ToRunModes(mode);
			}
			if (!ContainsMode(counter.Modes, TestSuiteSettings::Mode::Editor))
				return MakeError(ErrorCode::Validation, "coverage counter '{}' does not exist in the Editor; every entry does (RunModes are EditorOnly or All)", counter.Id);
			if (RequiresAllModes(counter.Kind) && counter.Modes.size() != AllTestModes.size())
				return MakeError(ErrorCode::Validation, "coverage counter '{}' of kind '{}' must exist in the Editor, Release and Dist modes", counter.Id, counter.Kind);
			return {};
		}

		// One RFC 6901 reference token: '~' becomes "~0" and '/' becomes "~1".
		std::string EscapePointerToken(std::string_view token)
		{
			std::string escaped;
			escaped.reserve(token.size());
			for (const char c : token)
			{
				if (c == '~')
					escaped += "~0";
				else if (c == '/')
					escaped += "~1";
				else
					escaped += c;
			}
			return escaped;
		}

		const Json* FindMember(const Json* object, const std::string& key)
		{
			if (object == nullptr || !object->is_object())
				return nullptr;
			const auto found = object->find(key);
			return found == object->end() ? nullptr : &*found;
		}

	}

	std::string_view CoverageGateName(CoverageGate gate)
	{
		const auto found = std::ranges::find(CoverageGateNames, gate, &std::pair<CoverageGate, std::string_view>::first);
		return found == CoverageGateNames.end() ? std::string_view() : found->second;
	}

	std::optional<CoverageGate> ParseCoverageGateName(std::string_view name)
	{
		const auto found = std::ranges::find(CoverageGateNames, name, &std::pair<CoverageGate, std::string_view>::second);
		if (found == CoverageGateNames.end())
			return std::nullopt;
		return found->first;
	}

	bool IsPerModeCoverageGate(CoverageGate gate)
	{
		return std::ranges::find(PerModeCoverageGates, gate) != PerModeCoverageGates.end();
	}

	std::optional<CoverageGate> GetCoverageGateOfKind(std::string_view kind)
	{
		const CoverageKindInfo* found = FindCoverageKind(kind);
		if (found == nullptr)
			return std::nullopt;
		return found->Gate;
	}

	std::vector<TestSuiteSettings::Mode> ToTestModes(RunModes modes)
	{
		std::vector<TestSuiteSettings::Mode> result;
		for (const TestSuiteSettings::Mode mode : AllTestModes)
			if (HasFlag(modes, ToRunModes(mode)))
				result.push_back(mode);
		return result;
	}

	RunModes ToRunModes(TestSuiteSettings::Mode mode)
	{
		switch (mode)
		{
			case TestSuiteSettings::Mode::Editor:  return RunModes::Editor;
			case TestSuiteSettings::Mode::Release: return RunModes::Release;
			case TestSuiteSettings::Mode::Dist:    return RunModes::Dist;
		}
		return RunModes::None;
	}

	Result<std::vector<TestCoverageGateResult>> EvaluateCoverageGates(std::span<const TestCoverageCounter> counters, TestSuiteSettings::Mode mode)
	{
		if (ToRunModes(mode) == RunModes::None)
			return MakeError(ErrorCode::Validation, "coverage gates are evaluated for the Editor, Release or Dist mode");
		std::set<std::string_view> ids;
		for (const TestCoverageCounter& counter : counters)
		{
			ENGINE_TRY(ValidateCounter(counter));
			if (!ids.insert(counter.Id).second)
				return MakeError(ErrorCode::Validation, "coverage counter '{}' is listed twice", counter.Id);
		}

		std::vector<TestCoverageGateResult> results;
		results.reserve(PerModeCoverageGates.size());
		for (const CoverageGate gate : PerModeCoverageGates)
		{
			TestCoverageGateResult result;
			result.Gate = std::string(CoverageGateName(gate));
			result.Evaluated = true;
			size_t available = 0;
			for (const TestCoverageCounter& counter : counters)
			{
				if (FindCoverageKind(counter.Kind)->Gate != gate || !ContainsMode(counter.Modes, mode))
					continue;
				++available;
				if (counter.Count == 0)
					result.Missing.push_back(counter.Id);
			}
			if (available == 0)
				result.Missing.emplace_back(NoCountersEntry);
			std::ranges::sort(result.Missing);
			result.Passed = result.Missing.empty();
			results.push_back(std::move(result));
		}
		return results;
	}

	std::vector<TestCoverageGateResult> MakeUnevaluatedCoverageGates()
	{
		std::vector<TestCoverageGateResult> gates;
		gates.reserve(PerModeCoverageGates.size());
		for (const CoverageGate gate : PerModeCoverageGates)
			gates.push_back(TestCoverageGateResult{ .Gate = std::string(CoverageGateName(gate)), .Evaluated = false, .Passed = false, .Missing = {} });
		return gates;
	}

	Result<std::vector<TestCoverageCounter>> MakeScriptApiCoverageCounters(const ScriptApiCoverage& current, const ScriptApiCoverage& baseline)
	{
		if (current.Mode != baseline.Mode)
			return MakeError(ErrorCode::InvalidArgument, "script API coverage snapshots of different run modes cannot be compared");
		if (current.Mode != RunModes::Editor && current.Mode != RunModes::Release && current.Mode != RunModes::Dist)
			return MakeError(ErrorCode::InvalidArgument, "script API coverage snapshots must name exactly one run mode");
		if (current.Members.size() != baseline.Members.size())
			return MakeError(ErrorCode::InvalidArgument, "script API coverage snapshots list {} and {} members: they come from different registries", current.Members.size(), baseline.Members.size());

		std::vector<TestCoverageCounter> counters;
		counters.reserve(current.Members.size());
		for (size_t index = 0; index < current.Members.size(); ++index)
		{
			const ScriptApiCounter& now = current.Members[index];
			const ScriptApiCounter& before = baseline.Members[index];
			const std::string name = std::format("{}.{}", now.Owner, now.Member);
			if (now.Owner != before.Owner || now.Member != before.Member || now.Kind != before.Kind || now.Modes != before.Modes
				|| now.RequiresRead != before.RequiresRead || now.RequiresWrite != before.RequiresWrite || now.EnumValues.size() != before.EnumValues.size())
				return MakeError(ErrorCode::InvalidArgument, "script API coverage snapshots differ at member '{}': they come from different registries", name);
			const auto delta = [&name](uint64_t after, uint64_t earlier, std::string_view what) -> Result<uint32_t>
			{
				if (after < earlier)
					return MakeError(ErrorCode::InvalidArgument, "the {} count of script API member '{}' went backwards ({} -> {})", what, name, earlier, after);
				return SaturateCount(after - earlier);
			};
			const std::vector<TestSuiteSettings::Mode> modes = ToTestModes(now.Modes);
			const auto add = [&counters, &modes](std::string id, std::string_view kind, uint32_t count)
			{
				counters.push_back(TestCoverageCounter{ .Id = std::move(id), .Kind = std::string(kind), .Modes = modes, .Count = count });
			};
			switch (now.Kind)
			{
				case ScriptApiMemberKind::Function:
				case ScriptApiMemberKind::Method:
				case ScriptApiMemberKind::Operator:
				case ScriptApiMemberKind::Constructor:
				{
					const std::string_view kind = now.Kind == ScriptApiMemberKind::Function ? CoverageKindFunction
						: now.Kind == ScriptApiMemberKind::Method                           ? CoverageKindMethod
						: now.Kind == ScriptApiMemberKind::Operator                         ? CoverageKindOperator
																							: CoverageKindConstructor;
					ENGINE_TRY_ASSIGN(const uint32_t calls, delta(now.Calls, before.Calls, "call"));
					add("api:" + name, kind, calls);
					break;
				}
				case ScriptApiMemberKind::Property:
				case ScriptApiMemberKind::ProxyField:
				{
					const bool proxy = now.Kind == ScriptApiMemberKind::ProxyField;
					const std::string id = (proxy ? "proxy:" : "api:") + name;
					if (now.RequiresRead)
					{
						ENGINE_TRY_ASSIGN(const uint32_t reads, delta(now.Reads, before.Reads, "read"));
						add(id + "/read", proxy ? CoverageKindProxyRead : CoverageKindPropertyRead, reads);
					}
					if (now.RequiresWrite)
					{
						ENGINE_TRY_ASSIGN(const uint32_t writes, delta(now.Writes, before.Writes, "write"));
						add(id + "/write", proxy ? CoverageKindProxyWrite : CoverageKindPropertyWrite, writes);
					}
					break;
				}
				case ScriptApiMemberKind::Callback:
				{
					ENGINE_TRY_ASSIGN(const uint32_t calls, delta(now.Calls, before.Calls, "call"));
					add("callback:" + name, CoverageKindCallback, calls);
					break;
				}
				case ScriptApiMemberKind::Constant:
					break;
			}
			for (size_t value = 0; value < now.EnumValues.size(); ++value)
			{
				const ScriptEnumCounter& hit = now.EnumValues[value];
				const ScriptEnumCounter& earlier = before.EnumValues[value];
				if (hit.ArgumentIndex != earlier.ArgumentIndex || hit.EnumName != earlier.EnumName || hit.ValueName != earlier.ValueName)
					return MakeError(ErrorCode::InvalidArgument, "script API coverage snapshots differ in the argument values of member '{}'", name);
				ENGINE_TRY_ASSIGN(const uint32_t count, delta(hit.Count, earlier.Count, "argument value"));
				add(std::format("api:{}/argument{}/{}/{}", name, hit.ArgumentIndex, hit.EnumName, hit.ValueName), CoverageKindArgumentValue, count);
			}
		}
		std::ranges::sort(counters, {}, &TestCoverageCounter::Id);
		return counters;
	}

	// --- SceneCoverageRecorder ---------------------------------------------------------------------------------------

	struct SceneCoverageRecorder::State
	{
		// One serialized field of a struct, below its owner's JSON object at Key.
		struct StructNode;
		struct FieldNode
		{
			enum class Kind : uint8_t
			{
				Leaf,    // compared whole with its default (scalars, variants, containers of non-structs, recursive structs)
				Nested,  // a struct field: its own fields are compared against the owner's default at this key
				Elements // an array or map of structs: each element's fields are compared against the element default
			};
			std::string Key{};
			Kind NodeKind = Kind::Leaf;
			size_t Counter = 0;             // Leaf: the field counter
			const TypeInfo* Type = nullptr; // Leaf: the value's type, for the enum values it holds
			Scope<StructNode> Struct{};     // Nested and Elements
			Json ElementDefaults{};         // Elements: the element struct's MakeDefaultJson
		};
		struct StructNode
		{
			std::vector<FieldNode> Fields{};
		};
		struct ComponentNode
		{
			const ComponentInfo* Info = nullptr;
			size_t Counter = 0;
			Json Defaults{}; // MakeDefaultJson
			StructNode Root{};
		};

		const TypeRegistry* Types = nullptr; // documented back-reference, outlives the recorder
		std::vector<TestCoverageCounter> Counters{};
		std::map<std::string, size_t, std::less<>> Ids{};
		std::vector<ComponentNode> Components{};
		// The enum value counters of every enum a serialized field can hold, by enum and entry index.
		std::map<const EnumInfo*, std::vector<size_t>> EnumCounters{};

		size_t AddCounter(std::string id, std::string_view kind, std::vector<TestSuiteSettings::Mode> modes)
		{
			if (const auto found = Ids.find(id); found != Ids.end())
				return found->second;
			const size_t index = Counters.size();
			Ids.emplace(id, index);
			Counters.push_back(TestCoverageCounter{ .Id = std::move(id), .Kind = std::string(kind), .Modes = std::move(modes), .Count = 0 });
			return index;
		}

		// Registers a counter for every value of every registry enum `type` can hold, recursively through containers and
		// structs (whose serialized fields count); `visiting` stops recursive struct types.
		void AddEnums(const TypeInfo& type, std::vector<const StructInfo*>& visiting)
		{
			switch (type.GetKind())
			{
				case FieldType::Enum:
				{
					const EnumInfo* enumeration = type.GetEnum();
					if (enumeration == nullptr || EnumCounters.contains(enumeration))
						return;
					std::vector<size_t> indices;
					for (const EnumEntry& entry : enumeration->GetEntries())
						indices.push_back(AddCounter(std::format("enum:{}/{}", enumeration->GetName(), entry.Name), CoverageKindEnumValue, ToTestModes(RunModes::All)));
					EnumCounters.emplace(enumeration, std::move(indices));
					return;
				}
				case FieldType::Array:
				case FieldType::Map:
					if (type.GetElement() != nullptr)
						AddEnums(*type.GetElement(), visiting);
					return;
				case FieldType::Struct:
				{
					const StructInfo* info = type.GetStruct();
					if (info == nullptr || std::ranges::find(visiting, info) != visiting.end())
						return;
					visiting.push_back(info);
					for (const Scope<FieldInfo>& field : info->GetFields())
						if (field->GetMeta().Serialized)
							AddEnums(field->GetType(), visiting);
					visiting.pop_back();
					return;
				}
				default:
					return;
			}
		}

		void BuildStruct(const StructInfo& info, StructNode& node, const std::string& component, const std::string& pointer, RunModes inherited,
			std::vector<const StructInfo*>& visiting)
		{
			visiting.push_back(&info);
			for (const Scope<FieldInfo>& field : info.GetFields())
			{
				const FieldMeta& meta = field->GetMeta();
				if (!meta.Serialized)
					continue;
				const RunModes modes = inherited & meta.Modes;
				const std::string fieldPointer = pointer + "/" + EscapePointerToken(field->GetName());
				const TypeInfo& type = field->GetType();
				FieldNode child;
				child.Key = field->GetName();
				const StructInfo* nested = type.GetKind() == FieldType::Struct ? type.GetStruct() : nullptr;
				const TypeInfo* element = IsContainerFieldType(type.GetKind()) ? type.GetElement() : nullptr;
				const StructInfo* elementStruct = element != nullptr && element->GetKind() == FieldType::Struct ? element->GetStruct() : nullptr;
				if (nested != nullptr && std::ranges::find(visiting, nested) == visiting.end())
				{
					child.NodeKind = FieldNode::Kind::Nested;
					child.Struct = CreateScope<StructNode>();
					BuildStruct(*nested, *child.Struct, component, fieldPointer, modes, visiting);
				}
				else if (elementStruct != nullptr && std::ranges::find(visiting, elementStruct) == visiting.end())
				{
					child.NodeKind = FieldNode::Kind::Elements;
					child.Struct = CreateScope<StructNode>();
					child.ElementDefaults = elementStruct->MakeDefaultJson();
					BuildStruct(*elementStruct, *child.Struct, component, fieldPointer + "/*", modes, visiting);
				}
				else
				{
					child.NodeKind = FieldNode::Kind::Leaf;
					child.Type = &type;
					child.Counter = AddCounter("field:" + component + fieldPointer, CoverageKindField, ToTestModes(modes));
					AddEnums(type, visiting);
				}
				node.Fields.push_back(std::move(child));
			}
			visiting.pop_back();
		}

		// Counts each enum value `value` (of `type`) holds.
		void CountEnums(const TypeInfo& type, const Json& value)
		{
			switch (type.GetKind())
			{
				case FieldType::Enum:
				{
					const auto counters = EnumCounters.find(type.GetEnum());
					const std::string* name = value.get_ptr<const std::string*>();
					if (counters == EnumCounters.end() || name == nullptr)
						return;
					const std::span<const EnumEntry> entries = type.GetEnum()->GetEntries();
					const auto entry = std::ranges::find(entries, *name, &EnumEntry::Name);
					if (entry != entries.end())
						Increment(counters->second[static_cast<size_t>(entry - entries.begin())]);
					return;
				}
				case FieldType::Array:
					if (type.GetElement() != nullptr && value.is_array())
						for (const Json& element : value)
							CountEnums(*type.GetElement(), element);
					return;
				case FieldType::Map:
					if (type.GetElement() != nullptr && value.is_object())
						for (const auto& item : value.items())
							CountEnums(*type.GetElement(), item.value());
					return;
				case FieldType::Struct:
					if (type.GetStruct() != nullptr && value.is_object())
						for (const Scope<FieldInfo>& field : type.GetStruct()->GetFields())
							if (const Json* member = FindMember(&value, field->GetName()); member != nullptr && field->GetMeta().Serialized)
								CountEnums(field->GetType(), *member);
					return;
				default:
					return;
			}
		}

		void Increment(size_t counter)
		{
			uint32_t& count = Counters[counter].Count;
			if (count != std::numeric_limits<uint32_t>::max())
				++count;
		}

		// Compares every field of `node` in `value` with `defaults` (the owner's default object at this level).
		void Visit(const StructNode& node, const Json& value, const Json* defaults)
		{
			for (const FieldNode& field : node.Fields)
			{
				const Json* member = FindMember(&value, field.Key);
				const Json* fallback = FindMember(defaults, field.Key);
				switch (field.NodeKind)
				{
					case FieldNode::Kind::Leaf:
						if ((member == nullptr) != (fallback == nullptr) || (member != nullptr && *member != *fallback))
							Increment(field.Counter);
						if (member != nullptr)
							CountEnums(*field.Type, *member);
						break;
					case FieldNode::Kind::Nested:
						if (member != nullptr && member->is_object())
							Visit(*field.Struct, *member, fallback);
						break;
					case FieldNode::Kind::Elements:
						if (member != nullptr && member->is_array())
							for (const Json& element : *member)
								Visit(*field.Struct, element, &field.ElementDefaults);
						else if (member != nullptr && member->is_object())
							for (const auto& item : member->items())
								Visit(*field.Struct, item.value(), &field.ElementDefaults);
						break;
				}
			}
		}
	};

	SceneCoverageRecorder::SceneCoverageRecorder(const TypeRegistry& types)
		: m_State(CreateScope<State>())
	{
		m_State->Types = &types;
		std::vector<const StructInfo*> visiting;
		for (const ComponentInfo* info : types.GetComponents())
		{
			State::ComponentNode component;
			component.Info = info;
			component.Counter = m_State->AddCounter("component:" + info->GetName(), CoverageKindComponent, ToTestModes(RunModes::All));
			component.Defaults = info->MakeDefaultJson();
			m_State->BuildStruct(*info, component.Root, info->GetName(), {}, RunModes::All, visiting);
			m_State->Components.push_back(std::move(component));
		}
	}

	SceneCoverageRecorder::~SceneCoverageRecorder() = default;

	Status SceneCoverageRecorder::Record(const Scene& scene)
	{
		State& state = *m_State;
		if (&scene.GetTypeRegistry() != state.Types)
			return MakeError(ErrorCode::InvalidState, "scene '{}' uses another type registry than the coverage recorder", scene.GetName());
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			if (!entity.IsValid())
				continue;
			for (const State::ComponentNode& component : state.Components)
			{
				const ComponentHostOps* ops = component.Info->GetHostOps();
				const void* object = ops != nullptr ? ops->GetConst(entity) : nullptr;
				if (object == nullptr)
					continue;
				state.Increment(component.Counter);
				Result<Json> json = component.Info->ToJson(object);
				if (!json.has_value())
					return std::unexpected(std::move(json).error().WithContext(std::format("while recording the coverage of component '{}' of entity {} ('{}')",
						component.Info->GetName(), id.ToString(), entity.GetName())));
				state.Visit(component.Root, *json, &component.Defaults);
			}
		}
		return {};
	}

	std::vector<TestCoverageCounter> SceneCoverageRecorder::GetCounters() const
	{
		std::vector<TestCoverageCounter> counters = m_State->Counters;
		std::ranges::sort(counters, {}, &TestCoverageCounter::Id);
		return counters;
	}

	void SceneCoverageRecorder::Reset()
	{
		for (TestCoverageCounter& counter : m_State->Counters)
			counter.Count = 0;
	}

	// --- FormatCoverageRecorder --------------------------------------------------------------------------------------

	struct FormatCoverageRecorder::State
	{
		std::vector<TestCoverageCounter> Counters{}; // one per importable source format, in GetSourceFormats order
		std::vector<std::string> Extensions{};       // Counters' extensions, index for index
		std::set<AssetHandle> Seen{};                // the handles counted since the last Reset
	};

	FormatCoverageRecorder::FormatCoverageRecorder()
		: m_State(CreateScope<State>())
	{
		for (const SourceFormat& format : GetSourceFormats())
		{
			if (!format.Importable)
				continue;
			m_State->Counters.push_back(TestCoverageCounter{ .Id = std::format("format:{}/{}", format.Importer, format.Extension), .Kind = std::string(CoverageKindFormat), .Modes = ToTestModes(RunModes::All), .Count = 0 });
			m_State->Extensions.emplace_back(format.Extension);
		}
	}

	FormatCoverageRecorder::~FormatCoverageRecorder() = default;

	void FormatCoverageRecorder::RecordLoad(const AssetManager& assets, AssetHandle handle)
	{
		State& state = *m_State;
		if (!handle.IsValid() || !state.Seen.insert(handle).second)
			return;
		const std::string reference = assets.GetReferencePath(handle);
		std::string_view path = reference;
		// Engine assets and procedural built-ins have a scheme (engine://); project and pak paths never do.
		if (path.empty() || path.find("://") != std::string_view::npos)
			return;
		path = path.substr(0, path.find('#'));
		const size_t name = path.find_last_of('/');
		const size_t dot = path.find_last_of('.');
		if (dot == std::string_view::npos || (name != std::string_view::npos && dot < name))
			return;
		const SourceFormat* format = FindSourceFormat(path.substr(dot));
		if (format == nullptr || !format->Importable)
			return;
		const auto found = std::ranges::find(state.Extensions, format->Extension);
		if (found == state.Extensions.end())
			return;
		uint32_t& count = state.Counters[static_cast<size_t>(found - state.Extensions.begin())].Count;
		if (count != std::numeric_limits<uint32_t>::max())
			++count;
	}

	std::vector<TestCoverageCounter> FormatCoverageRecorder::GetCounters() const
	{
		std::vector<TestCoverageCounter> counters = m_State->Counters;
		std::ranges::sort(counters, {}, &TestCoverageCounter::Id);
		return counters;
	}

	void FormatCoverageRecorder::Reset()
	{
		for (TestCoverageCounter& counter : m_State->Counters)
			counter.Count = 0;
		m_State->Seen.clear();
	}

}
