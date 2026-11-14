// When a run that nobody watches sends its timed console commands, prints its
// figures and quits: the PS Vita's runs (docs/vita.md, "Runs nobody watches")
// and the Windows and Linux clients' runs (docs/runs.md, "Runs that play
// themselves").
//
// A start in an emulator used to run for a fixed 225 to 420 seconds, because
// the memory lines came once a minute and vita_quitAfter counted from
// start-up, while the menu was up after 25 seconds and a map after 45. The
// plan below times a run from what it waits for instead. On the Vita the
// settings are vita_*, on the other clients run_* (DvarNames below):
//
//   vita_statsEvery <s>      the period of the figures (60 by default)
//   *_quitAfterMenu <s>      quit this long after the main menu first showed
//   *_quitAfterMap <s>       quit this long after the map was first in play
//   *_quitAfter <s>          quit this long after start-up, as before
//   *_atMenu, *_atMap        console commands to send that long after the
//                            menu or the map came up:
//                            "<seconds> <command> | <seconds> <command>",
//                            or on the Vita "@<file>": the same, one step a
//                            line, from a file in its data folder
//                            (StepFileName, JoinStepFile)
//
// Every quit timer prints all figures first and leaves the renderer and the
// sound thread a moment to add theirs.
//
// The settings that carry text are read once, at start-up (StartText below).
//
// No engine call and no platform call: src/vita/vita_stats.cpp and
// src/client_mp/cl_main_mp.cpp feed it the clock and what the client shows,
// and scripts/tests/test_vita_run_plan.py runs it on the host.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kisak::run_plan
{

// A picture run steps through a dozen settings in one launch, each with its
// screenshot, so the list holds more than a Vita run's few menu answers.
constexpr size_t kMaxTimedCommands = 32;
constexpr size_t kMaxCommandLength = 160;
// Room for a full list: every command at its longest, with its time and bar.
constexpr size_t kMaxTimedListLength = kMaxTimedCommands * (kMaxCommandLength + 32);
constexpr uint64_t kSecond = 1000000u;
// From the last figures to the quit. The renderer prints its memory lines
// at its next frame and the sound thread at its next update, so the wait is
// both a time and a number of frames (an emulator can take a second a frame).
constexpr uint64_t kQuitGrace = kSecond;
constexpr unsigned kQuitGraceFrames = 3;

struct TimedCommand
{
    uint64_t afterMicros = 0;
    char text[kMaxCommandLength] = {};
    bool sent = false;
};

struct TimedCommands
{
    TimedCommand items[kMaxTimedCommands];
    size_t count = 0;
};

// "3 openscriptmenu team autoassign | 6.5 vitastats": each part is a time in
// seconds since the event, then the command. Parts need not be in order. On a
// part it cannot read it returns false with the reason in error, and out
// holds no command.
inline bool ParseTimedCommands(const char *text, TimedCommands *out, char *error, size_t errorSize)
{
    *out = TimedCommands();
    if (errorSize)
        error[0] = '\0';
    if (!text)
        return true;
    const char *part = text;
    while (true)
    {
        const char *const bar = strchr(part, '|');
        const char *end = bar ? bar : part + strlen(part);
        while (part < end && (*part == ' ' || *part == '\t'))
            ++part;
        while (end > part && (end[-1] == ' ' || end[-1] == '\t'))
            --end;
        if (part < end)
        {
            char *afterNumber = nullptr;
            const double seconds = strtod(part, &afterNumber);
            const bool numberRead = afterNumber && afterNumber > part && afterNumber <= end;
            const char *command = numberRead ? afterNumber : part;
            const bool spaceAfter = numberRead && command < end && (*command == ' ' || *command == '\t');
            while (command < end && (*command == ' ' || *command == '\t'))
                ++command;
            const char *reason = nullptr;
            if (numberRead && command == end)
                reason = "has no command after its time";
            else if (!numberRead || !spaceAfter || !(seconds >= 0.0) || seconds > 86400.0)
                reason = "does not start with a time in seconds";
            else if (size_t(end - command) >= kMaxCommandLength)
                reason = "is too long";
            else if (out->count == kMaxTimedCommands)
                reason = "is one more than fit";
            if (reason)
            {
                snprintf(error, errorSize, "part %u \"%.*s\" %s", unsigned(out->count + 1), int(end - part), part,
                         reason);
                *out = TimedCommands();
                return false;
            }
            TimedCommand &item = out->items[out->count++];
            item.afterMicros = uint64_t(seconds * double(kSecond) + 0.5);
            memcpy(item.text, command, size_t(end - command));
            item.text[end - command] = '\0';
        }
        if (!bar)
            break;
        part = bar + 1;
    }
    return true;
}

// "@<name>": the timed commands are in the file <name> in the game's data
// folder (the Vita's ux0:data/KisakCOD). Writes the name and returns true when text asks for a file and the
// name is plain (letters, digits, '.', '_', '-'; no leading '.', so no path
// and no "..").
constexpr size_t kMaxStepFileName = 64;
inline bool StepFileName(const char *text, char *name, size_t nameSize)
{
    if (nameSize)
        name[0] = '\0';
    if (!text || text[0] != '@' || !text[1] || text[1] == '.')
        return false;
    const size_t length = strlen(text + 1);
    if (length >= nameSize || length >= kMaxStepFileName)
        return false;
    for (size_t i = 0; i < length; ++i)
    {
        const char c = text[1 + i];
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                           c == '_' || c == '-';
        if (!plain)
            return false;
    }
    memcpy(name, text + 1, length);
    name[length] = '\0';
    return true;
}

// A step file's text as one timed-command list: each line is
// "<seconds> <command>", blank lines and lines starting with '#' are left
// out, and the lines are joined with " | " for ParseTimedCommands. A line may
// not hold a bar itself (it would split into two steps unseen). Returns the
// number of steps, or -1 with the reason in error and out empty.
inline int JoinStepFile(const char *fileText, char *out, size_t outSize, char *error, size_t errorSize)
{
    if (errorSize)
        error[0] = '\0';
    if (!outSize)
        return -1;
    out[0] = '\0';
    size_t used = 0;
    int steps = 0;
    unsigned lineNumber = 0;
    for (const char *line = fileText ? fileText : ""; *line;)
    {
        const char *next = strchr(line, '\n');
        const char *end = next ? next : line + strlen(line);
        ++lineNumber;
        const char *from = line;
        while (from < end && (*from == ' ' || *from == '\t'))
            ++from;
        while (end > from && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
            --end;
        if (from < end && *from != '#')
        {
            const char *reason = nullptr;
            if (memchr(from, '|', size_t(end - from)))
                reason = "holds a '|' (one step a line)";
            else if (used + (steps ? 3 : 0) + size_t(end - from) >= outSize)
                reason = "does not fit (the steps are too long together)";
            if (reason)
            {
                snprintf(error, errorSize, "line %u %s", lineNumber, reason);
                out[0] = '\0';
                return -1;
            }
            if (steps)
            {
                memcpy(out + used, " | ", 3);
                used += 3;
            }
            memcpy(out + used, from, size_t(end - from));
            used += size_t(end - from);
            out[used] = '\0';
            ++steps;
        }
        if (!next)
            break;
        line = next + 1;
    }
    return steps;
}

// A text setting as the start gave it.
//
// The text of vita_atMenu and vita_atMap goes to the console as commands, and
// vita_keyboardAnswer's is typed into the chat line and run by
// vita_typecommand. A game server sets a client's dvars by name (its
// systeminfo names any dvar it likes), so a dvar read while a game is on can
// hold a server's text. Each of these dvars is therefore read once, at
// start-up, into a StartText, and only the StartText is used after that: the
// command line decides, and a later change does nothing.
template <size_t Size> class StartText
{
  public:
    // Takes `value` the first time it is called and returns whether it fit.
    // Text that does not fit is left out whole, not cut: a cut command is
    // another command. Later calls change nothing and return the same.
    bool Take(const char *value)
    {
        if (m_taken)
            return m_fits;
        m_taken = true;
        const size_t length = value ? strlen(value) : 0;
        m_fits = length < Size;
        if (m_fits && length)
            memcpy(m_text, value, length);
        m_text[m_fits ? length : 0] = '\0';
        return m_fits;
    }

    // The text taken; empty before Take and when it did not fit.
    const char *Get() const
    {
        return m_text;
    }

  private:
    char m_text[Size] = {};
    bool m_taken = false;
    bool m_fits = true;
};

// The names of the settings, for the messages: the Vita's and the other
// clients'.
struct DvarNames
{
    const char *quitAfter;
    const char *quitAfterMenu;
    const char *quitAfterMap;
    const char *atMenu;
    const char *atMap;
};

inline constexpr DvarNames kVitaDvarNames = {"vita_quitAfter", "vita_quitAfterMenu", "vita_quitAfterMap",
                                             "vita_atMenu", "vita_atMap"};
inline constexpr DvarNames kRunDvarNames = {"run_quitAfter", "run_quitAfterMenu", "run_quitAfterMap", "run_atMenu",
                                            "run_atMap"};

struct Settings
{
    unsigned quitAfter = 0;     // seconds since start-up; 0: never
    unsigned quitAfterMenu = 0; // seconds since the menu first showed
    unsigned quitAfterMap = 0;  // seconds since the map was first in play
    unsigned statsEvery = 60;   // seconds between the figures
    const char *atMenu = "";    // timed commands, read when the menu comes up; the caller's StartText
    const char *atMap = "";     // timed commands, read when the map comes up; the caller's StartText
    const DvarNames *names = &kRunDvarNames;
};

// What the caller does after a frame, in this order.
struct Step
{
    bool menuCameUp = false; // this frame, for the first time
    bool mapCameUp = false;
    // A timed-command list that could not be read ("run_atMap: part 2 ...").
    char commandError[kMaxCommandLength + 96] = {};
    size_t commandCount = 0;
    const char *commands[2 * kMaxTimedCommands] = {};
    bool report = false;             // print every figure now
    const char *quitDvar = nullptr;  // with report: the timer that ran out
    unsigned quitSeconds = 0;
    bool quit = false;               // once: ask the engine to quit
};

class Plan
{
  public:
    // startMicros: the clock value the "since start-up" timer counts from.
    void Start(uint64_t startMicros, uint64_t nowMicros)
    {
        *this = Plan();
        m_started = true;
        m_start = startMicros;
        m_lastReport = nowMicros;
    }

    bool Quitting() const
    {
        return m_quitting;
    }

    Step Frame(uint64_t now, bool menuUp, bool mapActive, const Settings &settings)
    {
        Step step;
        if (!m_started || m_quitSent)
            return step;
        if (m_quitting)
        {
            ++m_graceFrames;
            if (now - m_quitStarted >= kQuitGrace && m_graceFrames >= kQuitGraceFrames)
            {
                m_quitSent = true;
                step.quit = true;
            }
            return step;
        }

        if (menuUp && !m_menu.up)
            step.menuCameUp = ComeUp(m_menu, now, settings.atMenu, settings.names->atMenu, step);
        if (mapActive && !m_map.up)
            step.mapCameUp = ComeUp(m_map, now, settings.atMap, settings.names->atMap, step);
        SendDue(m_menu, now, step);
        SendDue(m_map, now, step);

        if (settings.quitAfterMap && m_map.up && now - m_map.at >= settings.quitAfterMap * kSecond)
            StartQuit(settings.names->quitAfterMap, settings.quitAfterMap, now, step);
        else if (settings.quitAfterMenu && m_menu.up && now - m_menu.at >= settings.quitAfterMenu * kSecond)
            StartQuit(settings.names->quitAfterMenu, settings.quitAfterMenu, now, step);
        else if (settings.quitAfter && now - m_start >= settings.quitAfter * kSecond)
            StartQuit(settings.names->quitAfter, settings.quitAfter, now, step);
        else if (step.menuCameUp || step.mapCameUp ||
                 now - m_lastReport >= (settings.statsEvery ? settings.statsEvery : 1u) * kSecond)
        {
            // The figures also come the moment the menu or the map is up, and
            // the period counts from there: the load before it and the play
            // after it are not mixed in one line, and a quit timer that is a
            // multiple of the period ends on a whole one.
            step.report = true;
            m_lastReport = now;
        }
        return step;
    }

  private:
    struct Event
    {
        bool up = false;
        uint64_t at = 0;
        TimedCommands commands;
    };

    static bool ComeUp(Event &event, uint64_t now, const char *commands, const char *dvar, Step &step)
    {
        event.up = true;
        event.at = now;
        char error[kMaxCommandLength + 64];
        if (!ParseTimedCommands(commands, &event.commands, error, sizeof(error)))
            snprintf(step.commandError, sizeof(step.commandError), "%s: %s", dvar, error);
        return true;
    }

    static void SendDue(Event &event, uint64_t now, Step &step)
    {
        if (!event.up)
            return;
        for (size_t i = 0; i < event.commands.count; ++i)
        {
            TimedCommand &item = event.commands.items[i];
            if (!item.sent && now - event.at >= item.afterMicros)
            {
                item.sent = true;
                step.commands[step.commandCount++] = item.text;
            }
        }
    }

    void StartQuit(const char *dvar, unsigned seconds, uint64_t now, Step &step)
    {
        m_quitting = true;
        m_quitStarted = now;
        m_graceFrames = 0;
        step.report = true;
        step.quitDvar = dvar;
        step.quitSeconds = seconds;
    }

    bool m_started = false;
    bool m_quitting = false;
    bool m_quitSent = false;
    uint64_t m_start = 0;
    uint64_t m_lastReport = 0;
    uint64_t m_quitStarted = 0;
    unsigned m_graceFrames = 0;
    Event m_menu;
    Event m_map;
};

} // namespace kisak::run_plan
