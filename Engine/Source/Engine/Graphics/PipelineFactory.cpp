#include "EnginePCH.h"
#include "Engine/Graphics/PipelineFactory.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <ranges>
#include <set>

// The reflection check compares descriptors by (descriptor set, Vulkan binding number): slangc reports the binding
// numbers already shifted by CompileShaders.py's -fvk-*-shift flags (§8.4), and NVRHI places each layout item at its
// slot plus the layout's VulkanBindingOffsets for the item's register class, so a layout with other offsets than the
// shifts disagrees with the reflection and is reported. A constant buffer's C++ size is its shared struct, which
// "Shaders: SharedStructsMatchReflection" compares with the same reflection; here every entry point must agree on it.
// Every binding an entry point uses must be visible to that entry point's stage. Push constants carry no use flag in
// slangc's reflection and count as used (ParseShaderReflection), so their layout must be visible to every stage that
// declares them; a stage in a push-constant range that does not use it is harmless in Vulkan.

namespace Engine {

	namespace {

		// A binding as the pipeline's entry points reflect it: its first declaration and the stages that use it.
		struct ReflectedBinding
		{
			const ShaderBinding* Binding = nullptr; // owned by the ShaderLibrary's reflection cache
			std::string Entry{};                    // the entry point of the first declaration
			nvrhi::ShaderType UsedBy = nvrhi::ShaderType::None;
			bool IsUsed = false;
		};

		// A layout item with the descriptor set and the Vulkan binding number it occupies.
		struct DeclaredItem
		{
			nvrhi::BindingLayoutItem Item{};
			uint32_t Set = 0;
			uint32_t VulkanBinding = 0;
			ShaderBindingKind Kind = ShaderBindingKind::ConstantBuffer;
			nvrhi::ShaderType Visibility = nvrhi::ShaderType::None;
			bool IsMatched = false;
		};

		// (descriptor set, Vulkan binding number)
		using BindingKey = std::pair<uint32_t, uint32_t>;

	}

	namespace Utils {

		static std::optional<ShaderBindingKind> GetBindingKind(nvrhi::ResourceType type)
		{
			switch (type)
			{
				case nvrhi::ResourceType::ConstantBuffer:
				case nvrhi::ResourceType::VolatileConstantBuffer:
					return ShaderBindingKind::ConstantBuffer;
				case nvrhi::ResourceType::Texture_SRV:
				case nvrhi::ResourceType::TypedBuffer_SRV:
				case nvrhi::ResourceType::StructuredBuffer_SRV:
				case nvrhi::ResourceType::RawBuffer_SRV:
					return ShaderBindingKind::ShaderResource;
				case nvrhi::ResourceType::Texture_UAV:
				case nvrhi::ResourceType::TypedBuffer_UAV:
				case nvrhi::ResourceType::StructuredBuffer_UAV:
				case nvrhi::ResourceType::RawBuffer_UAV:
					return ShaderBindingKind::UnorderedAccess;
				case nvrhi::ResourceType::Sampler:
					return ShaderBindingKind::Sampler;
				case nvrhi::ResourceType::PushConstants:
					return ShaderBindingKind::PushConstantBuffer;
				case nvrhi::ResourceType::None:
				case nvrhi::ResourceType::RayTracingAccelStruct:
				case nvrhi::ResourceType::SamplerFeedbackTexture_UAV:
				case nvrhi::ResourceType::Count:
					// Not part of the binding model of §8.4.
					return std::nullopt;
			}
			return std::nullopt;
		}

		static uint32_t GetBindingOffset(const nvrhi::VulkanBindingOffsets& offsets, ShaderBindingKind kind)
		{
			switch (kind)
			{
				case ShaderBindingKind::ConstantBuffer:
				case ShaderBindingKind::PushConstantBuffer: return offsets.constantBuffer;
				case ShaderBindingKind::ShaderResource:     return offsets.shaderResource;
				case ShaderBindingKind::UnorderedAccess:    return offsets.unorderedAccess;
				case ShaderBindingKind::Sampler:            return offsets.sampler;
			}

			ENGINE_CORE_ASSERT(false, "Unknown ShaderBindingKind {}", std::to_underlying(kind));
			return 0;
		}

