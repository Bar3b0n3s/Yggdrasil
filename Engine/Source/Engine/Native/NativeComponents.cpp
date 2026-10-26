#include "EnginePCH.h"
#include "Engine/Native/NativeComponents.h"

#include "Engine/Core/Error.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Native/NativeModule.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/MergePatch.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>

namespace Engine {

	namespace Utils {

		// True when both trees are written to the same canonical text (JsonWriter writes every fractional number as a float).
		static bool HaveSameText(const Json& left, const Json& right)
		{
			const Result<std::string> leftText = JsonWriter::Write(left, JsonStyle::Minified);
			const Result<std::string> rightText = JsonWriter::Write(right, JsonStyle::Minified);
			return leftText.has_value() && rightText.has_value() && *leftText == *rightText;
		}

		// True when two lists of preserved components would write the same scene text: names in the same order, the same
		// versions and the same JSON text.
		static bool AreSameComponents(const std::vector<UnknownComponentData>& left, const std::vector<UnknownComponentData>& right)
		{
			if (left.size() != right.size())
				return false;
			for (size_t index = 0; index < left.size(); ++index)
			{
				if (left[index].Name != right[index].Name || left[index].Version != right[index].Version || !HaveSameText(left[index].Data.Get(), right[index].Data.Get()))
					return false;
			}
			return true;
		}

		// The canonical order of an entity's preserved components (decision 27): its "Native." components sorted by registry
		// name (byte order), then its unknown components in the order they had.
		static void SortNativeFirst(std::vector<UnknownComponentData>& components)
		{
			const auto natives = std::stable_partition(components.begin(), components.end(), [](const UnknownComponentData& data)
			{
				return IsNativeComponentName(data.Name);
			});
			std::stable_sort(components.begin(), natives, [](const UnknownComponentData& left, const UnknownComponentData& right)
			{
				return left.Name < right.Name;
			});
		}

		// The pointer of a component below its entity: "/Components/Native.Spinner".
		static std::string MakeComponentPointer(std::string_view name)
		{
			return JsonReader::AppendPointer("/Components", name);
		}

		static LoadDiagnostic MakeDiagnostic(DiagnosticSeverity severity, std::string_view code, std::string message, std::string pointer, UUID entity)
		{
			return LoadDiagnostic{ .Severity = severity, .Code = std::string(code), .Message = std::move(message), .JsonPointer = std::move(pointer), .Entity = entity };
		}

		// Replaces the entity's preserved components with `components`: removes the holder when none is left.
		static void StoreComponents(Entity entity, std::vector<UnknownComponentData> components)
		{
			if (components.empty())
			{
				if (entity.HasComponent<UnknownComponentsComponent>())
					entity.RemoveComponent<UnknownComponentsComponent>();
				return;
			}
			if (!entity.HasComponent<UnknownComponentsComponent>())
			{
				entity.AddComponent<UnknownComponentsComponent>(UnknownComponentsComponent{ std::move(components) });
				return;
			}
			entity.Patch<UnknownComponentsComponent>([&components](UnknownComponentsComponent& preserved)
			{
				preserved.Components = std::move(components);
			});
		}

		// An entity's canonicalization: the components it would hold and its diagnostics.
		struct EntityCanonicalization
		{
			std::vector<UnknownComponentData> Components{};
			std::vector<LoadDiagnostic> Diagnostics{};
			uint32_t NativeCount = 0;
		};

