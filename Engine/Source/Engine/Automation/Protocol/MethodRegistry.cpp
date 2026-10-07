#include "EnginePCH.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <optional>

// Params are read in two passes (Invoke): FieldInfo::ValidateJson on the params struct's self field reports every problem
// with the messages of §13.3 (Variant values, such as component maps, are checked against their resolved schema as
// errors), and only a clean document is then read with StructInfo::FromJson. The validation pass locates its issues below
// the self field's name, which ReadParams strips again, so pointers are relative to the params object.
//
// Component values may also set their component's writable virtual fields (Transform.EulerAngles, WorldPosition, §5.4;
// the entity.create example of §13.10). Those are not part of a component's stored JSON, so the validation pass checks
// them against their own fields on a copy of the params without them, and the full schemas list them in the component
// definitions they use; the handlers apply them through the fields' setters (ComponentAccess::SetFieldValue).

namespace Engine {

	struct MethodRegistry::Storage
	{
		// One registered method: its descriptor (handed out by pointer) and what invoking it needs.
		struct Entry
		{
			MethodDescriptor Descriptor{};
			TypeKey Host = nullptr; // the context type the handler takes
			Invoker Call{};
		};

		std::map<std::string, Scope<Entry>, std::less<>> Entries{}; // by name; Scope keeps descriptor addresses stable
		// JsonSchema::ForComponents of the type registry with the writable virtual fields added to the component
		// definitions (MakeParamsComponentSchema), built by Freeze (full schemas reference the component $defs).
		Json ComponentSchema{};
	};

	namespace Utils {

		constexpr std::string_view DryRunMember = "dryRun";
		constexpr std::string_view IfRevisionMember = "ifRevision";
		// The handshake (Handshake.h): the I/O thread reads its params strictly (ParseHelloRequest), so its schema lists no
		// reserved member.
		constexpr std::string_view HelloMethodName = "session.hello";
		constexpr std::string_view MetaMember = "_meta";
		constexpr std::array<std::string_view, 3> ReservedMembers = { DryRunMember, IfRevisionMember, MetaMember };
		// How many Variant values PrepareParams resolves inside each other before it stops looking for enums (as
		// Reflection's own walks bound it).
		constexpr uint32_t MaxCanonicalVariantDepth = 16;
		// How deep compact schemas inline struct definitions before they show a nested struct as a free-form object.
		constexpr uint32_t MaxInlineDepth = 32;
		constexpr std::string_view DefsPrefix = "#/$defs/";

		[[maybe_unused]] [[nodiscard]] static bool IsLowerAlpha(char character)
		{
			return character >= 'a' && character <= 'z';
		}

		[[maybe_unused]] [[nodiscard]] static bool IsDigit(char character)
		{
			return character >= '0' && character <= '9';
		}

		// "domain.verb": a lowercase domain of letters and digits starting with a letter, a dot, and a camelCase verb. Only
		// registration asserts read it (and the two helpers above), so Dist builds, which drop asserts, do not need them.
		[[maybe_unused]] [[nodiscard]] static bool IsWellFormedMethodName(std::string_view name)
		{
			const size_t dot = name.find('.');
			if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size())
				return false;
			const std::string_view domain = name.substr(0, dot);
			const std::string_view verb = name.substr(dot + 1);
			if (!IsLowerAlpha(domain.front()) || !IsLowerAlpha(verb.front()))
				return false;
			for (const char character : domain)
			{
				if (!IsLowerAlpha(character) && !IsDigit(character))
					return false;
			}
			for (const char character : verb)
			{
				if (!IsLowerAlpha(character) && !IsDigit(character) && !(character >= 'A' && character <= 'Z'))
					return false;
			}
			return true;
		}

		// A params member mapping component registry names to component values (convention 10).
		[[nodiscard]] static bool IsComponentMap(const FieldInfo& field)
		{
			const TypeInfo& type = field.GetType();
			return type.GetKind() == FieldType::Map && type.GetElement() != nullptr && type.GetElement()->GetKind() == FieldType::Variant && field.GetResolver() == &ResolveComponentValue;
		}

		// Takes the virtual fields out of every component value of a params object (component maps at any depth of the
		// params struct), validating each writable one against its own field and reporting read-only ones, so the copy
		// that remains holds only stored component JSON for the struct validation. Issues go to the validation context
		// at the member's pointer.
		class VirtualComponentFieldSplitter
		{
		public:
			VirtualComponentFieldSplitter(const TypeRegistry& types, ValidationContext& validation)
				: m_Types(&types), m_Validation(&validation)
			{
			}

			void SplitStruct(const StructInfo& type, Json& object)
			{
				if (!object.is_object())
					return;
				for (const Scope<FieldInfo>& field : type.GetFields())
				{
					if (field->IsVirtual() || !field->GetMeta().Serialized)
						continue;
					const auto member = object.find(field->GetName());
					if (member == object.end())
						continue;
					m_Validation->PushKey(field->GetName());
					if (IsComponentMap(*field))
						SplitComponentMap(*member);
					else
						SplitValue(field->GetType(), *member);
					m_Validation->PopKey();
				}
			}

			// How many virtual fields were taken out.
			[[nodiscard]] size_t GetSplitCount() const { return m_SplitCount; }
		private:
			void SplitValue(const TypeInfo& type, Json& value)
			{
				if (type.GetKind() == FieldType::Struct && type.GetStruct() != nullptr)
				{
					SplitStruct(*type.GetStruct(), value);
				}
				else if (type.GetKind() == FieldType::Array && type.GetElement() != nullptr && value.is_array())
				{
					for (size_t index = 0; index < value.size(); ++index)
					{
						m_Validation->PushKey(std::to_string(index));
						SplitValue(*type.GetElement(), value[index]);
						m_Validation->PopKey();
					}
				}
				else if (type.GetKind() == FieldType::Map && type.GetElement() != nullptr && value.is_object())
				{
					for (auto entry = value.begin(); entry != value.end(); ++entry)
					{
						m_Validation->PushKey(entry.key());
						SplitValue(*type.GetElement(), entry.value());
						m_Validation->PopKey();
					}
				}
			}