		// The layouts ordered by descriptor set; layouts of the same set keep their order.
		static std::vector<const nvrhi::BindingLayoutDesc*> SortBySet(std::span<const nvrhi::BindingLayoutDesc> layouts)
		{
			std::vector<const nvrhi::BindingLayoutDesc*> ordered;
			ordered.reserve(layouts.size());
			for (const nvrhi::BindingLayoutDesc& layout : layouts)
				ordered.push_back(&layout);
			std::ranges::stable_sort(ordered, std::less<>(), [](const nvrhi::BindingLayoutDesc* layout)
			{
				return layout->registerSpace;
			});
			return ordered;
		}

		static bool IsTextureShape(ShaderResourceShape shape)
		{
			switch (shape)
			{
				case ShaderResourceShape::Texture1D:
				case ShaderResourceShape::Texture2D:
				case ShaderResourceShape::Texture2DArray:
				case ShaderResourceShape::Texture3D:
				case ShaderResourceShape::TextureCube:
				case ShaderResourceShape::TextureCubeArray:
					return true;
				case ShaderResourceShape::None:
				case ShaderResourceShape::StructuredBuffer:
				case ShaderResourceShape::ByteAddressBuffer:
				case ShaderResourceShape::TypedBuffer:
					return false;
			}

			ENGINE_CORE_ASSERT(false, "Unknown ShaderResourceShape {}", std::to_underlying(shape));
			return false;
		}

		// Whether a layout item of `type` provides the descriptor a binding of `shape` needs (NVRHI picks the Vulkan
		// descriptor type from the item type: sampled or storage image, texel buffer, storage buffer, uniform buffer).
		static bool IsShapeCompatible(nvrhi::ResourceType type, ShaderResourceShape shape)
		{
			switch (type)
			{
				case nvrhi::ResourceType::Texture_SRV:
				case nvrhi::ResourceType::Texture_UAV:
					return IsTextureShape(shape);
				case nvrhi::ResourceType::TypedBuffer_SRV:
				case nvrhi::ResourceType::TypedBuffer_UAV:
					return shape == ShaderResourceShape::TypedBuffer;
				case nvrhi::ResourceType::StructuredBuffer_SRV:
				case nvrhi::ResourceType::StructuredBuffer_UAV:
					return shape == ShaderResourceShape::StructuredBuffer;
				case nvrhi::ResourceType::RawBuffer_SRV:
				case nvrhi::ResourceType::RawBuffer_UAV:
					return shape == ShaderResourceShape::ByteAddressBuffer;
				case nvrhi::ResourceType::ConstantBuffer:
				case nvrhi::ResourceType::VolatileConstantBuffer:
				case nvrhi::ResourceType::Sampler:
				case nvrhi::ResourceType::PushConstants:
					return shape == ShaderResourceShape::None;
				case nvrhi::ResourceType::None:
				case nvrhi::ResourceType::RayTracingAccelStruct:
				case nvrhi::ResourceType::SamplerFeedbackTexture_UAV:
				case nvrhi::ResourceType::Count:
					return false;
			}
			return false;
		}

		static std::string_view ResourceTypeToString(nvrhi::ResourceType type)
		{
			switch (type)
			{
				case nvrhi::ResourceType::None:                       return "None";
				case nvrhi::ResourceType::Texture_SRV:                return "Texture_SRV";
				case nvrhi::ResourceType::Texture_UAV:                return "Texture_UAV";
				case nvrhi::ResourceType::TypedBuffer_SRV:            return "TypedBuffer_SRV";
				case nvrhi::ResourceType::TypedBuffer_UAV:            return "TypedBuffer_UAV";
				case nvrhi::ResourceType::StructuredBuffer_SRV:       return "StructuredBuffer_SRV";
				case nvrhi::ResourceType::StructuredBuffer_UAV:       return "StructuredBuffer_UAV";
				case nvrhi::ResourceType::RawBuffer_SRV:              return "RawBuffer_SRV";
				case nvrhi::ResourceType::RawBuffer_UAV:              return "RawBuffer_UAV";
				case nvrhi::ResourceType::ConstantBuffer:             return "ConstantBuffer";
				case nvrhi::ResourceType::VolatileConstantBuffer:     return "VolatileConstantBuffer";
				case nvrhi::ResourceType::Sampler:                    return "Sampler";
				case nvrhi::ResourceType::RayTracingAccelStruct:      return "RayTracingAccelStruct";
				case nvrhi::ResourceType::PushConstants:              return "PushConstants";
				case nvrhi::ResourceType::SamplerFeedbackTexture_UAV: return "SamplerFeedbackTexture_UAV";
				case nvrhi::ResourceType::Count:                      break;
			}
			return "Unknown";
		}

