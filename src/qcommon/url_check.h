#pragma once

#include <cstddef>

// Which addresses Sys_OpenURL hands to the desktop.
//
// On Windows Sys_OpenURL is ShellExecute "open", which runs whatever it is
// given: a program, a document, a file on a network share, any protocol a
// program has registered.  The address is not always the player's own.  A game
// server chooses it when it redirects a download (CL_ParseWWWDownload opens the
// name the server sent), and a server can set the update dvar that
// CL_GetAutoUpdate opens.  Retail passed all of that to ShellExecute, so a
// server could start a program on the player's machine.
//
// Only a plain web address is opened: http:// or https://, a host, and no byte
// that a URL cannot hold.  The player's browser gets it and nothing else can.
//
// Header-only and free of engine types so the asset-free check
// (scripts/tests/test_open_url.py) compiles exactly this rule.

// Longer than any address the engine builds (a download name is 256 bytes) and
// shorter than what ShellExecute and browsers take.
constexpr size_t OPEN_URL_MAX_LENGTH = 1024;

// The bytes RFC 3986 lets a URL hold: letters, digits, -._~ and the reserved
// and percent characters.  A space, a quote, a backslash, <>^`{|} and every
// control or 8-bit byte are not among them.
inline bool Com_IsUrlByte(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    switch (c)
    {
    case '-': case '.': case '_': case '~':
    case ':': case '/': case '?': case '#': case '[': case ']': case '@':
    case '!': case '$': case '&': case '\'': case '(': case ')':
    case '*': case '+': case ',': case ';': case '=':
    case '%':
        return true;
    default:
        return false;
    }
}

// True for "http://host..." and "https://host...", in any letter case, made
// only of URL bytes and no longer than OPEN_URL_MAX_LENGTH.  The host must
// start with a letter, a digit or the '[' of an IPv6 address, so "http:///x",
// "http://@x" and "http://:80" are not web addresses here.
inline bool Com_IsWebUrl(const char* url)
{
    static const char http[] = "http";
    size_t pos = 0;

    if (!url)
        return false;
    for (; pos < 4; ++pos)
    {
        if ((url[pos] | 0x20) != http[pos])
            return false;
    }
    if ((url[pos] | 0x20) == 's')
        ++pos;
    if (url[pos] != ':' || url[pos + 1] != '/' || url[pos + 2] != '/')
        return false;
    pos += 3;

    const char first = url[pos];
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || (first >= '0' && first <= '9')
            || first == '['))
        return false;

    for (; url[pos]; ++pos)
    {
        if (pos >= OPEN_URL_MAX_LENGTH || !Com_IsUrlByte(url[pos]))
            return false;
    }
    return true;
}

// Copies `text` for a console line or an error box: a byte that is not
// printable ASCII becomes '?', and the copy is cut to fit `out`.
inline void Com_CopyPrintable(const char* text, char* out, size_t outSize)
{
    size_t i = 0;

    if (!out || !outSize)
        return;
    if (text)
    {
        for (; text[i] && i + 1 < outSize; ++i)
            out[i] = (text[i] >= 0x20 && text[i] <= 0x7E) ? text[i] : '?';
    }
    out[i] = 0;
}
