#pragma once

#include "Engine/Renderer/RenderSnapshot.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

namespace Engine {

	namespace Detail {

		[[nodiscard]] bool IsDepthGtaoCameraValid(const CameraData& camera);
		[[nodiscard]] bool IsDepthGtaoTexture2D(const nvrhi::TextureDesc& desc, uint32_t width, uint32_t height,
			nvrhi::Format format, uint32_t mipCount);
		[[nodiscard]] bool IsDepthGtaoTarget(const nvrhi::TextureDesc& desc, const nvrhi::TextureDesc& expected);

	}

}