		static std::string_view ShapeToString(ShaderResourceShape shape)
		{
			switch (shape)
			{
				case ShaderResourceShape::None:              return "None";
				case ShaderResourceShape::Texture1D:         return "Texture1D";
				case ShaderResourceShape::Texture2D:         return "Texture2D";
				case ShaderResourceShape::Texture2DArray:    return "Texture2DArray";
				case ShaderResourceShape::Texture3D:         return "Texture3D";
				case ShaderResourceShape::TextureCube:       return "TextureCube";
				case ShaderResourceShape::TextureCubeArray:  return "TextureCubeArray";
				case ShaderResourceShape::StructuredBuffer:  return "StructuredBuffer";
				case ShaderResourceShape::ByteAddressBuffer: return "ByteAddressBuffer";
				case ShaderResourceShape::TypedBuffer:       return "TypedBuffer";
			}

			ENGINE_CORE_ASSERT(false, "Unknown ShaderResourceShape {}", std::to_underlying(shape));
			return "Unknown";
		}

		// "Vertex|Pixel"; "no stage" for None.
		static std::string StagesToString(nvrhi::ShaderType stages)
		{
			constexpr auto Names = std::to_array<std::pair<nvrhi::ShaderType, std::string_view>>({
				{ nvrhi::ShaderType::Vertex, "Vertex" },
				{ nvrhi::ShaderType::Hull, "Hull" },
				{ nvrhi::ShaderType::Domain, "Domain" },
				{ nvrhi::ShaderType::Geometry, "Geometry" },
				{ nvrhi::ShaderType::Pixel, "Pixel" },
				{ nvrhi::ShaderType::Compute, "Compute" },
				{ nvrhi::ShaderType::Amplification, "Amplification" },
				{ nvrhi::ShaderType::Mesh, "Mesh" },
			});
			std::string text;
			for (const auto& [stage, name] : Names)
			{
				if ((stages & stage) == stage)
					text += text.empty() ? std::string(name) : std::format("|{}", name);
			}
			return text.empty() ? std::string("no stage") : text;
		}

		static std::string_view GetRegisterPrefix(ShaderBindingKind kind)
		{
			switch (kind)
			{
				case ShaderBindingKind::ConstantBuffer:     return "b";
				case ShaderBindingKind::ShaderResource:     return "t";
				case ShaderBindingKind::UnorderedAccess:    return "u";
				case ShaderBindingKind::Sampler:            return "s";
				case ShaderBindingKind::PushConstantBuffer: return "push";
			}

			ENGINE_CORE_ASSERT(false, "Unknown ShaderBindingKind {}", std::to_underlying(kind));
			return "?";
		}

		// The JSON pointer of an issue: "/<set>/<binding name>".
		static std::string MakeIssuePointer(uint32_t set, std::string_view name)
		{
			return std::format("/{}/{}", set, name);
		}

		// The pointer of a layout item no binding name is known for: its register ("/0/t7").
		static std::string MakeItemPointer(const DeclaredItem& item)
		{
			return MakeIssuePointer(item.Set, std::format("{}{}", GetRegisterPrefix(item.Kind), item.Item.slot));
		}

		// Appends an issue at `pointer` with a std::format message.
		template<typename... Args>
		static void AddIssue(std::vector<ErrorIssue>& issues, std::string pointer, std::format_string<Args...> format, Args&&... args)
		{
			issues.push_back(ErrorIssue{
				.JsonPointer = std::move(pointer),
				.Message = std::format(format, std::forward<Args>(args)...),
				.Hint = {},
				.Suggestions = {},
			});
		}

		// "'View' (ConstantBuffer at Vulkan binding 256)"
		static std::string DescribeBinding(const ShaderBinding& binding)
		{
			if (binding.Kind == ShaderBindingKind::PushConstantBuffer)
				return std::format("push constants '{}' ({} bytes)", binding.Name, binding.ByteSize);
			return std::format("'{}' ({} at Vulkan binding {})", binding.Name, ShaderBindingKindToString(binding.Kind), binding.Binding);
		}

		// "ConstantBuffer b0 (Vulkan binding 256)"
		static std::string DescribeItem(const DeclaredItem& item)
		{
			return std::format("{} {}{} (Vulkan binding {})", ResourceTypeToString(item.Item.type), GetRegisterPrefix(item.Kind), item.Item.slot,
				item.VulkanBinding);
		}

