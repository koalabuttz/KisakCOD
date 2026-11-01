#include "pixel_buffer.h"
#include <climits>
#include <limits>
#include <stdexcept>

namespace kisak::window {
namespace {
size_t RowBytes(int width,int height) {
    // Match the window host's existing maximum dimensions. This also caps an
    // owned tightly packed allocation at 256 MiB, including on 32-bit builds.
    if(width<1 || height<1 || width>8192 || height>8192)
        throw std::invalid_argument("Pixel dimensions must be between 1 and 8192");
    return size_t(width)*4;
}
}
void ValidatePixelBuffer(PixelBufferView frame) {
    const size_t row=RowBytes(frame.width,frame.height);
    if(frame.pitch<row || frame.pitch>INT_MAX)
        throw std::invalid_argument("Pixel pitch must fit SDL and cover one RGBA row");
    const size_t rows=size_t(frame.height-1);
    if(rows && frame.pitch>(std::numeric_limits<size_t>::max()-row)/rows)
        throw std::invalid_argument("Pixel row span overflows addressable storage");
    const size_t needed=rows*frame.pitch+row;
    if(!frame.bytes.data() || frame.bytes.size()<needed)
        throw std::invalid_argument("Pixel storage does not cover all rows");
}
bool PixelBuffer::Resize(int width,int height) {
    const size_t row=RowBytes(width,height);
    if(width==width_ && height==height_)return false;
    if(size_t(height)>std::numeric_limits<size_t>::max()/row)
        throw std::invalid_argument("Pixel allocation exceeds addressable storage");
    std::vector<uint8_t> replacement(row*size_t(height));
    bytes_.swap(replacement);width_=width;height_=height;
    return true;
}
}
