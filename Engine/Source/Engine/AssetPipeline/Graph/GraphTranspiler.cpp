#include "EnginePCH.h"
#include "Engine/AssetPipeline/Graph/GraphTranspiler.h"

#include "Engine/AssetPipeline/Private/GraphTextLimits.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scripting/ScriptEngine.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The graph transpiler (Docs/Decisions/0035-m24-contract.md decisions 7 and 8). FindGraphNodeAtLine, GetGraphProbeKey and
// ParseGraphProbeKey are the contract's. The text is built without recursion (a work stack for the flows, an explicit
// stack for the pure values), so a graph at MaxGraphNodes' scale never deepens the native stack; its depth and size are
// ValidateGraph's limits (AssetPipeline/Private/GraphTextLimits.h), and Luau's other limits (200 locals, 255 upvalues) fail
// the import's compile at the line of the node that exceeds them.

namespace Engine {

	namespace Utils {

		// --- Luau spellings -------------------------------------------------------------------------------------------

		// The shortest decimal that reads back to `value` (std::to_chars), which Luau's lexer reads as written.
		[[nodiscard]] static std::string FormatLuauNumber(double value)
		{
			if (!std::isfinite(value))
				return "0";
			std::array<char, 64> buffer{};
			const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
			if (error != std::errc())
				return "0";
			return std::string(buffer.data(), end);
		}

		// A double-quoted Luau string: '"', '\' and the control bytes escaped, every other byte kept.
		[[nodiscard]] static std::string FormatLuauString(std::string_view text)
		{
			std::string quoted = "\"";
			for (const char c : text)
			{
				const auto byte = static_cast<unsigned char>(c);
				if (c == '"')
					quoted += "\\\"";
				else if (c == '\\')
					quoted += "\\\\";
				else if (c == '\n')
					quoted += "\\n";
				else if (c == '\r')
					quoted += "\\r";
				else if (c == '\t')
					quoted += "\\t";
				else if (byte < 0x20 || byte == 0x7f)
					quoted += std::format("\\x{:02X}", byte);
				else
					quoted += c;
			}
			quoted += '"';
			return quoted;
		}

		[[nodiscard]] static double ReadNumber(const Json& value)
		{
			return JsonReader(value).ReadDouble().value_or(0.0);
		}

		// "a, b, c" of the numbers of a JSON array, padded with `fill` to `count` (a document that passes
		// ValidateGraphStructure has exactly `count`).
		[[nodiscard]] static std::string FormatNumbers(const Json& value, size_t count, std::span<const double> fill)
		{
			std::string text;
			for (size_t i = 0; i < count; ++i)
			{
				double number = i < fill.size() ? fill[i] : 0.0;
				if (value.is_array() && i < value.size())
				{
					size_t index = 0;
					for (const Json& element : value)
					{
						if (index++ == i)
						{
							number = ReadNumber(element);
							break;
						}
					}
				}
				if (!text.empty())
					text += ", ";
				text += FormatLuauNumber(number);
			}
			return text;
		}

		constexpr std::array<double, 3> ZeroVector = { 0.0, 0.0, 0.0 };
		constexpr std::array<double, 4> WhiteColor = { 1.0, 1.0, 1.0, 1.0 };
		constexpr std::array<double, 4> IdentityQuat = { 0.0, 0.0, 0.0, 1.0 };

		[[nodiscard]] static std::string FormatVector(const Json& value)
		{
			return std::format("vector.create({})", FormatNumbers(value, 3, ZeroVector));
		}

		[[nodiscard]] static std::string FormatColor(const Json& value)
		{
			return std::format("Color.New({})", FormatNumbers(value, 4, WhiteColor));
		}

		[[nodiscard]] static std::string FormatQuat(const Json& value)
		{
			return std::format("Quat.New({})", FormatNumbers(value, 4, IdentityQuat));
		}

		[[nodiscard]] static std::string ReadText(const Json& value)
		{
			return JsonReader(value).ReadString().value_or(std::string());
		}

		// The Luau type of a pin's value: its type, the named type of an Enum or Object, any for Any, "?" when optional.
		[[nodiscard]] static std::string GetLuauType(const GraphPinDescription& pin)
		{
			std::string type;
			switch (pin.Type)
			{
				case GraphPinType::Bool:   type = "boolean"; break;
				case GraphPinType::Number: type = "number"; break;
				case GraphPinType::String: type = "string"; break;
				case GraphPinType::Vector: type = "vector"; break;
				case GraphPinType::Color:  type = "Color"; break;
				case GraphPinType::Quat:   type = "Quat"; break;
				case GraphPinType::Entity: type = "Entity"; break;
				case GraphPinType::Asset:  type = "AssetRef"; break;
				case GraphPinType::Enum:
				case GraphPinType::Object: type = pin.TypeName; break;
				case GraphPinType::Exec:
				case GraphPinType::Any:    return "any";
			}
			return pin.Optional ? type + "?" : type;
		}

		// The Luau spelling of literal `value` (not null) of an input of `pin`'s type (decision 7).
		[[nodiscard]] static std::string FormatLiteral(const Json& value, const GraphPinDescription& pin)
		{
			switch (pin.Type)
			{
				case GraphPinType::Bool:   return value.is_boolean() && JsonReader(value).ReadBool().value_or(false) ? "true" : "false";
				case GraphPinType::Number: return FormatLuauNumber(ReadNumber(value));
				case GraphPinType::String:
				case GraphPinType::Enum:   return FormatLuauString(ReadText(value));
				case GraphPinType::Vector: return FormatVector(value);
				case GraphPinType::Color:  return FormatColor(value);
				case GraphPinType::Quat:   return FormatQuat(value);
				case GraphPinType::Asset:
				{
					// The AssetRef a required Asset input takes: nil at run time only for an asset that no longer exists.
					const std::optional<UUID> handle = UUID::FromString(ReadText(value));
					return std::format("(Assets.Load({}) :: AssetRef)", FormatLuauString(handle.has_value() ? handle->ToString() : ReadText(value)));
				}
				case GraphPinType::Any:
				{
					if (value.is_boolean())
						return JsonReader(value).ReadBool().value_or(false) ? "true" : "false";
					if (value.is_number())
						return FormatLuauNumber(ReadNumber(value));
					return FormatLuauString(ReadText(value));
				}
				case GraphPinType::Exec:
				case GraphPinType::Entity:
				case GraphPinType::Object: return "nil";
			}
			return "nil";
		}