		static bool IsSameDeclaration(const ShaderBinding& first, const ShaderBinding& second)
		{
			return first.Name == second.Name && first.Kind == second.Kind && first.Shape == second.Shape && first.ArraySize == second.ArraySize
				&& first.ByteSize == second.ByteSize && first.StructName == second.StructName && first.StorageFormat == second.StorageFormat;
		}

		static std::string_view GetFormatName(nvrhi::Format format)
		{
			return nvrhi::getFormatInfo(format).name;
		}

		// The checks of one reflected descriptor against the layout item at its set and binding.
		static void CompareDescriptor(const ReflectedBinding& reflected, const DeclaredItem& item, const std::string& pointer,
			std::vector<ErrorIssue>& issues)
		{
			const ShaderBinding& binding = *reflected.Binding;
			if (item.Kind != binding.Kind)
			{
				AddIssue(issues, pointer, "the shaders declare {} but the layout of set {} declares {} there",
					DescribeBinding(binding), item.Set, DescribeItem(item));
				return;
			}
			if (!IsShapeCompatible(item.Item.type, binding.Shape))
			{
				AddIssue(issues, pointer, "{} is a {} but the layout of set {} declares {}",
					DescribeBinding(binding), ShapeToString(binding.Shape), item.Set, DescribeItem(item));
			}
			if (binding.ArraySize == 0)
			{
				AddIssue(issues, pointer, "{} is an unbounded array, which needs a bindless layout; engine pipelines bind fixed-size arrays",
					DescribeBinding(binding));
			}
			else if (item.Item.getArraySize() != binding.ArraySize)
			{
				AddIssue(issues, pointer, "{} has {} element(s) but {} in the layout of set {} has {}",
					DescribeBinding(binding), binding.ArraySize, DescribeItem(item), item.Set, item.Item.getArraySize());
			}
			if (reflected.IsUsed && (item.Visibility & reflected.UsedBy) != reflected.UsedBy)
			{
				AddIssue(issues, pointer, "{} is used by the {} stage(s) but the layout of set {} is visible to {}",
					DescribeBinding(binding), StagesToString(reflected.UsedBy), item.Set, StagesToString(item.Visibility));
			}
		}

		// §8.4: every storage image declares its format, and it equals the format the pass creates for it.
		static void CompareStorageFormat(const ShaderBinding& binding, const DeclaredItem& item, const PipelineLayoutDescription& description,
			std::vector<bool>& matchedStorageImages, const std::string& pointer, std::vector<ErrorIssue>& issues)
		{
			const auto created = std::ranges::find_if(description.StorageImages, [&item](const StorageImageFormat& image)
			{
				return image.Set == item.Set && image.Register == item.Item.slot;
			});
			if (created != description.StorageImages.end())
				matchedStorageImages[static_cast<size_t>(created - description.StorageImages.begin())] = true;

			if (!binding.StorageFormat)
			{
				AddIssue(issues, pointer, "storage image {} declares no [vk::image_format]; §8.4 requires an explicit storage format",
					DescribeBinding(binding));
			}
			else if (created == description.StorageImages.end())
			{
				AddIssue(issues, pointer, "no StorageImages entry gives the pass's format for storage image {} (the shader declares {})",
					DescribeBinding(binding), GetFormatName(*binding.StorageFormat));
			}
			else if (created->Format != *binding.StorageFormat)
			{
				AddIssue(issues, pointer, "the pass creates {} for storage image {} but the shader declares {}",
					GetFormatName(created->Format), DescribeBinding(binding), GetFormatName(*binding.StorageFormat));
			}
		}

		// §8.12: the C++ size of every constant buffer equals the reflected size of its element struct.
		static void CompareConstantBufferSize(const ShaderBinding& binding, const DeclaredItem& item, const PipelineLayoutDescription& description,
			std::vector<bool>& matchedConstantBuffers, const std::string& pointer, std::vector<ErrorIssue>& issues)
		{
			const auto declared = std::ranges::find_if(description.ConstantBuffers, [&item](const ConstantBufferSize& buffer)
			{
				return buffer.Set == item.Set && buffer.Register == item.Item.slot;
			});
			if (declared == description.ConstantBuffers.end())
			{
				AddIssue(issues, pointer, "no ConstantBuffers entry gives the C++ size of constant buffer {} (the shader's struct is {} bytes)",
					DescribeBinding(binding), binding.ByteSize);
				return;
			}
			matchedConstantBuffers[static_cast<size_t>(declared - description.ConstantBuffers.begin())] = true;
			if (declared->ByteSize != binding.ByteSize)
			{
				AddIssue(issues, pointer, "the pass binds {} bytes to constant buffer {}, whose struct is {} bytes in the shader",
					declared->ByteSize, DescribeBinding(binding), binding.ByteSize);
			}
		}

