// The bots' names (docs/bots.md): "[BOT] Name", a tag the server keeps for
// bots, so a player can tell a bot from a person by the name alone.
//
// No engine call and no engine type: tests/bots/bot_team_test.cpp checks it
// on the host.

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace kisak::bots
{

// The game keeps 15 characters of a name ("[BOT] " is 6 of them).
constexpr size_t kNameChars = 15;
constexpr const char *kBotTag = "[BOT] ";

// David's names, in his spelling. Server owners can replace them with
// bots/names.txt in the home folder, one name to a line.
constexpr const char *kBotNames[] = {
    "Oinks", "Guillaume", "Bjorn", "Bjarna", "Gorbachev", "Skoggy", "Ginkgo", "Sunshine", "Capy", "ms rat",
    "anna",
};
constexpr int kBotNameCount = static_cast<int>(sizeof(kBotNames) / sizeof(kBotNames[0]));

// `text` into `out`, cut to fit.
inline void CopyName(const char *text, char *out, size_t size)
{
    if (!size)
        return;
    size_t length = std::strlen(text);
    if (length > size - 1)
        length = size - 1;
    std::memcpy(out, text, length);
    out[length] = '\0';
}

// `name` without colour codes (^ and a character) or spaces, lower case.
inline void PlainName(const char *name, char *out, size_t size)
{
    size_t n = 0;
    for (const char *p = name; *p && n + 1 < size; ++p)
    {
        if (p[0] == '^' && p[1])
        {
            ++p;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(*p)))
            continue;
        out[n++] = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }
    out[n] = '\0';
}

// Whether a name wears the bot tag, however it is coloured, spaced or cased:
// "[BOT]", "^1[ b o t ]", "x[Bot]y".
inline bool WearsBotTag(const char *name)
{
    char plain[64];
    PlainName(name, plain, sizeof(plain));
    return std::strstr(plain, "[bot]") != nullptr;
}

// A person's name with the bot tag taken out. Only a name that wears it is
// changed: its colours go too, and a name that was nothing but the tag
// becomes "Player". What is left never wears the tag.
inline void RemoveBotTag(const char *name, char *out, size_t size)
{
    if (!size)
        return;
    if (!WearsBotTag(name))
    {
        CopyName(name, out, size);
        return;
    }
    // Without colours; then every "[", "b", "o", "t", "]" run that spells
    // the tag (spaces allowed between) goes.
    char bare[64];
    size_t n = 0;
    for (const char *p = name; *p && n + 1 < sizeof(bare); ++p)
    {
        if (p[0] == '^' && p[1])
        {
            ++p;
            continue;
        }
        bare[n++] = *p;
    }
    bare[n] = '\0';
    // Until none is left: taking one out can close another around it
    // ("[B[BOT]OT] Capy" would be left as "[BOT] Capy").
    char kept[64];
    size_t k = 0;
    static const char kTag[] = "[bot]";
    for (bool removed = true; removed;)
    {
        removed = false;
        k = 0;
        for (size_t i = 0; bare[i];)
        {
            // Does the tag start here?
            size_t j = i;
            size_t t = 0;
            while (bare[j] && t < 5)
            {
                if (std::isspace(static_cast<unsigned char>(bare[j])) && t > 0)
                {
                    ++j;
                    continue;
                }
                if (std::tolower(static_cast<unsigned char>(bare[j])) != kTag[t])
                    break;
                ++j;
                ++t;
            }
            if (t == 5)
            {
                i = j;
                removed = true;
                continue;
            }
            if (k + 1 < sizeof(kept))
                kept[k++] = bare[i];
            ++i;
        }
        kept[k] = '\0';
        std::memcpy(bare, kept, k + 1);
    }
    // Trim, and fold runs of spaces.
    char *start = kept;
    while (*start == ' ')
        ++start;
    size_t o = 0;
    for (const char *p = start; *p && o + 1 < size; ++p)
    {
        if (*p == ' ' && (p[1] == ' ' || p[1] == '\0'))
            continue;
        out[o++] = *p;
    }
    out[o] = '\0';
    if (o == 0)
        std::snprintf(out, size, "Player");
}

// The name the server keeps for a person. `name` is the whole "name" value of
// the userinfo, not its start: the game shows the first 15 characters that
// are not colour codes or leading spaces, however far into the value they
// are, so 64 colour codes and then "[BOT] Capy" shows as a bot's name.
// False if the name stays as it is; else `out` gets it without the tag.
inline bool PersonNameWithoutBotTag(const char *name, char *out, size_t size)
{
    if (!WearsBotTag(name))
        return false;
    RemoveBotTag(name, out, size);
    return true;
}

// A bot's name: "[BOT] " and a name from `names` that `taken` says no one
// has, starting at `start`; when every one is taken, the first with a
// number ("[BOT] Capy 2", "[BOT] Sunshine2"), shortened to fit.
// `taken(name, context)` gets the whole name.
inline void PickBotName(const char *const *names,
                        int count,
                        int start,
                        bool (*taken)(const char *, void *),
                        void *context,
                        char *out,
                        size_t size)
{
    if (!size)
        return;
    if (count <= 0)
    {
        std::snprintf(out, size, "%sBot", kBotTag);
        return;
    }
    if (start < 0)
        start = -start;
    const size_t tagLength = std::strlen(kBotTag);
    char candidate[64];
    for (int number = 1; number < 1000; ++number)
    {
        for (int i = 0; i < count; ++i)
        {
            const char *name = names[(start + i) % count];
            if (number == 1)
            {
                std::snprintf(candidate, sizeof(candidate), "%s%.*s", kBotTag,
                              static_cast<int>(kNameChars - tagLength), name);
            }
            else
            {
                // "Capy 2"; "Sunshine2" when the space does not fit; else the
                // name is shortened: "Guillau 2".
                char suffix[16];
                const int space = static_cast<int>(kNameChars - tagLength);
                std::snprintf(suffix, sizeof(suffix), " %d", number);
                if (static_cast<int>(std::strlen(name) + std::strlen(suffix)) > space
                    && static_cast<int>(std::strlen(name) + std::strlen(suffix)) - 1 <= space)
                    std::snprintf(suffix, sizeof(suffix), "%d", number);
                const int room = space - static_cast<int>(std::strlen(suffix));
                std::snprintf(candidate, sizeof(candidate), "%s%.*s%s", kBotTag, room, name, suffix);
            }
            if (!taken || !taken(candidate, context))
            {
                CopyName(candidate, out, size);
                return;
            }
        }
    }
    std::snprintf(out, size, "%sBot", kBotTag);
}

} // namespace kisak::bots
