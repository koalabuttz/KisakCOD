// ---------------------------------------------------------------------------
// Implementation of the bounded image payload capture. No engine header is included
// here: the header fields are read out of the 16 bytes the loader copied in, so the
// unit builds and tests without an asset tree.
// ---------------------------------------------------------------------------

#include "world_image_capture.h"

#include <cstring>
#include <new>

namespace kisak_world_capture
{
namespace
{

// GfxImageLoadDef fields are streamed from a little-endian fast file; read them byte
// by byte so neither alignment nor aliasing decides what the header says.
int16_t LoadI16(const unsigned char *bytes)
{
    return int16_t(uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8));
}

int32_t LoadI32(const unsigned char *bytes)
{
    return int32_t(uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
                   (uint32_t(bytes[3]) << 24));
}

ImageMeta ParseMeta(const unsigned char *header)
{
    ImageMeta meta;
    meta.levelCount = int(header[0]);
    meta.flags = int(header[1]);
    meta.width = int(LoadI16(header + 2));
    meta.height = int(LoadI16(header + 4));
    meta.depth = int(LoadI16(header + 6));
    meta.format = LoadI32(header + 8);
    meta.resourceSize = int64_t(LoadI32(header + 12));
    return meta;
}

bool HasLightmapPrefix(const char *name)
{
    constexpr size_t prefixBytes = sizeof(kNamePrefix) - 1;
    return std::strncmp(name, kNamePrefix, prefixBytes) == 0;
}

// True when the name fits, and copies it. A name that does not fit is refused rather
// than shortened, because a shortened name is a different resource.
bool CopyName(char out[kMaxNameBytes], const char *name) noexcept
{
    if (!name)
        return false;
    const size_t length = strnlen(name, kMaxNameBytes);
    if (length >= kMaxNameBytes)
        return false;
    std::memcpy(out, name, length + 1);
    return true;
}

// Every field the payload's layout depends on, checked against the layout the engine itself
// consumes (r_image.cpp:327-352). A header that fails here is not truncated or guessed at:
// the image is refused with the fields it carried, so the next run can read why.
bool LayoutIsComplete(const ImageMeta &meta, bool hasPayload, CaptureOutcome *outcome) noexcept
{
    if (!outcome)
        return false;
    if (meta.width < 1 || meta.height < 1 || meta.width > kMaxDimension || meta.height > kMaxDimension)
    {
        *outcome = CaptureOutcome::MalformedHeader;
        return false;
    }
    if (meta.resourceSize <= 0)
    {
        *outcome = CaptureOutcome::MalformedHeader;
        return false;
    }
    if (!IsSupportedTextureFormat(meta.format))
    {
        *outcome = CaptureOutcome::UnsupportedFormat;
        return false;
    }
    // CUBEMAP and VOLMAP say these bytes are not a 2D image's, and the remaining bits are
    // sampling or picmip hints that do not move a byte.
    if ((meta.flags & ~kAllowed2DFlags) != 0)
    {
        *outcome = CaptureOutcome::FlagsContradicted;
        return false;
    }
    // IMG_FLAG_NOMIPMAPS is the engine's "one level" answer, so a header that claims several
    // levels while setting it is describing two different images.
    if ((meta.flags & kImageFlagNoMipmaps) != 0 && meta.levelCount != 1)
    {
        *outcome = CaptureOutcome::FlagsContradicted;
        return false;
    }
    if (meta.levelCount < 1 || unsigned(meta.levelCount) > kMaxMipLevels)
    {
        *outcome = CaptureOutcome::ImpossibleMipCount;
        return false;
    }
    if (unsigned(meta.levelCount) > MaxMipLevels(meta.width, meta.height))
    {
        *outcome = CaptureOutcome::ImpossibleMipCount;
        return false;
    }
    // A 2D image has one slice. Depth 0 is "not stated" and no more usable than depth 2.
    if (meta.depth != 1)
    {
        *outcome = CaptureOutcome::DepthNotFlat;
        return false;
    }
    // A mip chain is only sized exactly on power-of-two dimensions: the engine takes its level
    // count from Image_CountMipmaps but sizes each level with the clamped halving, so on a 5x5
    // image it walks four levels while Expected2DPayloadBytes sums three (see the header). A
    // single level has one agreed layout at any dimensions, so IMG_FLAG_NOMIPMAPS is admitted
    // as is. Refusing beats comparing against a number only one of the engine's two functions
    // agrees with.
    if (!LayoutBytesAreExact(meta.flags, meta.width, meta.height))
    {
        *outcome = CaptureOutcome::UnsupportedLayout;
        return false;
    }
    if (!hasPayload)
    {
        *outcome = CaptureOutcome::MalformedHeader;
        return false;
    }
    // The exact size, not a range: Load_Texture walks the mip chain and asserts the walk
    // ends at resourceSize (r_image.cpp:352), so any other size is a payload this gate
    // cannot claim to describe.
    uint64_t expected = 0;
    if (!Expected2DPayloadBytes(meta.format, meta.flags, meta.width, meta.height, &expected))
    {
        *outcome = CaptureOutcome::MalformedHeader;
        return false;
    }
    if (uint64_t(meta.resourceSize) != expected)
    {
        *outcome = CaptureOutcome::SizeMismatch;
        return false;
    }
    return true;
}

bool SameMeta(const ImageMeta &left, const ImageMeta &right) noexcept
{
    return left.levelCount == right.levelCount && left.flags == right.flags && left.width == right.width &&
           left.height == right.height && left.depth == right.depth && left.format == right.format &&
           left.resourceSize == right.resourceSize;
}

bool IsPowerOfTwo(unsigned value) noexcept
{
    return value != 0 && (value & (value - 1)) == 0;
}

} // namespace