		static void ComparePushConstants(const ReflectedBinding& reflected, const std::optional<DeclaredItem>& declared,
			std::vector<ErrorIssue>& issues)
		{
			// BindingLayoutItem::size is a bit-field, which std::format cannot take by reference.
			const uint32_t declaredSize = declared ? declared->Item.size : 0;
			if (reflected.Binding == nullptr)
			{
				if (declared)
				{
					AddIssue(issues, MakeIssuePointer(declared->Set, "PushConstants"),
						"the layout of set {} declares {} bytes of push constants, which no entry point declares", declared->Set, declaredSize);
				}
				return;
			}

			const ShaderBinding& binding = *reflected.Binding;
			const std::string pointer = MakeIssuePointer(declared ? declared->Set : 0, binding.Name);
			if (!declared)
			{
				AddIssue(issues, pointer, "the shaders declare {} but no layout has a PushConstants item", DescribeBinding(binding));
			}
			else
			{
				if (declaredSize != binding.ByteSize)
				{
					AddIssue(issues, pointer, "the shaders declare {} but the PushConstants item of the layout of set {} declares {} bytes",
						DescribeBinding(binding), declared->Set, declaredSize);
				}
				// NVRHI gives the push-constant range the visibility of the layout that holds the item.
				if ((declared->Visibility & reflected.UsedBy) != reflected.UsedBy)
				{
					AddIssue(issues, pointer, "{} are declared by the {} stage(s) but the layout of set {} is visible to {}",
						DescribeBinding(binding), StagesToString(reflected.UsedBy), declared->Set, StagesToString(declared->Visibility));
				}
			}
			if (binding.ByteSize > nvrhi::c_MaxPushConstantSize)
			{
				AddIssue(issues, pointer, "{} exceed the {} bytes of push constants Vulkan guarantees",
					DescribeBinding(binding), nvrhi::c_MaxPushConstantSize);
			}
		}

		// The reflection of `entry` (an index into description.Entries) must be a `stage` shader.
		static Status CheckEntryStage(ShaderLibrary& shaders, const PipelineLayoutDescription& description, size_t entry,
			nvrhi::ShaderType stage)
		{
			ENGINE_TRY_ASSIGN(const ShaderReflection* reflection,
				shaders.GetReflection(description.Program, description.Entries[entry], description.Permutation));
			if (reflection->Stage != stage)
			{
				return MakeError(ErrorCode::InvalidArgument, "entry point {} of the pipeline '{}' must be a {} shader, but '{}' of '{}' is a {} shader",
					entry, description.Name, StagesToString(stage), description.Entries[entry], description.Program,
					StagesToString(reflection->Stage));
			}
			return {};
		}

		// The shader of `entry` (an index into layout.Entries), specialized when `specializations` is not empty.
		static Result<nvrhi::ShaderHandle> CreateEntryShader(GraphicsDevice& device, ShaderLibrary& shaders,
			const PipelineLayoutDescription& layout, size_t entry, std::span<const nvrhi::ShaderSpecialization> specializations)
		{
			ENGINE_TRY_ASSIGN(nvrhi::ShaderHandle shader, shaders.Get(layout.Program, layout.Entries[entry], layout.Permutation));
			if (specializations.empty())
				return shader;
			return device.CreateShaderSpecialization(*shader, specializations);
		}

		// One binding layout per set, in set order (ValidatePipelineLayout rejected duplicate sets).
		static Result<std::vector<nvrhi::BindingLayoutHandle>> CreateBindingLayouts(GraphicsDevice& device, const PipelineLayoutDescription& layout)
		{
			std::vector<nvrhi::BindingLayoutHandle> bindingLayouts;
			bindingLayouts.reserve(layout.BindingLayouts.size());
			for (const nvrhi::BindingLayoutDesc* desc : SortBySet(layout.BindingLayouts))
			{
				ENGINE_TRY_ASSIGN(nvrhi::BindingLayoutHandle bindingLayout, device.CreateBindingLayout(*desc));
				bindingLayouts.push_back(std::move(bindingLayout));
			}
			return bindingLayouts;
		}

	}

