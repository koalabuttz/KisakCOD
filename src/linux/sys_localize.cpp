#include <universal/q_shared.h>

#include <win32/win_local.h>
#include <win32/win_localize.h>

#include <qcommon/qcommon.h>
#include <linux/sys_path_case.h>
#include <stringed/stringed_hooks.h>
#include <universal/com_files.h>
#include <universal/q_parse.h>

#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Localization.
//
// win32/win_localize.cpp reads localization.txt from the install folder (the
// working directory there) before the engine starts. Its first line names the
// install's language, and the rest holds "REFERENCE" "text" pairs for the few
// system messages (WIN_OUT_OF_MEM_TITLE and so on). The language picks the
// zone folder the fastfiles load from (DB_BuildOSPath: zone/<language>/), so a
// French, German or Russian install has no zone/english/ to load.
//
// Here the install folder is fs_basepath, which exists only once the file
// system has started; the first fastfile load comes after that. So the file is
// read on first use rather than at start-up. Until fs_basepath exists, and
// when the file is missing or names a language the engine does not know, the
// language is "english", as this build always assumed before. A reference with
// no text is returned as it is.
// ---------------------------------------------------------------------------

namespace
{
constexpr size_t kLocalizationFileLimit = 0x1000; // win_localize.cpp's LANGUAGE_BUF_SIZE

std::mutex s_lock;
bool s_read = false;
char s_language[64] = "english";
char s_file[kLocalizationFileLimit];
const char *s_strings = nullptr; // the text after the language line, in s_file

// The language becomes a folder name, so only a name the engine knows
// (stringed_hooks.cpp's table: english, french, german, ...) is taken.
bool KnownLanguage(const char *name)
{
    int index = 0;
    return name[0] && strlen(name) < sizeof(s_language) && SEH_GetLanguageIndexForName(name, &index);
}

// Reads <fs_basepath>/localization.txt once fs_basepath exists. Called with
// s_lock held.
void ReadLocalization()
{
    if (s_read || !fs_basepath || !fs_basepath->current.string[0])
        return;
    s_read = true;

    char path[1024];
    Com_sprintf(path, sizeof(path), "%s/localization.txt", fs_basepath->current.string);
    FILE *file = fopen(path, "rb");
    char matched[sizeof(path)];
    if (!file && Sys_MatchPathCase(path, matched, sizeof(matched)))
        file = fopen(matched, "rb"); // as Windows finds it, ignoring case
    if (!file)
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM,
            "WARNING: no localization.txt in %s; loading the english zone folder\n", fs_basepath->current.string);
        return;
    }
    const size_t size = fread(s_file, 1, sizeof(s_file) - 1, file);
    const bool tooLong = size == sizeof(s_file) - 1 && fgetc(file) != EOF;
    fclose(file);
    if (tooLong)
    {
        // win_localize.cpp asserts the file fits its 4 KiB buffer.
        Com_PrintWarning(CON_CHANNEL_SYSTEM,
            "WARNING: localization.txt is larger than %u bytes; loading the english zone folder\n",
            (unsigned)(sizeof(s_file) - 1));
        return;
    }
    s_file[size] = '\0';

    // The language is the first line. Windows reads the file in text mode, so a
    // CRLF line end arrives as LF there; drop the CR and any spaces around it.
    char *const lineEnd = strchr(s_file, '\n');
    if (lineEnd)
    {
        *lineEnd = '\0';
        s_strings = lineEnd + 1;
    }
    char *name = s_file;
    while (*name == ' ' || *name == '\t')
        ++name;
    size_t length = strlen(name);
    while (length && (name[length - 1] == '\r' || name[length - 1] == ' ' || name[length - 1] == '\t'))
        name[--length] = '\0';

    if (!KnownLanguage(name))
    {
        Com_PrintWarning(CON_CHANNEL_SYSTEM,
            "WARNING: localization.txt names the language '%s', which this game does not know; loading the english zone folder\n",
            name);
        s_strings = nullptr;
        return;
    }
    I_strncpyz(s_language, name, (int)sizeof(s_language));
    Com_Printf(CON_CHANNEL_SYSTEM, "Install language from localization.txt: %s\n", s_language);
}
} // namespace

int Win_InitLocalization()
{
    std::lock_guard<std::mutex> lock(s_lock);
    ReadLocalization();
    int index = 0;
    SEH_GetLanguageIndexForName(s_language, &index);
    return index;
}

void Win_ShutdownLocalization()
{
    std::lock_guard<std::mutex> lock(s_lock);
    s_read = false;
    s_strings = nullptr;
    I_strncpyz(s_language, "english", (int)sizeof(s_language));
}

char *Win_LocalizeRef(const char *ref)
{
    if (!ref)
        ref = "";
    std::lock_guard<std::mutex> lock(s_lock);
    ReadLocalization();
    if (s_strings)
    {
        // The same walk as win_localize.cpp: reference, text, reference, text.
        const char *strings = s_strings;
        Com_BeginParseSession("localization");
        for (;;)
        {
            const char *token = Com_Parse(&strings)->token;
            if (!*token)
                break;
            const bool match = strcmp(token, ref) == 0;
            token = Com_Parse(&strings)->token;
            if (!*token)
                break;
            if (match)
            {
                char *const text = Win_CopyLocalizationString(token);
                Com_EndParseSession();
                return text;
            }
        }
        Com_EndParseSession();
    }
    // No text for it: the reference itself rather than a made-up translation.
    return const_cast<char *>(ref);
}

char *Win_GetLanguage()
{
    std::lock_guard<std::mutex> lock(s_lock);
    ReadLocalization();
    return s_language;
}

char *Win_CopyLocalizationString(const char *string)
{
    if (!string)
        return nullptr;

    const size_t length = strlen(string);
    char *copy = (char *)malloc(length + 1);
    if (copy)
        memcpy(copy, string, length + 1);
    return copy;
}
