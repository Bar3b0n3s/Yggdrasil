#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

// The interface of a game's native gameplay module (Docs/Decisions/0030-m19-contract.md decisions 26 to 28, C-NATIVE-STATIC
// and C-NATIVE-DLL): the one engine header a module's sources in <Game>/Native/ include (decision 35's include rule), beside
// the standard library and glm. It is self-contained and header-only on purpose: the module never links the engine's
// libraries, so the editor's shared-library build of it holds no second copy of the engine's process-level state (§3 rule
// 5), and the exported Runtime links the same sources statically. Everything the module reaches in the engine goes through
// the two abstract classes below, which the engine implements: INativeRegistrar while the module registers, and
// INativeStepContext while a system runs.
//
// A module registers, from the one function its ENGINE_NATIVE_MODULE names:
//   - component types (plain data on entities, stored in scene and prefab files under "Native.<Name>", decision 27),
//   - resource types (one value per play session, hashed in the state hash's native block, decision 28),
//   - systems (functions run once per tick in the NativeStep phase, decision 28),
// with the builders below. A type is a trivially copyable, standard-layout struct with a default constructor, every member
// default-initialized, that names itself with `static constexpr std::string_view NativeTypeName = "<Name>";`. Its reflected
// fields are members of the kinds of NativeFieldKind; members that are not registered are neither saved nor hashed, and
// they are zero, like the padding, in every value the engine makes (NativeTypeDescriptor::Defaults), so a system never keeps
// state in them.
//
//     struct Spinner
//     {
//         static constexpr std::string_view NativeTypeName = "Spinner";
//         float DegreesPerSecond = 90.0f;
//         glm::vec3 Axis = glm::vec3(0.0f, 1.0f, 0.0f);
//     };
//
//     static void Spin(Engine::INativeStepContext& context)
//     {
//         for (const Engine::NativeEntity entity : context.Query<Spinner>())
//         {
//             const Spinner* spinner = context.Get<Spinner>(entity);
//             Engine::NativeTransform transform{};
//             if (spinner == nullptr || !context.GetTransform(entity, transform))
//                 continue;
//             const float radians = spinner->DegreesPerSecond * 0.017453292f * static_cast<float>(context.GetFixedDelta());
//             transform.Rotation = context.AngleAxis(radians, spinner->Axis) * transform.Rotation;
//             context.SetTransform(entity, transform);
//         }
//     }
//
//     static void Register(Engine::INativeRegistrar& registrar)
//     {
//         registrar.Add(Engine::NativeComponent<Spinner>("Turns its entity about an axis.")
//             .Field("DegreesPerSecond", &Spinner::DegreesPerSecond, "How fast it turns.", { .Unit = "deg/s" })
//             .Field("Axis", &Spinner::Axis, "The local axis it turns about."));
//         registrar.AddSystem({ .Name = "Spin", .Description = "Turns every Spinner.", .Run = &Spin });
//     }
//
//     ENGINE_NATIVE_MODULE(Register)
//
// Determinism (§1.3, §4.12): native code is simulation code. It is compiled with the first-party settings (precise floating
// point, warnings as errors) and checked by the determinism lint (decision 35), so it calls no CRT transcendental function
// and no glm function built on them: the step context's Sin to Pow and AngleAxis are DetMath's. It reads time only as
// ticks, randomness only from the session's stream, and input only from the latched step view. It keeps no mutable state of
// its own: every variable of static or thread storage duration (namespace scope, a class's static member, a function's
// static local, thread_local) is const or constexpr, which the lint checks (decision 35), because the editor keeps a
// module's library loaded across play sessions and reloads while each exported Runtime starts fresh; a session's state
// lives in its resources and components. A source includes, of the repository, only this header and the module's own .h
// files below Native/, so every file a build reads is in its source hash (decision 30).

// The export of the shared-library build's two entry functions (ENGINE_NATIVE_MODULE). Only Windows builds a module as a
// shared library (decision 34); the other spelling keeps the header portable.
#if defined(ENGINE_PLATFORM_WINDOWS)
	#define ENGINE_NATIVE_EXPORT __declspec(dllexport)
