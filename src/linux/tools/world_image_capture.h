// ---------------------------------------------------------------------------
// Offline capture of an embedded image payload the dedicated loader discards.
//
// A streamed texture record does not carry pixels the image file loader can find
// again: the fast file appends a GfxImageLoadDef header and its payload behind the
// GfxImage inside the zone, and stores that pointer in the image's own GfxTexture
// union (src/database/db_load.cpp:2091-2121). Load_Texture is the one place where
// the header, the payload and the image identity are all reachable, and it ends by
// writing the null device handle over the union (the dedicated Load_Texture in
// src/linux/server_device_counterparts.cpp), after which the payload cannot be named from the image any more. Reading it out
// of the zone file instead would mean re-implementing the fast-file stream, so the
// payload is copied here, while the union still holds it.
//
// What this unit is: a bounded, allocation-free-at-capture-time copy of the image
// payloads a lit world run needs, keyed by the image's own name. What it is not:
// a texture loader, a decoder, a format converter or a renderer path. Nothing here
// touches D3D, allocates inside the loader, writes a file or throws across the
// database: the arena is reserved when the run arms the capture, and every bound is
// checked before a byte is copied.
//
// The engine-free part is deliberate. src/linux/server_device_counterparts.cpp
// fills in one POD CaptureRequest from the engine types, and this unit - plus its
// test - compiles with no engine header, no asset tree and no zone.
// ---------------------------------------------------------------------------

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace kisak_world_capture
{

// GfxImageLoadDef is 0x14 bytes - levelCount, flags, dimensions[3], format,
// resourceSize, then data[4] standing for the payload that follows it
// (src/gfx_d3d/r_gfx.h:197-205). OFFSET_TO_GfxImageLoadDef_DATA is the 16-byte
// header the database streams before that payload, and db_load.cpp:2132 asserts the
// same split, so the header is copied as 16 bytes and the payload as resourceSize.
constexpr unsigned kHeaderBytes = 16;

// The bounds every copy obeys. A refusal is a reported outcome, never a truncated
// payload: a reader that gets bytes gets all of the image's bytes.
constexpr uint64_t kMaxImageBytes = 16ull * 1024ull * 1024ull;
constexpr uint64_t kMaxTotalBytes = 64ull * 1024ull * 1024ull;
constexpr unsigned kMaxImages = 16;
// Reasons kept for the report. Past this the counts still move; a reason is then
// only counted, not named.
constexpr unsigned kMaxRejections = 8;
// The distinct lightmap names this table can remember having been offered at all - captured
// or refused. Deciding whether a name still names one image needs the history, not just the
// captures: a name whose first offer was refused cannot be compared against a later one,
// because refusing an offer is also a decision not to copy its bytes. The table is bounded so
// the loader path stays allocation-free; a run that offers more names than this cannot be
// vouched for, and the lookup below says so instead of answering (see CaptureStats::overflow).
constexpr unsigned kMaxTrackedNames = 128;
constexpr unsigned kMaxNameBytes = 128;
// levelCount is a byte, but no packaged image has this many mips; a larger value
// means the bytes being read are not an image header.
constexpr unsigned kMaxMipLevels = 16;
constexpr int kMaxDimension = 16384;

// MAPTYPE_2D (src/gfx_d3d/r_gfx.h:170). The lightmap arrays hold 2D images only; a
// captured payload whose map type is not 2D has no agreed layout to export.
constexpr int kMapType2D = 3;

// GfxImageLoadDef::flags, the bits the engine acts on (src/gfx_d3d/r_image.h:44-54).
constexpr int kImageFlagNoPicmip = 0x1;
constexpr int kImageFlagNoMipmaps = 0x2;
constexpr int kImageFlagCubemap = 0x4;
constexpr int kImageFlagVolmap = 0x8;
constexpr int kImageFlagStreaming = 0x10;
constexpr int kImageFlagLegacyNormals = 0x20;
constexpr int kImageFlagClampU = 0x40;
constexpr int kImageFlagClampV = 0x80;
// The bits a 2D payload can carry. CUBEMAP and VOLMAP are the header contradicting its own
// map type - those bytes are not a 2D image's - so they are refused rather than skipped.
constexpr int kAllowed2DFlags = kImageFlagNoPicmip | kImageFlagNoMipmaps | kImageFlagStreaming |
                                kImageFlagLegacyNormals | kImageFlagClampU | kImageFlagClampV;

// The lightmaps a map zone carries are named "*lightmapN_primary" and
// "*lightmapN_secondary". Only names with this prefix are copied, which is the
// whole filter: the thousands of other images in the zone keep their metadata
// description in the inventory and are not touched here.
constexpr char kNamePrefix[] = "*lightmap";

// The two roles GfxWorld::lightmaps splits into.
constexpr int kRolePrimary = 0;
constexpr int kRoleSecondary = 1;

// MAKEFOURCC, as d3d9types.h defines it, so this unit can name the compressed
// formats without including a D3D header.
constexpr int32_t FourCC(char a, char b, char c, char d)
{
    return int32_t(uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16) |
                   (uint32_t(uint8_t(d)) << 24));
}