			void SplitComponentMap(Json& map)
			{
				if (!map.is_object())
					return;
				for (auto entry = map.begin(); entry != map.end(); ++entry)
				{
					const ComponentInfo* component = m_Types->FindComponent(entry.key());
					if (component == nullptr || !entry.value().is_object())
						continue; // the struct validation reports both
					m_Validation->PushKey(entry.key());
					Json& value = entry.value();
					for (const Scope<FieldInfo>& field : component->GetFields())
					{
						if (!field->IsVirtual())
							continue;
						const auto member = value.find(field->GetName());
						if (member == value.end())
							continue;
						if (field->IsReadOnly())
						{
							m_Validation->Error(field->GetName(), std::format("'{}.{}' is read-only: it is computed from the entity", component->GetName(), field->GetName()));
						}
						else
						{
							ResolveContext resolve;
							resolve.Registry = m_Types;
							resolve.OwnerType = component;
							field->ValidateJson(JsonReader(*member), resolve, *m_Validation);
						}
						value.erase(member);
						++m_SplitCount;
					}
					m_Validation->PopKey();
				}
			}
		private:
			const TypeRegistry* m_Types = nullptr;
			ValidationContext* m_Validation = nullptr;
			size_t m_SplitCount = 0;
		};

		// Whether `type` holds a component map in any of its fields, nested structs, arrays and maps included.
		[[nodiscard]] static bool HasComponentMap(const StructInfo& type, uint32_t depth)
		{
			if (depth > MaxInlineDepth)
				return false;
			for (const Scope<FieldInfo>& field : type.GetFields())
			{
				if (IsComponentMap(*field))
					return true;
				const TypeInfo* element = &field->GetType();
				while ((element->GetKind() == FieldType::Array || element->GetKind() == FieldType::Map) && element->GetElement() != nullptr)
					element = element->GetElement();
				if (element->GetKind() == FieldType::Struct && element->GetStruct() != nullptr && HasComponentMap(*element->GetStruct(), depth + 1))
					return true;
			}
			return false;
		}

		// ForComponents with every component definition also listing its writable virtual fields: the shape of a component
		// value in params (component maps), whose virtual fields go through their setters.
		[[nodiscard]] static Json MakeParamsComponentSchema(const TypeRegistry& types)
		{
			Json schema = JsonSchema::ForComponents(types);
			const auto definitions = schema.find("$defs");
			if (definitions == schema.end())
				return schema;
			for (const ComponentInfo* component : types.GetComponents())
			{
				const auto definition = definitions->find(component->GetName());
				if (definition == definitions->end())
					continue;
				for (const Scope<FieldInfo>& field : component->GetFields())
				{
					if (!field->IsVirtual() || field->IsReadOnly())
						continue;
					Json fieldSchema = JsonSchema::ForField(*field);
					// Virtual fields are plain values (vectors, quaternions); a struct-typed one would need its $defs merged.
					ENGINE_CORE_ASSERT(!fieldSchema.contains("$defs"), "The virtual field '{}.{}' references struct definitions",
						component->GetName(), field->GetName());
					(*definition)["properties"][field->GetName()] = std::move(fieldSchema);
				}
			}
			return schema;
		}

		// An InvalidArgument error of `issues` (at least one) about the params of `method`.
		[[nodiscard]] static Error MakeParamsError(std::string_view method, std::vector<ErrorIssue> issues)
		{
			std::string message = issues.size() == 1 ? std::format("invalid params of '{}': {}", method, issues.front().Message)
													 : std::format("{} invalid params of '{}'", issues.size(), method);
			ErrorLocation location;
			if (issues.size() == 1)
				location.JsonPointer = issues.front().JsonPointer;
			std::string hint = issues.size() == 1 ? issues.front().Hint : std::string();
			return Error(ErrorCode::InvalidArgument, std::move(message))
				.WithHint(std::move(hint))
				.WithLocation(std::move(location))
				.WithIssues(std::move(issues));
		}

		// `error`'s issues with `prefix` in front of each pointer (an error without issues becomes one issue at its location).
		[[nodiscard]] static std::vector<ErrorIssue> PrefixIssues(const Error& error, std::string_view prefix)
		{
			std::vector<ErrorIssue> issues = error.GetIssues();
			if (issues.empty())
			{
				issues.push_back(ErrorIssue{ .JsonPointer = error.GetLocation().JsonPointer.value_or(std::string()),
					.Message = error.GetMessageText(),
					.Hint = error.GetHint(),
					.Suggestions = {} });
			}
			for (ErrorIssue& issue : issues)
				issue.JsonPointer = std::string(prefix) + issue.JsonPointer;
			return issues;
		}

		// `error` with `prefix` in front of its location's pointer and of every issue's pointer (pointers relative to an op's
		// params become relative to the op, InvokeNested).
		[[nodiscard]] static Error PrefixErrorPointers(Error error, std::string_view prefix)
		{
			Error prefixed(error.GetCode(), error.GetMessageText());
			for (const std::string& context : error.GetContexts())
				prefixed = std::move(prefixed).WithContext(context);
			ErrorLocation location = error.GetLocation();
			if (location.JsonPointer.has_value())
				location.JsonPointer = std::string(prefix) + *location.JsonPointer;
			std::vector<ErrorIssue> issues = error.GetIssues();
			for (ErrorIssue& issue : issues)
				issue.JsonPointer = std::string(prefix) + issue.JsonPointer;
			return std::move(prefixed).WithLocation(std::move(location)).WithHint(error.GetHint()).WithIssues(std::move(issues));
		}

