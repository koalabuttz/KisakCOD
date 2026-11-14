#pragma once

// KISAK: Steam is opt-in for the multiplayer client. A player who wants to
// join with Steam starts the game with "+set cl_steam 1"; without it the client
// never loads or talks to Steam and joins with its key code.
//
// Steam_Init runs before Com_Init parses the command line (a +connect on it
// needs a ticket), so the choice is read here from the raw command line: every
// "+set cl_steam <value>" or "+seta cl_steam <value>", the last one winning, as
// Com_StartupVariable would set it. Names and commands match case-insensitively
// and a value may be quoted. Anything else, including a value that is not a
// whole number, leaves Steam off.
//
// Header-only and free of engine types, so the asset-free check in
// scripts/tests/test_linux_steam_session.py compiles exactly this code.

#include <cstddef>
#include <cstdlib>

namespace steam_optin
{
// Copies the next command line token into token (at most capacity - 1 bytes,
// longer tokens are cut) and returns where scanning continues, or nullptr at
// the end. A token is a run of non-blank characters or a double-quoted string.
inline const char *NextToken(const char *text, char *token, size_t capacity)
{
    while (*text && static_cast<unsigned char>(*text) <= ' ')
        ++text;
    if (!*text)
        return nullptr;
    size_t length = 0;
    if (*text == '"')
    {
        ++text;
        while (*text && *text != '"')
        {
            if (length + 1 < capacity)
                token[length++] = *text;
            ++text;
        }
        if (*text == '"')
            ++text;
    }
    else
    {
        while (*text && static_cast<unsigned char>(*text) > ' ')
        {
            if (length + 1 < capacity)
                token[length++] = *text;
            ++text;
        }
    }
    token[length] = 0;
    return text;
}

inline bool SameName(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }
    return *a == *b;
}

// True for a whole number other than zero ("1", "2", "-1"), as a bool dvar
// reads it; false for "0", "", "yes", "1x".
inline bool IsTrue(const char *value)
{
    if (!*value)
        return false;
    char *end = nullptr;
    const long number = std::strtol(value, &end, 10);
    return end && !*end && number != 0;
}
} // namespace steam_optin

inline bool Steam_ClientOptedIn(const char *commandLine)
{
    if (!commandLine)
        return false;
    bool optedIn = false;
    char token[64];
    const char *text = commandLine;
    while ((text = steam_optin::NextToken(text, token, sizeof(token))) != nullptr)
    {
        if (!steam_optin::SameName(token, "+set") && !steam_optin::SameName(token, "+seta"))
            continue;
        const char *afterName = steam_optin::NextToken(text, token, sizeof(token));
        if (!afterName || !steam_optin::SameName(token, "cl_steam"))
            continue;
        text = afterName;
        const char *afterValue = steam_optin::NextToken(text, token, sizeof(token));
        if (!afterValue)
            break;
        text = afterValue;
        optedIn = steam_optin::IsTrue(token);
    }
    return optedIn;
}
