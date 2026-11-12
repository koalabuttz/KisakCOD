// A list of starts for a PS Vita run that nobody watches: one tap on the
// bubble runs every start in turn (docs/vita.md, "A list of starts").
//
// A console run compares builds of the same program started several times,
// each with its own command line and its own empty marker files in the
// program folder, and every start used to need a person: FTP the command
// line and the markers over, close the FTP program, start the bubble, wait,
// FTP the log back. With a list on the card the program does it by itself:
// at start-up it takes the next line, sets the markers, writes its log under
// that start's name, and when the start's quit timer ends it, it starts
// itself again for the line after.
//
// The list is text, one start a line:
//
//     <name> | <markers, or - for none> | <command line>
//
// The name is letters, digits, '.', '_' and '-' (the log is
// console-mp-<name>.log); the markers are separated by spaces, lower-case
// letters, digits and '-'; the command line is the rest of the line, bars
// and all. Blank lines and lines starting with '#' are left out.
//
// No engine call and no Vita call: src/vita/vita_main.cpp reads the files
// and scripts/tests/test_vita_run_list.py runs this on the host.

#pragma once

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace kisak::vita_run
{
constexpr size_t kMaxListStarts = 32;
constexpr size_t kMaxListMarkers = 8;
constexpr size_t kMaxListName = 32;
constexpr size_t kMaxListMarkerName = 40;
constexpr size_t kMaxListCommandLine = 960;

struct ListStart
{
    char name[kMaxListName] = {};
    char markers[kMaxListMarkers][kMaxListMarkerName] = {};
    size_t markerCount = 0;
    char commandLine[kMaxListCommandLine] = {};
};

inline bool ListNameCharacter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-';
}

// A marker's name as the program folder may hold it: an empty file without
// a dot, so it is never taken for a program or a dump.
inline bool ListMarkerName(const char *name, size_t length)
{
    if (length == 0 || length >= kMaxListMarkerName)
        return false;
    for (size_t i = 0; i < length; ++i)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') || name[i] == '-'))
            return false;
    return true;
}

// Reads the whole list, so a bad line anywhere refuses it before the first
// start, and copies start number `index` (from 0) to out when there is one.
// count gets the number of starts. False with the reason in error.
inline bool ParseRunList(const char *text, size_t index, ListStart *out, size_t *count, char *error, size_t errorSize)
{
    *count = 0;
    if (errorSize)
        error[0] = '\0';
    unsigned lineNumber = 0;
    const char *line = text;
    while (*line)
    {
        ++lineNumber;
        const char *end = strchr(line, '\n');
        const char *next = end ? end + 1 : line + strlen(line);
        if (!end)
            end = next;
        while (end > line && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
            --end;
        const char *at = line;
        while (at < end && (*at == ' ' || *at == '\t'))
            ++at;
        line = next;
        if (at == end || *at == '#')
            continue;

        auto fail = [&](const char *reason) {
            snprintf(error, errorSize, "run list line %u: %s", lineNumber, reason);
            *count = 0;
            return false;
        };
        auto trimmed = [](const char *&from, const char *&to) {
            while (from < to && (*from == ' ' || *from == '\t'))
                ++from;
            while (to > from && (to[-1] == ' ' || to[-1] == '\t'))
                --to;
        };
        const char *bar1 = static_cast<const char *>(memchr(at, '|', size_t(end - at)));
        const char *bar2 = bar1 ? static_cast<const char *>(memchr(bar1 + 1, '|', size_t(end - bar1 - 1))) : nullptr;
        if (!bar2)
            return fail("expected <name> | <markers or -> | <command line>");
        if (*count == kMaxListStarts)
            return fail("more starts than a list holds");

        ListStart start;
        const char *nameFrom = at, *nameTo = bar1;
        trimmed(nameFrom, nameTo);
        if (nameFrom == nameTo || size_t(nameTo - nameFrom) >= kMaxListName)
            return fail("the name is empty or too long");
        for (const char *c = nameFrom; c < nameTo; ++c)
            if (!ListNameCharacter(*c))
                return fail("the name may hold letters, digits, '.', '_' and '-' only");
        memcpy(start.name, nameFrom, size_t(nameTo - nameFrom));

        const char *markersFrom = bar1 + 1, *markersTo = bar2;
        trimmed(markersFrom, markersTo);
        if (markersFrom == markersTo)
            return fail("no markers given; write - for none");
        if (!(markersTo - markersFrom == 1 && *markersFrom == '-'))
        {
            const char *word = markersFrom;
            while (word < markersTo)
            {
                const char *wordEnd = word;
                while (wordEnd < markersTo && *wordEnd != ' ' && *wordEnd != '\t')
                    ++wordEnd;
                if (!ListMarkerName(word, size_t(wordEnd - word)))
                    return fail("a marker may hold lower-case letters, digits and '-' only");
                if (start.markerCount == kMaxListMarkers)
                    return fail("more markers than a start holds");
                memcpy(start.markers[start.markerCount++], word, size_t(wordEnd - word));
                word = wordEnd;
                while (word < markersTo && (*word == ' ' || *word == '\t'))
                    ++word;
            }
        }

        const char *commandFrom = bar2 + 1, *commandTo = end;
        trimmed(commandFrom, commandTo);
        if (size_t(commandTo - commandFrom) >= kMaxListCommandLine)
            return fail("the command line is too long");
        memcpy(start.commandLine, commandFrom, size_t(commandTo - commandFrom));

        if (*count == index && out)
            *out = start;
        ++*count;
    }
    return true;
}

// The number a state file holds: digits, then anything. 0 for none.
inline size_t ParseListIndex(const char *text)
{
    size_t value = 0;
    while (*text == ' ' || *text == '\t')
        ++text;
    for (; *text >= '0' && *text <= '9' && value < 100000; ++text)
        value = value * 10 + size_t(*text - '0');
    return value;
}
} // namespace kisak::vita_run
