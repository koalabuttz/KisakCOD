#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <utility>

namespace kisak::window {
struct PixelExtent { int width,height; };
// Rows contain RGBA bytes (not endian-dependent packed integers), top to bottom.
// Borrowed storage must remain valid and unchanged through synchronous Present.
// Pitch includes optional row padding; only width*4 bytes per row are consumed.
struct PixelBufferView {
    int width=0,height=0;
    size_t pitch=0;
    std::span<const uint8_t> bytes;
};
void ValidatePixelBuffer(PixelBufferView frame);

// Renderer-independent CPU resource. Resize invalidates borrowed spans/views.
// Invalid sizes/allocation failure preserve the previous resource; same-size
// resize preserves storage and contents. Moves leave an empty, reusable source;
// self-move preserves the resource. No SDL or game renderer is owned here.
class PixelBuffer {
public:
    PixelBuffer()=default;
    PixelBuffer(const PixelBuffer&)=delete;
    PixelBuffer& operator=(const PixelBuffer&)=delete;
    PixelBuffer(PixelBuffer&& other) noexcept
        : width_(std::exchange(other.width_,0)),height_(std::exchange(other.height_,0)),bytes_(std::move(other.bytes_)) {}
    PixelBuffer& operator=(PixelBuffer&& other) noexcept {
        if(this!=&other) {
            bytes_=std::move(other.bytes_);
            width_=std::exchange(other.width_,0);height_=std::exchange(other.height_,0);
        }
        return *this;
    }
    bool Resize(int width,int height);
    PixelExtent Extent() const { return {width_,height_}; }
    std::span<uint8_t> Bytes() { return bytes_; }
    PixelBufferView View() const { return {width_,height_,size_t(width_)*4,bytes_}; }
private:
    int width_=0,height_=0;
    std::vector<uint8_t> bytes_;
};
}