	Status ValidatePipelineLayout(const PipelineLayoutDescription& description, ShaderLibrary& shaders)
	{
		const std::string context = std::format("while checking the binding layouts of the pipeline '{}' against the reflection of '{}'",
			description.Name, description.Program);
		if (description.Entries.empty())
			return std::unexpected(Error(ErrorCode::Validation, "the pipeline names no entry point").WithContext(context));

		std::vector<ErrorIssue> issues;

		// The reflected bindings of every entry point, merged by set and binding.
		std::map<BindingKey, ReflectedBinding> reflected;
		ReflectedBinding pushConstants;
		for (const std::string& entry : description.Entries)
		{
			Result<const ShaderReflection*> reflection = shaders.GetReflection(description.Program, entry, description.Permutation);
			if (!reflection)
				return std::unexpected(std::move(reflection).error().WithContext(context));
			for (const ShaderBinding& binding : (*reflection)->Bindings)
			{
				ReflectedBinding& merged = binding.Kind == ShaderBindingKind::PushConstantBuffer ? pushConstants
																								 : reflected[BindingKey(binding.Set, binding.Binding)];
				if (merged.Binding == nullptr)
				{
					merged.Binding = &binding;
					merged.Entry = entry;
				}
				else if (!Utils::IsSameDeclaration(*merged.Binding, binding))
				{
					const std::string pointer = Utils::MakeIssuePointer(binding.Set, binding.Name);
					Utils::AddIssue(issues, pointer, "{} of '{}' and {} of '{}' occupy the same binding with different declarations",
						Utils::DescribeBinding(*merged.Binding), merged.Entry, Utils::DescribeBinding(binding), entry);
				}
				if (binding.Used)
				{
					merged.UsedBy = merged.UsedBy | (*reflection)->Stage;
					merged.IsUsed = true;
				}
			}
		}

		// The layout items, by set and binding.
		if (description.BindingLayouts.size() > nvrhi::c_MaxBindingLayouts)
		{
			Utils::AddIssue(issues, "", "{} binding layouts exceed NVRHI's limit of {}",
				description.BindingLayouts.size(), nvrhi::c_MaxBindingLayouts);
		}
		std::set<uint32_t> sets;
		std::map<BindingKey, DeclaredItem> declared;
		std::optional<DeclaredItem> declaredPushConstants;
		for (const nvrhi::BindingLayoutDesc* layoutInSetOrder : Utils::SortBySet(description.BindingLayouts))
		{
			const nvrhi::BindingLayoutDesc& layout = *layoutInSetOrder;
			const uint32_t set = layout.registerSpace;
			const std::string setPointer = std::format("/{}", set);
			if (!layout.registerSpaceIsDescriptorSet)
			{
				Utils::AddIssue(issues, setPointer,
					"the layout of register space {} must set registerSpaceIsDescriptorSet, so that space {} is descriptor set {} (§8.4)",
					set, set, set);
			}
			if (set >= nvrhi::c_MaxBindingLayouts)
			{
				Utils::AddIssue(issues, setPointer, "descriptor set {} is beyond NVRHI's limit of {} sets",
					set, nvrhi::c_MaxBindingLayouts);
			}
			if (!sets.insert(set).second)
			{
				Utils::AddIssue(issues, setPointer, "two layouts declare descriptor set {}", set);
				continue;
			}

			for (const nvrhi::BindingLayoutItem& item : layout.bindings)
			{
				const std::optional<ShaderBindingKind> kind = Utils::GetBindingKind(item.type);
				if (!kind)
				{
					const std::string pointer = Utils::MakeIssuePointer(set, std::format("slot{}", item.slot));
					Utils::AddIssue(issues, pointer, "the layout of set {} declares a {} item, which is not part of the binding model of §8.4",
						set, Utils::ResourceTypeToString(item.type));
					continue;
				}

				DeclaredItem declaredItem{ .Item = item, .Set = set, .Kind = *kind, .Visibility = layout.visibility };
				if (*kind == ShaderBindingKind::PushConstantBuffer)
				{
					if (declaredPushConstants)
					{
						Utils::AddIssue(issues, Utils::MakeIssuePointer(set, "PushConstants"),
							"more than one layout item declares push constants; a pipeline has one push-constant range");
					}
					else
					{
						declaredPushConstants = declaredItem;
					}
					continue;
				}

				declaredItem.VulkanBinding = Utils::GetBindingOffset(layout.bindingOffsets, *kind) + item.slot;
				if (!declared.emplace(BindingKey(set, declaredItem.VulkanBinding), declaredItem).second)
				{
					Utils::AddIssue(issues, Utils::MakeItemPointer(declaredItem), "two items of the layout of set {} occupy Vulkan binding {}", set,
						declaredItem.VulkanBinding);
				}
			}
		}

		// Every reflected descriptor against its layout item; the used ones must have one.
		std::vector<bool> matchedStorageImages(description.StorageImages.size(), false);
		std::vector<bool> matchedConstantBuffers(description.ConstantBuffers.size(), false);
		for (const auto& [key, merged] : reflected)
		{
			const ShaderBinding& binding = *merged.Binding;
			const std::string pointer = Utils::MakeIssuePointer(binding.Set, binding.Name);
			const auto found = declared.find(key);
			if (found == declared.end())
			{
				if (merged.IsUsed && sets.contains(binding.Set))
				{
					Utils::AddIssue(issues, pointer, "{} is used by the {} stage(s) but the layout of set {} does not declare it",
						Utils::DescribeBinding(binding), Utils::StagesToString(merged.UsedBy), binding.Set);
				}
				else if (merged.IsUsed)
				{
					Utils::AddIssue(issues, pointer, "{} is used by the {} stage(s) but no layout declares set {}",
						Utils::DescribeBinding(binding), Utils::StagesToString(merged.UsedBy), binding.Set);
				}
				continue;
			}

			DeclaredItem& item = found->second;
			item.IsMatched = true;
			Utils::CompareDescriptor(merged, item, pointer, issues);
			if (binding.Kind == ShaderBindingKind::UnorderedAccess && Utils::IsTextureShape(binding.Shape)
				&& item.Kind == ShaderBindingKind::UnorderedAccess)
			{
				Utils::CompareStorageFormat(binding, item, description, matchedStorageImages, pointer, issues);
			}
			if (binding.Kind == ShaderBindingKind::ConstantBuffer && item.Kind == ShaderBindingKind::ConstantBuffer)
				Utils::CompareConstantBufferSize(binding, item, description, matchedConstantBuffers, pointer, issues);
		}
		Utils::ComparePushConstants(pushConstants, declaredPushConstants, issues);

		// Layout items and storage formats that no shader declares.
		for (const DeclaredItem& item : declared | std::views::values)
		{
			if (!item.IsMatched)
			{
				Utils::AddIssue(issues, Utils::MakeItemPointer(item), "the layout of set {} declares {}, which no entry point of '{}' declares",
					item.Set, Utils::DescribeItem(item), description.Program);
			}
		}
		for (size_t index = 0; index < description.StorageImages.size(); ++index)
		{
			if (matchedStorageImages[index])
				continue;
			const StorageImageFormat& image = description.StorageImages[index];
			const std::string pointer = Utils::MakeIssuePointer(image.Set, std::format("u{}", image.Register));
			Utils::AddIssue(issues, pointer, "StorageImages gives {} for u{} of set {}, which is not a storage image of the layout and shaders",
				Utils::GetFormatName(image.Format), image.Register, image.Set);
		}
		for (size_t index = 0; index < description.ConstantBuffers.size(); ++index)
		{
			if (matchedConstantBuffers[index])
				continue;
			const ConstantBufferSize& buffer = description.ConstantBuffers[index];
			const std::string pointer = Utils::MakeIssuePointer(buffer.Set, std::format("b{}", buffer.Register));
			Utils::AddIssue(issues, pointer, "ConstantBuffers gives {} bytes for b{} of set {}, which is not a constant buffer of the layout and shaders",
				buffer.ByteSize, buffer.Register, buffer.Set);
		}

		if (issues.empty())
			return {};
		const size_t count = issues.size();
		Error error(ErrorCode::Validation, std::format("{} binding layout mismatch{} with the shader reflection", count, count == 1 ? "" : "es"));
		return std::unexpected(std::move(error).WithIssues(std::move(issues)).WithContext(context));
	}

