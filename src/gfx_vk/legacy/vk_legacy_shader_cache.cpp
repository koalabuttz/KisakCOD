// Vulkan Legacy adapter: the shader cache. Translated shaders by their D3D9
// bytecode, for the whole process, and in a file between runs. See vk_legacy.h.

#include "vk_legacy_internal.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gfx_vk::legacy
{
namespace
{
// The file: the magic, the file version, the translator's revision, the entry
// count, then per entry the bytecode, the serialised translation and the
// vertex shader's SPIR-V (empty for pixel shaders, and where the device fixes
// depth compare in shaders), and last a 64-bit FNV-1a
// of everything before it, so a damaged file is refused whole. Integers are
// little endian, and every byte string has its length first.
constexpr char kMagic[8] = {'K', 'I', 'S', 'A', 'K', 'V', 'K', 'S'};
constexpr uint32_t kFileVersion = 2;
// Bounds, so a damaged or hostile file cannot make the game allocate wildly.
constexpr uint32_t kMaxEntries = 65536;
constexpr uint32_t kMaxBytecode = 1u << 20;
constexpr long kMaxFileBytes = 512l << 20;

struct Entry
{
    std::vector<uint8_t> bytecode;
    std::shared_ptr<const CachedShader> shader;
};

struct Cache
{
    std::mutex lock;
    std::unordered_multimap<uint64_t, Entry> entries; // by FNV-1a 64 of the bytecode
    std::string loadedPath; // the file read into it, if any
    bool changed = false;   // entries added since the file was last read or written
};

// Never destroyed: a device can still be released while statics go away.
Cache &TheCache()
{
    static Cache *cache = new Cache();
    return *cache;
}

uint64_t Hash(const void *data, size_t bytes)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < bytes; ++i)
        hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
}

const Entry *Find(Cache &cache, uint64_t hash, const void *bytecode, size_t bytes)
{
    const auto range = cache.entries.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
        if (it->second.bytecode.size() == bytes && std::memcmp(it->second.bytecode.data(), bytecode, bytes) == 0)
            return &it->second;
    return nullptr;
}

void Add(Cache &cache, const void *bytecode, size_t bytes, std::shared_ptr<const CachedShader> shader)
{
    const uint64_t hash = Hash(bytecode, bytes);
    if (Find(cache, hash, bytecode, bytes) || cache.entries.size() >= kMaxEntries)
        return;
    Entry entry;
    entry.bytecode.assign(static_cast<const uint8_t *>(bytecode), static_cast<const uint8_t *>(bytecode) + bytes);
    entry.shader = std::move(shader);
    cache.entries.emplace(hash, std::move(entry));
    cache.changed = true;
}