bool IsSupportedTextureFormat(int32_t format)
{
    switch (format)
    {
    // 32-bit ARGB, the uncompressed layout packaged lightmaps use.
    case kFormatA8R8G8B8:
    // The block-compressed set. DXT2 and DXT4 are premultiplied variants of DXT3 and DXT5
    // with the same block size, but the engine's own sizer names only these three, so a
    // header carrying one of the other two is refused rather than sized by analogy.
    case FourCC('D', 'X', 'T', '1'):
    case FourCC('D', 'X', 'T', '3'):
    case FourCC('D', 'X', 'T', '5'):
        return true;
    default:
        // Everything else - including D3DFMT_UNKNOWN, the other uncompressed formats, the
        // depth/stencil and vertex/index formats - has no byte layout this gate checks, and
        // an unchecked layout is what a size claim cannot survive.
        return false;
    }
}

// src/gfx_d3d/r_image_load_obj.cpp:81-135. The compressed formats are 8 or 16 bytes per
// 4x4 block, so a level is sized in whole blocks; ARGB is four bytes per pixel. Both are
// multiplied by depth, which a 2D payload holds at 1.
bool MipLevelBytes(int32_t format, uint64_t width, uint64_t height, uint64_t depth, uint64_t *bytes) noexcept
{
    if (!bytes)
        return false;
    *bytes = 0;
    switch (format)
    {
    case kFormatA8R8G8B8:
        *bytes = 4 * depth * height * width;
        return true;
    case FourCC('D', 'X', 'T', '1'):
        *bytes = 8 * depth * ((height + 3) >> 2) * ((width + 3) >> 2);
        return true;
    case FourCC('D', 'X', 'T', '3'):
    case FourCC('D', 'X', 'T', '5'):
        *bytes = 16 * depth * ((height + 3) >> 2) * ((width + 3) >> 2);
        return true;
    default:
        return false;
    }
}