		// The default value of a member of `kind` (the file's Default, decision 4) as Luau, for a variable's OnCreate line.
		[[nodiscard]] static std::string FormatMemberValue(GraphValueKind kind, const Json& value)
		{
			switch (kind)
			{
				case GraphValueKind::Number:
				case GraphValueKind::Integer: return FormatLuauNumber(ReadNumber(value));
				case GraphValueKind::Bool:    return value.is_boolean() && JsonReader(value).ReadBool().value_or(false) ? "true" : "false";
				case GraphValueKind::String:
				case GraphValueKind::Enum:    return FormatLuauString(ReadText(value));
				case GraphValueKind::Vector:  return FormatVector(value);
				case GraphValueKind::Color:   return FormatColor(value);
				case GraphValueKind::Quat:    return FormatQuat(value);
				case GraphValueKind::Entity:
				case GraphValueKind::Asset:   return "nil";
				case GraphValueKind::Array:   return "{}";
			}
			return "nil";
		}

		[[nodiscard]] static std::string FormatEnumChoices(const std::vector<std::string>& choices)
		{
			std::string text = "{ ";
			for (size_t i = 0; i < choices.size(); ++i)
				text += (i == 0 ? "" : ", ") + FormatLuauString(choices[i]);
			return text + " }";
		}

		// A field's or an array element's Field constructor without its default ("Field.Asset(\"AudioClip\")").
		[[nodiscard]] static std::string FormatElementConstructor(GraphValueKind kind, std::string_view assetType, const std::vector<std::string>& choices)
		{
			switch (kind)
			{
				case GraphValueKind::Number:  return "Field.Number()";
				case GraphValueKind::Integer: return "Field.Integer()";
				case GraphValueKind::Bool:    return "Field.Bool()";
				case GraphValueKind::String:  return "Field.String()";
				case GraphValueKind::Vector:  return "Field.Vector()";
				case GraphValueKind::Color:   return "Field.Color()";
				case GraphValueKind::Quat:    return "Field.Quat()";
				case GraphValueKind::Entity:  return "Field.Entity()";
				case GraphValueKind::Asset:   return std::format("Field.Asset({})", FormatLuauString(assetType));
				case GraphValueKind::Enum:    return std::format("Field.Enum({})", FormatEnumChoices(choices));
				case GraphValueKind::Array:   return "Field.Array(Field.Number())";
			}
			return "Field.Number()";
		}

		// "<Name> = Field.<Kind>(<default>, <options>)" of one field (decision 7).
		[[nodiscard]] static std::string FormatField(const GraphField& field)
		{
			std::string options;
			for (const auto& [key, value] : field.Options)
			{
				if (value.IsNull())
					continue;
				const std::string text = value.Get().is_string() ? FormatLuauString(ReadText(value.Get())) : FormatLuauNumber(ReadNumber(value.Get()));
				options += std::format("{}{} = {}", options.empty() ? "" : ", ", key, text);
			}
			const std::string withOptions = options.empty() ? std::string() : std::format(", {{ {} }}", options);
			const Json& value = field.Default.Get();
			std::string constructor;
			switch (field.Kind)
			{
				case GraphValueKind::Number:
				case GraphValueKind::Integer:
				case GraphValueKind::Bool:
				case GraphValueKind::String:
				case GraphValueKind::Vector:
				case GraphValueKind::Color:
				case GraphValueKind::Quat:
				{
					static constexpr std::array<std::string_view, 7> Names = { "Number", "Integer", "Bool", "String", "Vector", "Color", "Quat" };
					constructor = std::format("Field.{}({}{})", Names[static_cast<size_t>(field.Kind)], FormatMemberValue(field.Kind, value), withOptions);
					break;
				}
				case GraphValueKind::Entity: constructor = "Field.Entity()"; break;
				case GraphValueKind::Asset:  constructor = std::format("Field.Asset({})", FormatLuauString(field.AssetType)); break;
				case GraphValueKind::Enum:
					constructor = std::format("Field.Enum({}, {})", FormatEnumChoices(field.EnumValues), FormatLuauString(ReadText(value)));
					break;
				case GraphValueKind::Array:
					constructor = std::format("Field.Array({})", FormatElementConstructor(field.Element, field.AssetType, field.EnumValues));
					break;
			}
			return std::format("{} = {},", field.Name, constructor);
		}

		// The kind's parts after its first ("Call.Input.GetAxis": "Input", "GetAxis").
		[[nodiscard]] static std::pair<std::string_view, std::string_view> GetGroupAndMember(std::string_view kind)
		{
			const size_t first = kind.find('.');
			const size_t second = first == std::string_view::npos ? std::string_view::npos : kind.find('.', first + 1);
			if (second == std::string_view::npos)
				return { {}, {} };
			return { kind.substr(first + 1, second - first - 1), kind.substr(second + 1) };
		}

		// --- The text -------------------------------------------------------------------------------------------------

		// The text and its line map: every line is the node whose code it is, 0 for the layout's own.
		class LuauText
		{
		public:
			void Line(size_t indent, std::string_view text, uint32_t node)
			{
				if (!text.empty())
				{
					m_Luau.append(indent, '\t');
					m_Luau += text;
				}
				m_Luau += '\n';
				m_LineNodes.push_back(node);
			}

			[[nodiscard]] size_t GetSize() const { return m_Luau.size(); }
			[[nodiscard]] std::string TakeLuau() { return std::move(m_Luau); }
			[[nodiscard]] std::vector<uint32_t> TakeLineNodes() { return std::move(m_LineNodes); }
		private:
			std::string m_Luau{};
			std::vector<uint32_t> m_LineNodes{};
		};

