#pragma once

#include "Engine/Core/Assert.h"
#include "Engine/Core/Base.h"
#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeInfo.h"
#include "Engine/Reflection/TypeList.h"
#include "Engine/Reflection/ValidationContext.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Engine {

	class Random;

	template<typename Derived, typename T>
	class FieldBuilder;
	template<typename T>
	class StructBuilder;
	template<typename T>
	class ComponentBuilder;
	template<typename E>
	class EnumBuilder;

	// The single source of truth for reflected types (Architecture §5.4): components, structs and enums with their fields,
	// metadata, validators, migrations and type-erased operations. It drives the serializer, inspector, script proxies,
	// automation validation and schemas, docs, undo and the coverage gates, and FuzzySuggest hints for unknown names.
	//
	// Lifecycle: an owner (EngineContext, a test) creates one registry, registers everything (RegisterBuiltinComponents,
	// RegisterProjectSettingsTypes, M4+ automation structs) on one thread, then calls Freeze. Registration after Freeze is a
	// programmer error (asserted). After Freeze the registry is immutable and every const member is thread-safe. There is
	// no global registry. Every pointer and reference it hands out stays valid for its lifetime; it is neither copyable
	// nor movable for that reason.
	//
	// Names are the persistent identities (files store registry names, never C++ or EnTT type ids, §5.1); TypeKeys are the
	// in-process C++ identities. Registering a second type under a used name, or a C++ type twice, is asserted.
	class TypeRegistry
	{
	public:
		TypeRegistry();
		~TypeRegistry();

		TypeRegistry(const TypeRegistry&) = delete;
		TypeRegistry& operator=(const TypeRegistry&) = delete;
		TypeRegistry(TypeRegistry&&) = delete;
		TypeRegistry& operator=(TypeRegistry&&) = delete;

		// Registers component type T under `name` with its mandatory description; T must be default-constructible,
		// copyable and non-empty. Components registered with a scene host go through RegisterComponent<T>
		// (Scene/ComponentRegistration.h) instead of calling this directly.
		template<typename T>
		[[nodiscard]] ComponentBuilder<T> Component(std::string_view name, std::string_view description)
		{
			static_assert(std::is_class_v<T> && !std::is_empty_v<T>, "a reflected component holds data");
			ComponentInfo& info = AddComponent(TypeKeyOf<T>(), name, description, Detail::MakeTypeOps<T, FieldType::Struct>());
			return ComponentBuilder<T>(*this, info);
		}

		// Registers struct type T (default-constructible and copyable) under `name`.
		template<typename T>
		[[nodiscard]] StructBuilder<T> Struct(std::string_view name, std::string_view description)
		{
			static_assert(std::is_class_v<T>, "a reflected struct is a class type");
			StructInfo& info = AddStruct(TypeKeyOf<T>(), name, description, Detail::MakeTypeOps<T, FieldType::Struct>());
			return StructBuilder<T>(*this, info);
		}

		// Registers enum type E under `name`.
		template<typename E>
		[[nodiscard]] EnumBuilder<E> Enum(std::string_view name, std::string_view description)
		{
			static_assert(std::is_scoped_enum_v<E>, "reflected enums are enum class types");
			EnumInfo& info = AddEnum(TypeKeyOf<E>(), name, description, Detail::MakeTypeOps<E, FieldType::Enum>());
			return EnumBuilder<E>(info);
		}

		// The TypeInfo of C++ type M, created on first use (builders call it). Arrays and maps create their element type;
		// an enum or struct M must already be registered (asserted, naming the type key's owner field). Returns nullptr only
		// after a failed assertion in builds without asserts.
		template<typename M>
		[[nodiscard]] const TypeInfo* GetOrCreateType()
		{
			if (const TypeInfo* existing = FindType(TypeKeyOf<M>()))
				return existing;

			constexpr FieldType Kind = Detail::DeduceFieldType<M>();
			TypeInfo::Specification specification;
			specification.Kind = Kind;
			specification.Name = std::string(FieldTypeToString(Kind));
			specification.Key = TypeKeyOf<M>();
			if constexpr (Kind == FieldType::Enum || Kind == FieldType::Struct)
			{
				// Registered enums and structs get their TypeInfo from AddEnum/AddStruct/AddComponent.
				ENGINE_CORE_ASSERT(false, "Type '{}' is used as a field type before it is registered", FieldTypeToString(Kind));
				return nullptr;
			}
			else
			{
				specification.Ops = Detail::MakeTypeOps<M, Kind>();
				if constexpr (Kind == FieldType::Array)
					specification.Element = GetOrCreateType<typename Detail::VectorTraits<M>::ElementType>();
				else if constexpr (Kind == FieldType::Map)
					specification.Element = GetOrCreateType<typename Detail::StringMapTraits<M>::ElementType>();
				else if constexpr (Kind == FieldType::AssetRef)
					specification.AssetTypeName = std::string(AssetFieldTraits<M>::AssetTypeName);
				return AddType(std::move(specification));
			}
		}

		// The Color3 (glm::vec3) or Color4 (glm::vec4) TypeInfo, created on first use (ColorField).
		template<typename M>
		[[nodiscard]] const TypeInfo* GetOrCreateColorType()
		{
			constexpr TypeKey Key = TypeKeyOf<Detail::ColorOf<M>>();
			if (const TypeInfo* existing = FindType(Key))
				return existing;

			constexpr FieldType Kind = std::is_same_v<M, glm::vec3> ? FieldType::Color3 : FieldType::Color4;
			TypeInfo::Specification specification;
			specification.Kind = Kind;
			specification.Name = std::string(FieldTypeToString(Kind));
			specification.Key = Key;
			specification.Ops = Detail::MakeTypeOps<M, Kind>();
			return AddType(std::move(specification));
		}

		// Ends registration: resolves Requires/Excludes, checks that every type and field has a description, that every
		// component has a category and a complete migration chain, that every struct field type is registered and that
		// every type with a Validate rule also has a Generate hook, so the registry suite's random values satisfy it
		// (asserted, naming the type; these are programmer errors in registration files). Idempotent.
		void Freeze();
		[[nodiscard]] bool IsFrozen() const { return m_IsFrozen; }

		// Lookups. Names are exact (case-sensitive, §6); nullptr when absent.
		[[nodiscard]] const ComponentInfo* FindComponent(std::string_view name) const;
		[[nodiscard]] const ComponentInfo* FindComponentByKey(TypeKey key) const;
		template<typename T>
		[[nodiscard]] const ComponentInfo* FindComponent() const
		{
			return FindComponentByKey(TypeKeyOf<T>());
		}

		[[nodiscard]] const StructInfo* FindStruct(std::string_view name) const;
		[[nodiscard]] const StructInfo* FindStructByKey(TypeKey key) const;
		template<typename T>
		[[nodiscard]] const StructInfo* FindStruct() const
		{
			return FindStructByKey(TypeKeyOf<T>());
		}

		[[nodiscard]] const EnumInfo* FindEnum(std::string_view name) const;
		[[nodiscard]] const EnumInfo* FindEnumByKey(TypeKey key) const;
		template<typename E>
		[[nodiscard]] const EnumInfo* FindEnum() const
		{
			return FindEnumByKey(TypeKeyOf<E>());
		}

		// The TypeInfo registered for `key` (scalars, containers, enums, structs and components alike), or nullptr.
		[[nodiscard]] const TypeInfo* FindType(TypeKey key) const;

		// Every component in registration order (the canonical component order, ComponentInfo::GetIndex()).
		[[nodiscard]] std::span<const ComponentInfo* const> GetComponents() const { return m_ComponentList; }
		// Every struct that is not a component, in registration order.
		[[nodiscard]] std::span<const StructInfo* const> GetStructs() const { return m_StructList; }
		// Every enum, in registration order.
		[[nodiscard]] std::span<const EnumInfo* const> GetEnums() const { return m_EnumList; }

		// Up to `maxResults` component names close to `name`, best first (FuzzySuggest), for "did you mean" hints.
		[[nodiscard]] std::vector<std::string> SuggestComponentNames(std::string_view name, size_t maxResults = 3) const;

		// True when every type of the list is a registered component (the startup check of BuiltinComponents, §5.4).
		template<typename... Types>
		[[nodiscard]] bool AreComponentsRegistered(TypeList<Types...> /*list*/) const
		{
			return ((FindComponent<Types>() != nullptr) && ...);
		}
	private:
		// Registration internals used by the templates above. Each asserts that the registry is not frozen, that `name` is
		// non-empty and unused among components, structs and enums, that `key` is not registered yet and that `description`
		// is non-empty. The new type also gets its TypeInfo (kind Struct or Enum) under `key`.
		[[nodiscard]] ComponentInfo& AddComponent(TypeKey key, std::string_view name, std::string_view description, TypeOps ops);
		[[nodiscard]] StructInfo& AddStruct(TypeKey key, std::string_view name, std::string_view description, TypeOps ops);
		[[nodiscard]] EnumInfo& AddEnum(TypeKey key, std::string_view name, std::string_view description, TypeOps ops);
		// Stores a scalar or container TypeInfo under specification.Key (asserted unused) and returns it.
		[[nodiscard]] const TypeInfo* AddType(TypeInfo::Specification specification);
	private:
		struct Storage; // owning containers and the name/key indexes (TypeRegistry.cpp)
	private:
		Scope<Storage> m_Storage;
		std::vector<const ComponentInfo*> m_ComponentList;
		std::vector<const StructInfo*> m_StructList;
		std::vector<const EnumInfo*> m_EnumList;
		bool m_IsFrozen = false;
	};

	// The shared builder methods of StructBuilder and ComponentBuilder (CRTP: each returns the derived builder so calls
	// chain). Builders are short-lived views returned by TypeRegistry::Struct/Component; they never outlive the
	// registration statement. Field names are PascalCase and unique per type, descriptions are mandatory (asserted), and
	// fields are registered in member declaration order, which becomes the canonical JSON key order (§6).
	template<typename Derived, typename T>
	class FieldBuilder
	{
	public:
		// A stored field of member type M, reflected as Detail::DeduceFieldType<M>(): bool, int32_t, uint32_t, float,
		// glm::vec2/vec3/vec4/quat/bvec3, std::string, UUID (EntityRef), TypedAssetHandle (AssetRef, AssetFilter from the
		// handle's type), a registered enum, std::vector<E>, std::map<std::string, E>, a registered struct, VariantValue
		// (a free-form Variant; use VariantField to attach a resolver). Enums and structs must be registered first.
		template<typename M>
		Derived& Field(std::string_view name, M T::* member, std::string_view description, FieldMeta meta = {})
		{
			return AddStoredField(name, m_Registry->GetOrCreateType<M>(), CreateScope<MemberFieldAccessor<T, M>>(member),
				description, std::move(meta), nullptr);
		}

		// A stored glm::vec3 or glm::vec4 member reflected as Color3 or Color4 (linear colour, §5.3).
		template<typename M>
		Derived& ColorField(std::string_view name, M T::* member, std::string_view description, FieldMeta meta = {})
		{
			static_assert(std::is_same_v<M, glm::vec3> || std::is_same_v<M, glm::vec4>, "ColorField needs a glm::vec3 or glm::vec4 member");
			return AddStoredField(name, m_Registry->GetOrCreateColorType<M>(), CreateScope<MemberFieldAccessor<T, M>>(member),
				description, std::move(meta), nullptr);
		}

		// A stored field holding Variant values (VariantValue, or a std::vector/std::map of them) whose schema `resolver`
		// resolves at run time (§5.4: ScriptComponent.Fields, PrefabOverride.Value).
		template<typename M>
		Derived& VariantField(std::string_view name, M T::* member, std::string_view description, VariantSchemaResolver resolver,
			FieldMeta meta = {})
		{
			return AddStoredField(name, m_Registry->GetOrCreateType<M>(), CreateScope<MemberFieldAccessor<T, M>>(member),
				description, std::move(meta), resolver);
		}

		// A virtual field of value type V (reflected as Detail::DeduceFieldType<V>()): registered getter and optional setter
		// (null = read-only), accepted by scripts and automation, never serialized (§5.3). Meta.Virtual is set and
		// Meta.Serialized cleared.
		template<typename V>
		Derived& VirtualField(std::string_view name, std::string_view description, VirtualFieldGetter getter, VirtualFieldSetter setter,
			FieldMeta meta = {})
		{
			ENGINE_CORE_ASSERT(getter != nullptr, "Virtual field '{}' needs a getter", name);
			meta.Virtual = true;
			meta.Serialized = false;
			FieldInfo::Specification specification;
			specification.Name = std::string(name);
			specification.Description = std::string(description);
			specification.Type = m_Registry->GetOrCreateType<V>();
			specification.Meta = std::move(meta);
			specification.Getter = getter;
			specification.Setter = setter;
			m_Info->AddField(std::move(specification));
			return static_cast<Derived&>(*this);
		}

		// A type-level rule run after the field checks, as in §5.4:
		//     .Validate([](const RigidBodyComponent& component, ValidationContext& context) { ... })
		Derived& Validate(void (*validator)(const T& object, ValidationContext& context))
		{
			ENGINE_CORE_ASSERT(validator != nullptr, "Validate needs a function");
			m_Info->AddValidator([validator](const void* object, ValidationContext& context)
			{
				validator(*static_cast<const T*>(object), context);
			});
			return static_cast<Derived&>(*this);
		}

		// The generation hook that turns randomized field values into an object the type's Validate rules accept
		// (RandomValueGenerator, the registry suite's 200 random round trips). Mandatory for every type with a Validate rule
		// (asserted by Freeze); it changes only what the rules need and draws only from `random`, for example:
		//     .Generate([](SpotLightComponent& light, Random& random) { ... make InnerConeAngle < OuterConeAngle ... })
		Derived& Generate(void (*generator)(T& object, Random& random))
		{
			ENGINE_CORE_ASSERT(generator != nullptr, "Generate needs a function");
			m_Info->AddGenerator([generator](void* object, Random& random)
			{
				generator(*static_cast<T*>(object), random);
			});
			return static_cast<Derived&>(*this);
		}
	protected:
		FieldBuilder(TypeRegistry& registry, StructInfo& info)
			: m_Registry(&registry), m_Info(&info)
		{
		}
	private:
		Derived& AddStoredField(std::string_view name, const TypeInfo* type, Scope<IFieldAccessor> accessor, std::string_view description,
			FieldMeta meta, VariantSchemaResolver resolver)
		{
			FieldInfo::Specification specification;
			specification.Name = std::string(name);
			specification.Description = std::string(description);
			specification.Type = type;
			specification.Meta = std::move(meta);
			specification.Accessor = std::move(accessor);
			specification.Resolver = resolver;
			m_Info->AddField(std::move(specification));
			return static_cast<Derived&>(*this);
		}
	private:
		TypeRegistry* m_Registry = nullptr;
		StructInfo* m_Info = nullptr;
	};

	// Registers the fields of a reflected struct (TypeRegistry::Struct<T>).
	template<typename T>
	class StructBuilder final : public FieldBuilder<StructBuilder<T>, T>
	{
	public:
		StructBuilder(TypeRegistry& registry, StructInfo& info)
			: FieldBuilder<StructBuilder<T>, T>(registry, info)
		{
		}
	};

	// Registers a component (TypeRegistry::Component<T>), as in §5.4:
	//     registry.Component<RigidBodyComponent>("RigidBody", "Simulates the entity with Jolt physics.")
	//         .Category("Physics").Version(1).Requires<TransformComponent>().Excludes<CharacterControllerComponent>()
	//         .Field("Type", &RigidBodyComponent::Type, "Static never moves; ...")
	// Components registered for a scene go through Scene's RegisterComponent<T> (Scene/ComponentRegistration.h), which
	// also installs the ECS operations (HostOps).
	template<typename T>
	class ComponentBuilder final : public FieldBuilder<ComponentBuilder<T>, T>
	{
	public:
		ComponentBuilder(TypeRegistry& registry, ComponentInfo& info)
			: FieldBuilder<ComponentBuilder<T>, T>(registry, info), m_Component(&info)
		{
		}

		// Mandatory: "Core", "Rendering", "Physics", "Audio" or "Scripting" for the built-in components.
		ComponentBuilder& Category(std::string_view category)
		{
			m_Component->SetCategory(std::string(category));
			return *this;
		}

		ComponentBuilder& Version(uint32_t version)
		{
			m_Component->SetVersion(version);
			return *this;
		}

		// Adds `flags` to the current flags (initially ComponentFlags::Default).
		ComponentBuilder& Flags(ComponentFlags flags)
		{
			m_Component->SetFlags(m_Component->GetFlags() | flags);
			return *this;
		}

		// Removes `flags` from the current flags.
		ComponentBuilder& RemoveFlags(ComponentFlags flags)
		{
			m_Component->SetFlags(m_Component->GetFlags() & ~flags);
			return *this;
		}

		template<typename Required>
		ComponentBuilder& Requires()
		{
			m_Component->AddRequires(TypeKeyOf<Required>());
			return *this;
		}

		template<typename Excluded>
		ComponentBuilder& Excludes()
		{
			m_Component->AddExcludes(TypeKeyOf<Excluded>());
			return *this;
		}

		// The migration from `fromVersion` to fromVersion + 1.
		ComponentBuilder& Migration(uint32_t fromVersion, ComponentMigration migration)
		{
			m_Component->AddMigration(fromVersion, migration);
			return *this;
		}

		ComponentBuilder& HostOps(const ComponentHostOps* hostOps)
		{
			m_Component->SetHostOps(hostOps);
			return *this;
		}
	private:
		ComponentInfo* m_Component = nullptr;
	};

	// Registers the enumerators of a reflected enum (TypeRegistry::Enum<E>). Every enumerator that may appear in data is
	// registered with its PascalCase name and a description (gate 1b counts each value).
	template<typename E>
	class EnumBuilder final
	{
	public:
		explicit EnumBuilder(EnumInfo& info)
			: m_Info(&info)
		{
		}

		EnumBuilder& Entry(E value, std::string_view name, std::string_view description)
		{
			m_Info->AddEntry(EnumEntry{ std::string(name), static_cast<int64_t>(std::to_underlying(value)), std::string(description) });
			return *this;
		}
	private:
		EnumInfo* m_Info = nullptr;
	};

}
