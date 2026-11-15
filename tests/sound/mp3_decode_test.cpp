// Decodes a made-up MP3 stream with dr_mp3 and writes the samples, so two
// builds of the decoder can be compared (scripts/tests/test_mp3_neon_decode.py:
// the PS Vita builds dr_mp3 for NEON, everything else of it for scalar VFP).
//
// No game data and no encoder: the stream is frames of MPEG-1 Layer III,
// 44.1 kHz, 128 kbit/s, two channels, whose headers and side information are
// well formed and whose main data is noise from a fixed seed. A decoder makes
// samples of any such frame: the Huffman tables, the requantizer, both stereo
// modes, the long, short and mixed block transforms and the synthesis filter
// all run on it.
//
// Usage: mp3_decode_test <output file>
// Writes the 16-bit samples as they come, and prints how many there were.

#define DR_MP3_IMPLEMENTATION
#include <dr_libs/dr_mp3.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace
{
uint32_t g_seed = 0x2545f491u;

uint32_t Random()
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

uint32_t RandomBelow(uint32_t count) { return Random() % count; }

struct BitWriter
{
    std::vector<uint8_t> &bytes;
    uint32_t held = 0;
    int count = 0;

    void Put(uint32_t value, int bits)
    {
        for (int bit = bits - 1; bit >= 0; --bit)
        {
            held = held << 1 | ((value >> bit) & 1u);
            if (++count == 8)
            {
                bytes.push_back(static_cast<uint8_t>(held));
                held = 0;
                count = 0;
            }
        }
    }
};

constexpr int kFrameBytes = 417;    // 144 * 128000 / 44100, no padding
constexpr int kSideInfoBytes = 32;  // MPEG-1, two channels
constexpr int kMainDataBits = (kFrameBytes - 4 - kSideInfoBytes) * 8;

void AddFrame(std::vector<uint8_t> &stream, uint32_t frame)
{
    // Plain stereo, then joint stereo with each of its four extensions
    // (intensity and mid/side off and on).
    const uint32_t mode = frame % 5 == 0 ? 0u : 1u;
    const uint32_t extension = frame % 5 == 0 ? 0u : (frame % 5) - 1;
    stream.push_back(0xff);
    stream.push_back(0xfb); // MPEG-1, Layer III, no CRC
    stream.push_back(0x90); // 128 kbit/s, 44.1 kHz, no padding
    stream.push_back(static_cast<uint8_t>(mode << 6 | extension << 4));

    const size_t sideStart = stream.size();
    BitWriter side{stream};
    side.Put(0, 9);            // main_data_begin: every frame holds its own data
    side.Put(0, 3);            // private bits
    side.Put(RandomBelow(256), 8); // scfsi, four bits a channel
    for (int granule = 0; granule < 2; ++granule)
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            side.Put(kMainDataBits / 4 - RandomBelow(64), 12); // part2_3_length
            side.Put(RandomBelow(289), 9);                     // big_values
            // Quiet enough that most samples are not clipped: clipping would
            // hide a wrong sample.
            side.Put(120 + RandomBelow(50), 8);                // global_gain
            side.Put(RandomBelow(16), 4);                      // scalefac_compress
            const bool switched = RandomBelow(3) == 0;
            side.Put(switched, 1);
            if (switched)
            {
                side.Put(1 + RandomBelow(3), 2); // block_type: start, short, stop
                side.Put(RandomBelow(2), 1);     // mixed_block_flag
                side.Put(RandomBelow(32), 5);    // table_select
                side.Put(RandomBelow(32), 5);
                side.Put(RandomBelow(8), 3);     // subblock_gain
                side.Put(RandomBelow(8), 3);
                side.Put(RandomBelow(8), 3);
            }
            else
            {
                side.Put(RandomBelow(32), 5);
                side.Put(RandomBelow(32), 5);
                side.Put(RandomBelow(32), 5);
                side.Put(RandomBelow(16), 4); // region0_count
                side.Put(RandomBelow(8), 3);  // region1_count
            }
            side.Put(RandomBelow(2), 1); // preflag
            side.Put(RandomBelow(2), 1); // scalefac_scale
            side.Put(RandomBelow(2), 1); // count1table_select
        }
    }
    if (side.count != 0 || stream.size() - sideStart != kSideInfoBytes)
    {
        std::fprintf(stderr, "the side information is not %d bytes\n", kSideInfoBytes);
        std::exit(2);
    }
    for (int i = 0; i < kFrameBytes - 4 - kSideInfoBytes; ++i)
        stream.push_back(static_cast<uint8_t>(Random() >> 11));
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: mp3_decode_test <output file>\n");
        return 2;
    }
    constexpr uint32_t kFrames = 400;
    std::vector<uint8_t> stream;
    for (uint32_t frame = 0; frame < kFrames; ++frame)
        AddFrame(stream, frame);

    drmp3 mp3;
    if (!drmp3_init_memory(&mp3, stream.data(), stream.size(), nullptr))
    {
        std::fprintf(stderr, "dr_mp3 did not take the stream\n");
        return 1;
    }
    std::FILE *out = std::fopen(argv[1], "wb");
    if (!out)
    {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 2;
    }
    // In the sizes the game's driver reads (8192 frames a buffer).
    std::vector<int16_t> pcm(8192 * 2);
    uint64_t frames = 0;
    for (;;)
    {
        const drmp3_uint64 got = drmp3_read_pcm_frames_s16(&mp3, 8192, pcm.data());
        if (!got)
            break;
        std::fwrite(pcm.data(), sizeof(int16_t) * 2, static_cast<size_t>(got), out);
        frames += got;
    }
    std::fclose(out);
    drmp3_uninit(&mp3);
    std::printf("decoded %llu frames of %u channels at %u Hz from %u MP3 frames\n",
        static_cast<unsigned long long>(frames), mp3.channels, mp3.sampleRate, kFrames);
    return 0;
}