		// One graph's transpilation (decision 7); `graph` passes ValidateGraph without an issue, or with Incomplete ones only
		// when FindGraphTextLimitIssues measures its layout.
		class GraphTranspilation
		{
		public:
			GraphTranspilation(const GraphDocument& graph, const GraphNodeLibrary& library, std::string_view path)
				: m_Graph(graph), m_Path(path)
			{
				const std::vector<GraphNodeDescription> members = library.GetMemberNodes(graph);
				m_Descriptions.reserve(graph.Nodes.size());
				for (size_t i = 0; i < graph.Nodes.size(); ++i)
				{
					const GraphNode& node = graph.Nodes[i];
					m_Ids.emplace_back(node.Id, i);
					if (const GraphNodeDescription* found = library.Find(node.Kind))
						m_Descriptions.push_back(*found);
					else
					{
						const auto member = std::ranges::lower_bound(members, node.Kind, std::less<>(), &GraphNodeDescription::Kind);
						m_Descriptions.push_back(member != members.end() && member->Kind == node.Kind ? *member : GraphNodeDescription());
					}
				}
				std::ranges::sort(m_Ids);
				for (const GraphLink& link : graph.Links)
				{
					const std::optional<size_t> from = FindNode(link.From.Node);
					const std::optional<size_t> to = FindNode(link.To.Node);
					if (!from.has_value() || !to.has_value())
						continue;
					const GraphPinDescription* pin = FindPin(*from, link.From.Pin, GraphPinDirection::Output);
					if (pin == nullptr)
						continue;
					if (pin->Type == GraphPinType::Exec)
						m_ExecTargets[{ *from, link.From.Pin }] = *to;
					else
						m_DataSources[{ *to, link.To.Pin }] = { *from, link.From.Pin };
				}
			}

			// The text, its line map and its probes; a flow that goes deeper than MaxGraphTextDepth is left out at the statement
			// that goes too deep, which GetStatementsTooDeep names, and the layout stops where the text passes MaxGraphTextBytes,
			// in the statement GetStatementTooLarge names.
			[[nodiscard]] GraphTranspileResult Run()
			{
				Emit(0, "--!strict", 0, std::nullopt);
				Emit(0, std::format("-- Generated from {} by the graph transpiler: edit the graph, not this text.", m_Path), 0, std::nullopt);
				Emit(0, "local Graph = {}", 0, std::nullopt);
				Emit(0, "", 0, std::nullopt);
				if (m_Graph.Fields.empty())
					Emit(0, "Graph.Fields = {}", 0, std::nullopt);
				else
				{
					Emit(0, "Graph.Fields = {", 0, std::nullopt);
					for (const GraphField& field : m_Graph.Fields)
						Emit(1, FormatField(field), 0, std::nullopt);
					Emit(0, "}", 0, std::nullopt);
				}
				std::map<std::string, std::vector<size_t>> callbacks;
				for (size_t i = 0; i < m_Graph.Nodes.size(); ++i)
				{
					if (IsEvent(i))
						callbacks[m_Descriptions[i].Kind.substr(std::string_view("Event.").size())].push_back(i);
				}
				Emit(0, "", 0, std::nullopt);
				const auto onCreate = callbacks.find("OnCreate");
				EmitFunction("OnCreate", onCreate != callbacks.end() ? onCreate->second : std::vector<size_t>());
				for (const auto& [callback, events] : callbacks)
				{
					if (callback == "OnCreate" || m_Full)
						continue;
					Emit(0, "", 0, std::nullopt);
					EmitFunction(callback, events);
				}
				Emit(0, "", 0, std::nullopt);
				Emit(0, std::format("return Script.Define({}, Graph)", FormatLuauString(m_Graph.Name)), 0, std::nullopt);

				GraphTranspileResult result;
				result.Luau = m_Text.TakeLuau();
				result.LineNodes = m_Text.TakeLineNodes();
				for (const auto& [node, pin] : m_Probes)
					result.Probes.push_back({ .Node = node, .Pin = pin, .Key = GetGraphProbeKey(node, pin) });
				return result;
			}

			// After Run: the statements whose code would start deeper than MaxGraphTextDepth (node indexes, sorted).
			[[nodiscard]] std::vector<size_t> GetStatementsTooDeep() const { return { m_TooDeep.begin(), m_TooDeep.end() }; }
			// After Run: the statement whose code the text passed MaxGraphTextBytes in (a node index); none within the limit.
			[[nodiscard]] std::optional<size_t> GetStatementTooLarge() const { return m_TooLarge; }
		private:
			struct Source
			{
				size_t Node = 0;
				std::string Pin{};
			};

			// A line of the text, or the flow that enters a statement (Flow), whose code Expand builds.
			struct Work
			{
				bool Flow = false;
				size_t Indent = 0;
				std::string Text{};
				uint32_t Node = 0;
				size_t Target = 0;   // Flow: the statement's index
				bool Define = false; // Flow: the statement's own code even when it is a merge (its flow function's body)
				// Flow: the pure values' locals declared in the blocks around it, which a value computed again assigns instead
				// of declaring a second local that would shadow the first (shared, immutable).
				Ref<const std::set<std::string>> Declared{};
				// A line: the statement whose code it is (a node index), set when its statement is expanded.
				std::optional<size_t> Statement{};
			};

			// An input's expression, and whether its type is any (an Any output's local, a member read through self: any, an
			// Any event parameter, nil).
			struct InputValue
			{
				std::string Expression{};
				bool Any = false;
			};

			[[nodiscard]] std::optional<size_t> FindNode(uint32_t id) const
			{
				const auto found = std::ranges::lower_bound(m_Ids, std::pair<uint32_t, size_t>(id, 0));
				if (found == m_Ids.end() || found->first != id)
					return std::nullopt;
				return found->second;
			}

			[[nodiscard]] const GraphPinDescription* FindPin(size_t node, std::string_view name, GraphPinDirection direction) const
			{
				for (const GraphPinDescription& pin : m_Descriptions[node].Pins)
				{
					if (pin.Name == name && pin.Direction == direction)
						return &pin;
				}
				return nullptr;
			}

			[[nodiscard]] bool IsEvent(size_t node) const { return m_Descriptions[node].Category == GraphNodeCategory::Event && !m_Descriptions[node].Kind.empty(); }
			[[nodiscard]] bool IsPure(size_t node) const { return m_Descriptions[node].Pure; }
			[[nodiscard]] uint32_t GetId(size_t node) const { return m_Graph.Nodes[node].Id; }
			[[nodiscard]] const std::string& GetKind(size_t node) const { return m_Descriptions[node].Kind; }

			[[nodiscard]] std::optional<size_t> GetExecTarget(size_t node, std::string_view pin) const
			{
				const auto found = m_ExecTargets.find({ node, std::string(pin) });
				if (found == m_ExecTargets.end())
					return std::nullopt;
				return found->second;
			}