// src/gfx_d3d/r_image_load_obj.cpp:137-175 with the depth of a 2D image: the first level,
// then every halving until the level is 1x1. A 4x4 DXT1 image is three levels of one block
// each, 24 bytes; with IMG_FLAG_NOMIPMAPS it is one level, 8 bytes.
bool Expected2DPayloadBytes(int32_t format, int flags, int width, int height, uint64_t *bytes) noexcept
{
    if (!bytes || width < 1 || height < 1)
        return false;
    uint64_t total = 0;
    if (!MipLevelBytes(format, uint64_t(width), uint64_t(height), 1, &total))
        return false;
    if ((flags & kImageFlagNoMipmaps) != 0)
    {
        *bytes = total;
        return true;
    }
    uint64_t levelWidth = uint64_t(width);
    uint64_t levelHeight = uint64_t(height);
    while (levelWidth + levelHeight + 1 > 3)
    {
        // The engine's own halving: below 2 the level stays 1, it never reaches 0.
        levelWidth = levelWidth >> 1 > 1 ? levelWidth >> 1 : 1;
        levelHeight = levelHeight >> 1 > 1 ? levelHeight >> 1 : 1;
        uint64_t levelBytes = 0;
        if (!MipLevelBytes(format, levelWidth, levelHeight, 1, &levelBytes))
            return false;
        total += levelBytes;
    }
    *bytes = total;
    return true;
}

bool LayoutBytesAreExact(int flags, int width, int height) noexcept
{
    // One level is one level's bytes: the block rounding and the pixel count are the whole sum,
    // so nothing about the chain can be misread.
    if ((flags & kImageFlagNoMipmaps) != 0)
        return true;
    if (width < 1 || height < 1)
        return false;
    return IsPowerOfTwo(unsigned(width)) && IsPowerOfTwo(unsigned(height));
}

unsigned MaxMipLevels(int width, int height) noexcept
{
    if (width < 1 && height < 1)
        return 0;
    unsigned levels = 1;
    uint64_t largest = uint64_t(width > height ? width : height);
    for (uint64_t resolution = 1; resolution < largest; resolution *= 2)
        ++levels;
    return levels;
}

const char *CaptureOutcomeName(CaptureOutcome outcome)
{
    switch (outcome)
    {
    case CaptureOutcome::NotArmed: return "not_armed";
    case CaptureOutcome::NameNotMatched: return "name_not_matched";
    case CaptureOutcome::NameTooLong: return "name_too_long";
    case CaptureOutcome::MapTypeUnsupported: return "map_type_unsupported";
    case CaptureOutcome::MalformedHeader: return "malformed_header";
    case CaptureOutcome::UnsupportedFormat: return "unsupported_format";
    case CaptureOutcome::FlagsContradicted: return "flags_contradicted";
    case CaptureOutcome::ImpossibleMipCount: return "impossible_mip_count";
    case CaptureOutcome::UnsupportedLayout: return "unsupported_layout";
    case CaptureOutcome::DepthNotFlat: return "depth_not_flat";
    case CaptureOutcome::SizeMismatch: return "size_mismatch";
    case CaptureOutcome::OverflowBytes: return "overflow_bytes";
    case CaptureOutcome::OverflowCount: return "overflow_count";
    case CaptureOutcome::Duplicate: return "duplicate";
    case CaptureOutcome::AmbiguousName: return "ambiguous_name";
    case CaptureOutcome::InternalError: return "internal_error";
    case CaptureOutcome::Captured: return "captured";
    }
    return "malformed_header";
}