void PutU32(std::vector<uint8_t> &out, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void PutBytes(std::vector<uint8_t> &out, const void *data, size_t size)
{
    PutU32(out, static_cast<uint32_t>(size));
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    out.insert(out.end(), bytes, bytes + size);
}

class Reader
{
public:
    Reader(const uint8_t *data, size_t size) : data_(data), size_(size) {}
    bool U32(uint32_t &value)
    {
        if (size_ - at_ < 4)
            return false;
        value = 0;
        for (int i = 0; i < 4; ++i)
            value |= uint32_t(data_[at_++]) << (8 * i);
        return true;
    }
    // A length-prefixed byte string, in place.
    bool Bytes(const uint8_t *&data, uint32_t &size, uint32_t limit)
    {
        if (!U32(size) || size > limit || size_ - at_ < size)
            return false;
        data = data_ + at_;
        at_ += size;
        return true;
    }
    bool Raw(void *out, size_t size)
    {
        if (size_ - at_ < size)
            return false;
        std::memcpy(out, data_ + at_, size);
        at_ += size;
        return true;
    }
    bool Done() const { return at_ == size_; }

private:
    const uint8_t *data_;
    size_t size_;
    size_t at_ = 0;
};

bool ReadFile(const char *path, std::vector<uint8_t> &out, std::string &message)
{
    std::FILE *file = std::fopen(path, "rb");
    if (!file)
    {
        message = "no cache file yet";
        return false;
    }
    bool ok = std::fseek(file, 0, SEEK_END) == 0;
    const long size = ok ? std::ftell(file) : -1;
    ok = ok && size >= 0 && size <= kMaxFileBytes && std::fseek(file, 0, SEEK_SET) == 0;
    if (ok)
    {
        out.resize(static_cast<size_t>(size));
        ok = out.empty() || std::fread(out.data(), 1, out.size(), file) == out.size();
    }
    std::fclose(file);
    if (!ok)
        message = "the file could not be read";
    return ok;
}

// Parses a whole file into entries; nothing is kept unless all of it is good.
bool Parse(const std::vector<uint8_t> &file, std::vector<std::pair<std::vector<uint8_t>, CachedShader>> &out,
           std::string &message)
{
    uint64_t stored = 0;
    if (file.size() < sizeof(kMagic) + sizeof(stored))
    {
        message = "not a shader cache file";
        return false;
    }
    const size_t body = file.size() - sizeof(stored);
    for (size_t i = 0; i < sizeof(stored); ++i)
        stored |= uint64_t(file[body + i]) << (8 * i);
    Reader r(file.data(), body);
    char magic[sizeof(kMagic)];
    uint32_t version = 0, count = 0;
    const uint8_t *revision = nullptr;
    uint32_t revisionSize = 0;
    if (!r.Raw(magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 || !r.U32(version))
    {
        message = "not a shader cache file";
        return false;
    }
    if (version != kFileVersion)
    {
        message = "made by another file version";
        return false;
    }
    if (Hash(file.data(), body) != stored)
    {
        message = "damaged (checksum)";
        return false;
    }
    const char *const ours = legacy_shader::TranslatorRevision();
    if (!r.Bytes(revision, revisionSize, 256) || revisionSize != std::strlen(ours)
        || std::memcmp(revision, ours, revisionSize) != 0)
    {
        message = "made by another translator revision";
        return false;
    }
    if (!r.U32(count) || count > kMaxEntries)
    {
        message = "damaged (entry count)";
        return false;
    }
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t *bytecode = nullptr, *blob = nullptr, *spirv = nullptr;
        uint32_t bytecodeSize = 0, blobSize = 0, spirvSize = 0;
        CachedShader shader;
        if (!r.Bytes(bytecode, bytecodeSize, kMaxBytecode) || !bytecodeSize || bytecodeSize % 4
            || !r.Bytes(blob, blobSize, 64u << 20) || !legacy_shader::Deserialize(blob, blobSize, shader.translated)
            || !r.Bytes(spirv, spirvSize, 64u << 20) || spirvSize % 4
            || (shader.translated.reflection.stage == legacy_shader::Stage::Pixel && spirvSize != 0))
        {
            message = "damaged (entry " + std::to_string(i) + ")";
            out.clear();
            return false;
        }
        shader.spirv.resize(spirvSize / 4);
        if (spirvSize)
            std::memcpy(shader.spirv.data(), spirv, spirvSize);
        out.emplace_back(std::vector<uint8_t>(bytecode, bytecode + bytecodeSize), std::move(shader));
    }
    if (!r.Done())
    {
        message = "damaged (trailing bytes)";
        out.clear();
        return false;
    }
    return true;
}
} // namespace

bool FindCachedShader(const void *bytecode, size_t bytes, CachedShader &out)
{
    Cache &cache = TheCache();
    std::lock_guard<std::mutex> lock(cache.lock);
    const Entry *entry = Find(cache, Hash(bytecode, bytes), bytecode, bytes);
    if (!entry)
        return false;
    out = *entry->shader;
    return true;
}

void AddCachedShader(const void *bytecode, size_t bytes, CachedShader shader)
{
    Cache &cache = TheCache();
    std::lock_guard<std::mutex> lock(cache.lock);
    Add(cache, bytecode, bytes, std::make_shared<const CachedShader>(std::move(shader)));
}