		static EntityCanonicalization CanonicalizeEntity(const std::vector<UnknownComponentData>& preserved, const NativeModuleTypes& types, bool strictUnknowns,
			UUID entity)
		{
			const DiagnosticSeverity noteSeverity = strictUnknowns ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning;
			EntityCanonicalization result;
			result.Components = preserved;
			SortNativeFirst(result.Components);
			for (UnknownComponentData& data : result.Components)
			{
				if (!IsNativeComponentName(data.Name))
					break;
				++result.NativeCount;
				const std::string pointer = MakeComponentPointer(data.Name);
				const NativeTypeInfo* type = types.FindComponent(data.Name);
				if (type == nullptr)
				{
					result.Diagnostics.push_back(MakeDiagnostic(noteSeverity, NativeComponentUnregisteredCode,
						std::format("the loaded native module registers no component '{}'; it is kept as written", data.Name), pointer, entity));
					continue;
				}
				const Result<NativeCanonicalValue> canonical = CanonicalizeNativeValue(*type, data.Data.Get(), pointer);
				if (!canonical.has_value())
				{
					for (const ErrorIssue& issue : canonical.error().GetIssues())
					{
						result.Diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Error, NativeComponentInvalidCode,
							std::format("{} holds a value its type rejects: {}; it is kept as written", data.Name, issue.Message), issue.JsonPointer, entity));
					}
					if (canonical.error().GetIssues().empty())
					{
						result.Diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Error, NativeComponentInvalidCode,
							std::format("{} holds a value its type rejects: {}", data.Name, canonical.error().GetMessageText()), pointer, entity));
					}
					continue;
				}
				for (const NativeValueNote& note : canonical->Report.Notes)
					result.Diagnostics.push_back(MakeDiagnostic(noteSeverity, note.Code, note.Message, JsonReader::AppendPointer(pointer, note.Field), entity));
				data.Data = canonical->Value;
				data.Version = NativeComponentVersion;
			}
			return result;
		}

		static bool HasErrors(std::span<const LoadDiagnostic> diagnostics)
		{
			return std::ranges::any_of(diagnostics, [](const LoadDiagnostic& diagnostic)
			{
				return diagnostic.Severity == DiagnosticSeverity::Error;
			});
		}

		// The issue a patch's own member gets for a note on it: a member that names no field, or a value that does not
		// convert, is a mistake of the request rather than a migration.
		static ErrorIssue MakePatchNoteIssue(const NativeTypeInfo& type, const NativeValueNote& note, const std::string& pointer)
		{
			if (note.Code == NativeFieldDroppedCode)
			{
				std::vector<std::string> names;
				for (const NativeFieldInfo& field : type.Fields)
					names.push_back(field.Name);
				std::vector<std::string> suggestions = FuzzySuggest(note.Field, names);
				std::string hint = MakeDidYouMeanHint(suggestions);
				return ErrorIssue{ .JsonPointer = JsonReader::AppendPointer(pointer, note.Field),
					.Message = std::format("unknown field '{}' on '{}'", note.Field, type.RegistryName),
					.Hint = std::move(hint),
					.Suggestions = std::move(suggestions) };
			}
			return ErrorIssue{ .JsonPointer = JsonReader::AppendPointer(pointer, note.Field), .Message = note.Message, .Hint = {}, .Suggestions = {} };
		}

	}

	namespace Utils {

		// A scene's canonicalization (CanonicalizeNativeComponents, CheckNativeComponents): the report, its Changed not yet
		// set, and the preserved components of each entity it would rewrite, in canonical entity order.
		struct SceneCanonicalization
		{
			NativeSceneReport Report{};
			std::vector<std::pair<UUID, std::vector<UnknownComponentData>>> Updates{};
		};

		static SceneCanonicalization CanonicalizeScene(const Scene& scene, const NativeModuleTypes* types, bool strictUnknowns)
		{
			SceneCanonicalization result;
			NativeSceneReport& report = result.Report;
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const ConstEntity entity = scene.FindEntityByID(id);
				const UnknownComponentsComponent* preserved = entity.IsValid() ? entity.TryGetComponent<UnknownComponentsComponent>() : nullptr;
				if (preserved == nullptr)
					continue;

				if (types == nullptr)
				{
					// Without a module nothing is rewritten or reordered: every native component is unregistered.
					for (const UnknownComponentData& data : preserved->Components)
					{
						if (!IsNativeComponentName(data.Name))
							continue;
						++report.ComponentCount;
						report.Diagnostics.push_back(MakeDiagnostic(strictUnknowns ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
							NativeComponentUnregisteredCode, std::format("no native module is loaded; '{}' is kept as written", data.Name),
							MakeComponentPointer(data.Name), id));
					}
					continue;
				}

				EntityCanonicalization canonical = CanonicalizeEntity(preserved->Components, *types, strictUnknowns, id);
				report.ComponentCount += canonical.NativeCount;
				std::ranges::move(canonical.Diagnostics, std::back_inserter(report.Diagnostics));
				if (!AreSameComponents(canonical.Components, preserved->Components))
					result.Updates.emplace_back(id, std::move(canonical.Components));
			}
			return result;
		}

	}

	Result<NativeSceneReport> CanonicalizeNativeComponents(Scene& scene, const NativeModuleTypes* types, bool strictUnknowns)
	{
		Utils::SceneCanonicalization canonical = Utils::CanonicalizeScene(scene, types, strictUnknowns);
		// An error leaves the scene as it was: the session does not start, the strict document check refuses the project.
		if (Utils::HasErrors(canonical.Report.Diagnostics))
			return std::move(canonical.Report);
		for (auto& [id, components] : canonical.Updates)
			Utils::StoreComponents(scene.FindEntityByID(id), std::move(components));
		canonical.Report.Changed = !canonical.Updates.empty();
		return std::move(canonical.Report);
	}

	NativeSceneReport CheckNativeComponents(const Scene& scene, const NativeModuleTypes* types, bool strictUnknowns)
	{
		Utils::SceneCanonicalization canonical = Utils::CanonicalizeScene(scene, types, strictUnknowns);
		canonical.Report.Changed = !Utils::HasErrors(canonical.Report.Diagnostics) && !canonical.Updates.empty();
		return std::move(canonical.Report);
	}

	NativeEntityComponents ReadNativeComponents(ConstEntity entity, const NativeModuleTypes* types)
	{
		NativeEntityComponents components;
		const UnknownComponentsComponent* preserved = entity.IsValid() ? entity.TryGetComponent<UnknownComponentsComponent>() : nullptr;
		if (preserved == nullptr)
			return components;
		for (const UnknownComponentData& data : preserved->Components)
		{
			if (!IsNativeComponentName(data.Name))
				continue;
			const bool registered = types != nullptr && types->FindComponent(data.Name) != nullptr;
			(registered ? components.Registered : components.Unregistered).emplace_back(data.Name, data.Data);
		}
		return components;
	}

	Result<bool> PatchNativeComponents(Entity entity, const NativeModuleTypes& types, const Json& patch, std::string_view pointer)
	{
		if (!entity.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "native components can only be patched on a live entity");
		if (!patch.is_object())
		{
			return std::unexpected(Error(ErrorCode::InvalidArgument,
				std::format("a native component patch is an object of \"Native.<Name>\": <object> | null members, got {}", JsonTypeToString(GetJsonType(patch))))
					.WithLocation({ .File = {}, .Line = 0, .Column = 0, .JsonPointer = std::string(pointer), .Entity = entity.GetUUID() }));
		}

		const UnknownComponentsComponent* preserved = entity.TryGetComponent<UnknownComponentsComponent>();
		const std::vector<UnknownComponentData> before = preserved != nullptr ? preserved->Components : std::vector<UnknownComponentData>{};
		std::vector<UnknownComponentData> after = before;
		std::vector<ErrorIssue> issues;
		for (auto member = patch.begin(); member != patch.end(); ++member)
		{
			const std::string& name = member.key();
			const std::string memberPointer = JsonReader::AppendPointer(pointer, name);
			const NativeTypeInfo* type = types.FindComponent(name);
			if (type == nullptr)
			{
				std::vector<std::string> names;
				for (const NativeTypeInfo& candidate : types.GetComponents())
					names.push_back(candidate.RegistryName);
				std::vector<std::string> suggestions = FuzzySuggest(name, names);
				std::string hint = MakeDidYouMeanHint(suggestions);
				return std::unexpected(Error(ErrorCode::NotFound, std::format("the loaded native module registers no component '{}'", name))
						.WithLocation({ .File = {}, .Line = 0, .Column = 0, .JsonPointer = memberPointer, .Entity = entity.GetUUID() })
						.WithHint(hint.empty() ? std::string("native.status lists the module's components") : hint)
						.WithIssue({ .JsonPointer = memberPointer, .Message = std::format("no native component '{}'", name), .Hint = hint, .Suggestions = std::move(suggestions) }));
			}

			const auto held = std::ranges::find(after, name, &UnknownComponentData::Name);
			if (member.value().is_null())
			{
				if (held != after.end())
					after.erase(held);
				continue;
			}

			// The patch merges into the component as it is (its defaults when the entity lacks it), then the result becomes
			// canonical; members the stored value carried from an older layout migrate as on load.
			const Json base = held != after.end() ? held->Data.Get() : NativeValueToJson(*type, type->Defaults);
			const Result<NativeCanonicalValue> canonical = CanonicalizeNativeValue(*type, ApplyMergePatch(base, member.value()), memberPointer);
			if (!canonical.has_value())
			{
				std::ranges::copy(canonical.error().GetIssues(), std::back_inserter(issues));
				continue;
			}
			const JsonReader patched(member.value());
			for (const NativeValueNote& note : canonical->Report.Notes)
			{
				if (patched.HasMember(note.Field))
					issues.push_back(Utils::MakePatchNoteIssue(*type, note, memberPointer));
			}
			if (held != after.end())
			{
				held->Data = canonical->Value;
				held->Version = NativeComponentVersion;
			}
			else
				after.push_back({ .Name = name, .Version = NativeComponentVersion, .Data = canonical->Value });
		}
		if (!issues.empty())
		{
			return std::unexpected(Error(ErrorCode::Validation, std::format("{} invalid native component value(s)", issues.size()))
					.WithLocation({ .File = {}, .Line = 0, .Column = 0, .JsonPointer = std::string(pointer), .Entity = entity.GetUUID() })
					.WithIssues(std::move(issues)));
		}

		Utils::SortNativeFirst(after);
		if (Utils::AreSameComponents(after, before))
			return false;
		Utils::StoreComponents(entity, std::move(after));
		return true;
	}

}