uint64_t Fnv1a64(const void *data, size_t bytes) noexcept
{
    uint64_t hash = 14695981039346656037ull;
    const auto *values = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < bytes; ++i)
    {
        hash ^= values[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string ResourceKey(int lightmapIndex, int role)
{
    if (lightmapIndex < 0)
        return std::string();
    return std::string("lightmap") + std::to_string(lightmapIndex) + (role == kRoleSecondary ? ".secondary" : ".primary");
}

CaptureTable &CaptureTable::Instance() noexcept
{
    static CaptureTable table;
    return table;
}

CaptureTable::~CaptureTable()
{
    delete[] arena_;
}

// The arena is reserved once, when the run arms the capture, so the loader-side call
// below never allocates: a copy that cannot fit the arena is refused at the bound
// rather than failing on the heap halfway through a zone load.
bool CaptureTable::Arm() noexcept
{
    std::lock_guard<std::mutex> guard(lock_);
    delete[] arena_;
    arena_ = nullptr;
    armed_ = false;
    overflow_ = false;
    namesUntracked_ = false;
    imageCount_ = 0;
    rejectionCount_ = 0;
    trackedCount_ = 0;
    examined_ = 0;
    matched_ = 0;
    captured_ = 0;
    duplicateCount_ = 0;
    rejectedCount_ = 0;
    ambiguousCount_ = 0;
    bytesCaptured_ = 0;
    lastOutcome_ = CaptureOutcome::NotArmed;
    for (unsigned i = 0; i < kMaxImages; ++i)
        images_[i] = CapturedImage();
    for (unsigned i = 0; i < kMaxRejections; ++i)
        rejections_[i] = RejectedImage();
    for (unsigned i = 0; i < kMaxTrackedNames; ++i)
        tracked_[i] = SeenName();
    arena_ = new (std::nothrow) unsigned char[size_t(kMaxTotalBytes)];
    armed_ = arena_ != nullptr;
    return armed_;
}

void CaptureTable::Disarm() noexcept
{
    std::lock_guard<std::mutex> guard(lock_);
    armed_ = false;
}

bool CaptureTable::Armed() const noexcept
{
    std::lock_guard<std::mutex> guard(lock_);
    return armed_;
}

CaptureOutcome CaptureTable::Capture(const CaptureRequest &request) noexcept
{
    try
    {
        std::lock_guard<std::mutex> guard(lock_);
        return CaptureLocked(request);
    }
    catch (...)
    {
        // Nothing below is expected to throw - the arena is fixed and no string or
        // container is built here - but a lock or an unforeseen path must not carry an
        // exception across the database loader, so it ends here as a refusal.
        return CaptureOutcome::InternalError;
    }
}

CaptureOutcome CaptureTable::CaptureLocked(const CaptureRequest &request) noexcept
{
    // Disabled means no effect at all: no counter moves and no record is kept, so a
    // build that never arms the capture reports exactly what it did before.
    if (!armed_)
        return CaptureOutcome::NotArmed;

    ++examined_;

    if (!request.name)
    {
        ImageMeta empty;
        RejectLocked("", request.mapType, empty, CaptureOutcome::MalformedHeader);
        return CaptureOutcome::MalformedHeader;
    }
    // Out of scope by the filter, and the ordinary case: every embedded image in the
    // zone lands here, and none of them is copied or remembered.
    if (!HasLightmapPrefix(request.name))
        return lastOutcome_ = CaptureOutcome::NameNotMatched;

    ++matched_;

    const size_t nameLength = strnlen(request.name, kMaxNameBytes);
    if (nameLength >= kMaxNameBytes)
    {
        // A name that does not fit is a different resource from any name this table holds, so it
        // needs no entry: no stored name can ever equal it.
        RejectLocked(request.name, request.mapType, ImageMeta(), CaptureOutcome::NameTooLong);
        return CaptureOutcome::NameTooLong;
    }

    // ---- The name comes first. -----------------------------------------------------------
    // Whether one name names one image does not depend on whether either offer was admissible, so
    // the name is recorded before anything about this offer can refuse it. A refusal that came
    // first must claim the name exactly as a capture would, or a later admissible offer would
    // become resolvable by arriving second.
    SeenName *seen = nullptr;
    bool seenBefore = false;
    if (!TrackNameLocked(request.name, &seen, &seenBefore))
    {
        // No room to remember this name. Without that record the table could not tell this offer
        // from one that arrives later, so it is refused rather than captured unwatched - and the
        // run can no longer vouch for any name, because the offers it did not record are exactly
        // the ones it cannot rule out (see Lookup).
        overflow_ = true;
        namesUntracked_ = true;
        RejectLocked(request.name, request.mapType, ImageMeta(), CaptureOutcome::OverflowCount);
        return CaptureOutcome::OverflowCount;
    }

    // ---- What this offer claims, decided but not yet acted on. ---------------------------
    // Read here so a refusal can carry the fields it saw, and so the comparison below only ever
    // touches a payload whose size and format the header has already been checked against. An
    // inadmissible offer's bytes are never read.
    ImageMeta meta;
    CaptureOutcome refusal = CaptureOutcome::Captured;
    if (request.mapType != kMapType2D)
    {
        refusal = CaptureOutcome::MapTypeUnsupported;
    }
    else if (!request.header)
    {
        refusal = CaptureOutcome::MalformedHeader;
    }
    else
    {
        meta = ParseMeta(request.header);
        CaptureOutcome layoutOutcome = CaptureOutcome::MalformedHeader;
        if (!LayoutIsComplete(meta, request.payload != nullptr, &layoutOutcome))
        {
            refusal = layoutOutcome;
        }
        else if (uint64_t(meta.resourceSize) > kMaxImageBytes)
        {
            overflow_ = true;
            refusal = CaptureOutcome::OverflowBytes;
        }
    }
    const bool admissible = refusal == CaptureOutcome::Captured;

    // A name that is already claimed resolves nothing, whatever this offer is: a repeat cannot
    // revive it, which is the whole point of marking it.
    if (seen->poisoned)
    {
        RejectLocked(request.name, request.mapType, meta, CaptureOutcome::AmbiguousName);
        return lastOutcome_ = CaptureOutcome::AmbiguousName;
    }

    const CapturedImage *existing = FindImageLocked(request.name);
    if (existing || seenBefore)
    {
        // The same payload offered twice - a re-load, or two references to one record - is one image.
        // A name that comes back with different bytes or a different header is not: which of the two a
        // surface's resource pointed at cannot be known here, so the name is claimed and is never
        // looked up again. Resolving it by load order would export whichever copy happened to load
        // first.
        //
        // The bytes are compared every time, so an equal payload address proves nothing. That buffer
        // is the zone's stream position: the next record reuses the same address, and a header equal
        // to the stored one is no guide either, because the second record can describe an image of the
        // same format and dimensions with different pixels.
        const bool sameStoredImage =
            existing && admissible && SameMeta(existing->meta, meta) &&
            !std::memcmp(arena_ + size_t(existing->payloadOffset), request.payload, size_t(meta.resourceSize));
        if (sameStoredImage)
        {
            ++duplicateCount_;
            return lastOutcome_ = CaptureOutcome::Duplicate;
        }
        // Claimed, permanently - the check above proves this name was not claimed yet. An offer this
        // table refuses still holds the name, and the refusal is also why its bytes are unavailable
        // to compare against: two offers under one name is a conflict whether or not either of them
        // could have been captured.
        seen->poisoned = true;
        ++ambiguousCount_;
        // When the second offer has faults of its own, they are the more useful thing to report; the
        // name is claimed either way.
        const CaptureOutcome reason = admissible ? CaptureOutcome::AmbiguousName : refusal;
        RejectLocked(request.name, request.mapType, meta, reason);
        return lastOutcome_ = reason;
    }

    if (!admissible)
    {
        RejectLocked(request.name, request.mapType, meta, refusal);
        return lastOutcome_ = refusal;
    }

    const uint64_t payloadBytes = uint64_t(meta.resourceSize);
    if (imageCount_ >= kMaxImages)
    {
        overflow_ = true;
        RejectLocked(request.name, request.mapType, meta, CaptureOutcome::OverflowCount);
        return CaptureOutcome::OverflowCount;
    }
    if (bytesCaptured_ + payloadBytes > kMaxTotalBytes)
    {
        overflow_ = true;
        RejectLocked(request.name, request.mapType, meta, CaptureOutcome::OverflowBytes);
        return CaptureOutcome::OverflowBytes;
    }

    CapturedImage &image = images_[imageCount_];
    image = CapturedImage();
    if (!CopyName(image.name, request.name))
    {
        // Unreachable: the length was checked above. Refusing keeps the record empty
        // rather than storing a name that does not match the one asked for.
        RejectLocked(request.name, request.mapType, meta, CaptureOutcome::NameTooLong);
        return CaptureOutcome::NameTooLong;
    }
    image.mapType = request.mapType;
    image.meta = meta;
    image.payloadOffset = bytesCaptured_;
    image.payloadBytes = payloadBytes;
    std::memcpy(arena_ + size_t(bytesCaptured_), request.payload, size_t(payloadBytes));
    image.fnv1a64 = Fnv1a64(arena_ + size_t(bytesCaptured_), size_t(payloadBytes));

    ++imageCount_;
    ++captured_;
    bytesCaptured_ += payloadBytes;
    return lastOutcome_ = CaptureOutcome::Captured;
}

bool CaptureTable::TrackNameLocked(const char *name, SeenName **seen, bool *seenBefore) noexcept
{
    if (!name || !seen || !seenBefore)
        return false;
    for (unsigned i = 0; i < trackedCount_; ++i)
    {
        if (std::strcmp(tracked_[i].name, name))
            continue;
        *seen = &tracked_[i];
        *seenBefore = true;
        return true;
    }
    if (trackedCount_ >= kMaxTrackedNames)
        return false;
    SeenName &entry = tracked_[trackedCount_];
    entry = SeenName();
    if (!CopyName(entry.name, name))
        return false;
    *seen = &entry;
    *seenBefore = false;
    ++trackedCount_;
    return true;
}

const CaptureTable::SeenName *CaptureTable::FindSeenLocked(const char *name) const noexcept
{
    if (!name)
        return nullptr;
    for (unsigned i = 0; i < trackedCount_; ++i)
    {
        if (!std::strcmp(tracked_[i].name, name))
            return &tracked_[i];
    }
    return nullptr;
}

const CapturedImage *CaptureTable::FindImageLocked(const char *name) const noexcept
{
    if (!name)
        return nullptr;
    for (unsigned i = 0; i < imageCount_; ++i)
    {
        if (!std::strcmp(images_[i].name, name))
            return &images_[i];
    }
    return nullptr;
}

void CaptureTable::RejectLocked(const char *name, int mapType, const ImageMeta &meta, CaptureOutcome outcome) noexcept
{
    ++rejectedCount_;
    lastOutcome_ = outcome;
    if (rejectionCount_ >= kMaxRejections)
        return;
    RejectedImage &rejection = rejections_[rejectionCount_];
    rejection = RejectedImage();
    CopyName(rejection.name, name);
    rejection.mapType = mapType;
    rejection.meta = meta;
    rejection.outcome = outcome;
    ++rejectionCount_;
}

bool CaptureTable::Lookup(const char *name, CaptureLookup *out) const noexcept
{
    if (!out)
        return false;
    *out = CaptureLookup();
    if (!name)
        return false;
    std::lock_guard<std::mutex> guard(lock_);
    if (!arena_)
        return false;
    // Once a name has been offered that this table could not record, no name can be reported as
    // naming one image: the offers it never saw are exactly the ones that might have contradicted
    // the record. Saying nothing is the only answer that cannot be wrong; the statistics carry the
    // overflow that caused it.
    if (namesUntracked_)
        return false;
    // The name is the unit of identity, so the name's own record decides. A captured image always
    // has one, and a name a second offer has claimed resolves nothing: returning either payload
    // would be a guess about which image the caller meant.
    const SeenName *seen = FindSeenLocked(name);
    if (!seen || seen->poisoned)
        return false;
    const CapturedImage *image = FindImageLocked(name);
    if (!image)
        return false;
    out->found = true;
    out->meta = image->meta;
    out->payloadBytes = image->payloadBytes;
    out->fnv1a64 = image->fnv1a64;
    out->payload = arena_ + size_t(image->payloadOffset);
    return true;
}

bool CaptureTable::LookupRejection(const char *name, RejectedImage *out) const noexcept
{
    if (!out || !name)
        return false;
    std::lock_guard<std::mutex> guard(lock_);
    for (unsigned i = 0; i < rejectionCount_; ++i)
    {
        if (std::strcmp(rejections_[i].name, name))
            continue;
        *out = rejections_[i];
        return true;
    }
    return false;
}

CaptureStats CaptureTable::Statistics() const noexcept
{
    std::lock_guard<std::mutex> guard(lock_);
    CaptureStats stats;
    stats.armed = armed_;
    stats.overflow = overflow_;
    stats.images = imageCount_;
    stats.rejectionRecords = rejectionCount_;
    stats.examined = examined_;
    stats.matched = matched_;
    stats.captured = captured_;
    stats.duplicates = duplicateCount_;
    stats.rejected = rejectedCount_;
    stats.ambiguous = ambiguousCount_;
    stats.bytesCaptured = bytesCaptured_;
    stats.lastOutcome = lastOutcome_;
    return stats;
}

} // namespace kisak_world_capture