		// Rewrites every enum spelling `value` holds that `type` recognizes, ignoring ASCII case, to its registry name
		// (convention 6), and, with a resolver, every asset reference that is not a handle to the handle it names
		// (convention 13), collecting one issue per reference that does not resolve. Values of the wrong JSON type are left
		// for the strict reader to report.
		class ParamsCanonicalizer
		{
		public:
			ParamsCanonicalizer(const TypeRegistry& types, IAssetReferenceResolver* assets)
				: m_Types(&types), m_Assets(assets)
			{
			}

			void CanonicalizeStruct(const StructInfo& type, Json& object, uint32_t variantDepth, const std::string& pointer)
			{
				if (!object.is_object())
					return;
				const JsonReader ownerJson(object);
				ResolveContext owner;
				owner.Registry = m_Types;
				owner.OwnerType = &type;
				owner.OwnerJson = &ownerJson;
				for (const Scope<FieldInfo>& field : type.GetFields())
				{
					// Stored fields, and the virtual fields a component value may set (VirtualComponentFieldSplitter).
					if (!field->IsVirtual() && !field->GetMeta().Serialized)
						continue;
					const auto member = object.find(field->GetName());
					if (member != object.end())
						CanonicalizeValue(field->GetType(), *field, *member, owner, variantDepth, JsonReader::AppendPointer(pointer, field->GetName()));
				}
			}

			// The asset references that did not resolve with NotFound or InvalidArgument, one issue each.
			[[nodiscard]] std::vector<ErrorIssue> TakeIssues() { return std::move(m_Issues); }
			// True when every reference that did not resolve names no asset (NotFound), so the call is NotFound rather than
			// InvalidParams.
			[[nodiscard]] bool AllNotFound() const { return m_AllNotFound; }
			// The first resolver error of another code (InvalidState without a project, a refresh's Io), located at its value;
			// Invoke returns it unchanged, so the code is the one the host gave.
			[[nodiscard]] std::optional<Error> TakeHostError() { return std::move(m_HostError); }
		private:
			void CanonicalizeValue(const TypeInfo& type, const FieldInfo& field, Json& value, const ResolveContext& owner,
				uint32_t variantDepth, const std::string& pointer)
			{
				switch (type.GetKind())
				{
					case FieldType::Enum:
						CanonicalizeEnum(type, value);
						break;
					case FieldType::AssetRef:
						ResolveAssetReference(type, value, pointer);
						break;
					case FieldType::Struct:
						if (type.GetStruct() != nullptr)
							CanonicalizeStruct(*type.GetStruct(), value, variantDepth, pointer);
						break;
					case FieldType::Array:
						if (value.is_array() && type.GetElement() != nullptr)
						{
							for (size_t index = 0; index < value.size(); ++index)
								CanonicalizeValue(*type.GetElement(), field, value[index], owner, variantDepth, JsonReader::AppendPointer(pointer, index));
						}
						break;
					case FieldType::Map:
						if (value.is_object() && type.GetElement() != nullptr)
						{
							for (auto entry = value.begin(); entry != value.end(); ++entry)
							{
								ResolveContext keyed = owner;
								keyed.Key = entry.key();
								CanonicalizeValue(*type.GetElement(), field, entry.value(), keyed, variantDepth, JsonReader::AppendPointer(pointer, entry.key()));
							}
						}
						break;
					case FieldType::Variant:
						CanonicalizeVariant(field, value, owner, variantDepth, pointer);
						break;
					case FieldType::Bool:
					case FieldType::Int32:
					case FieldType::UInt32:
					case FieldType::Float:
					case FieldType::Vec2:
					case FieldType::Vec3:
					case FieldType::Vec4:
					case FieldType::Quat:
					case FieldType::Color3:
					case FieldType::Color4:
					case FieldType::Bool3:
					case FieldType::String:
					case FieldType::EntityRef:
						break;
				}
			}

			static void CanonicalizeEnum(const TypeInfo& type, Json& value)
			{
				if (!value.is_string() || type.GetEnum() == nullptr)
					return;
				const Result<std::string> text = JsonReader(value).ReadString();
				if (!text)
					return;
				const EnumEntry* entry = type.GetEnum()->FindByNameIgnoreCase(*text);
				if (entry != nullptr && entry->Name != *text)
					value = entry->Name;
			}

			void ResolveAssetReference(const TypeInfo& type, Json& value, const std::string& pointer)
			{
				if (m_Assets == nullptr || !value.is_string())
					return;
				const Result<std::string> text = JsonReader(value).ReadString();
				if (!text || UUID::FromString(*text).has_value())
					return; // a handle (convention 13), or a value the strict reader reports
				Result<UUID> handle = m_Assets->ResolveAssetReference(*text, type.GetAssetTypeName());
				if (handle)
				{
					value = handle->ToString();
					return;
				}
				const ErrorCode code = handle.error().GetCode();
				if (code != ErrorCode::NotFound && code != ErrorCode::InvalidArgument)
				{
					if (!m_HostError.has_value())
					{
						ErrorLocation location;
						location.JsonPointer = pointer;
						m_HostError = std::move(handle).error().WithLocation(std::move(location));
					}
					return;
				}
				m_AllNotFound = m_AllNotFound && code == ErrorCode::NotFound;
				m_Issues.push_back(ErrorIssue{ .JsonPointer = pointer, .Message = handle.error().GetMessageText(), .Hint = handle.error().GetHint(), .Suggestions = {} });
			}