// The formats a streamed 2D payload may use. The set is the engine's own: these are the
// formats Image_GetCardMemoryAmountForMipLevel (src/gfx_d3d/r_image_load_obj.cpp:81-135)
// sizes for a texture, and the ones packaged lightmaps use - the block-compressed set and
// 32-bit ARGB. A format outside it has no agreed byte layout here, so a header carrying one
// is refused rather than passed through as a number nobody checked.
constexpr int32_t kFormatA8R8G8B8 = 21; // D3DFMT_A8R8G8B8
bool IsSupportedTextureFormat(int32_t format);

// The bytes one mip level of that format occupies: the engine's own arithmetic, including
// the 4x4 block rounding for the compressed formats. False for a format outside the set.
bool MipLevelBytes(int32_t format, uint64_t width, uint64_t height, uint64_t depth, uint64_t *bytes) noexcept;

// The bytes Load_Texture walks for a 2D payload with this header (src/gfx_d3d/r_image.cpp:
// 327-352): one level when IMG_FLAG_NOMIPMAPS is set, otherwise every level down to 1x1.
// The engine asserts that the walk ends exactly at the header's resourceSize, which is why
// the capture requires equality rather than a range.
//
// The sum is exact only for a single level or for power-of-two dimensions. The engine's upload
// loop takes its level count from Image_CountMipmaps (r_image.cpp:459-470) but sizes each level
// with the clamped halving below, and on a non-power-of-two image the two disagree: a 5x5 DXT1
// chain is four levels - 5x5, 2x2, 1x1, 1x1 - at 56 bytes, while this sum stops at three levels
// and 48 bytes. Which chain a packaged image used cannot be stated from the header, so
// LayoutBytesAreExact refuses the case rather than picking one.
bool Expected2DPayloadBytes(int32_t format, int flags, int width, int height, uint64_t *bytes) noexcept;

// True when Expected2DPayloadBytes describes the payload exactly: a single level, whatever the
// dimensions, or a chain on dimensions that halve down to exactly 1x1 on both axes. This is the
// admitted layout set, and the size rule is only applied once it has passed.
bool LayoutBytesAreExact(int flags, int width, int height) noexcept;

// The most mip levels these dimensions can hold: 1 + ceil(log2(max(width, height))), the count
// Image_CountMipmaps (r_image.cpp:459-470) answers with. For power-of-two dimensions it is also
// the number of levels the byte walk below visits; for other dimensions it is not, which is why
// such a chain is refused.
unsigned MaxMipLevels(int width, int height) noexcept;

enum class CaptureOutcome
{
    // The run never armed the capture, or has disarmed it: the loader call has no
    // effect at all, not even on the counters.
    NotArmed,
    // An embedded image, but not a lightmap: out of scope by the filter.
    NameNotMatched,
    NameTooLong,
    // A lightmap name on something that is not a 2D image.
    MapTypeUnsupported,
    // The header bytes cannot be one: no payload, no mip, or a dimension outside the
    // accepted range.
    MalformedHeader,
    // A format outside IsSupportedTextureFormat.
    UnsupportedFormat,
    // The flags say something a 2D image's flags cannot: a cubemap or volume-map bit, or
    // IMG_FLAG_NOMIPMAPS on a header that also claims several levels.
    FlagsContradicted,
    // More mip levels than the dimensions can hold, or none at all.
    ImpossibleMipCount,
    // A mip chain on a non-power-of-two image, where the engine's level count and its per-level
    // sizes do not describe one chain (see Expected2DPayloadBytes). A single level on the same
    // dimensions has one agreed layout and is still captured.
    UnsupportedLayout,
    // A MAPTYPE_2D image with a depth other than 1.
    DepthNotFlat,
    // resourceSize is not exactly the byte count the format, flags and dimensions imply.
    SizeMismatch,
    // The copy would cross kMaxImageBytes or kMaxTotalBytes.
    OverflowBytes,
    // kMaxImages captures already exist, or kMaxTrackedNames names are already tracked and this
    // offer's name cannot be recorded. A name that was not recorded is never captured: without it
    // the table could not tell a repeat from a different image arriving later.
    OverflowCount,
    // This loadDef pointer and name were already captured, with the same bytes.
    Duplicate,
    // A second offer arrived under a name this table already holds, and it is not the same image:
    // different bytes or a different header behind a captured name, or a name whose earlier offer
    // was refused. The name is refused permanently, whatever the second offer's own faults, rather
    // than resolved by load order.
    AmbiguousName,
    // A refusal this unit does not otherwise expect: the table stayed unchanged.
    InternalError,
    Captured,
};