bool LoadShaderCache(const char *path, std::string &message)
{
    message.clear();
    std::vector<uint8_t> file;
    std::vector<std::pair<std::vector<uint8_t>, CachedShader>> parsed;
    const bool ok = ReadFile(path, file, message) && Parse(file, parsed, message);
    Cache &cache = TheCache();
    std::lock_guard<std::mutex> lock(cache.lock);
    cache.loadedPath = path;
    if (!ok)
    {
        // Whatever is cached now replaces the file at the next save.
        cache.changed = cache.changed || !cache.entries.empty();
        return false;
    }
    for (auto &[bytecode, shader] : parsed)
        Add(cache, bytecode.data(), bytecode.size(), std::make_shared<const CachedShader>(std::move(shader)));
    // Entries only the process had still need writing; the file's own do not.
    cache.changed = cache.entries.size() > parsed.size();
    message = std::to_string(parsed.size()) + " shaders read";
    return true;
}

bool SaveShaderCache(const char *path, std::string &message)
{
    message.clear();
    Cache &cache = TheCache();
    std::vector<uint8_t> out;
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(cache.lock);
        if (!cache.changed)
        {
            message = "unchanged";
            return true;
        }
        out.insert(out.end(), kMagic, kMagic + sizeof(kMagic));
        PutU32(out, kFileVersion);
        const char *const revision = legacy_shader::TranslatorRevision();
        PutBytes(out, revision, std::strlen(revision));
        PutU32(out, static_cast<uint32_t>(cache.entries.size()));
        std::vector<uint8_t> blob;
        for (const auto &[hash, entry] : cache.entries)
        {
            legacy_shader::Serialize(entry.shader->translated, blob);
            PutBytes(out, entry.bytecode.data(), entry.bytecode.size());
            PutBytes(out, blob.data(), blob.size());
            PutBytes(out, entry.shader->spirv.data(), entry.shader->spirv.size() * sizeof(uint32_t));
        }
        const uint64_t checksum = Hash(out.data(), out.size());
        for (int i = 0; i < 8; ++i)
            out.push_back(static_cast<uint8_t>(checksum >> (8 * i)));
        count = cache.entries.size();
        cache.changed = false;
        cache.loadedPath = path;
    }
    // A new file beside the old one, then swapped in, so a crash mid-write
    // leaves the old file whole.
    const std::string temporary = std::string(path) + ".tmp";
    std::FILE *file = std::fopen(temporary.c_str(), "wb");
    bool ok = file && std::fwrite(out.data(), 1, out.size(), file) == out.size();
    ok = file && std::fclose(file) == 0 && ok;
#if defined(_WIN32)
    if (ok)
        std::remove(path); // rename does not replace an existing file on Windows
#endif
    ok = ok && std::rename(temporary.c_str(), path) == 0;
    if (!ok)
    {
        std::remove(temporary.c_str());
        std::lock_guard<std::mutex> lock(cache.lock);
        cache.changed = true;
        message = "could not write " + std::string(path);
        return false;
    }
    char line[96];
    std::snprintf(line, sizeof(line), "%zu shaders written (%.1f MB)", count, out.size() / (1024.0 * 1024.0));
    message = line;
    return true;
}

void ClearShaderCache()
{
    Cache &cache = TheCache();
    std::lock_guard<std::mutex> lock(cache.lock);
    cache.entries.clear();
    cache.loadedPath.clear();
    cache.changed = false;
}

uint32_t ShaderCacheSize()
{
    Cache &cache = TheCache();
    std::lock_guard<std::mutex> lock(cache.lock);
    return static_cast<uint32_t>(cache.entries.size());
}

bool ShaderCacheLoadedFrom(const char *path)
{
    Cache &cache = TheCache();
    std::lock_guard<std::mutex> lock(cache.lock);
    return cache.loadedPath == path;
}
} // namespace gfx_vk::legacy
