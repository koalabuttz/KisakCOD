// ---------------------------------------------------------------------------
// Asset-free checks for the bounded image capture.
//
// The unit under test is engine-free on purpose: it takes a POD CaptureRequest, so this
// test builds that request by hand with the same 16 header bytes and payload pointer the
// Load_Texture hook passes in (src/linux/server_assets.cpp). No engine header, no asset
// tree and no zone is involved.
//
// Every fixture is a layout-valid header: its resourceSize is the byte count the engine's
// own walk consumes for that format, flags and dimensions (src/gfx_d3d/r_image.cpp:327-352,
// src/gfx_d3d/r_image_load_obj.cpp:81-175), and the expected sizes below are written out
// from that arithmetic rather than read back from the unit. A test that used an arbitrary
// resourceSize would only prove the range check, which is exactly what the capture used to
// do and what the size rule replaces.
// ---------------------------------------------------------------------------

#include <linux/tools/world_image_capture.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
int failures = 0;

void Check(bool condition, const char *what)
{
    if (condition)
        return;
    std::printf("FAIL %s\n", what);
    ++failures;
}

using namespace kisak_world_capture;

// The 16 bytes GfxImageLoadDef streams: levelCount, flags, dimensions[3], format,
// resourceSize, little-endian, which is what db_load.cpp:2133 loads and what the payload
// follows.
struct Header
{
    unsigned char bytes[kHeaderBytes] = {};

    Header(int levelCount, int flags, int width, int height, int depth, int32_t format, int32_t resourceSize)
    {
        bytes[0] = (unsigned char)levelCount;
        bytes[1] = (unsigned char)flags;
        bytes[2] = (unsigned char)(width & 0xFF);
        bytes[3] = (unsigned char)((width >> 8) & 0xFF);
        bytes[4] = (unsigned char)(height & 0xFF);
        bytes[5] = (unsigned char)((height >> 8) & 0xFF);
        bytes[6] = (unsigned char)(depth & 0xFF);
        bytes[7] = (unsigned char)((depth >> 8) & 0xFF);
        const uint32_t formatBits = uint32_t(format);
        bytes[8] = (unsigned char)(formatBits & 0xFF);
        bytes[9] = (unsigned char)((formatBits >> 8) & 0xFF);
        bytes[10] = (unsigned char)((formatBits >> 16) & 0xFF);
        bytes[11] = (unsigned char)((formatBits >> 24) & 0xFF);
        const uint32_t sizeBits = uint32_t(resourceSize);
        bytes[12] = (unsigned char)(sizeBits & 0xFF);
        bytes[13] = (unsigned char)((sizeBits >> 8) & 0xFF);
        bytes[14] = (unsigned char)((sizeBits >> 16) & 0xFF);
        bytes[15] = (unsigned char)((sizeBits >> 24) & 0xFF);
    }
};

constexpr int32_t kDxt1 = FourCC('D', 'X', 'T', '1');
constexpr int32_t kDxt3 = FourCC('D', 'X', 'T', '3');
constexpr int32_t kDxt5 = FourCC('D', 'X', 'T', '5');
constexpr int32_t kArgb32 = 21; // D3DFMT_A8R8G8B8
constexpr int kNoMips = 2;      // IMG_FLAG_NOMIPMAPS

CaptureRequest MakeRequest(const void *identity, const char *name, int mapType, const Header &header,
                           const unsigned char *payload)
{
    CaptureRequest request;
    request.loadDefIdentity = identity;
    request.name = name;
    request.mapType = mapType;
    request.header = header.bytes;
    request.payload = payload;
    return request;
}
} // namespace