#else
	#define ENGINE_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

namespace Engine {

	// The version of this interface. The engine refuses a module built against another one (UnsupportedVersion naming both,
	// decision 26); a change to anything a module compiles against here takes the next value.
	inline constexpr uint32_t NativeApiVersion = 1;

	// An entity as native data holds it: the entity's UUID (§4.8), 0 for none. Files and automation write it like an
	// EntityRef value, 16 lowercase hex digits or null (decision 27).
	struct NativeEntity
	{
		uint64_t Id = 0;

		[[nodiscard]] constexpr bool IsValid() const { return Id != 0; }
		bool operator==(const NativeEntity&) const = default;
	};

	// The member kinds a native type may reflect, with the C++ member type of each. The JSON form of each is that of the
	// matching built-in FieldType (Architecture §5.4, §6): numbers, [x, y], [x, y, z], [x, y, z, w], quaternions as
	// [x, y, z, w], colours as linear [r, g, b(, a)], an entity as a 16-hex string or null, an enum by its entry's name.
	enum class NativeFieldKind : uint8_t
	{
		Bool,   // bool
		Int32,  // int32_t
		UInt32, // uint32_t
		Float,  // float, always finite
		Vec2,   // glm::vec2
		Vec3,   // glm::vec3
		Vec4,   // glm::vec4
		Quat,   // glm::quat, a unit quaternion
		Color3, // glm::vec3 (ColorField)
		Color4, // glm::vec4 (ColorField)
		Entity, // NativeEntity
		Enum    // an enum class whose underlying type is int32_t (EnumField), saved by entry name
	};

	// One entry of an enum field: its name in files (PascalCase), its value and its description.
	struct NativeEnumEntry
	{
		std::string_view Name{};
		int32_t Value = 0;
		std::string_view Description{};
	};

	// The optional metadata of a field (the builders' last parameter).
	struct NativeFieldOptions
	{
		// Bounds of a numeric field, of every component of a vector or colour; both finite, Min <= Max.
		std::optional<double> Min{};
		std::optional<double> Max{};
		// A unit shown with the field ("m", "deg/s").
		std::string_view Unit{};
		// The field's previous name: canonicalization carries a value saved under that name to this field (decision 27).
		std::string_view RenamedFrom{};
	};

	// One reflected field of a native type, as the builders describe it.
	struct NativeFieldDescriptor
	{
		std::string_view Name{};
		std::string_view Description{};
		NativeFieldKind Kind = NativeFieldKind::Float;
		uint32_t Offset = 0; // of the member in the type, in bytes
		NativeFieldOptions Options{};
		std::span<const NativeEnumEntry> EnumEntries{}; // Enum: every entry, in the order the type declares them
	};

	// What a native type is.
	enum class NativeTypeRole : uint8_t
	{
		Component, // data on entities, saved in scenes and prefabs as "Native.<Name>"
		Resource   // one value per play session, hashed in the native block, never saved
	};

	// One native type, as the builders describe it. The views stay valid only during the INativeRegistrar call that receives
	// the descriptor: the engine copies everything it keeps.
	struct NativeTypeDescriptor
	{
		NativeTypeRole Role = NativeTypeRole::Component;
		std::string_view Name{}; // T::NativeTypeName
		std::string_view Description{};
		uint32_t Size = 0;      // sizeof(T)
		uint32_t Alignment = 0; // alignof(T)
		// Size bytes, the value every new component or resource starts from: each registered field's bytes as a T{} holds
		// them, and zero everywhere else (the padding and the members that are not registered), so the defaults are the
		// same bytes in every build and process, whatever the stack held (an aggregate's padding is indeterminate).
		const void* Defaults = nullptr;
		std::span<const NativeFieldDescriptor> Fields{}; // in registration order, the order files write them
	};

	class INativeStepContext;

	// A system: run once per tick in the NativeStep phase, in registration order (decision 28).
	using NativeSystemFunction = void (*)(INativeStepContext& context);