			[[nodiscard]] std::vector<size_t> GetExecTargets(size_t node) const
			{
				std::vector<size_t> targets;
				for (const GraphPinDescription& pin : m_Descriptions[node].Pins)
				{
					if (pin.Direction == GraphPinDirection::Output && pin.Type == GraphPinType::Exec)
					{
						if (const std::optional<size_t> target = GetExecTarget(node, pin.Name))
							targets.push_back(*target);
					}
				}
				return targets;
			}

			[[nodiscard]] std::string GetOutputName(size_t node, std::string_view pin) const
			{
				if (IsEvent(node))
					return std::string(pin);
				return std::format("{}{}_{}", IsPure(node) ? "v" : "n", GetId(node), pin);
			}

			// Whether an Any input of `node`'s kind needs a typed value cast to any to type-check: a generated member's (its Luau
			// type is a table or a union) but a variadic's ("...any"), Flow.ForEach's array (ipairs) and Flow.IfNotNil's value
			// (assigned to its Result's any local, which an assignment would otherwise narrow).
			[[nodiscard]] bool NeedsAnyCast(size_t node, const GraphPinDescription& pin) const
			{
				const std::string& kind = GetKind(node);
				if (kind.starts_with("Call.") || kind.starts_with("Set."))
					return pin.Name != "Values";
				return kind == "Flow.ForEach" || kind == "Flow.IfNotNil";
			}

			// The Luau expression of input `pin` of `node`: its link's value, its literal, its Default or nil.
			[[nodiscard]] InputValue GetRawInput(size_t node, const GraphPinDescription& pin) const
			{
				if (const auto source = m_DataSources.find({ node, pin.Name }); source != m_DataSources.end())
				{
					const auto& [from, output] = source->second;
					const GraphPinDescription* outputPin = FindPin(from, output, GraphPinDirection::Output);
					return { .Expression = GetOutputName(from, output),
						.Any = (outputPin != nullptr && outputPin->Type == GraphPinType::Any) || GetKind(from).starts_with("Self.Get.") };
				}
				if (const auto value = m_Graph.Nodes[node].Values.find(pin.Name); value != m_Graph.Nodes[node].Values.end() && !value->second.IsNull())
					return { .Expression = FormatLiteral(value->second.Get(), pin), .Any = false };
				if (!pin.Optional && !pin.Default.IsNull())
					return { .Expression = FormatLiteral(pin.Default.Get(), pin), .Any = false };
				return { .Expression = "nil", .Any = true };
			}

			[[nodiscard]] static std::string CastToAny(std::string_view expression)
			{
				return std::format("({} :: any)", expression);
			}

			// GetRawInput, cast to any where NeedsAnyCast asks.
			[[nodiscard]] std::string GetInput(size_t node, const GraphPinDescription& pin) const
			{
				InputValue value = GetRawInput(node, pin);
				if (pin.Type == GraphPinType::Any && !value.Any && NeedsAnyCast(node, pin))
					return CastToAny(value.Expression);
				return std::move(value.Expression);
			}

			[[nodiscard]] bool IsUnset(size_t node, const GraphPinDescription& pin) const
			{
				if (m_DataSources.contains({ node, pin.Name }))
					return false;
				const auto value = m_Graph.Nodes[node].Values.find(pin.Name);
				return pin.Optional && (value == m_Graph.Nodes[node].Values.end() || value->second.IsNull());
			}

			[[nodiscard]] std::vector<const GraphPinDescription*> GetDataPins(size_t node, GraphPinDirection direction) const
			{
				std::vector<const GraphPinDescription*> pins;
				for (const GraphPinDescription& pin : m_Descriptions[node].Pins)
				{
					if (pin.Direction == direction && pin.Type != GraphPinType::Exec)
						pins.push_back(&pin);
				}
				return pins;
			}

			[[nodiscard]] std::string GetNamedInput(size_t node, std::string_view name) const
			{
				const GraphPinDescription* pin = FindPin(node, name, GraphPinDirection::Input);
				return pin != nullptr ? GetInput(node, *pin) : std::string("nil");
			}

			// The arguments of a call: the data inputs in pin order but "self", trailing unset optional ones left out.
			[[nodiscard]] std::string GetArguments(size_t node) const
			{
				std::vector<const GraphPinDescription*> pins = GetDataPins(node, GraphPinDirection::Input);
				std::erase_if(pins, [](const GraphPinDescription* pin)
				{
					return pin->Name == "self";
				});
				while (!pins.empty() && IsUnset(node, *pins.back()))
					pins.pop_back();
				std::string arguments;
				for (const GraphPinDescription* pin : pins)
					arguments += (arguments.empty() ? "" : ", ") + GetInput(node, *pin);
				return arguments;
			}

			// The probes of `node`'s data outputs, as lines.
			void AddProbes(size_t node, size_t indent, std::vector<Work>& out)
			{
				for (const GraphPinDescription* pin : GetDataPins(node, GraphPinDirection::Output))
				{
					m_Probes.emplace(GetId(node), pin->Name);
					out.push_back(TextWork(indent, std::format("self.{}[{}] = {}", ScriptLiveValuesKey, FormatLuauString(GetGraphProbeKey(GetId(node), pin->Name)), GetOutputName(node, pin->Name)),
						GetId(node)));
				}
			}

			[[nodiscard]] static Work TextWork(size_t indent, std::string text, uint32_t node)
			{
				return Work{ .Flow = false, .Indent = indent, .Text = std::move(text), .Node = node, .Target = 0, .Define = false };
			}

			[[nodiscard]] static Work FlowWork(size_t indent, size_t target, bool define, Ref<const std::set<std::string>> declared)
			{
				return Work{ .Flow = true, .Indent = indent, .Text = {}, .Node = 0, .Target = target, .Define = define, .Declared = std::move(declared) };
			}

			// The pure locals in scope where a statement's code goes: those of the blocks around it (`Declared`) and those its
			// own block declares (`Added`).
			struct PureScope
			{
				const std::set<std::string>* Declared = nullptr; // borrowed from the Work being expanded
				std::set<std::string> Added{};

				[[nodiscard]] bool Contains(const std::string& name) const { return Declared->contains(name) || Added.contains(name); }
			};