	PipelineFactory::PipelineFactory(GraphicsDevice& device, ShaderLibrary& shaders)
		: m_Shaders(&shaders), m_Device(&device)
	{
	}

	PipelineFactory::~PipelineFactory() = default;

	Result<GraphicsPipeline> PipelineFactory::CreateGraphicsPipeline(const GraphicsPipelineSpecification& specification)
	{
		const PipelineLayoutDescription& layout = specification.Layout;
		if (layout.Entries.size() != 2)
		{
			return MakeError(ErrorCode::InvalidArgument, "the graphics pipeline '{}' needs a vertex and a fragment entry point, got {} entries",
				layout.Name, layout.Entries.size());
		}
		ENGINE_TRY(ValidatePipelineLayout(layout, *m_Shaders));
		ENGINE_TRY(Utils::CheckEntryStage(*m_Shaders, layout, 0, nvrhi::ShaderType::Vertex));
		ENGINE_TRY(Utils::CheckEntryStage(*m_Shaders, layout, 1, nvrhi::ShaderType::Pixel));

		const std::string context = std::format("while creating the graphics pipeline '{}'", layout.Name);
		ENGINE_TRY_ASSIGN(nvrhi::ShaderHandle vertexShader,
			WithContext(Utils::CreateEntryShader(*m_Device, *m_Shaders, layout, 0, specification.Specializations), context));
		ENGINE_TRY_ASSIGN(nvrhi::ShaderHandle pixelShader,
			WithContext(Utils::CreateEntryShader(*m_Device, *m_Shaders, layout, 1, specification.Specializations), context));
		ENGINE_TRY_ASSIGN(std::vector<nvrhi::BindingLayoutHandle> bindingLayouts, WithContext(Utils::CreateBindingLayouts(*m_Device, layout), context));

		nvrhi::GraphicsPipelineDesc desc;
		desc.primType = specification.Primitive;
		desc.VS = vertexShader;
		desc.PS = pixelShader;
		desc.renderState = specification.RenderState;
		for (const nvrhi::BindingLayoutHandle& bindingLayout : bindingLayouts)
			desc.bindingLayouts.push_back(bindingLayout);
		if (!specification.VertexAttributes.empty())
		{
			ENGINE_TRY_ASSIGN(desc.inputLayout, WithContext(m_Device->CreateInputLayout(specification.VertexAttributes, vertexShader.Get()), context));
		}

		ENGINE_TRY_ASSIGN(nvrhi::GraphicsPipelineHandle pipeline, WithContext(m_Device->CreateGraphicsPipeline(desc, specification.Framebuffer), context));
		return GraphicsPipeline{ .Pipeline = std::move(pipeline), .BindingLayouts = std::move(bindingLayouts) };
	}