	struct NativeSystemDescriptor
	{
		std::string_view Name{}; // PascalCase, unique among the module's systems
		std::string_view Description{};
		NativeSystemFunction Run = nullptr;
	};

	// Receives a module's registration (decision 26). The engine implements it; the first rule a registration breaks makes
	// the whole module refused (Validation naming the type, field or system), so a module is loaded entirely or not at all.
	class INativeRegistrar
	{
	public:
		virtual ~INativeRegistrar() = default;

		virtual void AddType(const NativeTypeDescriptor& type) = 0;
		virtual void AddSystem(const NativeSystemDescriptor& system) = 0;

		// A builder's type (NativeComponent, NativeResource).
		template<typename Builder>
		void Add(const Builder& builder)
		{
			AddType(builder.GetDescriptor());
		}
	};

	// A module's registration function, which ENGINE_NATIVE_MODULE exports or links.
	using NativeModuleRegisterFunction = void (*)(INativeRegistrar& registrar);

	// The module as the engine finds it: the interface version it was compiled against and its registration function.
	struct NativeModuleEntryPoint
	{
		uint32_t ApiVersion = 0;
		NativeModuleRegisterFunction Register = nullptr;
	};

	// The module statically linked into this executable (decision 30): the exported Runtime of a game with Native/ links the
	// module, whose ENGINE_NATIVE_MODULE defines this function; every other Runtime links Runtime/Source/Runtime/
	// NoNativeModule.cpp, whose definition returns an entry without a Register function. Only the Runtime calls it.
	[[nodiscard]] NativeModuleEntryPoint GetLinkedNativeModule();

	// A local transform as native code reads and writes it: TransformComponent's members (§5.2), relative to the parent.
	struct NativeTransform
	{
		glm::vec3 Translation = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity (glm's constructor order is w, x, y, z)
		glm::vec3 Scale = glm::vec3(1.0f);
	};

	enum class NativeLogLevel : uint8_t
	{
		Info,
		Warning,
		Error
	};

	// What a system sees of its play session while it runs (decision 28). The engine implements it; a system receives it by
	// reference and keeps nothing of it beyond its call. Every call is main-thread and deterministic. Pointers to component
	// and resource data stay valid until the system returns, whatever else it calls; writes through them reach the scene's
	// components when the NativeStep phase ends, before the transform update that precedes physics.
	class INativeStepContext
	{
	public:
		virtual ~INativeStepContext() = default;

		// The tick being stepped and the fixed delta in seconds (1 / Simulation.FixedHz).
		[[nodiscard]] virtual uint64_t GetTick() const = 0;
		[[nodiscard]] virtual double GetFixedDelta() const = 0;

		// The effectively active entities that hold the component "Native.<component>", in canonical order (§5.1), as they
		// were when the call was made; empty for a name the module does not register. Valid until the system returns.
		[[nodiscard]] virtual std::span<const NativeEntity> Query(std::string_view component) = 0;
		// The component's data on `entity`, or null when the entity does not exist, does not hold it, or the name or size
		// does not match a registered component.
		[[nodiscard]] virtual void* FindComponent(NativeEntity entity, std::string_view component, uint32_t size) = 0;
		// Adds the component with its defaults (or returns the one the entity holds); null as for FindComponent.
		[[nodiscard]] virtual void* AddComponent(NativeEntity entity, std::string_view component, uint32_t size) = 0;
		// Removes the component when the entity holds it; false otherwise.
		virtual bool RemoveComponent(NativeEntity entity, std::string_view component) = 0;
		// The session's value of the resource, or null for a name or size that does not match a registered resource.
		[[nodiscard]] virtual void* FindResource(std::string_view resource, uint32_t size) = 0;