			void CanonicalizeVariant(const FieldInfo& field, Json& value, const ResolveContext& owner, uint32_t variantDepth, const std::string& pointer)
			{
				if (field.GetResolver() == nullptr || variantDepth >= MaxCanonicalVariantDepth)
					return;
				const Result<const FieldInfo*> schema = field.ResolveVariant(owner);
				if (!schema.has_value() || *schema == nullptr)
					return; // the strict reader reports what cannot be resolved
				const FieldInfo& resolved = **schema;
				ResolveContext nested;
				nested.Registry = m_Types;
				nested.Schemas = owner.Schemas;
				CanonicalizeValue(resolved.GetType(), resolved, value, nested, variantDepth + 1, pointer);
			}
		private:
			const TypeRegistry* m_Types = nullptr;
			IAssetReferenceResolver* m_Assets = nullptr; // documented back-reference for one Invoke (convention 13); null in PrepareParams
			std::vector<ErrorIssue> m_Issues;
			std::optional<Error> m_HostError;
			bool m_AllNotFound = true;
		};

		// Replaces every {"$ref": "#/$defs/<Name>"} inside `node` by the definition from `definitions` (keeping the referring
		// schema's description), so a compact schema needs no "$defs".
		static void InlineDefinitions(Json& node, const Json& definitions, uint32_t depth)
		{
			if (node.is_array())
			{
				for (Json& element : node)
					InlineDefinitions(element, definitions, depth);
				return;
			}
			if (!node.is_object())
				return;

			const auto reference = node.find("$ref");
			if (reference != node.end() && reference->is_string())
			{
				const std::string target = JsonReader(*reference).ReadString().value_or(std::string());
				std::string name;
				if (target.starts_with(DefsPrefix))
				{
					// RFC 6901 unescaping of the one token.
					const std::string_view token = std::string_view(target).substr(DefsPrefix.size());
					for (size_t index = 0; index < token.size(); ++index)
					{
						if (token[index] == '~' && index + 1 < token.size())
						{
							name += token[index + 1] == '1' ? '/' : '~';
							++index;
						}
						else
						{
							name += token[index];
						}
					}
				}
				const auto definition = definitions.find(name);
				const auto description = node.find("description");
				Json replacement = Json::object();
				if (definition != definitions.end() && depth < MaxInlineDepth)
				{
					replacement = *definition;
					InlineDefinitions(replacement, definitions, depth + 1);
				}
				else
				{
					replacement["type"] = "object";
				}
				if (description != node.end())
					replacement["description"] = *description;
				node = std::move(replacement);
				return;
			}
			for (auto member = node.begin(); member != node.end(); ++member)
				InlineDefinitions(member.value(), definitions, depth);
		}

		// The schema of a component map: in full style each component name refers to its definition; in compact style a
		// free-form object (§13.8 "Compact tool schemas").
		[[nodiscard]] static Json MakeComponentMapSchema(const FieldInfo& field, SchemaStyle style, const Json& componentSchema)
		{
			Json schema = Json::object();
			schema["type"] = "object";
			if (style == SchemaStyle::Compact)
			{
				// §13.8's text; the field's own description, written for full schemas, already ends with a pointer to component.schema.
				schema["description"] = "Field values by registry name; see component_schema.";
				return schema;
			}
			schema["description"] = field.GetDescription();
			const auto properties = componentSchema.find("properties");
			schema["properties"] = properties != componentSchema.end() ? *properties : Json::object();
			schema["additionalProperties"] = false;
			return schema;
		}