	Result<ComputePipeline> PipelineFactory::CreateComputePipeline(const ComputePipelineSpecification& specification)
	{
		const PipelineLayoutDescription& layout = specification.Layout;
		if (layout.Entries.size() != 1)
		{
			return MakeError(ErrorCode::InvalidArgument, "the compute pipeline '{}' needs exactly one compute entry point, got {} entries",
				layout.Name, layout.Entries.size());
		}
		ENGINE_TRY(ValidatePipelineLayout(layout, *m_Shaders));
		ENGINE_TRY(Utils::CheckEntryStage(*m_Shaders, layout, 0, nvrhi::ShaderType::Compute));

		const std::string context = std::format("while creating the compute pipeline '{}'", layout.Name);
		ENGINE_TRY_ASSIGN(nvrhi::ShaderHandle computeShader,
			WithContext(Utils::CreateEntryShader(*m_Device, *m_Shaders, layout, 0, specification.Specializations), context));
		ENGINE_TRY_ASSIGN(std::vector<nvrhi::BindingLayoutHandle> bindingLayouts, WithContext(Utils::CreateBindingLayouts(*m_Device, layout), context));

		nvrhi::ComputePipelineDesc desc;
		desc.CS = computeShader;
		for (const nvrhi::BindingLayoutHandle& bindingLayout : bindingLayouts)
			desc.bindingLayouts.push_back(bindingLayout);

		ENGINE_TRY_ASSIGN(nvrhi::ComputePipelineHandle pipeline, WithContext(m_Device->CreateComputePipeline(desc), context));
		return ComputePipeline{ .Pipeline = std::move(pipeline), .BindingLayouts = std::move(bindingLayouts) };
	}

}
