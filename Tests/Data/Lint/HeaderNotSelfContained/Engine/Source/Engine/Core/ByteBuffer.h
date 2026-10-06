#pragma once

namespace Engine {

	// Seeded defect: uses std::vector without including <vector>.
	struct ByteBuffer
	{
		std::vector<unsigned char> Bytes;
	};

}