		// `schema` with "required" listing `required` after its "additionalProperties" (when not empty).
		[[nodiscard]] static Json AddRequired(const Json& schema, const std::vector<std::string>& required)
		{
			if (required.empty())
				return schema;
			Json list = Json::array();
			for (const std::string& name : required)
				list.push_back(name);
			Json result = Json::object();
			bool added = false;
			for (auto member = schema.begin(); member != schema.end(); ++member)
			{
				result[member.key()] = member.value();
				if (member.key() == "additionalProperties")
				{
					result["required"] = list;
					added = true;
				}
			}
			if (!added)
				result["required"] = std::move(list);
			return result;
		}

	}

	Result<const FieldInfo*> ResolveComponentValue(const ResolveContext& context)
	{
		if (context.Registry == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "resolving a component value needs the type registry");
		if (context.Key.empty())
			return MakeError(ErrorCode::InvalidArgument, "a component value needs a component name as its key");
		const ComponentInfo* component = context.Registry->FindComponent(context.Key);
		if (component == nullptr)
		{
			std::vector<std::string> suggestions = context.Registry->SuggestComponentNames(context.Key);
			std::string hint = MakeDidYouMeanHint(suggestions);
			return std::unexpected(Error(ErrorCode::NotFound, std::format("unknown component '{}'", context.Key))
					.WithHint(hint)
					.WithIssue(ErrorIssue{ .JsonPointer = {}, .Message = std::format("unknown component '{}'", context.Key), .Hint = hint, .Suggestions = std::move(suggestions) }));
		}
		return &component->GetSelfField();
	}

	MethodRegistry::MethodRegistry(const TypeRegistry& types)
		: m_Types(&types), m_Storage(CreateScope<Storage>())
	{
		ENGINE_CORE_ASSERT(types.IsFrozen(), "A method registry needs a frozen type registry");
	}

	MethodRegistry::~MethodRegistry() = default;

	void MethodRegistry::Freeze()
	{
		if (m_IsFrozen)
			return;
		for (const MethodDescriptor* method : m_Methods)
		{
			const MethodSpecification& specification = method->Specification;
			ENGINE_CORE_ASSERT(!specification.Examples.empty(), "Method '{}' has no example (rpc.discover shows one per method)",
				specification.Name);
			for (const MethodExample& example : specification.Examples)
			{
				[[maybe_unused]] const Result<PreparedParams> prepared = PrepareParams(*method, example.Params);
				ENGINE_CORE_ASSERT(prepared.has_value(), "An example of method '{}' is invalid: {}", specification.Name,
					prepared.has_value() ? std::string() : prepared.error().ToString());
			}
		}
		m_Storage->ComponentSchema = Utils::MakeParamsComponentSchema(*m_Types);
		m_IsFrozen = true;
	}

	const MethodDescriptor* MethodRegistry::Find(std::string_view name) const
	{
		const auto entry = m_Storage->Entries.find(name);
		return entry != m_Storage->Entries.end() ? &entry->second->Descriptor : nullptr;
	}

	std::vector<std::string> MethodRegistry::SuggestMethodNames(std::string_view name, size_t maxResults) const
	{
		std::vector<std::string_view> names;
		names.reserve(m_Methods.size());
		for (const MethodDescriptor* method : m_Methods)
			names.push_back(method->Specification.Name);
		return FuzzySuggest(name, names, maxResults);
	}

	std::vector<std::string> MethodRegistry::GetDomains() const
	{
		std::vector<std::string> domains;
		domains.reserve(m_Methods.size());
		for (const MethodDescriptor* method : m_Methods)
			domains.push_back(method->Domain);
		std::sort(domains.begin(), domains.end());
		domains.erase(std::unique(domains.begin(), domains.end()), domains.end());
		return domains;
	}

	Result<PreparedParams> MethodRegistry::PrepareParams(const MethodDescriptor& method, const Json& params) const
	{
		const MethodSpecification& specification = method.Specification;
		if (!params.is_null() && !params.is_object())
		{
			return std::unexpected(Utils::MakeParamsError(specification.Name,
				{ ErrorIssue{ .JsonPointer = {},
					.Message = std::format("params must be an object, got {}", JsonTypeToString(GetJsonType(params))),
					.Hint = {},
					.Suggestions = {} } }));
		}

		PreparedParams prepared;
		prepared.Params = params.is_null() ? Json::object() : params;
		std::vector<ErrorIssue> issues;

		if (const auto dryRun = prepared.Params.find(Utils::DryRunMember); dryRun != prepared.Params.end())
		{
			const Result<bool> value = JsonReader(*dryRun, "/dryRun").ReadBool();
			if (!value)
			{
				issues.push_back(ErrorIssue{ .JsonPointer = "/dryRun", .Message = "dryRun must be a boolean", .Hint = {}, .Suggestions = {} });
			}
			else if (*value && !specification.SupportsDryRun)
			{
				return MakeError(ErrorCode::Unsupported, "'{}' does not support dryRun: it cannot be validated without being applied",
					specification.Name);
			}
			else
			{
				prepared.Options.DryRun = *value;
			}
			prepared.Params.erase(dryRun);
		}
		if (const auto revision = prepared.Params.find(Utils::IfRevisionMember); revision != prepared.Params.end())
		{
			const Result<uint64_t> value = JsonReader(*revision, "/ifRevision").ReadUInt64();
			// Every method takes it (§13.4): a call that changes something only when asked (project.validate {fix},
			// scene.open {save}, session.shutdown {save}) is guarded like a mutation, and for a read it is a precondition.
			if (!value)
				issues.push_back(ErrorIssue{ .JsonPointer = "/ifRevision", .Message = "ifRevision must be an integer >= 0", .Hint = {}, .Suggestions = {} });
			else
				prepared.Options.IfRevision = *value;
			prepared.Params.erase(revision);
		}
		if (const auto meta = prepared.Params.find(Utils::MetaMember); meta != prepared.Params.end())
		{
			if (!meta->is_object())
				issues.push_back(ErrorIssue{ .JsonPointer = "/_meta", .Message = "_meta must be an object", .Hint = {}, .Suggestions = {} });
			prepared.Params.erase(meta);
		}

		for (const std::string& required : specification.RequiredParams)
		{
			if (!prepared.Params.contains(required))
			{
				issues.push_back(ErrorIssue{ .JsonPointer = JsonReader::AppendPointer("", required),
					.Message = std::format("missing required param '{}'", required),
					.Hint = {},
					.Suggestions = {} });
			}
		}
		if (!issues.empty())
			return std::unexpected(Utils::MakeParamsError(specification.Name, std::move(issues)));

		if (method.Params != nullptr)
			Utils::ParamsCanonicalizer(*m_Types, nullptr).CanonicalizeStruct(*method.Params, prepared.Params, 0, std::string());
		return prepared;
	}

	MethodResult MethodRegistry::Invoke(MethodContext& context) const
	{
		const MethodDescriptor& method = context.GetMethod();
		const auto entry = m_Storage->Entries.find(method.Specification.Name);
		ENGINE_CORE_ASSERT(entry != m_Storage->Entries.end() && &entry->second->Descriptor == &method,
			"MethodRegistry::Invoke: method '{}' is not registered here", method.Specification.Name);
		if (entry == m_Storage->Entries.end() || &entry->second->Descriptor != &method || method.Params == nullptr)
			return Error(ErrorCode::Unknown, std::format("method '{}' is not registered in this registry", method.Specification.Name));

		Storage::Entry& registered = *entry->second; // UniqueFunction is called through a non-const reference
		ENGINE_CORE_ASSERT(context.IsHostType(registered.Host), "Method '{}' was registered for another host's context",
			method.Specification.Name);
		if (!context.IsHostType(registered.Host))
			return Error(ErrorCode::Unknown, std::format("method '{}' cannot run in this host", method.Specification.Name));

		// Convention 13: asset references become handles before anything reads the params. This runs after the host admitted
		// the request, so a refresh the resolver makes for a dry run stays in the dry run's sandbox.
		if (IAssetReferenceResolver* assets = context.GetAssetReferenceResolver(); assets != nullptr)
		{
			Json resolved = context.GetParams();
			Utils::ParamsCanonicalizer canonicalizer(*m_Types, assets);
			canonicalizer.CanonicalizeStruct(*method.Params, resolved, 0, std::string());
			// A host's own error (InvalidState, Io) keeps its code; it outranks the references' NotFound and InvalidParams.
			if (std::optional<Error> hostError = canonicalizer.TakeHostError(); hostError.has_value())
				return std::move(*hostError);
			std::vector<ErrorIssue> unresolved = canonicalizer.TakeIssues();
			if (!unresolved.empty() && canonicalizer.AllNotFound())
			{
				// A reference that names no asset is NotFound, as for an asset param (§13.3), located at its value.
				const Error notFound = Utils::MakeParamsError(method.Specification.Name, std::move(unresolved));
				return Error(ErrorCode::NotFound, notFound.GetMessageText())
					.WithHint(notFound.GetHint())
					.WithLocation(notFound.GetLocation())
					.WithIssues(notFound.GetIssues());
			}
			if (!unresolved.empty())
				return Utils::MakeParamsError(method.Specification.Name, std::move(unresolved));
			context.m_Request.Params = std::move(resolved);
		}

		// Pass 1: every problem, with the messages of §13.3.
		const StructInfo& type = *method.Params;
		const Json& params = context.GetParams();
		ValidationContext validation;
		ResolveContext resolve;
		resolve.Registry = m_Types;
		bool hasVirtualFields = false;
		if (Utils::HasComponentMap(type, 0))
		{
			// Virtual component fields are checked on their own, the stored component JSON by the struct validation.
			Json stored = params;
			Utils::VirtualComponentFieldSplitter splitter(*m_Types, validation);
			validation.PushKey(type.GetSelfField().GetName());
			splitter.SplitStruct(type, stored);
			validation.PopKey();
			hasVirtualFields = splitter.GetSplitCount() > 0;
			type.GetSelfField().ValidateJson(JsonReader(stored), resolve, validation);
		}
		else
		{
			type.GetSelfField().ValidateJson(JsonReader(params), resolve, validation);
		}
		const std::string prefix = JsonReader::AppendPointer("", type.GetSelfField().GetName());
		std::vector<ErrorIssue> issues;
		for (ValidationIssue& issue : validation.TakeIssues())
		{
			const bool promoted = issue.Code == UnknownFieldCode || issue.Code == VariantUnresolvedCode || issue.Code == VariantSchemaMismatchCode;
			if (issue.Severity != DiagnosticSeverity::Error && !promoted)
				continue;
			std::string pointer = std::move(issue.JsonPointer);
			if (pointer.starts_with(prefix) && (pointer.size() == prefix.size() || pointer[prefix.size()] == '/'))
				pointer.erase(0, prefix.size());
			issues.push_back(ErrorIssue{ .JsonPointer = std::move(pointer),
				.Message = std::move(issue.Message),
				.Hint = std::move(issue.Hint),
				.Suggestions = std::move(issue.Suggestions) });
		}
		if (!issues.empty())
			return Utils::MakeParamsError(method.Specification.Name, std::move(issues));

		// Pass 2: read the clean document. Pass 1 checked every member, so Strict only repeats its checks; it is off when
		// component values set virtual fields, which a strict read of the component's stored JSON would call unknown. The
		// Variant values keep them verbatim for the handler.
		ObjectPtr object = type.CreateDefault();
		const Status read =
			type.FromJson(object.get(), JsonReader(params), ReadContext{ .Schemas = nullptr, .Strict = !hasVirtualFields, .Diagnostics = nullptr });
		if (!read)
			return Utils::MakeParamsError(method.Specification.Name, Utils::PrefixIssues(read.error(), ""));

		return registered.Call(context, object.get());
	}

	Result<Json> MethodRegistry::InvokeNested(MethodContext& parent, std::string_view method, const Json& params) const
	{
		const MethodDescriptor* descriptor = Find(method);
		if (descriptor == nullptr)
		{
			std::vector<std::string> suggestions = SuggestMethodNames(method);
			std::string hint = MakeDidYouMeanHint(suggestions);
			return std::unexpected(Error(ErrorCode::NotFound, std::format("no method '{}'", method))
					.WithHint(hint)
					.WithIssue(ErrorIssue{ .JsonPointer = "/method", .Message = std::format("no method '{}'", method), .Hint = hint, .Suggestions = std::move(suggestions) }));
		}
		const MethodSpecification& specification = descriptor->Specification;
		if (!specification.AllowedInBatch)
		{
			std::string message = std::format("{} cannot be an op of edit.batch", method);
			return std::unexpected(Error(ErrorCode::InvalidArgument, message)
					.WithHint("only reads and methods whose every change is one undoable command can be batched")
					.WithIssue(ErrorIssue{ .JsonPointer = "/method", .Message = std::move(message), .Hint = {}, .Suggestions = {} }));
		}
		if (!params.is_null() && !params.is_object())
		{
			std::string message = std::format("the params of an op must be an object, got {}", JsonTypeToString(GetJsonType(params)));
			return std::unexpected(Error(ErrorCode::InvalidArgument, message)
					.WithIssue(ErrorIssue{ .JsonPointer = "/params", .Message = std::move(message), .Hint = {}, .Suggestions = {} }));
		}

		std::vector<ErrorIssue> reserved;
		if (params.is_object())
		{
			for (const std::string_view member : Utils::ReservedMembers)
			{
				if (params.contains(member))
				{
					reserved.push_back(ErrorIssue{ .JsonPointer = JsonReader::AppendPointer("/params", member),
						.Message = std::format("'{}' is not accepted inside an op: the batch's own options apply to every op", member),
						.Hint = {},
						.Suggestions = {} });
				}
			}
		}
		if (!reserved.empty())
		{
			std::string message = reserved.size() == 1 ? reserved.front().Message : std::format("{} reserved members in the params of an op", reserved.size());
			return std::unexpected(Error(ErrorCode::InvalidArgument, std::move(message)).WithIssues(std::move(reserved)));
		}
		if (parent.IsDryRun() && !specification.SupportsDryRun)
			return MakeError(ErrorCode::Unsupported, "'{}' does not support dryRun, so it cannot be an op of a dry-run batch", method);

		Result<PreparedParams> prepared = PrepareParams(*descriptor, params);
		if (!prepared)
		{
			Error& error = prepared.error();
			if (error.GetCode() != ErrorCode::InvalidArgument)
				return std::unexpected(std::move(error));
			return std::unexpected(Utils::MakeParamsError(method, Utils::PrefixIssues(error, "/params")));
		}

		RequestInfo info = parent.GetRequest();
		info.Id = Json();
		info.Method = std::string(method);
		Scope<MethodContext> nested = parent.CreateNested(MethodRequest{
			.Info = std::move(info),
			.Options = parent.GetOptions(),
			.Method = descriptor,
			.Params = std::move(prepared->Params),
			.Registry = this,
			.PhaseMarker = nullptr, // the batch's phase marker covers its ops
			.NestingDepth = parent.GetNestingDepth() + 1,
		});
		ENGINE_CORE_ASSERT(nested != nullptr, "CreateNested returned no context for '{}'", method);
		if (nested == nullptr)
			return MakeError(ErrorCode::Unknown, "the host created no context for the op '{}'", method);

		parent.SetPhase(std::format("Automation:{} > {}", parent.GetRequest().Method, method));
		MethodResult result = Invoke(*nested);
		parent.SetPhase(std::format("Automation:{}", parent.GetRequest().Method));
		if (Json* json = std::get_if<Json>(&result))
			return std::move(*json);
		if (Error* error = std::get_if<Error>(&result))
			return std::unexpected(Utils::PrefixErrorPointers(std::move(*error), "/params")); // located relative to the op
		ENGINE_CORE_ASSERT(false, "The op '{}' returned a pending operation, which AllowedInBatch excludes", method);
		return MakeError(ErrorCode::Unknown, "the op '{}' did not resolve at once", method);
	}

	Json MethodRegistry::GetParamsSchema(const MethodDescriptor& method, SchemaStyle style) const
	{
		if (method.Params == nullptr)
			return Json::object();
		Json schema = JsonSchema::ForStruct(*method.Params);
		// Built once by Freeze; a registry that is still being filled builds it for this call.
		const Json unfrozenComponentSchema = m_IsFrozen ? Json() : Utils::MakeParamsComponentSchema(*m_Types);
		const Json& componentSchema = m_IsFrozen ? m_Storage->ComponentSchema : unfrozenComponentSchema;

		// Component maps, in the root and in every struct definition.
		bool usesComponents = false;
		const auto rewrite = [&usesComponents, style, &componentSchema](Json& definition, const StructInfo& type)
		{
			const auto properties = definition.find("properties");
			if (properties == definition.end())
				return;
			for (const Scope<FieldInfo>& field : type.GetFields())
			{
				if (!Utils::IsComponentMap(*field))
					continue;
				const auto property = properties->find(field->GetName());
				if (property == properties->end())
					continue;
				*property = Utils::MakeComponentMapSchema(*field, style, componentSchema);
				usesComponents = true;
			}
		};
		rewrite(schema, *method.Params);
		const auto definitions = schema.find("$defs");
		if (definitions != schema.end())
		{
			for (auto definition = definitions->begin(); definition != definitions->end(); ++definition)
			{
				if (const StructInfo* type = m_Types->FindStruct(definition.key()))
					rewrite(definition.value(), *type);
			}
		}

		const MethodSpecification& specification = method.Specification;
		Json& properties = schema["properties"];
		if (specification.SupportsDryRun)
		{
			Json dryRun = Json::object();
			dryRun["description"] = "Validate and return the would-be result without applying anything.";
			dryRun["type"] = "boolean";
			properties[std::string(Utils::DryRunMember)] = std::move(dryRun);
		}
		if (specification.Name != Utils::HelloMethodName)
		{
			Json revision = Json::object();
			revision["description"] = "Optimistic concurrency: fail with Conflict unless the editor's revision still equals this value.";
			revision["type"] = "integer";
			revision["minimum"] = 0;
			properties[std::string(Utils::IfRevisionMember)] = std::move(revision);
		}

		if (style == SchemaStyle::Full)
		{
			if (usesComponents)
			{
				const auto componentDefinitions = componentSchema.find("$defs");
				Json& merged = schema["$defs"];
				if (componentDefinitions != componentSchema.end())
				{
					for (auto definition = componentDefinitions->begin(); definition != componentDefinitions->end(); ++definition)
					{
						if (!merged.contains(definition.key()))
							merged[definition.key()] = definition.value();
					}
				}
			}
		}
		else
		{
			const Json allDefinitions = schema.contains("$defs") ? schema["$defs"] : Json::object();
			schema.erase("$defs");
			Utils::InlineDefinitions(schema, allDefinitions, 0);
		}
		return Utils::AddRequired(schema, specification.RequiredParams);
	}

	Json MethodRegistry::GetResultSchema(const MethodDescriptor& method, SchemaStyle style) const
	{
		if (method.Result == nullptr)
			return Json::object();
		Json schema = JsonSchema::ForStruct(*method.Result);
		Json& properties = schema["properties"];
		Json meta = Json::object();
		meta["description"] = "The diagnostics delta every response carries: revision, dirty, undoLabel, tick, playState, diagnostics.";
		meta["type"] = "object";
		properties["_meta"] = std::move(meta);
		Json dryRun = Json::object();
		dryRun["description"] = "Present and true when the call was a dry run.";
		dryRun["type"] = "boolean";
		properties[std::string(Utils::DryRunMember)] = std::move(dryRun);
		if (style == SchemaStyle::Compact)
		{
			const Json allDefinitions = schema.contains("$defs") ? schema["$defs"] : Json::object();
			schema.erase("$defs");
			Utils::InlineDefinitions(schema, allDefinitions, 0);
		}
		return schema;
	}

	Json MethodRegistry::Describe(const MethodDescriptor& method) const
	{
		const MethodSpecification& specification = method.Specification;
		Json description = Json::object();
		description["name"] = specification.Name;
		description["domain"] = method.Domain;
		description["description"] = specification.Description;
		description["exposeAsTool"] = specification.ExposeAsTool;
		description["mutates"] = specification.Mutates;
		description["supportsDryRun"] = specification.SupportsDryRun;
		description["availableInRuntime"] = specification.AvailableInRuntime;
		description["availableInLauncher"] = specification.AvailableInLauncher;
		description["pending"] = method.Pending;
		description["timeoutSeconds"] = specification.TimeoutSeconds;
		Json required = Json::array();
		for (const std::string& name : specification.RequiredParams)
			required.push_back(name);
		description["requiredParams"] = std::move(required);
		description["params"] = GetParamsSchema(method, SchemaStyle::Full);
		description["result"] = GetResultSchema(method, SchemaStyle::Full);
		Json examples = Json::array();
		for (const MethodExample& example : specification.Examples)
		{
			Json entry = Json::object();
			entry["description"] = example.Description;
			entry["params"] = example.Params.is_null() ? Json::object() : example.Params;
			examples.push_back(std::move(entry));
		}
		description["examples"] = std::move(examples);
		return description;
	}

	Json MethodRegistry::BuildMethodCatalog() const
	{
		Json methods = Json::array();
		for (const MethodDescriptor* method : m_Methods)
		{
			if (!method->Specification.TestHook)
				methods.push_back(Describe(*method));
		}
		Json catalog = Json::object();
		catalog["Format"] = "MethodCatalog";
		catalog["Version"] = 1;
		catalog["ProtocolVersion"] = CurrentProtocolVersion.ToString();
		catalog["Methods"] = std::move(methods);
		return catalog;
	}

	Json MethodRegistry::BuildToolCatalog() const
	{
		std::map<std::string, const MethodDescriptor*> tools;
		for (const MethodDescriptor* method : m_Methods)
		{
			if (method->Specification.ExposeAsTool && !method->Specification.TestHook)
				tools.emplace(MethodNameToToolName(method->Specification.Name), method);
		}
		Json list = Json::array();
		for (const auto& [name, method] : tools)
		{
			const MethodSpecification& specification = method->Specification;
			Json tool = Json::object();
			tool["name"] = name;
			tool["method"] = specification.Name;
			tool["description"] = specification.Description;
			tool["inputSchema"] = GetParamsSchema(*method, SchemaStyle::Compact);
			tool["mutates"] = specification.Mutates;
			tool["supportsDryRun"] = specification.SupportsDryRun;
			tool["timeoutSeconds"] = specification.TimeoutSeconds;
			list.push_back(std::move(tool));
		}
		Json catalog = Json::object();
		catalog["Format"] = "McpCatalog";
		catalog["Version"] = 1;
		catalog["ProtocolVersion"] = CurrentProtocolVersion.ToString();
		catalog["Tools"] = std::move(list);
		return catalog;
	}

	void MethodRegistry::AddEntry(MethodSpecification specification, TypeKey host, TypeKey params, TypeKey result, bool pending, Invoker invoker)
	{
		const std::string name = specification.Name;
		ENGINE_CORE_ASSERT(!m_IsFrozen, "Method '{}' is registered after MethodRegistry::Freeze", name);
		ENGINE_CORE_ASSERT(Utils::IsWellFormedMethodName(name), "Method name '{}' is not 'domain.verb' (lowercase domain, camelCase verb)", name);
		ENGINE_CORE_ASSERT(!m_Storage->Entries.contains(name), "Method '{}' is registered twice", name);
		ENGINE_CORE_ASSERT(!specification.Description.empty(), "Method '{}' needs a description", name);
		ENGINE_CORE_ASSERT(!(specification.TestHook && specification.ExposeAsTool), "Test hook '{}' cannot be a tool", name);
		ENGINE_CORE_ASSERT(!(pending && specification.AllowedInBatch), "Pending method '{}' cannot be an op of edit.batch", name);
		ENGINE_CORE_ASSERT(!(pending && specification.SupportsDryRun), "Pending method '{}' cannot support dry runs", name);
		if (m_IsFrozen || m_Storage->Entries.contains(name))
			return;

		const StructInfo* paramsType = m_Types->FindStructByKey(params);
		const StructInfo* resultType = m_Types->FindStructByKey(result);
		ENGINE_CORE_ASSERT(paramsType != nullptr, "The params struct of method '{}' is not registered", name);
		ENGINE_CORE_ASSERT(resultType != nullptr, "The result struct of method '{}' is not registered", name);
		for ([[maybe_unused]] const std::string& required : specification.RequiredParams)
		{
			ENGINE_CORE_ASSERT(paramsType != nullptr && paramsType->FindField(required) != nullptr,
				"Required param '{}' of method '{}' is not a field of its params struct", required, name);
		}

		Scope<Storage::Entry> entry = CreateScope<Storage::Entry>();
		entry->Descriptor.Domain = name.substr(0, name.find('.'));
		entry->Descriptor.Specification = std::move(specification);
		entry->Descriptor.Params = paramsType;
		entry->Descriptor.Result = resultType;
		entry->Descriptor.Pending = pending;
		entry->Host = host;
		entry->Call = std::move(invoker);
		const MethodDescriptor* descriptor = &entry->Descriptor;
		m_Storage->Entries.emplace(name, std::move(entry));

		const auto position = std::lower_bound(m_Methods.begin(), m_Methods.end(), name,
			[](const MethodDescriptor* method, const std::string& value)
		{
			return method->Specification.Name < value;
		});
		m_Methods.insert(position, descriptor);
	}

	std::string MethodNameToToolName(std::string_view method)
	{
		std::string tool;
		tool.reserve(method.size() + 4);
		for (const char character : method)
		{
			if (character == '.')
			{
				tool.push_back('_');
			}
			else if (character >= 'A' && character <= 'Z')
			{
				tool.push_back('_');
				tool.push_back(static_cast<char>(character - 'A' + 'a'));
			}
			else
			{
				tool.push_back(character);
			}
		}
		return tool;
	}

}
