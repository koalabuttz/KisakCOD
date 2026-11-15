// RmlUi's system and file interfaces over the engine's clock, console and
// file system.
//
// Files come through FS_ReadFile, so documents and style sheets load from the
// game folders and .iwd archives like the stock .menu files do. A file is read
// whole when opened and served from memory: RmlUi reads small text files, and
// the engine's seek rules differ between loose and zipped files.

#include "rml_internal.h"

#include <universal/q_shared.h>
#include <universal/com_files.h>
#include <qcommon/qcommon.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace kisak_rml
{
namespace
{
class EngineSystemInterface final : public Rml::SystemInterface
{
public:
    double GetElapsedTime() override { return Sys_Milliseconds() / 1000.0; }

    bool LogMessage(Rml::Log::Type type, const Rml::String &message) override
    {
        switch (type)
        {
        case Rml::Log::LT_ERROR:
        case Rml::Log::LT_ASSERT:
            Com_PrintError(CON_CHANNEL_UI, "RmlUi: %s\n", message.c_str());
            break;
        case Rml::Log::LT_WARNING:
            Com_PrintWarning(CON_CHANNEL_UI, "RmlUi: %s\n", message.c_str());
            break;
        case Rml::Log::LT_INFO:
        case Rml::Log::LT_ALWAYS:
            Com_Printf(CON_CHANNEL_UI, "RmlUi: %s\n", message.c_str());
            break;
        default:
            // LT_DEBUG: only with developer on.
            Com_DPrintf(CON_CHANNEL_UI, "RmlUi: %s\n", message.c_str());
            break;
        }
        return true;
    }
};

struct OpenFile
{
    std::vector<char> data;
    size_t position;
};

class EngineFileInterface final : public Rml::FileInterface
{
public:
    Rml::FileHandle Open(const Rml::String &path) override
    {
        void *buffer = nullptr;
        const int length = FS_ReadFile(path.c_str(), &buffer);
        if (length < 0 || !buffer)
        {
            const char *data = nullptr;
            size_t size = 0;
            if (!EmbeddedFile(path.c_str(), &data, &size))
                return 0;
            auto *file = new OpenFile();
            file->data.assign(data, data + size);
            file->position = 0;
            return reinterpret_cast<Rml::FileHandle>(file);
        }
        auto *file = new OpenFile();
        file->data.assign(static_cast<const char *>(buffer), static_cast<const char *>(buffer) + length);
        file->position = 0;
        FS_FreeFile(static_cast<char *>(buffer));
        return reinterpret_cast<Rml::FileHandle>(file);
    }

    void Close(Rml::FileHandle handle) override { delete reinterpret_cast<OpenFile *>(handle); }

    size_t Read(void *buffer, size_t size, Rml::FileHandle handle) override
    {
        auto *file = reinterpret_cast<OpenFile *>(handle);
        const size_t left = file->data.size() - file->position;
        const size_t count = size < left ? size : left;
        if (count)
            memcpy(buffer, file->data.data() + file->position, count);
        file->position += count;
        return count;
    }

    bool Seek(Rml::FileHandle handle, long offset, int origin) override
    {
        auto *file = reinterpret_cast<OpenFile *>(handle);
        long base = 0;
        if (origin == SEEK_CUR)
            base = (long)file->position;
        else if (origin == SEEK_END)
            base = (long)file->data.size();
        const long target = base + offset;
        if (target < 0 || target > (long)file->data.size())
            return false;
        file->position = (size_t)target;
        return true;
    }

    size_t Tell(Rml::FileHandle handle) override { return reinterpret_cast<OpenFile *>(handle)->position; }

    size_t Length(Rml::FileHandle handle) override { return reinterpret_cast<OpenFile *>(handle)->data.size(); }
};
} // namespace

Rml::SystemInterface *SystemInterface()
{
    static EngineSystemInterface instance;
    return &instance;
}

Rml::FileInterface *FileInterface()
{
    static EngineFileInterface instance;
    return &instance;
}
} // namespace kisak_rml