			// "local v<Id>_<Pin> = <expression>", the outputs of a pure node annotated any where an output is Any; or, when an
			// enclosing block already declared them (the value is computed again for a nested statement), the assignment, a
			// single Any output's value cast to any so that the assignment keeps the local's type any.
			[[nodiscard]] std::string BindOutputs(size_t node, std::string_view expression, PureScope& scope) const
			{
				const std::vector<const GraphPinDescription*> outputs = GetDataPins(node, GraphPinDirection::Output);
				const bool assigned = std::ranges::all_of(outputs, [this, node, &scope](const GraphPinDescription* pin)
				{
					return scope.Contains(GetOutputName(node, pin->Name));
				});
				std::string names;
				for (const GraphPinDescription* pin : outputs)
				{
					const std::string name = GetOutputName(node, pin->Name);
					names += std::format("{}{}{}", names.empty() ? "" : ", ", name, !assigned && pin->Type == GraphPinType::Any ? ": any" : "");
					scope.Added.insert(name);
				}
				if (!assigned)
					return std::format("local {} = {}", names, expression);
				if (outputs.size() == 1 && outputs.front()->Type == GraphPinType::Any)
					return std::format("{} = {}", names, CastToAny(expression));
				return std::format("{} = {}", names, expression);
			}

			// A pure node's code: its comment, its value and its probes.
			void AddPureNode(size_t node, size_t indent, std::vector<Work>& out, PureScope& scope)
			{
				const std::string& kind = GetKind(node);
				const uint32_t id = GetId(node);
				out.push_back(TextWork(indent, std::format("-- {}: {}", id, kind), id));
				const auto input = [this, node](std::string_view name)
				{
					return GetNamedInput(node, name);
				};
				static constexpr std::pair<std::string_view, std::string_view> BinaryOperators[] = {
					{ "Number.Add", "+" },
					{ "Number.Subtract", "-" },
					{ "Number.Multiply", "*" },
					{ "Number.Divide", "/" },
					{ "Number.Modulo", "%" },
					{ "Compare.Equal", "==" },
					{ "Compare.NotEqual", "~=" },
					{ "Compare.Less", "<" },
					{ "Compare.LessEqual", "<=" },
					{ "Compare.Greater", ">" },
					{ "Compare.GreaterEqual", ">=" },
					{ "Logic.And", "and" },
					{ "Logic.Or", "or" },
					{ "Vector.Add", "+" },
					{ "Vector.Subtract", "-" },
				};
				std::string expression;
				for (const auto& [name, symbol] : BinaryOperators)
				{
					if (kind != name)
						continue;
					if (kind == "Compare.Equal" || kind == "Compare.NotEqual")
					{
						// The strict checker refuses == between unrelated types, so a typed operand meets an any one.
						const GraphPinDescription* a = FindPin(node, "A", GraphPinDirection::Input);
						const GraphPinDescription* b = FindPin(node, "B", GraphPinDirection::Input);
						const InputValue left = a != nullptr ? GetRawInput(node, *a) : InputValue{ .Expression = "nil", .Any = true };
						const InputValue right = b != nullptr ? GetRawInput(node, *b) : InputValue{ .Expression = "nil", .Any = true };
						expression = std::format("{} {} {}", left.Any || right.Any ? left.Expression : CastToAny(left.Expression), symbol, right.Expression);
					}
					else
						expression = std::format("{} {} {}", input("A"), symbol, input("B"));
				}
				if (!expression.empty())
				{
				}
				else if (kind.starts_with("Self.Get."))
					expression = std::format("self.{}", kind.substr(std::string_view("Self.Get.").size()));
				else if (kind == "Self.Entity")
					expression = "self.Entity";
				else if (kind == "Number.Negate")
				{
					const std::string value = input("Value");
					expression = value.starts_with('-') ? std::format("-({})", value) : "-" + value;
				}
				else if (kind == "Logic.Not")
					expression = "not " + input("Value");
				else if (kind == "Vector.Make")
					expression = std::format("vector.create({}, {}, {})", input("X"), input("Y"), input("Z"));
				else if (kind == "Vector.Split")
				{
					const std::string value = input("Value");
					for (const std::string_view component : { "X", "Y", "Z" })
					{
						const std::string name = GetOutputName(node, component);
						const bool assigned = scope.Contains(name);
						scope.Added.insert(name);
						out.push_back(TextWork(indent, std::format("{}{} = {}.{}", assigned ? "" : "local ", name, value, component == "X" ? "x" : (component == "Y" ? "y" : "z")), id));
					}
					AddProbes(node, indent, out);
					return;
				}
				else if (kind == "Vector.Scale")
					expression = std::format("{} * {}", input("Value"), input("Factor"));
				else if (kind == "Vector.Dot")
					expression = std::format("vector.dot({}, {})", input("A"), input("B"));
				else if (kind == "Vector.Cross")
					expression = std::format("vector.cross({}, {})", input("A"), input("B"));
				else if (kind == "Vector.Length")
					expression = std::format("vector.magnitude({})", input("Value"));
				else if (kind == "Vector.Normalize")
					expression = std::format("vector.normalize({})", input("Value"));
				else if (kind == "Text.Concat")
					expression = std::format("tostring({}) .. tostring({})", input("A"), input("B"));
				else
				{
					const auto [group, member] = GetGroupAndMember(kind);
					const bool self = FindPin(node, "self", GraphPinDirection::Input) != nullptr;
					const std::string owner = self ? input("self") : std::string(group);
					if (kind.starts_with("Call."))
						expression = std::format("{}{}{}({})", owner, self ? ":" : ".", member, GetArguments(node));
					else
						expression = std::format("{}.{}", owner, member); // Get and Const
				}
				out.push_back(TextWork(indent, BindOutputs(node, expression, scope), id));
				AddProbes(node, indent, out);
			}

			// The pure nodes that `statement`'s inputs read, each once, depth first in pin order (their inputs before them).
			[[nodiscard]] std::vector<Work> EvaluatePureInputs(size_t statement, size_t indent, PureScope& scope)
			{
				std::vector<Work> lines;
				std::set<size_t> evaluated;
				std::vector<std::pair<size_t, size_t>> stack; // (node, next data input)
				stack.emplace_back(statement, 0);
				while (!stack.empty())
				{
					const size_t node = stack.back().first;
					const std::vector<const GraphPinDescription*> inputs = GetDataPins(node, GraphPinDirection::Input);
					if (stack.back().second < inputs.size())
					{
						const GraphPinDescription* pin = inputs[stack.back().second++];
						const auto source = m_DataSources.find({ node, pin->Name });
						if (source != m_DataSources.end() && IsPure(source->second.Node) && !evaluated.contains(source->second.Node))
						{
							evaluated.insert(source->second.Node);
							stack.emplace_back(source->second.Node, 0);
						}
						continue;
					}
					stack.pop_back();
					if (node != statement)
						AddPureNode(node, indent, lines, scope);
				}
				return lines;
			}

