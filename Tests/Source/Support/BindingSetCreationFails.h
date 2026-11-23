#pragma once

#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <functional>
#include <utility>

// Out-of-memory binding sets for the tests of a binding-set owner's failure path (M42, W42.2's review,
// Docs/Decisions/0059-m42-planar-reflections.md): the engine's own fault before NVRHI is called
// (GpuDiagnostics::SetBindingSetCreationFault), since NVRHI's createBindingSet leaks the set it allocated when its
// descriptor pool or set allocation fails (Vendor/NVRHI/VENDOR.md), which a Vulkan-level injection such as
// Support/BufferCreationFails.h's would reach.

namespace Engine {

	namespace Test {

		// While alive, `device`'s binding-set creations that `fails` selects fail with Gpu before NVRHI is called, and every
		// other one is created. At most one at a time per device; main thread, around the calls under test; not copyable or
		// movable.
		class BindingSetCreationFails
		{
		public:
			using Selector = std::function<bool(const nvrhi::BindingSetDesc& desc, const nvrhi::IBindingLayout& layout)>;

			BindingSetCreationFails(GraphicsDevice& device, Selector fails)
				: m_Diagnostics(&device.GetDiagnostics()), m_Fails(std::move(fails))
			{
				m_Diagnostics->SetBindingSetCreationFault([this](const nvrhi::BindingSetDesc& desc, const nvrhi::IBindingLayout& layout)
				{
					if (!m_Fails(desc, layout))
						return false;
					++m_FailedCount;
					return true;
				});
			}

			~BindingSetCreationFails() { m_Diagnostics->SetBindingSetCreationFault({}); }

			BindingSetCreationFails(const BindingSetCreationFails&) = delete;
			BindingSetCreationFails& operator=(const BindingSetCreationFails&) = delete;
			BindingSetCreationFails(BindingSetCreationFails&&) = delete;
			BindingSetCreationFails& operator=(BindingSetCreationFails&&) = delete;

			// The creations it failed so far.
			[[nodiscard]] uint32_t GetFailedCount() const { return m_FailedCount; }
		private:
			GpuDiagnostics* m_Diagnostics = nullptr; // documented back-reference: the device outlives the guard
			Selector m_Fails{};
			uint32_t m_FailedCount = 0;
		};

	}

}