		// Entities. An entity pending destruction is not valid (§5.7).
		[[nodiscard]] virtual bool IsValid(NativeEntity entity) const = 0;
		// The effective active state (§5.2); false for an invalid entity.
		[[nodiscard]] virtual bool IsActive(NativeEntity entity) const = 0;
		// Sets the entity's own active flag; false for an invalid entity.
		virtual bool SetActive(NativeEntity entity, bool active) = 0;
		// The name, empty for an invalid entity; valid until the system returns.
		[[nodiscard]] virtual std::string_view GetName(NativeEntity entity) const = 0;
		[[nodiscard]] virtual NativeEntity GetParent(NativeEntity entity) const = 0;
		// The entity at an entity path ("/Board/Cell[3]", Scene::FindEntityByPath's grammar); none when no entity matches.
		[[nodiscard]] virtual NativeEntity FindEntity(std::string_view path) const = 0;
		// A new entity, last child of `parent` (none: a root), with a Transform, as a script's Scene.CreateEntity (§5.7):
		// none, with an error logged, when the session's entity limit is reached.
		[[nodiscard]] virtual NativeEntity CreateEntity(std::string_view name, NativeEntity parent) = 0;
		// Destroys the entity and its descendants at the tick's destroy flush (§5.7 step 8); false for an invalid entity.
		virtual bool DestroyEntity(NativeEntity entity) = 0;
		// The entity's local transform; false for an invalid entity.
		[[nodiscard]] virtual bool GetTransform(NativeEntity entity, NativeTransform& transform) const = 0;
		// Writes the local transform as a script's Transform write in OnFixedUpdate does (it interpolates). False, writing
		// nothing, for an invalid entity, a non-finite value, a rotation that is not unit length or a Scale component below
		// MinTransformScaleMagnitude in magnitude.
		virtual bool SetTransform(NativeEntity entity, const NativeTransform& transform) = 0;

		// The latched step view of the project's input actions (Input.Actions, §6.1), as scripts' Input.IsActionDown,
		// WasActionPressed, WasActionReleased and GetActionAxis read it in OnFixedUpdate. An action the project does not define
		// reads as up and 0, and logs one error per session and name.
		[[nodiscard]] virtual bool IsActionDown(std::string_view action) const = 0;
		[[nodiscard]] virtual bool WasActionPressed(std::string_view action) const = 0;
		[[nodiscard]] virtual bool WasActionReleased(std::string_view action) const = 0;
		[[nodiscard]] virtual float GetActionAxis(std::string_view action) const = 0;

		// The session's seeded Random stream (§4.12), shared with scripts in step order.
		[[nodiscard]] virtual uint32_t RandomU32() = 0;
		// [0, 1).
		[[nodiscard]] virtual float RandomFloat() = 0;

		// DetMath (§4.12): the deterministic transcendental functions, in radians.
		[[nodiscard]] virtual float Sin(float x) const = 0;
		[[nodiscard]] virtual float Cos(float x) const = 0;
		[[nodiscard]] virtual float Tan(float x) const = 0;
		[[nodiscard]] virtual float ASin(float x) const = 0;
		[[nodiscard]] virtual float ACos(float x) const = 0;
		[[nodiscard]] virtual float ATan(float x) const = 0;
		[[nodiscard]] virtual float ATan2(float y, float x) const = 0;
		[[nodiscard]] virtual float Exp(float x) const = 0;
		[[nodiscard]] virtual float Log(float x) const = 0;
		[[nodiscard]] virtual float Pow(float base, float exponent) const = 0;
		// The rotation of `radians` about `axis` (normalized first; the identity for a zero axis), through DetMath.
		[[nodiscard]] virtual glm::quat AngleAxis(float radians, const glm::vec3& axis) const = 0;

		// A line in the engine's log, attributed to the running system ("native:<System>").
		virtual void Write(NativeLogLevel level, std::string_view message) = 0;

		// The typed forms, by T::NativeTypeName and sizeof(T).
		template<typename T>
		[[nodiscard]] std::span<const NativeEntity> Query()
		{
			return Query(T::NativeTypeName);
		}

		template<typename T>
		[[nodiscard]] T* Get(NativeEntity entity)
		{
			return static_cast<T*>(FindComponent(entity, T::NativeTypeName, static_cast<uint32_t>(sizeof(T))));
		}