			// The code of the statement `node` at `indent`: its comment, its do ... end block of pure values (when it reads
			// any), its own code with its nested flows, then its continuation.
			[[nodiscard]] std::vector<Work> ExpandStatement(size_t node, size_t indent, const Ref<const std::set<std::string>>& declared)
			{
				const std::string& kind = GetKind(node);
				const uint32_t id = GetId(node);
				std::vector<Work> out;
				out.push_back(TextWork(indent, std::format("-- {}: {}", id, kind), id));
				PureScope scope{ .Declared = declared.get(), .Added = {} };
				std::vector<Work> pure = EvaluatePureInputs(node, indent + 1, scope);
				// A call or a write on an object that is no name (a cast or a literal) binds it first, so no line starts with "(".
				std::string self;
				std::string selfLocal;
				if (FindPin(node, "self", GraphPinDirection::Input) != nullptr)
				{
					self = GetNamedInput(node, "self");
					if (self.starts_with('('))
					{
						selfLocal = std::format("local o{} = {}", id, self);
						self = std::format("o{}", id);
					}
				}
				const bool block = !pure.empty() || !selfLocal.empty();
				const size_t inner = block ? indent + 1 : indent;
				// The flows nested in this statement's block see its pure locals too.
				Ref<const std::set<std::string>> nested = declared;
				if (!scope.Added.empty())
				{
					std::set<std::string> names = *declared;
					names.insert(scope.Added.begin(), scope.Added.end());
					nested = CreateRef<const std::set<std::string>>(std::move(names));
				}
				if (block)
				{
					out.push_back(TextWork(indent, "do", id));
					for (Work& work : pure)
						out.push_back(std::move(work));
					if (!selfLocal.empty())
						out.push_back(TextWork(inner, selfLocal, id));
				}
				std::optional<size_t> continuation = GetExecTarget(node, "Then");
				const auto branch = [&out, &nested, inner, id](std::string_view condition, std::string_view negated, std::optional<size_t> whenTrue, std::optional<size_t> whenFalse)
				{
					if (!whenTrue.has_value() && !whenFalse.has_value())
						return;
					out.push_back(TextWork(inner, std::format("if {} then", whenTrue.has_value() ? condition : negated), id));
					out.push_back(FlowWork(inner + 1, whenTrue.has_value() ? *whenTrue : *whenFalse, false, nested));
					if (whenTrue.has_value() && whenFalse.has_value())
					{
						out.push_back(TextWork(inner, "else", id));
						out.push_back(FlowWork(inner + 1, *whenFalse, false, nested));
					}
					out.push_back(TextWork(inner, "end", id));
				};
				if (kind == "Flow.Branch")
				{
					const std::string condition = GetNamedInput(node, "Condition");
					branch(condition, "not " + condition, GetExecTarget(node, "True"), GetExecTarget(node, "False"));
				}
				else if (kind == "Flow.Sequence")
				{
					continuation.reset();
					for (const std::string_view output : { "Then1", "Then2", "Then3", "Then4" })
					{
						if (const std::optional<size_t> target = GetExecTarget(node, output))
							out.push_back(FlowWork(inner, *target, false, nested));
					}
				}
				else if (kind == "Flow.ForRange" || kind == "Flow.ForEach")
				{
					const bool range = kind == "Flow.ForRange";
					if (range)
					{
						out.push_back(TextWork(inner, std::format("for index{} = {}, {} do", id, GetNamedInput(node, "First"), GetNamedInput(node, "Last")), id));
						out.push_back(TextWork(inner + 1, std::format("{} = index{}", GetOutputName(node, "Index"), id), id));
					}
					else
					{
						out.push_back(TextWork(inner, std::format("for index{}, element{} in ipairs({}) do", id, id, GetNamedInput(node, "Array")), id));
						out.push_back(TextWork(inner + 1, std::format("{} = element{}", GetOutputName(node, "Element"), id), id));
						out.push_back(TextWork(inner + 1, std::format("{} = index{}", GetOutputName(node, "Index"), id), id));
					}
					AddProbes(node, inner + 1, out);
					if (const std::optional<size_t> body = GetExecTarget(node, "Body"))
						out.push_back(FlowWork(inner + 1, *body, false, nested));
					out.push_back(TextWork(inner, "end", id));
					continuation = GetExecTarget(node, "Completed");
				}
				else if (kind == "Flow.Delay")
				{
					if (const std::optional<size_t> later = GetExecTarget(node, "Later"))
					{
						out.push_back(TextWork(inner, std::format("Task.Delay({}, function()", GetNamedInput(node, "Seconds")), id));
						out.push_back(FlowWork(inner + 1, *later, false, nested));
						out.push_back(TextWork(inner, "end)", id));
					}
				}
				else if (kind == "Flow.IfNotNil")
				{
					const std::string result = GetOutputName(node, "Result");
					out.push_back(TextWork(inner, std::format("{} = {}", result, GetNamedInput(node, "Value")), id));
					AddProbes(node, inner, out);
					branch(result + " ~= nil", result + " == nil", GetExecTarget(node, "True"), GetExecTarget(node, "False"));
				}
				else if (kind.starts_with("Self.Set."))
					out.push_back(TextWork(inner, std::format("self.{} = {}", kind.substr(std::string_view("Self.Set.").size()), GetNamedInput(node, "Value")), id));
				else
				{
					const auto [group, member] = GetGroupAndMember(kind);
					const std::string owner = self.empty() ? std::string(group) : self;
					if (kind.starts_with("Set."))
						out.push_back(TextWork(inner, std::format("{}.{} = {}", owner, member, GetNamedInput(node, "Value")), id));
					else
					{
						const std::vector<const GraphPinDescription*> outputs = GetDataPins(node, GraphPinDirection::Output);
						std::string results;
						for (const GraphPinDescription* pin : outputs)
							results += (results.empty() ? "" : ", ") + GetOutputName(node, pin->Name);
						std::string call = std::format("{}{}{}({})", owner, self.empty() ? "." : ":", member, GetArguments(node));
						// A single Any result is cast, so the assignment keeps its any local's type (an assignment narrows it).
						if (outputs.size() == 1 && outputs.front()->Type == GraphPinType::Any)
							call = CastToAny(call);
						out.push_back(TextWork(inner, results.empty() ? call : std::format("{} = {}", results, call), id));
						AddProbes(node, inner, out);
					}
				}
				if (block)
					out.push_back(TextWork(indent, "end", id));
				if (continuation.has_value())
					out.push_back(FlowWork(indent, *continuation, false, declared));
				return out;
			}

