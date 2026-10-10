#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>

namespace Engine {

	struct ScriptCall;

	namespace Detail {

		// One allowance per native input conversion, shared by every recursive branch and Variant. Fixed charges
		// bound temporary native containers/copies independently of STL layout; this is not a VM allocator breach.
		class NativeConversionBudget
		{
		public:
			static constexpr uint64_t MaximumBytes = 4 * 1024 * 1024;
			static constexpr uint64_t ValueBytes = 256;
			static constexpr uint64_t EntryBytes = 256;
			static constexpr uint64_t StringByteCopies = 4;

			explicit NativeConversionBudget(ScriptCall& call);
			[[nodiscard]] Status Poll() const;
			[[nodiscard]] Status Charge(uint64_t count, uint64_t bytesPerItem = 1);
			// Raising forms are for binding callbacks, under the existing Luau protected-call boundary.
			void Check(uint64_t count, uint64_t bytesPerItem = 1);
			void CheckDeadline() const;
			[[nodiscard]] std::string ReadString(int index);
		private:
			ScriptCall& m_Call; // Borrowed for this conversion only, never retained by the VM.
			uint64_t m_UsedBytes = 0;
		};

	}

}
