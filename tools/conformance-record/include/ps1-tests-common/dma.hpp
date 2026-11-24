/*
 * Stands in for ps1-tests' common/dma.hpp, which its later common/gpu.cpp
 * includes for its DMA helpers (vramWriteDMA, vramReadDMA). No recorded
 * program calls them and the recorder does not model DMA register
 * transfers, so this gives only the names those helpers use, and the
 * first of them they call stops the recording.
 */
#ifndef JBREC_PS1_TESTS_DMA_HPP
#define JBREC_PS1_TESTS_DMA_HPP

#include <stdint.h>

#include "io.h"

namespace DMA {

enum class Channel { GPU = 2 };

inline void refused()
{
    jbrec_unsupported("ps1-tests' DMA helpers");
}

inline void masterEnable(Channel, bool) { refused(); }
inline void waitForChannel(Channel) { refused(); }
inline uint32_t baseAddr(Channel) { return refused(), 0; }
inline uint32_t blockAddr(Channel) { return refused(), 0; }
inline uint32_t controlAddr(Channel) { return refused(), 0; }

/* A register's value; the helpers build them and write _reg. */
struct Word {
    uint32_t _reg;
};
struct MADDR : Word {
    MADDR(uint32_t) : Word{0} { refused(); }
};
struct BCR : Word {
    static BCR mode1(uint16_t, uint16_t) { return refused(), BCR{}; }
};
struct CHCR : Word {
    static CHCR VRAMwrite() { return refused(), CHCR{}; }
    static CHCR VRAMread() { return refused(), CHCR{}; }
};

} // namespace DMA

#endif
