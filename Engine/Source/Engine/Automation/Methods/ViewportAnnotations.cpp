#include "EnginePCH.h"
#include "Engine/Automation/Methods/ScreenshotMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Entity.h"

#include <algorithm>
#include <utility>

namespace Engine {

	Result<ViewportAnnotationOptions> ParseViewportAnnotations(const VariantValue& value)
	{
		const JsonReader reader(value.Get(), "/annotate");
		if (!reader.IsObject())
			return std::unexpected(reader.MakeLocatedError(ErrorCode::InvalidArgument, "expected annotation object"));
		ViewportAnnotationOptions result;
		ENGINE_TRY_ASSIGN(const auto names, reader.GetMemberNames());
		for (const std::string& name : names)
		{
			ENGINE_TRY_ASSIGN(const JsonReader field, reader.GetMember(name));
			if (name == "labels")
			{
				if (field.GetType() == JsonType::String)
				{
					ENGINE_TRY_ASSIGN(std::string mode, field.ReadString());
					for (char& letter : mode)
						if (letter >= 'A' && letter <= 'Z')
							letter = static_cast<char>(letter + ('a' - 'A'));
					if (mode == "all")
						result.Annotations.Labels = RenderAnnotationLabels::All;
					else if (mode == "selection" || mode == "selected")
						result.Annotations.Labels = RenderAnnotationLabels::Selected;
					else if (mode == "none")
						result.Annotations.Labels = RenderAnnotationLabels::None;
					else
						return std::unexpected(field.MakeLocatedError(ErrorCode::InvalidArgument, "expected all, selection, none or an EntityRef array"));
				}
				else if (field.IsArray())
				{
					ENGINE_TRY_ASSIGN(const size_t count, field.GetArraySize());
					if (count > 1000)
						return std::unexpected(field.MakeLocatedError(ErrorCode::InvalidArgument, "labels supports at most 1000 entity references"));
					result.Annotations.Labels = RenderAnnotationLabels::Explicit;
					for (size_t index = 0; index < count; ++index)
					{
						ENGINE_TRY_ASSIGN(const JsonReader element, field.GetElement(index));
						const auto reference = element.ReadString();
						if (!reference || reference->empty())
							return std::unexpected(element.MakeLocatedError(ErrorCode::InvalidArgument, "expected a nonempty EntityRef string"));
						result.LabelReferences.push_back(*reference);
					}
				}
				else
					return std::unexpected(field.MakeLocatedError(ErrorCode::InvalidArgument, "expected label mode or EntityRef array"));
			}
			else if (name == "bounds" || name == "axes" || name == "colliders")
			{
				const auto flag = field.ReadBool();
				if (!flag)
					return std::unexpected(field.MakeLocatedError(ErrorCode::InvalidArgument, "expected boolean"));
				if (name == "bounds")
					result.Annotations.Bounds = *flag;
				else if (name == "axes")
					result.Annotations.Axes = *flag;
				else
					result.Colliders = *flag;
			}
			else
				return std::unexpected(field.MakeLocatedError(ErrorCode::InvalidArgument, "unknown annotation option"));
		}
		return result;
	}

	Result<RenderAnnotations> ResolveViewportAnnotations(AutomationMethodContext& context, Scene& scene, const ViewportAnnotationOptions& options)
	{
		if (std::to_underlying(options.Annotations.Labels) > std::to_underlying(RenderAnnotationLabels::Explicit)
			|| !options.Annotations.LabelEntities.empty() || options.LabelReferences.size() > 1000
			|| (options.Annotations.Labels != RenderAnnotationLabels::Explicit && !options.LabelReferences.empty()))
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/annotate/labels", "inconsistent unresolved label options"));
		RenderAnnotations result = options.Annotations;
		for (size_t index = 0; index < options.LabelReferences.size(); ++index)
		{
			ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(scene, options.LabelReferences[index], JsonReader::AppendPointer("/annotate/labels", index)));
			result.LabelEntities.push_back(entity.GetUUID());
		}
		std::ranges::sort(result.LabelEntities);
		result.LabelEntities.erase(std::unique(result.LabelEntities.begin(), result.LabelEntities.end()), result.LabelEntities.end());
		return result;
	}

}
