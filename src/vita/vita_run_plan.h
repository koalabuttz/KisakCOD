// When a PS Vita run that nobody watches prints its figures, sends its
// commands and quits (docs/vita.md, "Runs nobody watches").
//
// A start in an emulator used to run for a fixed 225 to 420 seconds, because
// the memory lines came once a minute and vita_quitAfter counted from
// start-up, while the menu was up after 25 seconds and a map after 45. The
// plan below times a run from what it waits for instead:
//
//   vita_statsEvery <s>      the period of the figures (60 by default)
//   vita_quitAfterMenu <s>   quit this long after the main menu first showed
//   vita_quitAfterMap <s>    quit this long after the map was first in play
//   vita_quitAfter <s>       quit this long after start-up, as before
//   vita_atMenu, vita_atMap  console commands to send that long after the
//                            menu or the map came up:
//                            "<seconds> <command> | <seconds> <command>"
//
// Every quit timer prints all figures first and leaves the renderer and the
// sound thread a moment to add theirs.
//
// No engine call and no Vita call: src/vita/vita_stats.cpp feeds it the clock
// and what the client shows, and scripts/tests/test_vita_run_plan.py runs it
// on the host.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kisak::vita_run
{

constexpr size_t kMaxTimedCommands = 8;
constexpr size_t kMaxCommandLength = 160;
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

struct Settings
{
    unsigned quitAfter = 0;     // seconds since start-up; 0: never
    unsigned quitAfterMenu = 0; // seconds since the menu first showed
    unsigned quitAfterMap = 0;  // seconds since the map was first in play
    unsigned statsEvery = 60;   // seconds between the figures
    const char *atMenu = "";    // timed commands, read when the menu comes up
    const char *atMap = "";     // timed commands, read when the map comes up
};

// What the caller does after a frame, in this order.
struct Step
{
    bool menuCameUp = false; // this frame, for the first time
    bool mapCameUp = false;
    // A timed-command list that could not be read ("vita_atMap: part 2 ...").
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
            step.menuCameUp = ComeUp(m_menu, now, settings.atMenu, "vita_atMenu", step);
        if (mapActive && !m_map.up)
            step.mapCameUp = ComeUp(m_map, now, settings.atMap, "vita_atMap", step);
        SendDue(m_menu, now, step);
        SendDue(m_map, now, step);

        if (settings.quitAfterMap && m_map.up && now - m_map.at >= settings.quitAfterMap * kSecond)
            StartQuit("vita_quitAfterMap", settings.quitAfterMap, now, step);
        else if (settings.quitAfterMenu && m_menu.up && now - m_menu.at >= settings.quitAfterMenu * kSecond)
            StartQuit("vita_quitAfterMenu", settings.quitAfterMenu, now, step);
        else if (settings.quitAfter && now - m_start >= settings.quitAfter * kSecond)
            StartQuit("vita_quitAfter", settings.quitAfter, now, step);
        else if (now - m_lastReport >= (settings.statsEvery ? settings.statsEvery : 1u) * kSecond)
        {
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

} // namespace kisak::vita_run