const char *CaptureOutcomeName(CaptureOutcome outcome);

// The header fields as the stream laid them out. Recorded even when a copy is
// refused, so a run that captures nothing still reports the size and format it saw
// instead of leaving the next attempt to guess.
struct ImageMeta
{
    int levelCount = 0;
    int flags = 0;
    int width = 0;
    int height = 0;
    int depth = 0;
    int32_t format = 0;
    int64_t resourceSize = 0;
};

// What the loader hands over. POD on purpose: the engine-facing side fills one in
// and nothing else on the loader path touches this unit's storage.
struct CaptureRequest
{
    // GfxImageLoadDef*, the payload's own address. It is not consulted when deciding whether a
    // repeat is the same image: the loader's buffer is the zone's stream position, so the next
    // record reuses it, and one address can present different bytes on a later load. The bytes
    // decide (see CaptureTable::Capture), never the address or the header alone.
    const void *loadDefIdentity = nullptr;
    const char *name = nullptr;
    int mapType = 0;
    // The 16-byte GfxImageLoadDef header.
    const unsigned char *header = nullptr;
    // GfxImageLoadDef::data, resourceSize bytes long, as the header states.
    const unsigned char *payload = nullptr;
};

struct CapturedImage
{
    char name[kMaxNameBytes] = {};
    int mapType = 0;
    ImageMeta meta;
    uint64_t payloadOffset = 0;
    uint64_t payloadBytes = 0;
    uint64_t fnv1a64 = 0;
};

struct RejectedImage
{
    char name[kMaxNameBytes] = {};
    int mapType = 0;
    ImageMeta meta;
    CaptureOutcome outcome = CaptureOutcome::MalformedHeader;
};

struct CaptureLookup
{
    bool found = false;
    ImageMeta meta;
    uint64_t payloadBytes = 0;
    uint64_t fnv1a64 = 0;
    // Borrowed from the capture table. It stays valid until the next Arm(): Disarm()
    // stops capturing but keeps what was captured readable.
    const unsigned char *payload = nullptr;
};

struct CaptureStats
{
    bool armed = false;
    // A copy or a name record was refused at a bound. Each resource's availability is still its
    // own lookup result, so this records the event without condemning unrelated images - except
    // when the name bound was crossed, which is the one bound a per-name record cannot absorb
    // (see ambiguous below and CaptureTable::Lookup).
    bool overflow = false;
    unsigned images = 0;
    unsigned rejectionRecords = 0;
    // Embedded images the loader offered while armed. `matched` is those the name
    // filter kept, `captured` those with a complete payload, `duplicates` the exact
    // (pointer, name) repeats, `rejected` everything refused for a stated reason.
    uint64_t examined = 0;
    uint64_t matched = 0;
    uint64_t captured = 0;
    uint64_t duplicates = 0;
    uint64_t rejected = 0;
    // Payloads refused because their name already held a different image, or because the name had
    // already been offered once and its bytes were never copied to compare against.
    uint64_t ambiguous = 0;
    uint64_t bytesCaptured = 0;
    CaptureOutcome lastOutcome = CaptureOutcome::NotArmed;
};

// One process-wide table: the database load and the export that arms it run on the
// same thread, and the mutex is there so a future second caller cannot corrupt it.
// Every method is noexcept and none of them allocates once Arm() has returned.
class CaptureTable
{
public:
    static CaptureTable &Instance() noexcept;

    // Reserve the arena and start capturing. False means the arena could not be
    // reserved: the table stays inert and the export reports the capture as
    // unavailable rather than claiming a payload it never copied.
    bool Arm() noexcept;

    // Stop capturing. Captured payloads stay readable, so a run can arm, load, and
    // inspect afterwards. This is what an unwind out of a zone load does.
    void Disarm() noexcept;

    bool Armed() const noexcept;

    // The loader-side call. Never allocates, never does I/O, never throws.
    //
    // A repeat of a name is decided by comparing the bytes offered now against the bytes that were
    // copied, every time - an equal header and an equal payload address are not enough. A name
    // that comes back with different bytes is ambiguous, and an ambiguous name stays ambiguous.
    //
    // Ambiguity is tracked by name, not by capture: an offer this call refuses still claims the
    // name, so a later admissible offer under the same name cannot become resolvable by being the
    // one that happened to arrive after the refusal.
    CaptureOutcome Capture(const CaptureRequest &request) noexcept;

