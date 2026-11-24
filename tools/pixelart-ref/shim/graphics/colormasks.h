// Stand-in for ScummVM's graphics/colormasks.h: only the 32-bit ARGB8888 mask the harness uses.
#pragma once
#include "common/scummsys.h"
namespace Graphics {
template<int N> struct ColorMasks;
template<> struct ColorMasks<8888> {
    enum : uint32 {
        kBytesPerPixel = 4,
        kAlphaBits = 8, kRedBits = 8, kGreenBits = 8, kBlueBits = 8,
        kAlphaShift = 24, kRedShift = 16, kGreenShift = 8, kBlueShift = 0,
        kAlphaMask = 0xFF000000u, kRedMask = 0x00FF0000u, kGreenMask = 0x0000FF00u, kBlueMask = 0x000000FFu,
        kRedBlueMask = kRedMask | kBlueMask,
        kLowBits = 0x01010101u, kLow2Bits = 0x03030303u, kLow3Bits = 0x07070707u, kLow4Bits = 0x0F0F0F0Fu,
        kLowBitsMask = kLowBits, kHighBitsMask = ~kLowBits,
        qlowBits = kLow2Bits, qhighBits = ~kLow2Bits,
    };
    typedef uint32 PixelType;
};
}
