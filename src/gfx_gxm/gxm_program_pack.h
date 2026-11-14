#pragma once

// Many compiled GXM programs in one file: <program folder>/programs.pack.
//
// The program cache (gxm_program_cache.h) keeps one file a program, and a
// map asks for hundreds, each a separate open on the memory card. A pack
// holds them behind one open, and it can be made on another machine: Vita3K
// runs the Vita's own compiler, so the programs a PC run compiled are the
// ones the console would compile from the same build, without the minutes
// that takes there. scripts/platform/vita/vita_pack_programs.py makes a
// pack from a program folder. A program is found by the same key as its
// file, so a pack from another translator is simply not used.
//
// The file, little-endian:
//
//     8 bytes   "KGXPACK1"
//     u32       count
//     u32       0
//     count x   { u64 key, u32 offset from the file's start, u32 size },
//               in rising order of key, each key once
//     the programs
//
// This header reads the index and makes no Vita call; it is checked on the
// host by scripts/tests/test_vita_program_pack.py.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace gfx_gxm
{
constexpr char kPackName[] = "programs.pack";
constexpr size_t kPackHeadBytes = 16;
constexpr size_t kPackEntryBytes = 16;
constexpr uint32_t kMaxPackPrograms = 32768;
constexpr uint32_t kMaxPackedProgramBytes = 4u << 20;

struct PackEntry
{
    uint64_t key;
    uint32_t offset;
    uint32_t size;
};

inline uint32_t PackU32(const uint8_t *bytes)
{
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

// The number of programs a pack's first 16 bytes announce, or -1 when they
// are not a pack's.
inline long PackCount(const uint8_t head[kPackHeadBytes])
{
    if (memcmp(head, "KGXPACK1", 8) != 0 || PackU32(head + 12) != 0)
        return -1;
    const uint32_t count = PackU32(head + 8);
    return count <= kMaxPackPrograms ? long(count) : -1;
}

// Reads the index that follows the head. False when it is not in rising
// order of key or names bytes that are not a program's place in a file of
// fileBytes.
inline bool ReadPackIndex(const uint8_t *index, size_t count, uint64_t fileBytes, std::vector<PackEntry> &entries)
{
    entries.clear();
    entries.reserve(count);
    const uint64_t programsFrom = kPackHeadBytes + uint64_t(count) * kPackEntryBytes;
    for (size_t i = 0; i < count; ++i)
    {
        const uint8_t *at = index + i * kPackEntryBytes;
        PackEntry entry;
        entry.key = uint64_t(PackU32(at)) | uint64_t(PackU32(at + 4)) << 32;
        entry.offset = PackU32(at + 8);
        entry.size = PackU32(at + 12);
        const bool inOrder = entries.empty() || entries.back().key < entry.key;
        if (!inOrder || entry.size == 0 || entry.size > kMaxPackedProgramBytes || entry.offset < programsFrom ||
            uint64_t(entry.offset) + entry.size > fileBytes)
        {
            entries.clear();
            return false;
        }
        entries.push_back(entry);
    }
    return true;
}

inline const PackEntry *FindInPack(const std::vector<PackEntry> &entries, uint64_t key)
{
    size_t low = 0, high = entries.size();
    while (low < high)
    {
        const size_t middle = low + (high - low) / 2;
        if (entries[middle].key < key)
            low = middle + 1;
        else
            high = middle;
    }
    return low < entries.size() && entries[low].key == key ? &entries[low] : nullptr;
}

// A pack on the card, kept open. One thread at a time.
class ProgramPack
{
  public:
    ~ProgramPack()
    {
        Close();
    }

    // False when there is no file of that name. Refused() then says whether
    // one was there that is not a pack this reads.
    bool Open(const char *path)
    {
        Close();
        FILE *file = std::fopen(path, "rb");
        if (!file)
            return false;
        uint8_t head[kPackHeadBytes];
        std::vector<uint8_t> index;
        long count = -1, size = 0;
        if (std::fread(head, 1, sizeof(head), file) == sizeof(head) && (count = PackCount(head)) >= 0)
        {
            index.resize(size_t(count) * kPackEntryBytes);
            std::fseek(file, 0, SEEK_END);
            size = std::ftell(file);
            std::fseek(file, long(kPackHeadBytes), SEEK_SET);
        }
        if (count >= 0 && size > 0 && std::fread(index.data(), 1, index.size(), file) == index.size() &&
            ReadPackIndex(index.data(), size_t(count), uint64_t(size), entries_))
        {
            file_ = file;
            return true;
        }
        std::fclose(file);
        refused_ = true;
        return false;
    }

    bool Read(uint64_t key, std::vector<uint8_t> &program)
    {
        const PackEntry *entry = file_ ? FindInPack(entries_, key) : nullptr;
        if (!entry || std::fseek(file_, long(entry->offset), SEEK_SET) != 0)
            return false;
        program.resize(entry->size);
        return std::fread(program.data(), 1, program.size(), file_) == program.size();
    }

    void Close()
    {
        if (file_)
            std::fclose(file_);
        file_ = nullptr;
        entries_.clear();
        refused_ = false;
    }

    const std::vector<PackEntry> &Entries() const
    {
        return entries_;
    }

    bool Refused() const
    {
        return refused_;
    }

  private:
    FILE *file_ = nullptr;
    std::vector<PackEntry> entries_;
    bool refused_ = false;
};
} // namespace gfx_gxm