		template<typename T>
		[[nodiscard]] T* Add(NativeEntity entity)
		{
			return static_cast<T*>(AddComponent(entity, T::NativeTypeName, static_cast<uint32_t>(sizeof(T))));
		}

		template<typename T>
		bool Remove(NativeEntity entity)
		{
			return RemoveComponent(entity, T::NativeTypeName);
		}

		template<typename T>
		[[nodiscard]] T* GetResource()
		{
			return static_cast<T*>(FindResource(T::NativeTypeName, static_cast<uint32_t>(sizeof(T))));
		}
	};

	namespace Detail {

		// The NativeFieldKind of member type M for Field (ColorField and EnumField name theirs).
		template<typename M>
		[[nodiscard]] consteval NativeFieldKind GetNativeFieldKind()
		{
			if constexpr (std::is_same_v<M, bool>)
				return NativeFieldKind::Bool;
			else if constexpr (std::is_same_v<M, int32_t>)
				return NativeFieldKind::Int32;
			else if constexpr (std::is_same_v<M, uint32_t>)
				return NativeFieldKind::UInt32;
			else if constexpr (std::is_same_v<M, float>)
				return NativeFieldKind::Float;
			else if constexpr (std::is_same_v<M, glm::vec2>)
				return NativeFieldKind::Vec2;
			else if constexpr (std::is_same_v<M, glm::vec3>)
				return NativeFieldKind::Vec3;
			else if constexpr (std::is_same_v<M, glm::vec4>)
				return NativeFieldKind::Vec4;
			else if constexpr (std::is_same_v<M, glm::quat>)
				return NativeFieldKind::Quat;
			else
			{
				static_assert(std::is_same_v<M, NativeEntity>, "a native field is bool, int32_t, uint32_t, float, glm::vec2/vec3/vec4, glm::quat or "
															   "NativeEntity (colours use ColorField, enums EnumField)");
				return NativeFieldKind::Entity;
			}
		}

	}

	// Describes native type T for INativeRegistrar::Add (NativeComponent, NativeResource): its name (T::NativeTypeName), size,
	// alignment, defaults (each registered field as a T{} holds it, zero elsewhere: NativeTypeDescriptor::Defaults) and
	// reflected fields, each located by its member pointer. Fields are registered in the order files write them. The builder
	// owns the descriptors it hands out, so it outlives the Add call that receives them (a temporary in the Add expression
	// does).
	template<typename T>
	class NativeTypeBuilder
	{
	public:
		static_assert(std::is_class_v<T> && std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T> && std::is_default_constructible_v<T>,
			"a native type is a trivially copyable, standard-layout struct with a default constructor");
		static_assert(std::is_convertible_v<decltype(T::NativeTypeName), std::string_view>,
			"a native type names itself: static constexpr std::string_view NativeTypeName = \"<Name>\";");

		NativeTypeBuilder(NativeTypeRole role, std::string_view description)
			: m_Role(role), m_Description(description)
		{
		}

		// A field of member type M (Detail::GetNativeFieldKind).
		template<typename M>
		NativeTypeBuilder& Field(std::string_view name, M T::* member, std::string_view description, const NativeFieldOptions& options = {})
		{
			return AddField(name, Detail::GetNativeFieldKind<M>(), member, description, options, {});
		}

		// A glm::vec3 or glm::vec4 member reflected as a linear colour (Color3, Color4).
		template<typename M>
		NativeTypeBuilder& ColorField(std::string_view name, M T::* member, std::string_view description, const NativeFieldOptions& options = {})
		{
			static_assert(std::is_same_v<M, glm::vec3> || std::is_same_v<M, glm::vec4>, "ColorField needs a glm::vec3 or glm::vec4 member");
			const NativeFieldKind kind = std::is_same_v<M, glm::vec3> ? NativeFieldKind::Color3 : NativeFieldKind::Color4;
			return AddField(name, kind, member, description, options, {});
		}