    // Exact image name. A name stays resolvable only while it names one image: if a second offer
    // arrives under the same name - captured, refused, or inadmissible - the name is ambiguous and
    // is never looked up again, because which of the two a record meant cannot be known here. False
    // leaves `out` untouched; a name the filter never kept also fails.
    //
    // False is also the answer for every name once the run has offered more names than this unit
    // can track: with a name's history missing, "this name named one image" is not a claim the
    // table can make, so it says nothing rather than something it cannot check.
    bool Lookup(const char *name, CaptureLookup *out) const noexcept;

    // Why a name was not captured, when a reason was recorded for it.
    bool LookupRejection(const char *name, RejectedImage *out) const noexcept;

    CaptureStats Statistics() const noexcept;

    CaptureTable(const CaptureTable &) = delete;
    CaptureTable &operator=(const CaptureTable &) = delete;

private:
    CaptureTable() = default;
    ~CaptureTable();

    // One name the loader has offered under the filter, captured or refused. The entry outlives
    // whatever the offer produced, which is what makes a refusal claim a name.
    struct SeenName
    {
        char name[kMaxNameBytes] = {};
        // Set when a second offer arrives under this name that is not a byte-identical repeat of
        // what was captured - including an offer this table refuses. Sticky: a claimed name never
        // resolves again, so neither arrival order nor a later repeat can choose between the two.
        bool poisoned = false;
    };

    CaptureOutcome CaptureLocked(const CaptureRequest &request) noexcept;
    void RejectLocked(const char *name, int mapType, const ImageMeta &meta, CaptureOutcome outcome) noexcept;
    // The entry for this name, adding one when there is room. False means the table is full, and
    // the name therefore cannot be remembered; `seenBefore` says whether the name was already
    // tracked, so the caller can tell a first offer from a repeat.
    bool TrackNameLocked(const char *name, SeenName **seen, bool *seenBefore) noexcept;
    const SeenName *FindSeenLocked(const char *name) const noexcept;
    const CapturedImage *FindImageLocked(const char *name) const noexcept;

    mutable std::mutex lock_;
    unsigned char *arena_ = nullptr;
    bool armed_ = false;
    bool overflow_ = false;
    // Set when a name arrived that could not be tracked. Every lookup then fails: a name whose
    // offers this table did not all see cannot be reported as naming one image.
    bool namesUntracked_ = false;
    unsigned imageCount_ = 0;
    unsigned rejectionCount_ = 0;
    unsigned trackedCount_ = 0;
    uint64_t examined_ = 0;
    uint64_t matched_ = 0;
    uint64_t captured_ = 0;
    uint64_t duplicateCount_ = 0;
    uint64_t rejectedCount_ = 0;
    uint64_t ambiguousCount_ = 0;
    uint64_t bytesCaptured_ = 0;
    CaptureOutcome lastOutcome_ = CaptureOutcome::NotArmed;
    // One record per name. There is deliberately no second array keyed by payload address: an
    // address says nothing about which image is behind a name.
    CapturedImage images_[kMaxImages];
    RejectedImage rejections_[kMaxRejections];
    SeenName tracked_[kMaxTrackedNames];
};

// ---------------------------------------------------------------------------
// The scope that owns arming.
//
// It brackets a database zone load, which is a call that can end the run. The
// destructor is noexcept and does nothing but Disarm(), so an unwind out of the load
// leaves the loader inert rather than capturing into a run that has already given up.
// Disarm keeps the captured payloads readable: the document is written after the load
// has returned, and it reads what the load captured.
// ---------------------------------------------------------------------------
class ArmScope
{
public:
    explicit ArmScope(bool requested) noexcept
        : requested_(requested), armed_(requested && CaptureTable::Instance().Arm())
    {
    }
    ~ArmScope()
    {
        if (requested_)
            CaptureTable::Instance().Disarm();
    }

    // False when the run asked for a capture the table could not provide; the export
    // reports that rather than claiming a payload it never copied.
    bool Armed() const noexcept { return armed_; }

    ArmScope(const ArmScope &) = delete;
    ArmScope &operator=(const ArmScope &) = delete;

private:
    bool requested_ = false;
    bool armed_ = false;
};

// The resource a world surface's lightmap dependency names, spelled the same way in
// the document and in the payload file names: "lightmap0.secondary". An index below
// zero has no resource and yields an empty string.
std::string ResourceKey(int lightmapIndex, int role);

// FNV-1a, 64 bit: the same function the bundle writer reports its payloads with, so
// a reader checks every file in a run the same way.
uint64_t Fnv1a64(const void *data, size_t bytes) noexcept;

} // namespace kisak_world_capture
