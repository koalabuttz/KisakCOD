// Production parser/input translation units and message/string readers.
// Connection lookup, command arguments, clock and diagnostics are doubles.
// Unused engine subsystems are removed by section GC.
#include <universal/q_shared.h>
#include <universal/assertive.h>
#include <client_mp/client_mp.h>
#include <qcommon/msg_mp.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static clientConnection_t connection;
clientConnection_t *__cdecl CL_GetLocalClientConnection(int32_t localClientNum)
{
    iassert(localClientNum == 0);
    return &connection;
}
void MyAssertHandler(const char *file, int line, int, const char *fmt, ...)
{
    std::fprintf(stderr, "Assertion at %s:%d: ", file, line);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::abort();
}
// Input event boundary: the production functions still own all state updates.
uint32_t frame_msec;
int com_frameTime;
static const char *keyArgument = "", *timeArgument = "";
static int warnings;
const char *__cdecl Cmd_Argv(int index)
{
    iassert(index == 1 || index == 2);
    return index == 1 ? keyArgument : timeArgument;
}
void Com_Printf(int, const char *, ...) { ++warnings; }
static void event(const char *key, const char *time)
{
    keyArgument = key;
    timeArgument = time;
}
static int checks, failures;
static void check(bool condition, const char *name)
{
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", name); }
}
static void reset(int sequence = 126)
{
    connection.serverCommandSequence = sequence;
    connection.isServerRestarting = true;
    connection.lastClientArchiveIndex = 0x12345678;
    std::memset(connection.serverCommands, 0x5a, sizeof(connection.serverCommands));
}
static std::vector<uint8_t> packet(int sequence, const std::string &text, bool terminated = true)
{
    // Independent wire bytes, not a roundtrip through a potentially buggy writer.
    std::vector<uint8_t> bytes;
    for (int shift = 0; shift != 32; shift += 8)
        bytes.push_back(static_cast<uint32_t>(sequence) >> shift);
    bytes.insert(bytes.end(), text.begin(), text.end());
    if (terminated) bytes.push_back(0);
    return bytes;
}
static msg_t message(std::vector<uint8_t> &bytes)
{
    msg_t msg{};
    msg.data = bytes.data();
    msg.cursize = msg.maxsize = static_cast<int>(bytes.size());
    return msg;
}
static std::vector<char> saved_commands()
{
    const char *first = reinterpret_cast<const char *>(connection.serverCommands);
    return {first, first + sizeof(connection.serverCommands)};
}
static bool unchanged(const std::vector<char> &before)
{
    return std::memcmp(before.data(), connection.serverCommands, before.size()) == 0;
}
static std::vector<char> saved_tail()
{
    const char *first = reinterpret_cast<const char *>(&connection);
    return {first + offsetof(clientConnection_t, isServerRestarting), first + sizeof(connection)};
}
static bool tail_unchanged(const std::vector<char> &before)
{
    const char *first = reinterpret_cast<const char *>(&connection);
    return std::memcmp(before.data(), first + offsetof(clientConnection_t, isServerRestarting), before.size()) == 0;
}
static bool command_is(int slot, const std::string &expected)
{
    return expected.size() < sizeof(connection.serverCommands[slot])
        && std::memcmp(connection.serverCommands[slot], expected.c_str(), expected.size() + 1) == 0;
}
static void test_parser()
{
    reset();
    for (int seq : {127, 128, 129}) {
        auto bytes = packet(seq, "command " + std::to_string(seq));
        auto msg = message(bytes);
        auto before = saved_commands();
        CL_ParseCommandString(0, &msg);
        int slot = seq & (MAX_RELIABLE_COMMANDS - 1);
        check(!msg.overflowed && msg.readcount == static_cast<int>(bytes.size()), "complete command consumed");
        check(connection.serverCommandSequence == seq, "newer sequence committed, including ring wrap");
        check(command_is(slot, "command " + std::to_string(seq)), "command in correct ring slot");
        for (int i = 0; i < MAX_RELIABLE_COMMANDS; ++i)
            if (i != slot) check(std::memcmp(before.data() + i * 1024, connection.serverCommands[i], 1024) == 0,
                                 "other command slots preserved");
    }
    for (int seq : {129, 128, 1, -1}) {
        auto bytes = packet(seq, "replacement");
        auto msg = message(bytes);
        auto before = saved_commands();
        CL_ParseCommandString(0, &msg);
        check(!msg.overflowed && msg.readcount == static_cast<int>(bytes.size()), "duplicate/stale command consumed");
        check(connection.serverCommandSequence == 129 && unchanged(before), "duplicate/stale command cannot replace history");
    }
    auto full = packet(128, "disconnect");
    for (size_t length = 0; length < full.size(); ++length) {
        reset();
        auto bytes = std::vector<uint8_t>(full.begin(), full.begin() + length);
        auto msg = message(bytes);
        auto before = saved_commands();
        CL_ParseCommandString(0, &msg);
        check(msg.overflowed, "truncated command marks overflow");
        check(connection.serverCommandSequence == 126 && unchanged(before), "truncated command leaves reliable history unchanged");
        bytes = full;
        msg = message(bytes);
        CL_ParseCommandString(0, &msg);
        check(!msg.overflowed && connection.serverCommandSequence == 128 && command_is(0, "disconnect"),
              "complete retransmission of the rejected sequence is accepted");
    }
    reset();
    auto bytes = packet(128, "complete but already overflowed");
    auto msg = message(bytes);
    msg.overflowed = 1;
    auto before = saved_commands();
    CL_ParseCommandString(0, &msg);
    check(msg.overflowed && connection.serverCommandSequence == 126 && unchanged(before), "preexisting overflow cannot commit a command");

    for (size_t length : {size_t(0), size_t(1), size_t(1023), size_t(1024), size_t(2048)}) {
        reset();
        std::string text(length, 'x');
        auto tail = saved_tail();
        bytes = packet(127, text);
        bytes.push_back(0x6b);
        msg = message(bytes);
        before = saved_commands();
        CL_ParseCommandString(0, &msg);
        check(!msg.overflowed && msg.readcount == static_cast<int>(bytes.size() - 1), "bounded string consumes exactly through terminator");
        check(connection.serverCommandSequence == 127 && command_is(127, text.substr(0, 1023)), "bounded string retains existing truncation policy");
        check(MSG_ReadByte(&msg) == 0x6b, "following opcode preserved");
        check(tail_unchanged(tail), "bounded string preserves connection fields after the final ring slot");
        check(std::memcmp(before.data(), connection.serverCommands, 127 * 1024) == 0, "bounded string cannot overwrite adjacent slots");
        reset();
        bytes = packet(127, text, false);
        msg = message(bytes);
        before = saved_commands();
        CL_ParseCommandString(0, &msg);
        check(msg.overflowed && connection.serverCommandSequence == 126 && unchanged(before), "unterminated bounded/oversized string cannot commit");
    }
    full = packet(127, "split command");
    for (size_t split = 0; split <= full.size(); ++split) {
        reset();
        msg = message(full);
        msg.cursize = static_cast<int>(split);
        msg.splitData = full.data() + split;
        msg.splitSize = static_cast<int>(full.size() - split);
        CL_ParseCommandString(0, &msg);
        check(!msg.overflowed && connection.serverCommandSequence == 127
              && command_is(127, "split command"), "command spans production split-buffer reader");
    }
    reset();
    bytes = packet(127, std::string("a") + char(146) + "b");
    msg = message(bytes);
    CL_ParseCommandString(0, &msg);
    check(command_is(127, "a'b"), "production character cleaning retained");
    auto bad = packet(128, "incomplete", false);
    msg = message(bad);
    before = saved_commands();
    CL_ParseCommandString(0, &msg);
    check(msg.overflowed && connection.serverCommandSequence == 127 && unchanged(before), "bad next command preserves previously committed command");
}
static void test_input()
{
    frame_msec = 100;
    kbutton_t key{};
    event("11", "100"); IN_KeyDown(&key);
    check(key.active && key.wasPressed && key.down[0] == 11 && key.downtime == 100, "first key press starts timing and pulse");
    event("11", "120"); IN_KeyDown(&key);
    check(key.down[1] == 0 && key.downtime == 100, "repeat press does not restart timing or add a source");
    event("22", "130"); IN_KeyDown(&key);
    check(key.down[1] == 22 && key.downtime == 100, "second source shares one held interval");
    warnings = 0;
    event("33", "140"); IN_KeyDown(&key);
    check(warnings == 1 && key.down[0] == 11 && key.down[1] == 22, "third source warns without changing ownership");
    event("11", "150"); IN_KeyUp(&key);
    check(key.active && key.msec == 0 && key.down[1] == 22, "releasing one source keeps button held");
    event("99", "160"); IN_KeyUp(&key);
    check(key.active && key.msec == 0, "unrelated release is ignored");
    event("22", "180"); IN_KeyUp(&key);
    check(!key.active && key.msec == 80, "last release accounts for full held interval once");
    check(CL_KeyState(&key) == 0.8f && key.msec == 0, "released interval becomes a frame fraction and is consumed");
    check(CL_KeyState(&key) == 0.0f, "released time is not counted twice");
    event("11", "200"); IN_KeyDown(&key);
    com_frameTime = 250;
    check(CL_KeyState(&key) == 0.5f && key.downtime == 250, "active key accounts only through current frame");
    com_frameTime = 350;
    check(CL_KeyState(&key) == 1.0f && key.downtime == 350, "held key advances timing each frame");
    event("11", "370"); IN_KeyUp(&key);
    check(CL_KeyState(&key) == 0.2f, "release after sampling accounts only for unsampled tail");
    key = {};
    event("11", "400"); IN_KeyDown(&key);
    event("11", "0"); IN_KeyUp(&key);
    check(key.msec == 50 && CL_KeyState(&key) == 0.5f, "missing release timestamp uses half a frame");
    key.msec = 150;
    check(CL_KeyState(&key) == 1.0f && key.msec == 0, "accumulated fraction is clamped to one frame");
    key = {};
    event("", "0"); IN_KeyDown(&key);
    check(key.active && key.down[0] == -1, "console press without key number remains representable");
    com_frameTime = 500;
    check(CL_KeyState(&key) == 1.0f, "missing press timestamp uses frame clock and clamps");
    event("", ""); IN_KeyUp(&key);
    check(!key.active && !key.down[0] && !key.down[1], "console release clears both key sources");

    // Reach the real private playersKb array through production command wrappers.
    CL_ClearKeys(0);
    event("11", "600"); IN_Attack_Down();
    event("11", "610"); IN_Attack_Up();
    int buttons = BUTTON_RELOAD;
    CL_UpdateCmdButton(0, &buttons, 14, BUTTON_ATTACK);
    check(buttons == (BUTTON_RELOAD | BUTTON_ATTACK), "short press survives release and preserves unrelated command bits");
    buttons = 0;
    CL_UpdateCmdButton(0, &buttons, 14, BUTTON_ATTACK);
    check(buttons == 0, "short-press pulse is consumed exactly once");
    event("11", "700"); IN_Attack_Down();
    for (int frame = 0; frame < 3; ++frame) {
        buttons = 0;
        CL_UpdateCmdButton(0, &buttons, 14, BUTTON_ATTACK);
        check(buttons == BUTTON_ATTACK, "held button remains set after pulse consumption");
    }
    CL_ClearKeys(0);
    buttons = 0;
    CL_UpdateCmdButton(0, &buttons, 14, BUTTON_ATTACK);
    check(buttons == 0, "clear keys removes both held state and pending pulse");
}
int main()
{
    test_parser();
    std::printf("parser: %d checks, %d failures\n", checks, failures);
    int parserChecks = checks, parserFailures = failures;
    test_input();
    std::printf("input: %d checks, %d failures\n", checks - parserChecks, failures - parserFailures);
    std::printf("-- %d checks, %d failures --\n", checks, failures);
    return failures ? 1 : 0;
}
