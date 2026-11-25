#pragma once

#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <functional>
#include <utility>

// Out-of-memory textures for the tests of a texture owner's failure path (M45, W45.2; Docs/Decisions/0063-m45-sky-and-time-of-day.md):
// the engine's own fault before NVRHI is called (GpuDiagnostics::SetTextureCreationFault), since a failed vkCreateImage
// trips NVRHI's assertion in createTexture in a Debug build, which a Vulkan-level injection such as
// Support/BufferCreationFails.h's would reach.

namespace Engine {

	namespace Test {

		// While alive, `device`'s texture creations that `fails` selects fail with Gpu before NVRHI is called, and every other
		// one is created. At most one at a time per device; main thread, around the calls under test; not copyable or movable.
		class TextureCreationFails
		{
		public:
			using Selector = std::function<bool(const nvrhi::TextureDesc& desc)>;

			TextureCreationFails(GraphicsDevice& device, Selector fails)
				: m_Diagnostics(&device.GetDiagnostics()), m_Fails(std::move(fails))
			{
				m_Diagnostics->SetTextureCreationFault([this](const nvrhi::TextureDesc& desc)
				{
					if (!m_Fails(desc))
						return false;
					++m_FailedCount;
					return true;
				});
			}

			~TextureCreationFails() { m_Diagnostics->SetTextureCreationFault({}); }

			TextureCreationFails(const TextureCreationFails&) = delete;
			TextureCreationFails& operator=(const TextureCreationFails&) = delete;
			TextureCreationFails(TextureCreationFails&&) = delete;
			TextureCreationFails& operator=(TextureCreationFails&&) = delete;

			// The creations it failed so far.
			[[nodiscard]] uint32_t GetFailedCount() const { return m_FailedCount; }
		private:
			GpuDiagnostics* m_Diagnostics = nullptr; // documented back-reference: the device outlives the guard
			Selector m_Fails{};
			uint32_t m_FailedCount = 0;
		};

	}

}