		// An enum class member whose underlying type is int32_t, saved by entry name. `entries` lists every value the
		// member may hold and must outlive the builder (a static constexpr array does).
		template<typename E>
		NativeTypeBuilder& EnumField(std::string_view name, E T::* member, std::string_view description, std::span<const NativeEnumEntry> entries)
		{
			static_assert(std::is_enum_v<E> && std::is_same_v<std::underlying_type_t<E>, int32_t>, "EnumField needs an enum class over int32_t");
			return AddField(name, NativeFieldKind::Enum, member, description, {}, entries);
		}

		[[nodiscard]] NativeTypeDescriptor GetDescriptor() const
		{
			return NativeTypeDescriptor{ .Role = m_Role,
				.Name = std::string_view(T::NativeTypeName),
				.Description = m_Description,
				.Size = static_cast<uint32_t>(sizeof(T)),
				.Alignment = static_cast<uint32_t>(alignof(T)),
				.Defaults = m_Defaults.data(),
				.Fields = std::span<const NativeFieldDescriptor>(m_Fields) };
		}
	private:
		// Locates `member` in a T{} and copies its bytes, the field's default, into m_Defaults, whose other bytes stay zero.
		template<typename M>
		NativeTypeBuilder& AddField(std::string_view name, NativeFieldKind kind, M T::* member, std::string_view description,
			const NativeFieldOptions& options, std::span<const NativeEnumEntry> entries)
		{
			const T object{};
			const auto* base = reinterpret_cast<const std::byte*>(std::addressof(object));
			const auto* field = reinterpret_cast<const std::byte*>(std::addressof(object.*member));
			const auto offset = static_cast<uint32_t>(field - base);
			std::memcpy(m_Defaults.data() + offset, field, sizeof(M));
			m_Fields.push_back(NativeFieldDescriptor{ .Name = name, .Description = description, .Kind = kind, .Offset = offset, .Options = options, .EnumEntries = entries });
			return *this;
		}
	private:
		NativeTypeRole m_Role = NativeTypeRole::Component;
		std::string_view m_Description{};
		std::array<std::byte, sizeof(T)> m_Defaults{}; // zero but for each registered field's default
		std::vector<NativeFieldDescriptor> m_Fields{};
	};

	// A component type: data on entities, "Native.<T::NativeTypeName>" in files and automation.
	template<typename T>
	[[nodiscard]] NativeTypeBuilder<T> NativeComponent(std::string_view description)
	{
		return NativeTypeBuilder<T>(NativeTypeRole::Component, description);
	}

	// A resource type: one value per play session.
	template<typename T>
	[[nodiscard]] NativeTypeBuilder<T> NativeResource(std::string_view description)
	{
		return NativeTypeBuilder<T>(NativeTypeRole::Resource, description);
	}

}

// Names the module's registration function, once, in one of its source files (decision 26). The editor's shared-library build
// (Scripts/BuildNative.py --kind module, which defines ENGINE_NATIVE_SHARED_LIBRARY) exports EngineNativeModuleApiVersion and
// EngineNativeModuleRegister, which the editor looks up after loading the library; the exported Runtime's build (--kind
// runtime) defines GetLinkedNativeModule instead.
#if defined(ENGINE_NATIVE_SHARED_LIBRARY)
	#define ENGINE_NATIVE_MODULE(registerFunction)                                                                                         \
		extern "C" ENGINE_NATIVE_EXPORT uint32_t EngineNativeModuleApiVersion()                                                            \
		{                                                                                                                                  \
			return ::Engine::NativeApiVersion;                                                                                             \
		}                                                                                                                                  \
		extern "C" ENGINE_NATIVE_EXPORT void EngineNativeModuleRegister(::Engine::INativeRegistrar& registrar)                            \
		{                                                                                                                                  \
			registerFunction(registrar);                                                                                                   \
		}
#else
	#define ENGINE_NATIVE_MODULE(registerFunction)                                                                                         \
		::Engine::NativeModuleEntryPoint Engine::GetLinkedNativeModule()                                                                    \
		{                                                                                                                                  \
			return ::Engine::NativeModuleEntryPoint{ .ApiVersion = ::Engine::NativeApiVersion, .Register = &(registerFunction) };          \
		}
#endif
