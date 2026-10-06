#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/ShaderLibrary.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Pipelines checked against shader reflection (Architecture §8.4, §8.12). Every C++ BindingLayoutDesc is compared with the
// reflection JSON of the pipeline's shaders (set, binding, resource type, array size, constant-buffer and push-constant
// size, and the storage format of every storage image); a mismatch is a load error, never a silent binding bug. The check
// needs no GPU: "Shaders: LayoutsMatchReflection" runs ValidatePipelineLayout over the layout description of every engine
// pipeline in the CPU-only unit suite, and PipelineFactory runs it again before every creation.

namespace Engine {

	class GraphicsDevice;

	// The format a pass creates for one of its storage images: the binding's reflected [vk::image_format] must match it
	// (§8.4: "checks the reflected image format against the texture format the C++ pass creates").
	struct StorageImageFormat
	{
		uint32_t Set = 0;      // register space
		uint32_t Register = 0; // the u register (BindingLayoutItem::slot)
		nvrhi::Format Format = nvrhi::Format::UNKNOWN;
	};

	// Everything about a pipeline that the reflection check needs, available without a device. Each pass provides one
	// through a static function (TrianglePass::GetLayoutDescription, ImGuiRenderer::GetLayoutDescription), so the CPU test
	// can check every pipeline.
	struct PipelineLayoutDescription
	{
		std::string Name{};                 // for messages: "Triangle"
		std::string Program{};              // a program of Shaders.json
		std::vector<std::string> Entries{}; // the entry points of its stages: vertex then fragment, or the compute entry
		std::vector<ShaderDefine> Permutation{};
		// One layout per descriptor set the shaders use, each with registerSpace = its set and registerSpaceIsDescriptorSet =
		// true (§8.4). Push constants are a BindingLayoutItem::PushConstants of the set-0 layout.
		std::vector<nvrhi::BindingLayoutDesc> BindingLayouts{};
		std::vector<StorageImageFormat> StorageImages{};
	};

	// The reflection check (CPU only): every binding any of the entries uses must be declared by the layout of its set with
	// the same register class, shift-adjusted binding number and array size; every constant buffer's size and the push
	// constants' size must equal the reflected struct size; every layout item must exist in some entry's reflection; every
	// storage image must have a StorageImages entry whose format equals its reflected [vk::image_format]; and every layout
	// must set registerSpaceIsDescriptorSet. Errors: Validation listing every mismatch as an ErrorIssue (JSON pointer
	// "/<set>/<binding name>") with the pipeline's name as context; those of ShaderLibrary::GetReflection.
	[[nodiscard]] Status ValidatePipelineLayout(const PipelineLayoutDescription& description, ShaderLibrary& shaders);

	struct GraphicsPipelineSpecification
	{
		PipelineLayoutDescription Layout{}; // Entries: { vertex entry, fragment entry }
		// Vulkan specialization constants for small pipeline toggles (§8.5: the debug views), applied to every stage's shader
		// through GraphicsDevice::CreateShaderSpecialization; a stage ignores the constant IDs it does not declare. Empty:
		// the shaders as compiled.
		std::vector<nvrhi::ShaderSpecialization> Specializations{};
		std::vector<nvrhi::VertexAttributeDesc> VertexAttributes{}; // empty: no input layout (vertices from SV_VertexID)
		nvrhi::PrimitiveType Primitive = nvrhi::PrimitiveType::TriangleList;
		// Rasterizer, blend and depth-stencil state. Engine pipelines set rasterState.frontCounterClockwise = true, because the
		// projection flips Y for Vulkan clip space and glTF front faces are counter-clockwise (§8.3).
		nvrhi::RenderState RenderState{};
		// The target's formats and sample count.
		nvrhi::FramebufferInfo Framebuffer{};
	};

	struct ComputePipelineSpecification
	{
		PipelineLayoutDescription Layout{}; // Entries: { compute entry }
		// As GraphicsPipelineSpecification::Specializations, for the compute shader.
		std::vector<nvrhi::ShaderSpecialization> Specializations{};
	};

	// A created pipeline and the binding layouts it was created with (one per set, in set order), which binding sets for it
	// must use.
	struct GraphicsPipeline
	{
		nvrhi::GraphicsPipelineHandle Pipeline{};
		std::vector<nvrhi::BindingLayoutHandle> BindingLayouts{};
	};

	struct ComputePipeline
	{
		nvrhi::ComputePipelineHandle Pipeline{};
		std::vector<nvrhi::BindingLayoutHandle> BindingLayouts{};
	};

	// Not copyable or movable; main thread only (§4.11). All engine pipelines are created at startup (§8.12).
	class PipelineFactory
	{
	public:
		// `device` and `shaders` are documented back-references that must outlive the factory.
		PipelineFactory(GraphicsDevice& device, ShaderLibrary& shaders);
		~PipelineFactory();

		PipelineFactory(const PipelineFactory&) = delete;
		PipelineFactory& operator=(const PipelineFactory&) = delete;

		// ValidatePipelineLayout, then the shaders (ShaderLibrary::Get, specialized with GraphicsDevice::
		// CreateShaderSpecialization when Specializations is not empty), the binding layouts, the input layout and the
		// pipeline through the GraphicsDevice wrappers. Errors: those of ValidatePipelineLayout and ShaderLibrary::Get;
		// InvalidArgument for a wrong number of entries or an entry whose stage does not fit its position; Gpu when a
		// creation fails (callers creating at startup treat it as FatalError(OutOfMemory), §8.14 item 7).
		[[nodiscard]] Result<GraphicsPipeline> CreateGraphicsPipeline(const GraphicsPipelineSpecification& specification);
		[[nodiscard]] Result<ComputePipeline> CreateComputePipeline(const ComputePipelineSpecification& specification);

		[[nodiscard]] ShaderLibrary& GetShaderLibrary() { return *m_Shaders; }
	private:
		ShaderLibrary* m_Shaders = nullptr; // documented back-reference
	};

}
