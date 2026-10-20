#include "EnginePCH.h"
#include "Engine/Session/NativeWorld.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Native/NativeModule.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Engine/Session/PlayInput.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <exception>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		// The log lines the module's systems may write in one step (INativeStepContext::Write and the context's own errors);
		// the rest are counted and reported once, as scripts' lines are per frame (ScriptLimits.h).
		inline constexpr uint32_t MaxNativeLogLinesPerStep = MaxScriptLogEntriesPerFrame;
		// The largest exception message or log line kept.
		inline constexpr size_t MaxNativeMessageBytes = MaxScriptLogMessageBytes;

		// `text` with every byte that starts no well-formed UTF-8 sequence replaced by '?', cut at `maximumBytes` without
		// splitting a code point: native code may hand the log anything.
		static std::string SanitizeUtf8(std::string_view text, size_t maximumBytes)
		{
			std::string sanitized;
			size_t offset = 0;
			while (offset < text.size() && sanitized.size() < maximumBytes)
			{
				const size_t length = GetUtf8SequenceLength(text, offset);
				if (length == 0)
				{
					sanitized += '?';
					++offset;
					continue;
				}
				if (sanitized.size() + length > maximumBytes)
					break;
				sanitized.append(text.substr(offset, length));
				offset += length;
			}
			return sanitized;
		}

		// A native value's bytes at a fixed address for its whole life, aligned for every native type
		// (MaxNativeTypeAlignment): a system casts the pointer it gets to its type.
		class NativeValueStorage
		{
		public:
			NativeValueStorage() = default;
			explicit NativeValueStorage(std::span<const std::byte> initial)
				: m_Blocks((initial.size() + MaxNativeTypeAlignment - 1) / MaxNativeTypeAlignment), m_Size(initial.size())
			{
				std::ranges::copy(initial, Get().begin());
			}

			// The blocks' storage as bytes (CodeStyle §8: a byte-level view of a buffer).
			[[nodiscard]] std::span<std::byte> Get() { return { reinterpret_cast<std::byte*>(m_Blocks.data()), m_Size }; }
			[[nodiscard]] std::span<const std::byte> Get() const { return { reinterpret_cast<const std::byte*>(m_Blocks.data()), m_Size }; }
		private:
			struct alignas(MaxNativeTypeAlignment) Block
			{
				std::array<std::byte, MaxNativeTypeAlignment> Bytes{};
			};
		private:
			std::vector<Block> m_Blocks{};
			size_t m_Size = 0;
		};

		static bool AreSameBytes(std::span<const std::byte> left, std::span<const std::byte> right)
		{
			return std::ranges::equal(left, right);
		}

		// One native component a step touched (NativeWorld.h): its data from the first time a system asked for it, at a fixed
		// address until the step ends.
		struct CachedComponent
		{
			explicit CachedComponent(const NativeTypeInfo& type, std::span<const std::byte> initial)
				: Type(&type), Value(initial), Read(initial.begin(), initial.end()), Validated(initial.begin(), initial.end())
			{
			}

			const NativeTypeInfo* Type = nullptr; // into the world's NativeModuleTypes, which outlives the step
			NativeValueStorage Value{};
			std::vector<std::byte> Read{};      // what the step started from: the value read from the scene, or the defaults
			std::vector<std::byte> Validated{}; // the value as the last system left it, valid
			bool InScene = false;               // the entity held it in the scene when the step first touched it
			bool Present = true;                // the entity holds it now (false once a system removed it)
			bool Migrated = false;              // its read carried notes: it is written back canonical
		};

		// What stops the session (NativeWorld.h): the first invalid value or escaped exception.
		struct NativeFault
		{
			std::string System{};
			std::string Message{};
			UUID Entity{};
			std::string EntityName{};
		};

		// The world's state: the module, the session's resources and what lives for one step.
		struct NativeWorldData
		{
			Ref<const NativeModuleTypes> Types{};
			std::vector<NativeValueStorage> Resources{};
			std::vector<std::vector<std::byte>> ValidatedResources{};
			// Logged once per session: each (component, field) a read migrated, each action the project does not define.
			std::set<std::pair<std::string, std::string>, std::less<>> LoggedNotes{};
			std::set<std::string, std::less<>> LoggedActions{};
			// The step's components by (entity, component index): ordered, so the write-back's order is canonical.
			std::map<std::pair<UUID, uint32_t>, Scope<CachedComponent>> Components{};
			uint32_t LogLines = 0;
			uint32_t DroppedLogLines = 0;
		};

		static const NativeTypeInfo* FindComponentType(const NativeModuleTypes& types, std::string_view name)
		{
			const auto components = types.GetComponents();
			const auto found = std::ranges::find(components, name, &NativeTypeInfo::Name);
			return found == components.end() ? nullptr : &*found;
		}

		static const UnknownComponentData* FindStored(ConstEntity entity, std::string_view name)
		{
			const UnknownComponentsComponent* preserved = entity.TryGetComponent<UnknownComponentsComponent>();
			if (preserved == nullptr)
				return nullptr;
			const auto found = std::ranges::find(preserved->Components, name, &UnknownComponentData::Name);
			return found == preserved->Components.end() ? nullptr : &*found;
		}

		// Writes `json` as the entity's component `name` (a runtime system's direct write): in place, or as a new preserved
		// component placed before the first one that sorts after it, so a canonical entity stays canonical (decision 27).
		static void WriteStored(Entity entity, const std::string& name, Json json)
		{
			if (!entity.HasComponent<UnknownComponentsComponent>())
				entity.AddComponent<UnknownComponentsComponent>();
			std::vector<UnknownComponentData>& components = entity.GetComponent<UnknownComponentsComponent>().Components;
			const auto found = std::ranges::find(components, name, &UnknownComponentData::Name);
			if (found != components.end())
			{
				found->Data = VariantValue(std::move(json));
				found->Version = NativeComponentVersion;
				return;
			}
			const auto position = std::ranges::find_if(components, [&name](const UnknownComponentData& data)
			{
				return !IsNativeComponentName(data.Name) || name < data.Name;
			});
			components.insert(position, UnknownComponentData{ .Name = name, .Version = NativeComponentVersion, .Data = VariantValue(std::move(json)) });
		}

		static void EraseStored(Entity entity, std::string_view name)
		{
			UnknownComponentsComponent* preserved = entity.TryGetComponent<UnknownComponentsComponent>();
			if (preserved == nullptr)
				return;
			std::erase_if(preserved->Components, [name](const UnknownComponentData& data)
			{
				return data.Name == name;
			});
			if (preserved->Components.empty())
				entity.RemoveComponent<UnknownComponentsComponent>();
		}

		static std::string DescribeEntity(const Scene& scene, UUID id)
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			return entity.IsValid() ? std::format("'{}' ({})", scene.GetEntityPath(entity), id) : std::format("{}", id);
		}

		static std::string GetEntityName(const Scene& scene, UUID id)
		{
			const ConstEntity entity = scene.FindEntityByID(id);
			return entity.IsValid() ? entity.GetName() : std::string();
		}

		// The first rule `value` breaks, or nothing: a system writes the bytes, so they are checked as a read would check
		// their canonical JSON (NativeValueFromJson), where a value no field can hold (an enum value without an entry) is a
		// note.
		static std::optional<std::string> FindInvalidValue(const NativeTypeInfo& type, std::span<const std::byte> value)
		{
			std::vector<std::byte> scratch(type.Size);
			const Result<NativeValueReadReport> read = NativeValueFromJson(type, NativeValueToJson(type, value), "", scratch);
			if (!read.has_value())
			{
				const std::vector<ErrorIssue>& issues = read.error().GetIssues();
				return issues.empty() ? read.error().GetMessageText() : std::format("{}: {}", issues.front().JsonPointer, issues.front().Message);
			}
			if (!read->Notes.empty())
				return read->Notes.front().Message;
			return std::nullopt;
		}

		// Writes a line attributed to `system`, within the step's line budget.
		static void WriteLine(NativeWorldData& world, std::string_view system, NativeLogLevel level, std::string_view message)
		{
			if (world.LogLines >= MaxNativeLogLinesPerStep)
			{
				++world.DroppedLogLines;
				return;
			}
			++world.LogLines;
			const std::string text = SanitizeUtf8(message, MaxNativeMessageBytes);
			switch (level)
			{
				case NativeLogLevel::Info:
					ENGINE_CORE_INFO("native:{}: {}", system, text);
					return;
				case NativeLogLevel::Warning:
					ENGINE_CORE_WARN("native:{}: {}", system, text);
					return;
				case NativeLogLevel::Error:
					ENGINE_CORE_ERROR("native:{}: {}", system, text);
					return;
			}
			ENGINE_CORE_ERROR("native:{} (unknown log level {}): {}", system, static_cast<uint32_t>(level), text);
		}

		// INativeStepContext over one system's run (NativeApi.h, NativeWorld.h). It lives for the system's call only; what it
		// hands out (spans, names, data pointers) stays valid until then.
		class NativeStepContext final : public INativeStepContext
		{
		public:
			NativeStepContext(NativeWorldData& world, PlaySession& session, const SimStep& step, const NativeSystemInfo& system)
				: m_World(world), m_Session(session), m_Step(step), m_System(system)
			{
			}

			NativeStepContext(const NativeStepContext&) = delete;
			NativeStepContext& operator=(const NativeStepContext&) = delete;

			[[nodiscard]] std::optional<NativeFault>& GetFault() { return m_Fault; }

			[[nodiscard]] uint64_t GetTick() const override { return m_Step.Tick; }
			[[nodiscard]] double GetFixedDelta() const override { return m_Step.FixedDelta; }

			[[nodiscard]] std::span<const NativeEntity> Query(std::string_view component) override
			{
				const NativeTypeInfo* type = FindComponentType(*m_World.Types, component);
				if (type == nullptr)
					return {};
				std::vector<NativeEntity> entities;
				Scene& scene = m_Session.GetScene();
				for (const UUID id : scene.GetCanonicalOrder())
				{
					const Entity entity = scene.FindEntityByID(id);
					if (entity.IsValid() && entity.IsActive() && Holds(entity, *type))
						entities.push_back(NativeEntity{ id.GetValue() });
				}
				// Moving the vector into the list keeps its buffer, so every span handed out stays valid.
				m_Queries.push_back(std::move(entities));
				return m_Queries.back();
			}

			[[nodiscard]] void* FindComponent(NativeEntity entity, std::string_view component, uint32_t size) override
			{
				return Touch(entity, component, size, false);
			}

			[[nodiscard]] void* AddComponent(NativeEntity entity, std::string_view component, uint32_t size) override
			{
				return Touch(entity, component, size, true);
			}

			bool RemoveComponent(NativeEntity native, std::string_view component) override
			{
				const NativeTypeInfo* type = FindComponentType(*m_World.Types, component);
				const Entity entity = ResolveEntity(native);
				if (type == nullptr || !entity.IsValid())
					return false;
				const std::pair<UUID, uint32_t> key{ entity.GetUUID(), type->Index };
				if (const auto cached = m_World.Components.find(key); cached != m_World.Components.end())
				{
					const bool held = cached->second->Present;
					cached->second->Present = false;
					return held;
				}
				if (FindStored(entity, type->RegistryName) == nullptr)
					return false;
				auto removed = CreateScope<CachedComponent>(*type, type->Defaults);
				removed->InScene = true;
				removed->Present = false;
				m_World.Components.emplace(key, std::move(removed));
				return true;
			}

			[[nodiscard]] void* FindResource(std::string_view resource, uint32_t size) override
			{
				const NativeTypeInfo* type = m_World.Types->FindResource(resource);
				if (type == nullptr || type->Size != size)
					return nullptr;
				return m_World.Resources[type->Index].Get().data();
			}

			[[nodiscard]] bool IsValid(NativeEntity entity) const override { return ResolveEntity(entity).IsValid(); }

			[[nodiscard]] bool IsActive(NativeEntity native) const override
			{
				const Entity entity = ResolveEntity(native);
				return entity.IsValid() && entity.IsActive();
			}

			bool SetActive(NativeEntity native, bool active) override
			{
				const Entity entity = ResolveEntity(native);
				if (!entity.IsValid())
					return false;
				// Script instances see the change at the next synchronization of the step (its destroy flush, §5.7 step 8).
				if (entity.IsActiveSelf() != active)
					entity.SetActive(active);
				return true;
			}

			[[nodiscard]] std::string_view GetName(NativeEntity native) const override
			{
				const Entity entity = ResolveEntity(native);
				if (!entity.IsValid())
					return {};
				// A copy: a structural change later in the call may move the entity's name.
				return m_Names.emplace_back(entity.GetName());
			}

			[[nodiscard]] NativeEntity GetParent(NativeEntity native) const override
			{
				const Entity entity = ResolveEntity(native);
				if (!entity.IsValid())
					return {};
				const Entity parent = entity.GetParent();
				return parent.IsValid() ? NativeEntity{ parent.GetUUID().GetValue() } : NativeEntity{};
			}

			[[nodiscard]] NativeEntity FindEntity(std::string_view path) const override
			{
				const Entity entity = m_Session.GetScene().FindEntityByPath(path);
				return entity.IsValid() ? NativeEntity{ entity.GetUUID().GetValue() } : NativeEntity{};
			}

			[[nodiscard]] NativeEntity CreateEntity(std::string_view name, NativeEntity parent) override
			{
				if (name.size() > MaxScriptEntityNameBytes || !IsValidUtf8(name) || name.find('\0') != std::string_view::npos)
				{
					WriteLine(m_World, m_System.Name, NativeLogLevel::Error,
						std::format("CreateEntity: a name is UTF-8 without NUL bytes of at most {} bytes; no entity was created", MaxScriptEntityNameBytes));
					return {};
				}
				const Entity parentEntity = ResolveEntity(parent);
				if (parent.IsValid() && !parentEntity.IsValid())
				{
					WriteLine(m_World, m_System.Name, NativeLogLevel::Error, std::format("CreateEntity: no parent entity {}; no entity was created", UUID(parent.Id)));
					return {};
				}
				const Result<Entity> created = parentEntity.IsValid() ? m_Session.CreateEntity(name, parentEntity) : m_Session.CreateEntity(name);
				if (!created.has_value())
				{
					WriteLine(m_World, m_System.Name, NativeLogLevel::Error, std::format("CreateEntity: {}", created.error().ToString()));
					return {};
				}
				return NativeEntity{ created->GetUUID().GetValue() };
			}

			bool DestroyEntity(NativeEntity native) override
			{
				const Entity entity = ResolveEntity(native);
				if (!entity.IsValid())
					return false;
				m_Session.GetScene().DestroyEntity(entity);
				return true;
			}

			[[nodiscard]] bool GetTransform(NativeEntity native, NativeTransform& transform) const override
			{
				const Entity entity = ResolveEntity(native);
				if (!entity.IsValid())
					return false;
				const TransformComponent& local = entity.GetComponent<TransformComponent>();
				transform = NativeTransform{ .Translation = local.Translation, .Rotation = local.Rotation, .Scale = local.Scale };
				return true;
			}

			bool SetTransform(NativeEntity native, const NativeTransform& transform) override
			{
				const Entity entity = ResolveEntity(native);
				if (!entity.IsValid() || !IsWritable(transform))
					return false;
				// As a script's Transform write in OnFixedUpdate: through Patch, so the transform update before physics
				// propagates it and physics teleports a body whose pose it changed (§5.7 step 5).
				entity.Patch<TransformComponent>([&transform](TransformComponent& local)
				{
					local.Translation = transform.Translation;
					local.Rotation = transform.Rotation;
					local.Scale = transform.Scale;
				});
				return true;
			}

			[[nodiscard]] bool IsActionDown(std::string_view action) const override
			{
				const std::optional<uint32_t> index = FindAction(action);
				return index.has_value() && m_Session.GetInput().IsActionDown(InputPhase::Step, *index);
			}

			[[nodiscard]] bool WasActionPressed(std::string_view action) const override
			{
				const std::optional<uint32_t> index = FindAction(action);
				return index.has_value() && m_Session.GetInput().WasActionPressed(InputPhase::Step, *index);
			}

			[[nodiscard]] bool WasActionReleased(std::string_view action) const override
			{
				const std::optional<uint32_t> index = FindAction(action);
				return index.has_value() && m_Session.GetInput().WasActionReleased(InputPhase::Step, *index);
			}

			[[nodiscard]] float GetActionAxis(std::string_view action) const override
			{
				const std::optional<uint32_t> index = FindAction(action);
				return index.has_value() ? m_Session.GetInput().GetActionAxis(InputPhase::Step, *index) : 0.0f;
			}

			[[nodiscard]] uint32_t RandomU32() override { return m_Session.GetRandom().NextU32(); }
			[[nodiscard]] float RandomFloat() override { return m_Session.GetRandom().NextFloat(); }

			[[nodiscard]] float Sin(float x) const override { return DetMath::Sin(x); }
			[[nodiscard]] float Cos(float x) const override { return DetMath::Cos(x); }
			[[nodiscard]] float Tan(float x) const override { return DetMath::Tan(x); }
			[[nodiscard]] float ASin(float x) const override { return DetMath::ASin(x); }
			[[nodiscard]] float ACos(float x) const override { return DetMath::ACos(x); }
			[[nodiscard]] float ATan(float x) const override { return DetMath::ATan(x); }
			[[nodiscard]] float ATan2(float y, float x) const override { return DetMath::ATan2(y, x); }
			[[nodiscard]] float Exp(float x) const override { return DetMath::Exp(x); }
			[[nodiscard]] float Log(float x) const override { return DetMath::Log(x); }
			[[nodiscard]] float Pow(float base, float exponent) const override { return DetMath::Pow(base, exponent); }

			[[nodiscard]] glm::quat AngleAxis(float radians, const glm::vec3& axis) const override
			{
				// As the scripts' Quat.AngleAxis: in double precision through DetMath, then rounded to float.
				const glm::dvec3 wide(axis);
				const double length = std::sqrt(glm::dot(wide, wide));
				if (!(length > 0.0) || !std::isfinite(length))
					return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
				const auto half = DetMath::SinCos(static_cast<double>(radians) * 0.5);
				const glm::dvec3 imaginary = wide * (half.Sin / length);
				return glm::quat(static_cast<float>(half.Cos), static_cast<float>(imaginary.x), static_cast<float>(imaginary.y), static_cast<float>(imaginary.z));
			}

			void Write(NativeLogLevel level, std::string_view message) override
			{
				WriteLine(m_World, m_System.Name, level, message);
			}
		private:
			[[nodiscard]] Entity ResolveEntity(NativeEntity entity) const
			{
				return m_Session.GetScene().FindEntityByID(UUID(entity.Id));
			}

			[[nodiscard]] std::optional<uint32_t> FindAction(std::string_view action) const
			{
				const std::optional<uint32_t> index = m_Session.GetInput().GetActions().FindAction(action);
				if (!index.has_value() && !m_World.LoggedActions.contains(action))
				{
					m_World.LoggedActions.emplace(action);
					WriteLine(m_World, m_System.Name, NativeLogLevel::Error,
						std::format("the project defines no input action '{}'; it reads as up and 0 (Input.Actions in the project settings)", SanitizeUtf8(action, 256)));
				}
				return index;
			}

			// A local transform a script's Transform write would accept: finite, a unit rotation, no Scale component below
			// MinTransformScaleMagnitude in magnitude.
			[[nodiscard]] static bool IsWritable(const NativeTransform& transform)
			{
				const std::array<float, 10> values = { transform.Translation.x, transform.Translation.y, transform.Translation.z, transform.Rotation.x,
					transform.Rotation.y, transform.Rotation.z, transform.Rotation.w, transform.Scale.x, transform.Scale.y, transform.Scale.z };
				if (!std::ranges::all_of(values, [](float value)
				{
					return std::isfinite(value);
				}))
					return false;
				double lengthSquared = 0.0;
				for (const float component : { transform.Rotation.x, transform.Rotation.y, transform.Rotation.z, transform.Rotation.w })
					lengthSquared += static_cast<double>(component) * static_cast<double>(component);
				if (!(std::fabs(std::sqrt(lengthSquared) - 1.0) <= UnitQuaternionTolerance))
					return false;
				return std::fabs(transform.Scale.x) >= MinTransformScaleMagnitude && std::fabs(transform.Scale.y) >= MinTransformScaleMagnitude
					&& std::fabs(transform.Scale.z) >= MinTransformScaleMagnitude;
			}

			// The entity holds the component now: as the step left it, else as the scene holds it.
			[[nodiscard]] bool Holds(const Entity& entity, const NativeTypeInfo& type) const
			{
				if (const auto cached = m_World.Components.find({ entity.GetUUID(), type.Index }); cached != m_World.Components.end())
					return cached->second->Present;
				return FindStored(entity, type.RegistryName) != nullptr;
			}

			// The component's data for FindComponent (`create` false) and AddComponent (`create` true).
			[[nodiscard]] void* Touch(NativeEntity native, std::string_view component, uint32_t size, bool create)
			{
				const NativeTypeInfo* type = FindComponentType(*m_World.Types, component);
				const Entity entity = ResolveEntity(native);
				if (type == nullptr || type->Size != size || !entity.IsValid())
					return nullptr;
				const std::pair<UUID, uint32_t> key{ entity.GetUUID(), type->Index };
				if (const auto cached = m_World.Components.find(key); cached != m_World.Components.end())
				{
					CachedComponent& data = *cached->second;
					if (!data.Present)
					{
						if (!create)
							return nullptr;
						// Removed earlier in the step, added again: it starts over from its defaults.
						std::ranges::copy(type->Defaults, data.Value.Get().begin());
						data.Validated = type->Defaults;
						data.Present = true;
					}
					return data.Value.Get().data();
				}

				const UnknownComponentData* stored = FindStored(entity, type->RegistryName);
				if (stored == nullptr)
				{
					if (!create)
						return nullptr;
					auto added = CreateScope<CachedComponent>(*type, type->Defaults);
					void* data = added->Value.Get().data();
					m_World.Components.emplace(key, std::move(added));
					return data;
				}

				// Read by field name (decision 27): a scene Scene.Load loaded or a prefab instantiated in play reaches the step
				// as its file holds it.
				std::vector<std::byte> bytes(type->Size);
				const std::string pointer = JsonReader::AppendPointer("/Components", type->RegistryName);
				const Result<NativeValueReadReport> read = NativeValueFromJson(*type, stored->Data.Get(), pointer, bytes);
				if (!read.has_value())
				{
					if (!m_Fault.has_value())
					{
						const std::vector<ErrorIssue>& issues = read.error().GetIssues();
						const std::string issue = issues.empty() ? read.error().GetMessageText() : std::format("{}: {}", issues.front().JsonPointer, issues.front().Message);
						m_Fault = NativeFault{ .System = m_System.Name,
							.Message = std::format("{} of the entity {} holds a value its type rejects ({}): {}", type->RegistryName,
								DescribeEntity(m_Session.GetScene(), entity.GetUUID()), NativeComponentInvalidCode, issue),
							.Entity = entity.GetUUID(),
							.EntityName = entity.GetName() };
					}
					return nullptr;
				}
				for (const NativeValueNote& note : read->Notes)
				{
					if (m_World.LoggedNotes.emplace(type->RegistryName, note.Field).second)
						ENGINE_CORE_WARN("{} of the entity {} ({}): {}", type->RegistryName, DescribeEntity(m_Session.GetScene(), entity.GetUUID()), note.Code, note.Message);
				}
				auto cached = CreateScope<CachedComponent>(*type, bytes);
				cached->InScene = true;
				cached->Migrated = !read->Notes.empty();
				void* data = cached->Value.Get().data();
				m_World.Components.emplace(key, std::move(cached));
				return data;
			}
		private:
			NativeWorldData& m_World;
			PlaySession& m_Session;
			SimStep m_Step{};
			const NativeSystemInfo& m_System;
			std::vector<std::vector<NativeEntity>> m_Queries{};
			mutable std::deque<std::string> m_Names{}; // GetName's copies; a deque never moves its elements
			std::optional<NativeFault> m_Fault{};
		};

		// §4.6 boundary (item 8): a system is the module's code. Returns the message of an exception that escaped it.
		static std::optional<std::string> RunSystem(const NativeSystemInfo& system, NativeStepContext& context)
		{
			try
			{
				system.Run(context);
				return std::nullopt;
			}
			catch (const std::exception& exception)
			{
				return SanitizeUtf8(exception.what(), MaxNativeMessageBytes);
			}
			catch (...)
			{
				return std::string("an exception of an unknown type");
			}
		}

		// After each system: every value it changed must be one its type accepts. An invalid one is put back as the system
		// found it, so the world never holds it, and the first is the fault that stops the session.
		static std::optional<NativeFault> CheckWrites(NativeWorldData& world, const Scene& scene, const NativeSystemInfo& system)
		{
			std::optional<NativeFault> fault;
			for (auto& [key, cached] : world.Components)
			{
				CachedComponent& data = *cached;
				const std::span<std::byte> value = data.Value.Get();
				if (!data.Present || AreSameBytes(value, data.Validated))
					continue;
				if (const std::optional<std::string> invalid = FindInvalidValue(*data.Type, value); invalid.has_value())
				{
					std::ranges::copy(data.Validated, value.begin());
					if (!fault.has_value())
					{
						fault = NativeFault{ .System = system.Name,
							.Message = std::format("the native system '{}' left a value {} rejects in the entity {}: {}", system.Name, data.Type->RegistryName,
								DescribeEntity(scene, key.first), *invalid),
							.Entity = key.first,
							.EntityName = GetEntityName(scene, key.first) };
					}
					continue;
				}
				data.Validated.assign(value.begin(), value.end());
			}
			const std::span<const NativeTypeInfo> resources = world.Types->GetResources();
			for (size_t index = 0; index < resources.size(); ++index)
			{
				const std::span<std::byte> value = world.Resources[index].Get();
				if (AreSameBytes(value, world.ValidatedResources[index]))
					continue;
				if (const std::optional<std::string> invalid = FindInvalidValue(resources[index], value); invalid.has_value())
				{
					std::ranges::copy(world.ValidatedResources[index], value.begin());
					if (!fault.has_value())
					{
						fault = NativeFault{ .System = system.Name,
							.Message = std::format("the native system '{}' left a value the resource {} rejects: {}", system.Name, resources[index].Name, *invalid),
							.Entity = {},
							.EntityName = {} };
					}
					continue;
				}
				world.ValidatedResources[index].assign(value.begin(), value.end());
			}
			return fault;
		}

		// The end of the phase (NativeWorld.h step 2): the step's components back into the scene, in (entity, component)
		// order, as canonical JSON; nothing for an entity destroyed since.
		static void WriteBack(NativeWorldData& world, Scene& scene)
		{
			for (auto& [key, cached] : world.Components)
			{
				const Entity entity = scene.FindEntityByID(key.first);
				if (!entity.IsValid())
					continue;
				CachedComponent& data = *cached;
				const std::string& name = data.Type->RegistryName;
				if (!data.Present)
				{
					if (data.InScene)
						EraseStored(entity, name);
					continue;
				}
				const std::span<const std::byte> value = data.Value.Get();
				if (data.InScene && !data.Migrated && AreSameBytes(value, data.Read))
					continue;
				Json json = NativeValueToJson(*data.Type, value);
				if (const UnknownComponentData* stored = FindStored(entity, name); stored != nullptr && stored->Version == NativeComponentVersion && stored->Data.Get() == json)
					continue;
				WriteStored(entity, name, std::move(json));
			}
			world.Components.clear();
		}

		// Stops the session fatally (PlaySession::GetFatalError) through its script host, which publishes the error to the
		// session's error stream (the host reports it, as a fatal script error), pauses the session and invalidates a
		// recording. A Play session always has its script VM while it steps.
		static void StopSession(PlaySession& session, const SimStep& step, const NativeFault& fault)
		{
			ScriptError error{};
			error.Kind = ScriptErrorKind::Runtime;
			error.Script = std::format("native:{}", fault.System);
			error.Message = fault.Message;
			error.Callback = "NativeStep";
			error.Entity = fault.Entity;
			error.EntityName = fault.EntityName;
			error.Tick = step.Tick;
			ScriptEngine* scripts = session.GetScripts();
			ENGINE_CORE_VERIFY(scripts != nullptr, "a Play session that runs a native module has its script VM");
			scripts->GetHost().OnScriptError(error, true);
		}

	}

	struct NativeWorld::State
	{
		Utils::NativeWorldData Data{};
	};

	NativeWorld::NativeWorld(ConstructionKey /*key*/, Ref<const NativeModuleTypes> types)
		: m_State(CreateScope<State>())
	{
		ENGINE_CORE_ASSERT(types != nullptr, "a native world needs its module's types");
		Utils::NativeWorldData& world = m_State->Data;
		world.Types = std::move(types);
		// Resources start at their defaults (NativeTypeDescriptor::Defaults), which registration validated.
		for (const NativeTypeInfo& resource : world.Types->GetResources())
		{
			world.Resources.emplace_back(resource.Defaults);
			world.ValidatedResources.push_back(resource.Defaults);
		}
	}

	NativeWorld::~NativeWorld() = default;

	Result<Scope<NativeWorld>> NativeWorld::Create(Ref<const NativeModuleTypes> types)
	{
		ENGINE_CORE_ASSERT(types != nullptr, "NativeWorld::Create needs a native module's types");
		return CreateScope<NativeWorld>(ConstructionKey(), std::move(types));
	}

	void NativeWorld::Step(PlaySession& session, const SimStep& step)
	{
		Utils::NativeWorldData& world = m_State->Data;
		world.LogLines = 0;
		world.DroppedLogLines = 0;
		std::optional<Utils::NativeFault> fault;
		for (const NativeSystemInfo& system : world.Types->GetSystems())
		{
			Utils::NativeStepContext context(world, session, step, system);
			const std::optional<std::string> exception = Utils::RunSystem(system, context);
			// An invalid value it read fails it first: the exception may well be its consequence.
			fault = std::move(context.GetFault());
			if (!fault.has_value() && exception.has_value())
			{
				fault = Utils::NativeFault{ .System = system.Name,
					.Message = std::format("the native system '{}' raised an exception: {}", system.Name, *exception),
					.Entity = {},
					.EntityName = {} };
			}
			std::optional<Utils::NativeFault> invalid = Utils::CheckWrites(world, session.GetScene(), system);
			if (!fault.has_value())
				fault = std::move(invalid);
			if (fault.has_value())
				break;
		}
		Utils::WriteBack(world, session.GetScene());
		if (world.DroppedLogLines != 0)
		{
			ENGINE_CORE_WARN("{} log lines of the native module's systems were dropped at tick {}: more than {} in one step", world.DroppedLogLines, step.Tick,
				Utils::MaxNativeLogLinesPerStep);
		}
		if (fault.has_value())
			Utils::StopSession(session, step, *fault);
	}

	void NativeWorld::AppendStateHash(XXH64Hasher& hasher) const
	{
		const Utils::NativeWorldData& world = m_State->Data;
		const std::span<const NativeTypeInfo> resources = world.Types->GetResources();
		for (size_t index = 0; index < resources.size(); ++index)
			AppendNativeValueHash(resources[index], world.Resources[index].Get(), hasher);
	}

	Json NativeWorld::GetResources() const
	{
		const Utils::NativeWorldData& world = m_State->Data;
		Json resources = Json::object();
		const std::span<const NativeTypeInfo> types = world.Types->GetResources();
		for (size_t index = 0; index < types.size(); ++index)
			resources[types[index].Name] = NativeValueToJson(types[index], world.Resources[index].Get());
		return resources;
	}

	const NativeModuleTypes& NativeWorld::GetTypes() const
	{
		return *m_State->Data.Types;
	}

}