			// Emits the flow that enters `target` at `indent`, without recursion: a merge (several exec links enter it) is a
			// call of its flow function unless `define` asks for its own code.
			void EmitFlow(size_t indent, size_t target, bool define)
			{
				std::vector<Work> stack{ FlowWork(indent, target, define, CreateRef<const std::set<std::string>>()) };
				while (!stack.empty() && !m_Full)
				{
					Work work = std::move(stack.back());
					stack.pop_back();
					if (!work.Flow)
					{
						Emit(work.Indent, work.Text, work.Node, work.Statement);
						continue;
					}
					// Luau's parser recurses once per nested block, so a flow nested deeper than it can parse on an import
					// worker's stack is refused at the node that goes too deep (Docs/Decisions/0035-m24-contract.md risk 1):
					// ValidateGraph reports it, so TranspileGraph never lays such a flow out.
					if (work.Indent > MaxGraphTextDepth)
					{
						m_TooDeep.insert(work.Target);
						continue;
					}
					if (!work.Define && m_Merges.contains(work.Target))
					{
						Emit(work.Indent, std::format("flow{}()", GetId(work.Target)), GetId(work.Target), work.Target);
						continue;
					}
					std::vector<Work> expanded = ExpandStatement(work.Target, work.Indent, work.Declared);
					for (auto it = expanded.rbegin(); it != expanded.rend(); ++it)
					{
						if (!it->Flow)
							it->Statement = work.Target;
						stack.push_back(std::move(*it));
					}
				}
			}

			// One callback's function: its header, OnCreate's live-value table and variables, the statements' output locals,
			// the merges' flow functions and the event nodes' flows in id order.
			void EmitFunction(std::string_view callback, const std::vector<size_t>& events)
			{
				std::string parameters;
				if (!events.empty())
				{
					for (const GraphPinDescription* pin : GetDataPins(events.front(), GraphPinDirection::Output))
						parameters += std::format(", {}: {}", pin->Name, GetLuauType(*pin));
				}
				Emit(0, std::format("function Graph.{}(self: any{})", callback, parameters), 0, std::nullopt);
				if (callback == "OnCreate")
				{
					Emit(1, std::format("self.{} = {{}}", ScriptLiveValuesKey), 0, std::nullopt);
					for (const GraphVariable& variable : m_Graph.Variables)
						Emit(1, std::format("self.{} = {}", variable.Name, FormatMemberValue(variable.Kind, variable.Default.Get())), 0, std::nullopt);
				}

				// The statements the events reach, and how many exec links of this function enter each.
				const size_t count = m_Graph.Nodes.size();
				std::vector<bool> reached(count, false);
				std::vector<size_t> entries(count, 0);
				std::vector<size_t> pending(events.begin(), events.end());
				for (const size_t event : events)
					reached[event] = true;
				while (!pending.empty())
				{
					const size_t node = pending.back();
					pending.pop_back();
					for (const size_t next : GetExecTargets(node))
					{
						++entries[next];
						if (!reached[next])
						{
							reached[next] = true;
							pending.push_back(next);
						}
					}
				}
				m_Merges.clear();
				for (size_t i = 0; i < count; ++i)
				{
					if (reached[i] && !IsEvent(i))
					{
						if (entries[i] > 1)
							m_Merges.insert(i);
						for (const GraphPinDescription* pin : GetDataPins(i, GraphPinDirection::Output))
							Emit(1, std::format("local {}: {}", GetOutputName(i, pin->Name), GetLuauType(*pin)), GetId(i), i);
					}
				}
				// Flow functions, each after the ones its flow calls: the merges in reverse topological order.
				if (!m_Merges.empty())
				{
					std::vector<size_t> incoming = entries;
					std::set<size_t> ready;
					for (const size_t event : events)
					{
						for (const size_t next : GetExecTargets(event))
						{
							if (--incoming[next] == 0)
								ready.insert(next);
						}
					}
					std::vector<size_t> order;
					while (!ready.empty())
					{
						const size_t node = *ready.begin();
						ready.erase(ready.begin());
						order.push_back(node);
						for (const size_t next : GetExecTargets(node))
						{
							if (--incoming[next] == 0)
								ready.insert(next);
						}
					}
					for (auto it = order.rbegin(); it != order.rend(); ++it)
					{
						if (!m_Merges.contains(*it))
							continue;
						Emit(1, std::format("local function flow{}()", GetId(*it)), GetId(*it), *it);
						EmitFlow(2, *it, true);
						Emit(1, "end", GetId(*it), *it);
					}
				}
				for (const size_t event : events)
				{
					if (const std::optional<size_t> target = GetExecTarget(event, "Then"))
						EmitFlow(1, *target, false);
				}
				Emit(0, "end", 0, std::nullopt);
			}

			// Appends a line of the text, `statement` the statement whose code it is (none for the layout's own lines), until the
			// text passes MaxGraphTextBytes: the line that passes it records its statement (for a layout line, the last statement
			// laid out), and the layout stops. Before any statement's code the text holds only the fields, the variables and the
			// layout's lines, at most about 4.4 MB by the document's limits (GraphTextLimits.h), so the text passes the limit only
			// after a statement's code.
			void Emit(size_t indent, std::string_view text, uint32_t node, std::optional<size_t> statement)
			{
				if (m_Full)
					return;
				if (statement.has_value())
					m_LastStatement = statement;
				m_Text.Line(indent, text, node);
				if (m_Text.GetSize() <= MaxGraphTextBytes)
					return;
				m_Full = true;
				ENGINE_CORE_ASSERT(m_LastStatement.has_value(), "graph '{}': the text passed {} bytes before any statement's code", m_Graph.Name, MaxGraphTextBytes);
				m_TooLarge = m_LastStatement;
			}