int main()
{
    using namespace kisak_world_capture;

    // ---- The format set is the engine's, not "any D3D9 enumerator". -------------------
    Check(IsSupportedTextureFormat(kArgb32), "A8R8G8B8 is a supported 2D payload format");
    Check(IsSupportedTextureFormat(kDxt1), "DXT1 is supported");
    Check(IsSupportedTextureFormat(kDxt3), "DXT3 is supported");
    Check(IsSupportedTextureFormat(kDxt5), "DXT5 is supported");
    Check(!IsSupportedTextureFormat(0), "D3DFMT_UNKNOWN is refused");
    Check(!IsSupportedTextureFormat(50), "L8 has no layout here and is refused");
    Check(!IsSupportedTextureFormat(28), "A8 has no layout here and is refused");
    Check(!IsSupportedTextureFormat(FourCC('D', 'X', 'T', '2')), "DXT2 is not sized by the engine and is refused");
    Check(!IsSupportedTextureFormat(FourCC('D', 'X', 'T', '4')), "DXT4 is not sized by the engine and is refused");
    Check(!IsSupportedTextureFormat(75), "a depth/stencil format is refused");
    Check(!IsSupportedTextureFormat(101), "an index buffer format is refused");
    Check(!IsSupportedTextureFormat(-1), "a negative format is refused");

    // ---- Per-level and whole-chain byte counts, from the engine's arithmetic. ---------
    uint64_t bytes = 0;
    Check(MipLevelBytes(kDxt1, 4, 4, 1, &bytes) && bytes == 8, "a 4x4 DXT1 level is one 8-byte block");
    Check(MipLevelBytes(kDxt1, 8, 8, 1, &bytes) && bytes == 32, "an 8x8 DXT1 level is four blocks");
    Check(MipLevelBytes(kDxt1, 5, 5, 1, &bytes) && bytes == 32, "an odd size rounds up to whole 4x4 blocks");
    Check(MipLevelBytes(kDxt5, 4, 4, 1, &bytes) && bytes == 16, "a 4x4 DXT5 level is one 16-byte block");
    Check(MipLevelBytes(kArgb32, 4, 4, 1, &bytes) && bytes == 64, "a 4x4 ARGB level is four bytes a pixel");
    Check(!MipLevelBytes(50, 4, 4, 1, &bytes), "an unsupported format has no level size");
    Check(!MipLevelBytes(kArgb32, 4, 4, 1, nullptr), "a null output is refused");

    // 4x4 DXT1 has three levels (4x4, 2x2, 1x1) of one block each.
    Check(Expected2DPayloadBytes(kDxt1, 0, 4, 4, &bytes) && bytes == 24, "the 4x4 DXT1 chain is 24 bytes");
    Check(Expected2DPayloadBytes(kDxt1, kNoMips, 4, 4, &bytes) && bytes == 8,
          "a no-mipmap 4x4 DXT1 image is one level");
    // 8x8 DXT1: 32 + 8 + 8 + 8.
    Check(Expected2DPayloadBytes(kDxt1, 0, 8, 8, &bytes) && bytes == 56, "the 8x8 DXT1 chain is 56 bytes");
    // 8x8 DXT5: 64 + 16 + 16 + 16.
    Check(Expected2DPayloadBytes(kDxt5, 0, 8, 8, &bytes) && bytes == 112, "the 8x8 DXT5 chain is 112 bytes");
    // 8x8 ARGB: 256 + 64 + 16 + 4.
    Check(Expected2DPayloadBytes(kArgb32, 0, 8, 8, &bytes) && bytes == 340, "the 8x8 ARGB chain is 340 bytes");
    // The shape the real map's secondary lightmap has.
    Check(Expected2DPayloadBytes(kArgb32, kNoMips, 512, 2048, &bytes) && bytes == 4194304,
          "a 512x2048 no-mipmap ARGB image is 4194304 bytes");

    Check(MaxMipLevels(4, 4) == 3, "4x4 holds three levels");
    Check(MaxMipLevels(8, 8) == 4, "8x8 holds four levels");
    Check(MaxMipLevels(1, 1) == 1, "1x1 holds one level");
    Check(MaxMipLevels(512, 2048) == 12, "512x2048 holds twelve levels");

    // The resource name is the same string in the document and in the payload file names.
    Check(ResourceKey(0, kRoleSecondary) == "lightmap0.secondary", "secondary lightmap key");
    Check(ResourceKey(0, kRolePrimary) == "lightmap0.primary", "primary lightmap key");
    Check(ResourceKey(12, kRoleSecondary) == "lightmap12.secondary", "index is not padded");
    Check(ResourceKey(-1, kRolePrimary).empty(), "a negative lightmap index has no resource");

    // FNV-1a 64 known values: the hash the manifest reports is the standard one.
    Check(Fnv1a64("abc", 3) == 0xe71fa2190541574bull, "FNV-1a 64 of abc");
    Check(Fnv1a64("", 0) == 0xcbf29ce484222325ull, "FNV-1a 64 of nothing is the offset basis");

    CaptureTable &table = CaptureTable::Instance();

    // ---- 1. Disabled capture changes nothing at all. ---------------------------------
    {
        const Header lightmap(1, kNoMips, 512, 2048, 1, kArgb32, 4194304);
        std::vector<unsigned char> payload(size_t(4194304), 0x5A);
        const CaptureOutcome outcome =
            table.Capture(MakeRequest(payload.data(), "*lightmap0_secondary", kMapType2D, lightmap, payload.data()));
        Check(outcome == CaptureOutcome::NotArmed, "an unarmed capture refuses the image");
        const CaptureStats stats = table.Statistics();
        Check(!stats.armed, "statistics report the capture as not armed");
        Check(stats.examined == 0, "a disabled capture examines nothing");
        Check(stats.matched == 0, "a disabled capture matches nothing");
        Check(stats.captured == 0 && stats.rejected == 0 && stats.ambiguous == 0,
              "a disabled capture counts nothing");
        Check(stats.bytesCaptured == 0, "a disabled capture copies no byte");
        Check(stats.images == 0 && stats.rejectionRecords == 0, "a disabled capture keeps no record");
        Check(stats.lastOutcome == CaptureOutcome::NotArmed, "a disabled capture records no outcome");
        CaptureLookup lookup;
        Check(!table.Lookup("*lightmap0_secondary", &lookup) && !lookup.found,
              "a disabled capture retains nothing to look up");
        Check(!table.LookupRejection("*lightmap0_secondary", nullptr), "the rejection lookup tolerates a null output");
    }

    // ---- 2. The real shape: 512x2048 ARGB, no mipmaps, exactly 4194304 bytes. --------
    std::vector<unsigned char> lightmapPayload(size_t(4194304));
    for (size_t i = 0; i < lightmapPayload.size(); ++i)
        lightmapPayload[i] = (unsigned char)(i * 7u);
    const Header realLightmap(1, kNoMips, 512, 2048, 1, kArgb32, 4194304);
    Check(table.Arm(), "the capture arena is reserved");
    Check(table.Armed(), "the capture reports itself armed");
    {
        Check(table.Capture(MakeRequest(lightmapPayload.data(), "*lightmap0_secondary", kMapType2D, realLightmap,
                                        lightmapPayload.data())) == CaptureOutcome::Captured,
              "the shape the map really carries is captured");
        CaptureLookup lookup;
        Check(table.Lookup("*lightmap0_secondary", &lookup) && lookup.found, "the captured lightmap is found by name");
        Check(lookup.payloadBytes == 4194304, "the whole payload is retained");
        Check(lookup.meta.width == 512 && lookup.meta.height == 2048, "captured dimensions");
        Check(lookup.meta.depth == 1, "captured depth");
        Check(lookup.meta.levelCount == 1, "captured mip count");
        Check(lookup.meta.flags == kNoMips, "captured flags");
        Check(lookup.meta.format == kArgb32, "captured format");
        Check(lookup.meta.resourceSize == 4194304, "captured resource size");
        Check(lookup.fnv1a64 == Fnv1a64(lightmapPayload.data(), lightmapPayload.size()),
              "the payload hash is the payload's");
        Check(lookup.payload && !std::memcmp(lookup.payload, lightmapPayload.data(), lightmapPayload.size()),
              "the retained bytes are the offered bytes");
        const CaptureStats stats = table.Statistics();
        Check(stats.examined == 1 && stats.matched == 1 && stats.captured == 1, "counters after one capture");
        Check(stats.bytesCaptured == 4194304, "byte count after one capture");
        Check(!stats.overflow, "no bound was crossed");
    }

    // ---- 3. A repeat of the same pointer and name is the same image. -----------------
    {
        Check(table.Capture(MakeRequest(lightmapPayload.data(), "*lightmap0_secondary", kMapType2D, realLightmap,
                                        lightmapPayload.data())) == CaptureOutcome::Duplicate,
              "the same pointer and name is a duplicate");
        const CaptureStats stats = table.Statistics();
        Check(stats.images == 1 && stats.duplicates == 1, "a duplicate adds no record and is counted");
        Check(stats.bytesCaptured == 4194304, "a duplicate copies no byte");
    }

    // ---- 4. Exact size, not a range: 4x4 DXT1, sizes 1 / 8 / 16. ---------------------
    // One block, one level: 8 bytes is the only size the engine's walk consumes.
    {
        unsigned char block[16] = {0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A,
                                   0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5};
        const Header tooSmall(1, kNoMips, 4, 4, 1, kDxt1, 1);
        const Header exact(1, kNoMips, 4, 4, 1, kDxt1, 8);
        const Header tooLarge(1, kNoMips, 4, 4, 1, kDxt1, 16);
        Check(table.Capture(MakeRequest(&block, "*lightmap4_a_secondary", kMapType2D, tooSmall, block)) ==
                  CaptureOutcome::SizeMismatch,
              "a 4x4 DXT1 header claiming 1 byte is refused");
        Check(table.Capture(MakeRequest(&block, "*lightmap4_b_secondary", kMapType2D, tooLarge, block)) ==
                  CaptureOutcome::SizeMismatch,
              "a 4x4 DXT1 header claiming 16 bytes is refused");
        Check(table.Capture(MakeRequest(&block, "*lightmap4_c_secondary", kMapType2D, exact, block)) ==
                  CaptureOutcome::Captured,
              "a 4x4 DXT1 header claiming exactly 8 bytes is captured");
        CaptureLookup lookup;
        Check(table.Lookup("*lightmap4_c_secondary", &lookup) && lookup.payloadBytes == 8,
              "the 8-byte payload is retained in full");
        // The rejection names the size it saw, so the next run reads why.
        RejectedImage rejection;
        Check(table.LookupRejection("*lightmap4_a_secondary", &rejection) &&
                  rejection.outcome == CaptureOutcome::SizeMismatch && rejection.meta.resourceSize == 1,
              "the size refusal records the size it saw");
    }

    // ---- 5. Expected minus one and plus one, for each accepted format and chain. ----
    {
        // 8x8 chains: DXT1 56, DXT5 112, ARGB 340.
        unsigned char chain[512] = {};
        struct ChainCase
        {
            const char *name;
            int32_t format;
            int32_t exact;
        };
        const ChainCase cases[3] = {{"*lightmap5_dxt1_secondary", kDxt1, 56},
                                    {"*lightmap5_dxt5_secondary", kDxt5, 112},
                                    {"*lightmap5_argb_secondary", kArgb32, 340}};
        for (const ChainCase &chainCase : cases)
        {
            const Header minus(4, 0, 8, 8, 1, chainCase.format, chainCase.exact - 1);
            const Header exact(4, 0, 8, 8, 1, chainCase.format, chainCase.exact);
            const Header plus(4, 0, 8, 8, 1, chainCase.format, chainCase.exact + 1);
            Check(table.Capture(MakeRequest(&chain[0], chainCase.name, kMapType2D, exact, chain)) ==
                      CaptureOutcome::Captured,
                  "an exact chain size is captured");
            // A second capture of the same name would be ambiguous, so the two refusals are
            // checked against other names that differ only in the size they claim.
            char nameMinus[64];
            char namePlus[64];
            std::snprintf(nameMinus, sizeof(nameMinus), "%s_minus", chainCase.name);
            std::snprintf(namePlus, sizeof(namePlus), "%s_plus", chainCase.name);
            Check(table.Capture(MakeRequest(&chain[0], nameMinus, kMapType2D, minus, chain)) ==
                      CaptureOutcome::SizeMismatch,
                  "one byte short is refused");
            Check(table.Capture(MakeRequest(&chain[0], namePlus, kMapType2D, plus, chain)) ==
                      CaptureOutcome::SizeMismatch,
                  "one byte long is refused");
        }
    }

    // ---- 6. Depth, flags and mip counts the layout cannot have. ----------------------
    {
        unsigned char bytes8[8] = {};
        // Depth 2 on a MAPTYPE_2D image: the size for the flat image would otherwise pass,
        // so only the depth rule can refuse it.
        const Header deep(1, kNoMips, 4, 4, 2, kDxt1, 8);
        Check(table.Capture(MakeRequest(&bytes8[0], "*lightmap6_deep_secondary", kMapType2D, deep, bytes8)) ==
                  CaptureOutcome::DepthNotFlat,
              "a 2D image with depth 2 is refused");
        const Header zeroDepth(1, kNoMips, 4, 4, 0, kDxt1, 8);
        Check(table.Capture(MakeRequest(&bytes8[0], "*lightmap6_flat0_secondary", kMapType2D, zeroDepth, bytes8)) ==
                  CaptureOutcome::DepthNotFlat,
              "a 2D image with depth 0 is refused");
        // Cubemap and volume-map bits contradict the map type the record already fixed.
        const Header cube(1, kNoMips | 4, 4, 4, 1, kDxt1, 8);
        Check(table.Capture(MakeRequest(&bytes8[0], "*lightmap6_cube_secondary", kMapType2D, cube, bytes8)) ==
                  CaptureOutcome::FlagsContradicted,
              "a cubemap flag on a 2D image is refused");
        const Header volume(1, kNoMips | 8, 4, 4, 1, kDxt1, 8);
        Check(table.Capture(MakeRequest(&bytes8[0], "*lightmap6_volume_secondary", kMapType2D, volume, bytes8)) ==
                  CaptureOutcome::FlagsContradicted,
              "a volume-map flag on a 2D image is refused");
        // No-mipmaps on a header that also claims two levels describes two different images.
        const Header contradictory(2, kNoMips, 4, 4, 1, kDxt1, 8);
        Check(table.Capture(MakeRequest(&bytes8[0], "*lightmap6_mips_secondary", kMapType2D, contradictory, bytes8)) ==
                  CaptureOutcome::FlagsContradicted,
              "no-mipmaps with two levels is refused");
        // A 4x4 image cannot hold four levels.
        const Header tooManyLevels(4, 0, 4, 4, 1, kDxt1, 24);
        Check(table.Capture(MakeRequest(&bytes8[0], "*lightmap6_levels4_secondary", kMapType2D, tooManyLevels,
                                        bytes8)) == CaptureOutcome::ImpossibleMipCount,
              "more levels than the dimensions hold is refused");
        const Header noLevels(0, 0, 4, 4, 1, kDxt1, 24);
        Check(table.Capture(MakeRequest(&bytes8[0], "*lightmap6_levels0_secondary", kMapType2D, noLevels, bytes8)) ==
                  CaptureOutcome::ImpossibleMipCount,
              "zero levels is refused");
        const Header manyLevels(17, 0, 4, 4, 1, kDxt1, 24);
        Check(table.Capture(MakeRequest(&bytes8[0], "*lightmap6_levels17_secondary", kMapType2D, manyLevels,
                                        bytes8)) == CaptureOutcome::ImpossibleMipCount,
              "a level count past the bound is refused");
        // Three levels on a 4x4 image is exactly what the chain holds, and 24 is its size.
        const Header threeLevels(3, 0, 4, 4, 1, kDxt1, 24);
        unsigned char chain24[24] = {};
        Check(table.Capture(MakeRequest(&chain24[0], "*lightmap6_levels3_secondary", kMapType2D, threeLevels,
                                        chain24)) == CaptureOutcome::Captured,
              "three levels on a 4x4 image is the chain it can hold");
    }

    // ---- 7. Out of scope images are counted and not copied. -------------------------
    {
        const Header notLightmap(1, kNoMips, 16, 16, 1, kArgb32, 1024);
        std::vector<unsigned char> small(1024, 0);
        Check(table.Capture(MakeRequest(small.data(), "wc/sky_mp_crash", kMapType2D, notLightmap, small.data())) ==
                  CaptureOutcome::NameNotMatched,
              "a name outside the filter is refused");
        Check(table.Capture(MakeRequest(small.data(), "*fx_lightmap", kMapType2D, notLightmap, small.data())) ==
                  CaptureOutcome::NameNotMatched,
              "the filter is the lightmap prefix, not a substring");
        Check(table.Capture(MakeRequest(small.data(), "*lightmap7_3d_secondary", 4 /* MAPTYPE_3D */, notLightmap,
                                        small.data())) == CaptureOutcome::MapTypeUnsupported,
              "a 3D lightmap is refused before any layout check");
        CaptureLookup lookup;
        Check(!table.Lookup("wc/sky_mp_crash", &lookup), "an unfiltered image is not retained");
    }

    // ---- 8. Structural refusals. ----------------------------------------------------
    {
        std::vector<unsigned char> small(64, 0);
        const Header noPayload(1, kNoMips, 4, 4, 1, kDxt1, 0);
        Check(table.Capture(MakeRequest(&small[0], "*lightmap8_nopayload_secondary", kMapType2D, noPayload,
                                        small.data())) == CaptureOutcome::MalformedHeader,
              "a header with no payload is refused");
        const Header negative(1, kNoMips, 4, 4, 1, kDxt1, -8);
        Check(table.Capture(MakeRequest(&small[0], "*lightmap8_negative_secondary", kMapType2D, negative,
                                        small.data())) == CaptureOutcome::MalformedHeader,
              "a negative resource size is refused");
        const Header flatWidth(1, kNoMips, 0, 4, 1, kDxt1, 8);
        Check(table.Capture(MakeRequest(&small[0], "*lightmap8_width0_secondary", kMapType2D, flatWidth,
                                        small.data())) == CaptureOutcome::MalformedHeader,
              "a zero width is refused");
        const Header hugeWidth(1, kNoMips, kMaxDimension + 1, 4, 1, kDxt1, 8);
        Check(table.Capture(MakeRequest(&small[0], "*lightmap8_wide_secondary", kMapType2D, hugeWidth,
                                        small.data())) == CaptureOutcome::MalformedHeader,
              "a width past the bound is refused");
        const Header unknownFormat(1, kNoMips, 4, 4, 1, 0, 8);
        Check(table.Capture(MakeRequest(&small[0], "*lightmap8_format0_secondary", kMapType2D, unknownFormat,
                                        small.data())) == CaptureOutcome::UnsupportedFormat,
              "D3DFMT_UNKNOWN is refused");
        const Header l8(1, kNoMips, 4, 4, 1, 50, 16);
        Check(table.Capture(MakeRequest(&small[0], "*lightmap8_l8_secondary", kMapType2D, l8, small.data())) ==
                  CaptureOutcome::UnsupportedFormat,
              "a format without a layout here is refused");
        CaptureRequest missingPayload =
            MakeRequest(&small[0], "*lightmap8_missing_secondary", kMapType2D, noPayload, small.data());
        missingPayload.header = nullptr;
        Check(table.Capture(missingPayload) == CaptureOutcome::MalformedHeader, "a missing header is refused");
        CaptureRequest noBytes = MakeRequest(&small[0], "*lightmap8_nobytes_secondary", kMapType2D, noPayload,
                                             small.data());
        noBytes.payload = nullptr;
        Check(table.Capture(noBytes) == CaptureOutcome::MalformedHeader, "a missing payload pointer is refused");
        std::vector<char> longName(size_t(kMaxNameBytes) + 8);
        std::memcpy(longName.data(), "*lightmap", 9);
        std::memset(longName.data() + 9, 'x', longName.size() - 10);
        longName.back() = 0;
        const Header small4(1, kNoMips, 4, 4, 1, kDxt1, 8);
        unsigned char eight[8] = {};
        Check(table.Capture(MakeRequest(&eight[0], longName.data(), kMapType2D, small4, eight)) ==
                  CaptureOutcome::NameTooLong,
              "an over-long name is refused");
    }

    // ---- 9. The per-image bound, on a header whose layout is sound. -----------------
    {
        // 8192x4096 DXT5 without mipmaps is 32 MiB: a real layout, past the 16 MiB bound.
        const int32_t size = 1048576 * 32;
        std::vector<unsigned char> huge(size_t(size), 0);
        const Header oversized(1, kNoMips, 8192, 4096, 1, kDxt5, size);
        Check(table.Capture(MakeRequest(&huge[0], "*lightmap9_big_secondary", kMapType2D, oversized, huge.data())) ==
                  CaptureOutcome::OverflowBytes,
              "a payload past the per-image bound is refused");
        CaptureLookup lookup;
        Check(!table.Lookup("*lightmap9_big_secondary", &lookup), "an oversized image retains nothing");
        Check(table.Statistics().overflow, "the overflow is reported");
    }

    // ---- 10. Disarming stops capturing and keeps what was captured readable. --------
    {
        table.Disarm();
        Check(!table.Armed(), "disarm stops the capture");
        Check(table.Capture(MakeRequest(lightmapPayload.data(), "*lightmap10_primary", kMapType2D, realLightmap,
                                        lightmapPayload.data())) == CaptureOutcome::NotArmed,
              "a disarmed capture refuses the image");
        CaptureLookup lookup;
        Check(table.Lookup("*lightmap0_secondary", &lookup) && lookup.found,
              "a disarmed capture still reports what it kept");
    }

    // ---- 11. The run bound: exactly full is accepted, one more is refused. ---------
    {
        Check(table.Arm(), "re-arming reserves the arena again");
        const CaptureStats reset = table.Statistics();
        Check(reset.images == 0 && reset.bytesCaptured == 0 && reset.examined == 0, "arming resets the counters");
        CaptureLookup lookup;
        Check(!table.Lookup("*lightmap0_secondary", &lookup), "arming drops the previous captures");

        // 4096x4096 DXT5 without mipmaps is exactly 16 MiB, the per-image bound.
        std::vector<unsigned char> block(size_t(16777216), 0x11);
        const Header full(1, kNoMips, 4096, 4096, 1, kDxt5, 16777216);
        const unsigned whole = unsigned(kMaxTotalBytes / kMaxImageBytes);
        for (unsigned i = 0; i < whole; ++i)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "*lightmap11_%u_secondary", i);
            Check(table.Capture(MakeRequest(&block[0], name, kMapType2D, full, block.data())) ==
                      CaptureOutcome::Captured,
                  "a payload that exactly fits the per-image bound is captured");
        }
        Check(table.Statistics().bytesCaptured == kMaxTotalBytes, "the run bound is exactly full");
        unsigned char tinyBytes[8] = {};
        const Header tiny(1, kNoMips, 4, 4, 1, kDxt1, 8);
        Check(table.Capture(MakeRequest(&tinyBytes[0], "*lightmap11_extra_secondary", kMapType2D, tiny, tinyBytes)) ==
                  CaptureOutcome::OverflowBytes,
              "one more payload past the run bound is refused");
        Check(table.Statistics().overflow, "the run overflow is reported");
    }

    // ---- 12. The count bound, with payloads small enough that bytes cannot be it. ---
    {
        Check(table.Arm(), "arming for the count bound");
        unsigned captured = 0;
        for (unsigned i = 0; i < kMaxImages + 2; ++i)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "*lightmap12_%u_secondary", i);
            unsigned char eight[8] = {};
            const Header small(1, kNoMips, 4, 4, 1, kDxt1, 8);
            if (table.Capture(MakeRequest(&eight[0], name, kMapType2D, small, eight)) == CaptureOutcome::Captured)
                ++captured;
        }
        Check(captured == kMaxImages, "exactly the count bound is captured");
        const CaptureStats stats = table.Statistics();
        Check(stats.images == kMaxImages, "the table holds the count bound");
        Check(stats.overflow, "the count overflow is reported");
        Check(stats.rejected == 2, "each refusal past the bound is counted");
    }

    // ---- 13. One name, one image: repeats are deduplicated, differences refused. ----
    {
        Check(table.Arm(), "arming for the name rules");
        const Header small(1, kNoMips, 4, 4, 1, kDxt1, 8);
        unsigned char first[8] = {1, 1, 1, 1, 1, 1, 1, 1};
        unsigned char alsoFirst[8] = {1, 1, 1, 1, 1, 1, 1, 1};
        unsigned char other[8] = {2, 2, 2, 2, 2, 2, 2, 2};
        Check(table.Capture(MakeRequest(&first[0], "*lightmap13_secondary", kMapType2D, small, first)) ==
                  CaptureOutcome::Captured,
              "the first payload under a name is captured");
        Check(table.Capture(MakeRequest(&alsoFirst[0], "*lightmap13_secondary", kMapType2D, small, alsoFirst)) ==
                  CaptureOutcome::Duplicate,
              "the same bytes and header under the same name is the same image");
        Check(table.Capture(MakeRequest(&other[0], "*lightmap13_secondary", kMapType2D, small, other)) ==
                  CaptureOutcome::AmbiguousName,
              "different bytes under the same name are refused");
        CaptureLookup lookup;
        Check(!table.Lookup("*lightmap13_secondary", &lookup) && !lookup.found,
              "an ambiguous name is not resolvable");
        RejectedImage rejection;
        Check(table.LookupRejection("*lightmap13_secondary", &rejection) &&
                  rejection.outcome == CaptureOutcome::AmbiguousName,
              "the ambiguity is recorded with its reason");
        Check(table.Statistics().ambiguous == 1, "the ambiguity is counted once");
        Check(table.Statistics().images == 1, "an ambiguous name still holds one record, not two");

        // A different header under one name is ambiguous even when the bytes match.
        unsigned char dxt3[16] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        const Header asDxt1(1, kNoMips, 4, 4, 1, kDxt1, 8);
        const Header asDxt3(1, kNoMips, 4, 4, 1, kDxt3, 16);
        Check(table.Capture(MakeRequest(&dxt3[0], "*lightmap13b_secondary", kMapType2D, asDxt1, dxt3)) ==
                  CaptureOutcome::Captured,
              "the first header under a name is captured");
        Check(table.Capture(MakeRequest(&dxt3[0], "*lightmap13b_secondary", kMapType2D, asDxt3, dxt3)) ==
                  CaptureOutcome::AmbiguousName,
              "a different header under the same name is refused");
        Check(!table.Lookup("*lightmap13b_secondary", &lookup), "the second ambiguous name is not resolvable");
    }

    // ---- 14. A repeat is decided by the bytes, never by the payload address. ---------
    {
        Check(table.Arm(), "arming for the address rule");
        const Header small(1, kNoMips, 4, 4, 1, kDxt1, 8);
        // One buffer, as the loader presents it: the payload is read from the zone's stream
        // position, so every record arrives at the same address and the bytes need not be the
        // same. The address and the header are therefore no evidence that a repeat is a repeat.
        unsigned char buffer[8] = {9, 9, 9, 9, 9, 9, 9, 9};
        Check(table.Capture(MakeRequest(buffer, "*lightmap14_secondary", kMapType2D, small, buffer)) ==
                  CaptureOutcome::Captured,
              "the first payload under the name is captured");
        Check(table.Capture(MakeRequest(buffer, "*lightmap14_secondary", kMapType2D, small, buffer)) ==
                  CaptureOutcome::Duplicate,
              "the same bytes at the same address are a repeat");
        // The next record reuses the buffer: same address, same header, different pixels.
        buffer[3] = (unsigned char)(buffer[3] ^ 0x5A);
        Check(table.Capture(MakeRequest(buffer, "*lightmap14_secondary", kMapType2D, small, buffer)) ==
                  CaptureOutcome::AmbiguousName,
              "different bytes at the same address are not a repeat");
        CaptureLookup lookup;
        Check(!table.Lookup("*lightmap14_secondary", &lookup) && !lookup.found,
              "the name stops resolving once the second image arrives");
        CaptureStats stats = table.Statistics();
        Check(stats.ambiguous == 1, "the mismatch is counted as one ambiguity");
        Check(stats.images == 1, "the mismatch adds no record");
        Check(stats.bytesCaptured == 8, "the mismatch copies no second payload");
        // Back to the first payload: ambiguity is sticky, so the name cannot be revived.
        buffer[3] = (unsigned char)(buffer[3] ^ 0x5A);
        Check(table.Capture(MakeRequest(buffer, "*lightmap14_secondary", kMapType2D, small, buffer)) ==
                  CaptureOutcome::AmbiguousName,
              "an unchanged repeat arriving later cannot revive an ambiguous name");
        Check(!table.Lookup("*lightmap14_secondary", &lookup), "the revived repeat does not resolve either");
        stats = table.Statistics();
        Check(stats.ambiguous == 1, "the ambiguity is counted once, not once per arrival");
        Check(stats.duplicates == 1, "only the byte-identical repeat counts as a duplicate");
        RejectedImage rejection;
        Check(table.LookupRejection("*lightmap14_secondary", &rejection) &&
                  rejection.outcome == CaptureOutcome::AmbiguousName,
              "the ambiguity stays recorded with its reason");
    }

    // ---- 15. What the header cannot describe exactly is refused, not guessed. --------
    {
        Check(table.Arm(), "arming for the layout rule");
        // One level is one level's bytes at any dimensions; a chain is exact only on dimensions
        // that halve down to exactly 1x1 on both axes.
        Check(LayoutBytesAreExact(kNoMips, 5, 5), "a single level is an exact layout at any dimensions");
        Check(LayoutBytesAreExact(0, 8, 8), "a power-of-two chain is exact");
        Check(LayoutBytesAreExact(0, 512, 2048), "a rectangular power-of-two chain is exact");
        Check(!LayoutBytesAreExact(0, 5, 5), "a 5x5 chain is not exact");
        Check(!LayoutBytesAreExact(0, 5, 4), "a 5x4 chain is not exact");
        Check(!LayoutBytesAreExact(0, 8, 6), "one non-power-of-two axis is enough to refuse the chain");
        // The disagreement this rule exists for. The engine sizes each level with the clamped
        // halving but takes the level count from Image_CountMipmaps (r_image.cpp:459-470), which
        // is 1 + ceil(log2(5)) = 4 for 5x5: 5x5, 2x2, 1x1, 1x1, and 56 bytes as DXT1. The sum
        // below stops at three levels and 48 bytes, so neither number can be claimed as the
        // image's size.
        Check(MaxMipLevels(5, 5) == 4, "the engine counts four levels for 5x5");
        Check(Expected2DPayloadBytes(kDxt1, 0, 5, 5, &bytes) && bytes == 48,
              "the 5x5 DXT1 sum is three levels, and the engine's upload walk is four");
        Check(Expected2DPayloadBytes(kArgb32, 0, 5, 5, &bytes) && bytes == 120,
              "the 5x5 ARGB sum is 100 + 16 + 4");
        Check(Expected2DPayloadBytes(kDxt5, 0, 5, 4, &bytes) && bytes == 64,
              "the 5x4 DXT5 sum is 32 + 16 + 16");
        Check(Expected2DPayloadBytes(kArgb32, 0, 5, 4, &bytes) && bytes == 100,
              "the 5x4 ARGB sum is 80 + 16 + 4");

        // Large enough for the widest payload below: the 8x8 ARGB chain is 340 bytes.
        unsigned char payload[400] = {};
        struct LayoutCase
        {
            const char *name;
            int flags;
            int width;
            int height;
            int32_t format;
            int32_t resourceSize;
        };
        // Each case carries the size this unit's own sum gives, so the refusal has to come from
        // the layout rule and not from the size comparison behind it.
        const LayoutCase refused[4] = {{"*lightmap15_5x5_dxt1_secondary", 0, 5, 5, kDxt1, 48},
                                       {"*lightmap15_5x5_argb_secondary", 0, 5, 5, kArgb32, 120},
                                       {"*lightmap15_5x4_dxt5_secondary", 0, 5, 4, kDxt5, 64},
                                       {"*lightmap15_5x4_argb_secondary", 0, 5, 4, kArgb32, 100}};
        for (const LayoutCase &layoutCase : refused)
        {
            const Header header(4, layoutCase.flags, layoutCase.width, layoutCase.height, 1, layoutCase.format,
                                layoutCase.resourceSize);
            Check(table.Capture(MakeRequest(&payload[0], layoutCase.name, kMapType2D, header, payload)) ==
                      CaptureOutcome::UnsupportedLayout,
                  "a non-power-of-two mip chain is refused");
        }
        // The refusal names the layout it saw, so the next run reads why.
        RejectedImage rejection;
        Check(table.LookupRejection("*lightmap15_5x5_dxt1_secondary", &rejection) &&
                  rejection.outcome == CaptureOutcome::UnsupportedLayout && rejection.meta.width == 5 &&
                  rejection.meta.height == 5,
              "the layout refusal records the dimensions it saw");
        CaptureLookup lookup;
        Check(!table.Lookup("*lightmap15_5x5_dxt1_secondary", &lookup), "a refused layout retains nothing");

        // A single level on the same dimensions has one agreed layout, so it is still captured:
        // 5x5 DXT1 is ceil(5/4) by ceil(5/4) whole blocks, four of them, 32 bytes.
        const Header flat5x5(1, kNoMips, 5, 5, 1, kDxt1, 32);
        Check(table.Capture(MakeRequest(&payload[0], "*lightmap15_5x5_flat_secondary", kMapType2D, flat5x5,
                                        payload)) == CaptureOutcome::Captured,
              "a non-power-of-two image without mips is captured");
        const Header flat5x4(1, kNoMips, 5, 4, 1, kDxt5, 32);
        Check(table.Capture(MakeRequest(&payload[0], "*lightmap15_5x4_flat_secondary", kMapType2D, flat5x4,
                                        payload)) == CaptureOutcome::Captured,
              "a 5x4 DXT5 image without mips is captured");

        // And the power-of-two chains the rule admits are unchanged.
        const Header dxt1Chain(4, 0, 8, 8, 1, kDxt1, 56);
        Check(table.Capture(MakeRequest(&payload[0], "*lightmap15_8x8_mips_secondary", kMapType2D, dxt1Chain,
                                        payload)) == CaptureOutcome::Captured,
              "a power-of-two DXT1 chain is still captured");
        const Header argbChain(4, 0, 8, 8, 1, kArgb32, 340);
        Check(table.Capture(MakeRequest(&payload[0], "*lightmap15_8x8_argb_secondary", kMapType2D, argbChain,
                                        payload)) == CaptureOutcome::Captured,
              "a power-of-two ARGB chain is still captured");
        Check(table.Lookup("*lightmap15_8x8_mips_secondary", &lookup) && lookup.payloadBytes == 56,
              "the admitted chain keeps its whole payload");
        const CaptureStats stats = table.Statistics();
        Check(!stats.overflow, "a layout refusal is not an overflow");
        Check(stats.ambiguous == 0, "a layout refusal does not make the name ambiguous");
    }

    // ---- 16. A refused offer claims its name just as a capture does. -----------------
    // The loader offers one image per record, and two records under one lightmap name are two
    // images whichever of them this table can capture. Rejecting one of them is a decision not to
    // copy its bytes, so the two can never be compared afterwards - which is exactly why the name
    // has to be retired rather than left pointing at the one that was copied.
    {
        Check(table.Arm(), "arming for the refused-name rule");
        const Header argb(1, kNoMips, 4, 4, 1, kArgb32, 64);
        unsigned char pixels[64];
        for (unsigned i = 0; i < sizeof(pixels); ++i)
            pixels[i] = (unsigned char)(0x40u + i);
        // L8 is a format the engine loads and packaged assets carry, and one this unit has no
        // layout for: refused for its format, not for a size it could be compared at.
        const Header l8(1, kNoMips, 4, 4, 1, 50 /* D3DFMT_L8 */, 16);
        unsigned char grey[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                                  0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
        CaptureLookup lookup;

        // Captured first, then refused under the same name.
        Check(table.Capture(MakeRequest(pixels, "*lightmap16_a_secondary", kMapType2D, argb, pixels)) ==
                  CaptureOutcome::Captured,
              "the ARGB image is captured");
        Check(table.Lookup("*lightmap16_a_secondary", &lookup) && lookup.found, "the captured image resolves");
        Check(table.Capture(MakeRequest(grey, "*lightmap16_a_secondary", kMapType2D, l8, grey)) ==
                  CaptureOutcome::UnsupportedFormat,
              "the L8 image under the same name is refused");
        Check(!table.Lookup("*lightmap16_a_secondary", &lookup) && !lookup.found,
              "the refused same-name offer retires the payload that was already copied");
        // And the payload that was copied cannot be revived by arriving again.
        Check(table.Capture(MakeRequest(pixels, "*lightmap16_a_secondary", kMapType2D, argb, pixels)) ==
                  CaptureOutcome::AmbiguousName,
              "a later repeat of the first image cannot revive the name");
        Check(!table.Lookup("*lightmap16_a_secondary", &lookup), "the revived repeat does not resolve either");
        Check(table.Statistics().ambiguous == 1, "the conflict is counted once, not once per arrival");
        Check(table.Statistics().images == 1, "the refused offer adds no record");
        RejectedImage rejection;
        Check(table.LookupRejection("*lightmap16_a_secondary", &rejection) && rejection.meta.format == 50,
              "the refusal records the format it saw");

        // The other order: refused first, capturable second. The capturable image does not take the
        // name over by being the one that could have been copied.
        Check(table.Capture(MakeRequest(grey, "*lightmap16_b_secondary", kMapType2D, l8, grey)) ==
                  CaptureOutcome::UnsupportedFormat,
              "the L8 image on its own is refused");
        Check(!table.Lookup("*lightmap16_b_secondary", &lookup), "a refused image is not retained");
        Check(table.Capture(MakeRequest(pixels, "*lightmap16_b_secondary", kMapType2D, argb, pixels)) ==
                  CaptureOutcome::AmbiguousName,
              "the capturable image cannot claim a name a refusal already holds");
        Check(!table.Lookup("*lightmap16_b_secondary", &lookup), "so the name resolves to nothing");
        Check(table.Statistics().images == 1, "and no second payload is stored");
        Check(table.Statistics().bytesCaptured == 64, "and no second payload is copied");
        Check(table.Statistics().ambiguous == 2, "each conflicting name is counted once");

        // The rule refuses two different offers, not two copies of one offer: a byte-identical
        // repeat from another buffer is still the same image and the name keeps resolving.
        unsigned char same[64];
        std::memcpy(same, pixels, sizeof(same));
        Check(table.Capture(MakeRequest(same, "*lightmap16_c_secondary", kMapType2D, argb, same)) ==
                  CaptureOutcome::Captured,
              "the first offer of the kept name is captured");
        Check(table.Capture(MakeRequest(pixels, "*lightmap16_c_secondary", kMapType2D, argb, pixels)) ==
                  CaptureOutcome::Duplicate,
              "the same bytes at another address under the same name are a repeat");
        Check(table.Lookup("*lightmap16_c_secondary", &lookup) && lookup.found && lookup.payloadBytes == 64,
              "a repeat leaves the name resolvable");
    }

    // ---- 17. The same rule for the refusals that are not about the payload format. ----
    {
        Check(table.Arm(), "arming for the refused-name rule, second shape");
        const Header argb(1, kNoMips, 4, 4, 1, kArgb32, 64);
        unsigned char pixels[64] = {};
        pixels[0] = 0x7F;
        unsigned char blocks[64] = {};
        CaptureLookup lookup;

        // A 3D record under a lightmap name, offered behind a 2D image.
        Check(table.Capture(MakeRequest(pixels, "*lightmap17_maptype_secondary", kMapType2D, argb, pixels)) ==
                  CaptureOutcome::Captured,
              "the 2D image is captured");
        Check(table.Capture(MakeRequest(pixels, "*lightmap17_maptype_secondary", 4 /* MAPTYPE_3D */, argb, pixels)) ==
                  CaptureOutcome::MapTypeUnsupported,
              "a 3D offer under the same name is refused");
        Check(!table.Lookup("*lightmap17_maptype_secondary", &lookup), "the 3D offer retires the name");

        // A mip chain on non-power-of-two dimensions: a layout this unit refuses rather than pick
        // one of the engine's two answers for, offered under a captured name.
        const Header npot(4, 0, 5, 5, 1, kDxt1, 48);
        Check(table.Capture(MakeRequest(pixels, "*lightmap17_npot_secondary", kMapType2D, argb, pixels)) ==
                  CaptureOutcome::Captured,
              "the second 2D image is captured");
        Check(table.Capture(MakeRequest(blocks, "*lightmap17_npot_secondary", kMapType2D, npot, blocks)) ==
                  CaptureOutcome::UnsupportedLayout,
              "the non-power-of-two chain under the same name is refused");
        Check(!table.Lookup("*lightmap17_npot_secondary", &lookup), "the layout refusal retires the name");

        // And the reverse: layout refusal first, capturable image second.
        Check(table.Capture(MakeRequest(blocks, "*lightmap17_npot_first_secondary", kMapType2D, npot, blocks)) ==
                  CaptureOutcome::UnsupportedLayout,
              "the non-power-of-two chain on its own is refused");
        Check(table.Capture(MakeRequest(pixels, "*lightmap17_npot_first_secondary", kMapType2D, argb, pixels)) ==
                  CaptureOutcome::AmbiguousName,
              "the capturable image cannot claim that name");
        Check(!table.Lookup("*lightmap17_npot_first_secondary", &lookup), "and it stays unresolved");

        // A resource size that disagrees with its own header is a claim on the name too.
        const Header wrongSize(1, kNoMips, 4, 4, 1, kArgb32, 60);
        Check(table.Capture(MakeRequest(pixels, "*lightmap17_size_secondary", kMapType2D, argb, pixels)) ==
                  CaptureOutcome::Captured,
              "the third 2D image is captured");
        Check(table.Capture(MakeRequest(pixels, "*lightmap17_size_secondary", kMapType2D, wrongSize, pixels)) ==
                  CaptureOutcome::SizeMismatch,
              "the wrong size under the same name is refused");
        Check(!table.Lookup("*lightmap17_size_secondary", &lookup), "the size refusal retires the name");

        const CaptureStats stats = table.Statistics();
        // Four names claimed twice over: the map-type offer, the layout offer, the layout-first
        // name, and the wrong-size header. Each is counted once, however many offers follow.
        Check(stats.ambiguous == 4, "each conflicting name is counted once");
        Check(stats.images == 3, "no refused offer added a record");
        Check(stats.bytesCaptured == 192, "no refused offer copied a byte");
    }

    // ---- 18. The name table's own bound: a name it cannot record is refused, and so is
    //          every lookup, because the offers it never saw are the ones it cannot rule out. --
    {
        Check(table.Arm(), "arming for the name capacity rule");
        const Header argb(1, kNoMips, 4, 4, 1, kArgb32, 64);
        unsigned char pixels[64] = {};
        Check(table.Capture(MakeRequest(pixels, "*lightmap18_kept_secondary", kMapType2D, argb, pixels)) ==
                  CaptureOutcome::Captured,
              "the image captured before the name table fills");
        CaptureLookup lookup;
        Check(table.Lookup("*lightmap18_kept_secondary", &lookup) && lookup.found, "and the name resolves");

        // A refused offer costs no arena and still occupies a name record, which is the cheapest
        // way to reach the bound: an L8 name is recorded and refused.
        const Header l8(1, kNoMips, 4, 4, 1, 50 /* D3DFMT_L8 */, 16);
        char filler[64];
        unsigned refused = 0;
        for (unsigned i = 0; i + 1 < kMaxTrackedNames; ++i)
        {
            std::snprintf(filler, sizeof(filler), "*lightmap18_filler_%u_secondary", i);
            if (table.Capture(MakeRequest(pixels, filler, kMapType2D, l8, pixels)) ==
                CaptureOutcome::UnsupportedFormat)
                ++refused;
        }
        Check(refused == kMaxTrackedNames - 1, "every filler name was recorded and refused");
        Check(!table.Statistics().overflow, "a table that is exactly full has left no name untracked");
        Check(table.Lookup("*lightmap18_kept_secondary", &lookup) && lookup.found,
              "a full name table still resolves the names it holds");

        const char *overflowName = "*lightmap18_overflow_secondary";
        Check(table.Capture(MakeRequest(pixels, overflowName, kMapType2D, argb, pixels)) ==
                  CaptureOutcome::OverflowCount,
              "a name past the capacity is refused rather than captured unwatched");
        Check(table.Statistics().overflow, "the name overflow is reported");
        Check(!table.Lookup("*lightmap18_kept_secondary", &lookup) && !lookup.found,
              "no name is reported once one of the run's names could not be recorded");

        // Rearming forgets the untracked offer along with everything else, so the next run can
        // vouch for its own names again.
        Check(table.Arm(), "arming again after the name overflow");
        const Header small(1, kNoMips, 4, 4, 1, kDxt1, 8);
        unsigned char block[8] = {3, 3, 3, 3, 3, 3, 3, 3};
        Check(table.Capture(MakeRequest(block, "*lightmap18_kept_secondary", kMapType2D, small, block)) ==
                  CaptureOutcome::Captured,
              "the next run captures its own image");
        Check(table.Lookup("*lightmap18_kept_secondary", &lookup) && lookup.found, "and the lookup answers again");
    }

    if (failures)
        std::printf("world_image_capture_test=FAIL failures=%d\n", failures);
    else
        std::printf("world_image_capture_test=ok\n");
    return failures ? 1 : 0;
}
