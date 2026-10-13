#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

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
			// For a value the host retains (ScriptLimits.h): bounds the total bytes of the retained text this conversion
			// copies, that is ReadRetainedString strings and every JSON string and object key. Exceeding it is a Script error
			// naming fieldName and the limit, raised before the text is copied. Without a call the text is unbounded.
			void LimitRetainedText(uint64_t maximumBytes, std::string_view fieldName);
			// Counts length bytes of retained text against the limit above; the copy allowance is charged separately.
			[[nodiscard]] Status ChargeRetainedText(uint64_t length);
			// ReadString for text the converted value keeps (a string value or map key), counted by ChargeRetainedText.
			[[nodiscard]] std::string ReadRetainedString(int index);
		private:
			ScriptCall& m_Call; // Borrowed for this conversion only, never retained by the VM.
			uint64_t m_UsedBytes = 0;
			uint64_t m_RetainedBytes = 0;
			uint64_t m_RetainedLimit = std::numeric_limits<uint64_t>::max();
			std::string m_RetainedField{};
		};

	}

}