			const GraphDocument& m_Graph; // borrowed for the transpilation
			std::string_view m_Path{};    // borrowed for the transpilation
			std::vector<GraphNodeDescription> m_Descriptions{};
			std::vector<std::pair<uint32_t, size_t>> m_Ids{}; // (id, index), sorted
			std::map<std::pair<size_t, std::string>, size_t> m_ExecTargets{};
			std::map<std::pair<size_t, std::string>, Source> m_DataSources{};
			std::set<size_t> m_Merges{}; // of the function being emitted
			std::set<std::pair<uint32_t, std::string>> m_Probes{};
			LuauText m_Text{};
			std::set<size_t> m_TooDeep{};            // the statements whose code would start deeper than MaxGraphTextDepth
			bool m_Full = false;                     // the text passed MaxGraphTextBytes, and the layout stopped
			std::optional<size_t> m_LastStatement{}; // the statement whose code the last statement line was
			std::optional<size_t> m_TooLarge{};      // the statement the text passed MaxGraphTextBytes in
		};

		GraphTextLimitIssues FindGraphTextLimitIssues(const GraphDocument& graph, const GraphNodeLibrary& library)
		{
			GraphTranspilation transpilation(graph, library, {});
			static_cast<void>(transpilation.Run());
			return GraphTextLimitIssues{ .TooDeep = transpilation.GetStatementsTooDeep(), .TooLarge = transpilation.GetStatementTooLarge() };
		}

		std::string DescribeGraphStatementTooDeep(const GraphNode& node)
		{
			return std::format("node {} ({}): the flow that reaches it nests more than {} blocks deep, more than the script compiler parses; keep the flow "
							   "flatter (a Flow.Sequence, or several exec links into one node, runs a flow at its own depth)",
				node.Id, node.Kind, MaxGraphTextDepth);
		}

		std::string DescribeGraphTextTooLarge(const GraphNode& node)
		{
			return std::format("node {} ({}): the graph's transpiled text grows past {} bytes here, more than a graph's text may hold; every statement "
							   "evaluates again the values it reads, so many statements that read long chains of values repeat them: let fewer statements "
							   "read them, or split the graph into several behaviours",
				node.Id, node.Kind, MaxGraphTextBytes);
		}

	}

	Result<GraphTranspileResult> TranspileGraph(const GraphDocument& graph, const GraphNodeLibrary& library, std::string_view path)
	{
		const std::vector<GraphIssue> issues = ValidateGraph(graph, library);
		if (!issues.empty())
		{
			std::vector<ErrorIssue> located;
			located.reserve(issues.size());
			for (const GraphIssue& issue : issues)
				located.push_back({ .JsonPointer = issue.JsonPointer, .Message = issue.Message, .Hint = {}, .Suggestions = {} });
			return std::unexpected(Error(ErrorCode::Validation, std::format("graph '{}' does not transpile: {} issue{} (only a complete, valid graph has a text)", graph.Name, issues.size(), issues.size() == 1 ? "" : "s"))
					.WithIssues(std::move(located)));
		}
		Utils::GraphTranspilation transpilation(graph, library, path);
		GraphTranspileResult result = transpilation.Run();
		// ValidateGraph measured this layout (Utils::FindGraphTextLimitIssues), so no flow of a graph that validates is too deep
		// and its text holds at most MaxGraphTextBytes.
		ENGINE_CORE_ASSERT(transpilation.GetStatementsTooDeep().empty(), "graph '{}' validated with a flow nested too deep", graph.Name);
		ENGINE_CORE_ASSERT(!transpilation.GetStatementTooLarge().has_value(), "graph '{}' validated with a text of more than {} bytes", graph.Name,
			Utils::MaxGraphTextBytes);
		return result;
	}

	uint32_t FindGraphNodeAtLine(const GraphTranspileResult& result, uint32_t line)
	{
		if (line == 0 || line > result.LineNodes.size())
			return 0;
		return result.LineNodes[line - 1];
	}

	std::string GetGraphProbeKey(uint32_t node, std::string_view pin)
	{
		return std::format("{}.{}", node, pin);
	}

	std::optional<GraphPinRef> ParseGraphProbeKey(std::string_view key)
	{
		const size_t dot = key.find('.');
		if (dot == std::string_view::npos || dot == 0 || dot + 1 == key.size())
			return std::nullopt;
		const std::string_view digits = key.substr(0, dot);
		// The digits alone, without a sign or a leading zero, so a key reads back only as GetGraphProbeKey writes it.
		if (digits.front() == '0')
			return std::nullopt;
		uint32_t node = 0;
		const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), node);
		if (error != std::errc() || end != digits.data() + digits.size() || node == 0 || node > MaxGraphNodeId)
			return std::nullopt;
		return GraphPinRef{ .Node = node, .Pin = std::string(key.substr(dot + 1)) };
	}

	GraphAssetLiterals FindGraphAssetLiterals(const GraphDocument& graph, const GraphNodeLibrary& library)
	{
		std::set<AssetHandle> handles;
		const std::vector<GraphNodeDescription> members = library.GetMemberNodes(graph);
		for (const GraphNode& node : graph.Nodes)
		{
			const GraphNodeDescription* description = library.Find(node.Kind);
			if (description == nullptr)
			{
				const auto member = std::ranges::lower_bound(members, node.Kind, std::less<>(), &GraphNodeDescription::Kind);
				if (member == members.end() || member->Kind != node.Kind)
					continue;
				description = &*member;
			}
			for (const auto& [name, value] : node.Values)
			{
				const auto pin = std::ranges::find_if(description->Pins, [&name](const GraphPinDescription& candidate)
				{
					return candidate.Name == name && candidate.Direction == GraphPinDirection::Input;
				});
				if (pin == description->Pins.end() || pin->Type != GraphPinType::Asset || !value.Get().is_string())
					continue;
				const std::optional<UUID> handle = UUID::FromString(Utils::ReadText(value.Get()));
				if (handle.has_value() && handle->IsValid())
					handles.insert(*handle);
			}
		}
		return GraphAssetLiterals{ .Handles = { handles.begin(), handles.end() } };
	}

}
