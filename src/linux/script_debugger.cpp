//=============================================================================
// script_debugger.cpp
//
// Non-interactive script debugger for the native Linux dedicated server.
//
// The original implementation (src/script/scr_debugger.cpp) is the in-game
// script debugger: a set of UI_Component-derived windows (script list, watch
// list, call stack) plus the breakpoint bookkeeping that backs them.  None of
// the UI layer (src/ui/*) is compiled into the dedicated build, so including
// the original translation unit would drag in the UI class vtables and the
// Win32 remote-debug socket without ever being usable: the dedicated server
// has no client, no renderer and no window, so `cls.uiStarted` is always 0 and
// a breakpoint could never be displayed.
//
// This file therefore provides exactly the subset of the debugger API that the
// VM, the compiler, the expression evaluator and the server still reference,
// with the interactive parts removed rather than stubbed out wholesale:
//
//   * The debugger's data (`scrDebuggerGlob`, `g_breakpoints`) is initialized
//     without constructing UI objects; the dedicated header uses plain storage
//     for the unused window regions.
//   * No interactive breakpoint patches the code stream. Compiler-emitted
//     breakpoint statements retain an OP_NOP entry in the breakpoint table;
//     returning that byte preserves the VM's normal no-debugger continuation.
//     A breakpoint outside the available table is reported as an error.
//   * Error reporting, the compiler, the VM and `developer` execution are
//     untouched: Scr_IgnoreErrors/Scr_EnableBreakpoints still reflect
//     `scrDebuggerGlob.disableBreakpoints`, and the script error path still
//     runs Scr_ShowConsole/Scr_HitBreakpoint as before.
//
// The remote debug socket (Sys_*DebugSocket*, Sys_IsRemoteDebugClient) is a
// Windows/Winsock feature and is not part of this port; see src/linux/net.cpp.
//=============================================================================

#include <universal/q_shared.h>
#include <universal/com_memory.h>
#include <qcommon/qcommon.h>

#include <script/scr_debugger.h>
#include <script/scr_variable.h>
#include <script/scr_parser.h>
#include <script/scr_main.h>
#include <script/scr_compiler.h>
#include <script/scr_vm.h>

#include <string.h>

//=============================================================================
// Debugger data
//
// A real, statically initialised object of the dedicated layout declared in
// scr_debugger.h: with the debugger windows replaced by same-size storage, the
// struct holds no polymorphic members, so defining it emits no constructors,
// destructors or vtables.  Everything starts zeroed, which is the state the
// original globals start in: every pointer null and every flag clear until the
// Init functions below fill them in.
//=============================================================================

scrDebuggerGlob_t scrDebuggerGlob;

Scr_Breakpoint g_breakpoints[128];
Scr_Breakpoint *g_breakpointsHead;

// Filler byte written into the breakpoint table where no breakpoint has been
// installed.  This matches the original: the table is memset to 0x7F at init,
// and an entry only stops being 0x7F once a real breakpoint has saved the
// opcode it replaced.
static const unsigned char SCR_BREAKPOINT_INFO_EMPTY = 0x7F;

// Size of the per-variable breakpoint table allocated by Scr_InitDebuggerMain.
// 98304 entries of 4 bytes, as in the original.
static const uint32_t SCR_VARIABLE_BREAKPOINT_COUNT = 98304;

//=============================================================================
// Platform stubs for features the interactive debugger owns on Windows
//=============================================================================

/*
====================
Sys_IsRemoteDebugClient

The remote script debug socket is a Win32 feature (win_net_debug.cpp) and does
not exist on this platform, so this is never a remote debug client.  Callers use
this to pick the local debugger path; with no remote client and no UI, the
script debugger is simply inactive.
====================
*/
int Sys_IsRemoteDebugClient()
{
    return 0;
}

/*
====================
Sys_Listen_f

Registered as the "net_listen" command.  It connects the debug socket to a
listening remote debugger, which this build does not implement, so say so
instead of failing silently.
====================
*/
void Sys_Listen_f()
{
    Com_PrintWarning(CON_CHANNEL_DONT_FILTER,
        "net_listen: the remote script debugger is not available in this build.\n");
}

//=============================================================================
// Breakpoint table
//=============================================================================

void Scr_InitBreakpoints()
{
    uint32_t i;

    for (i = 0; i < ARRAY_COUNT(g_breakpoints) - 1; ++i)
        g_breakpoints[i].next = &g_breakpoints[i + 1];
    g_breakpoints[ARRAY_COUNT(g_breakpoints) - 1].next = 0;
    g_breakpointsHead = g_breakpoints;
}

/*
====================
Scr_FindBreakpointInfo

Same contract as the original: the byte in the breakpoint table that belongs to
the instruction at *codePos.  Returns 0 when the table is not allocated or the
position is outside the compiled program.
====================
*/
static char *ScrDebuggerFindBreakpointInfo(const char *codePos)
{
    ptrdiff_t index;

    if (!codePos || !scrDebuggerGlob.breakpoints || !scrVarPub.programBuffer)
        return 0;

    index = codePos - scrVarPub.programBuffer;
    if (index < 0 || index >= (ptrdiff_t)scrCompilePub.programLen)
        return 0;

    return &scrDebuggerGlob.breakpoints[index];
}

/*
====================
Scr_SavedOriginalOpcode

The saved opcode (including OP_NOP for a compiler-emitted breakpoint), or -1
when the instruction is outside the available table.
====================
*/
static int Scr_SavedOriginalOpcode(const char *codePos)
{
    const char *info;
    unsigned char saved;

    info = ScrDebuggerFindBreakpointInfo(codePos);
    if (!info)
        return -1;

    saved = (unsigned char)*info;
    // The compiler emits explicit breakpoint statements without replacing an
    // opcode. Their untouched table entry is OP_NOP (0x7F), which the original
    // debugger returns so execution can continue when no UI is available.
    return saved;
}

/*
====================
Scr_AllowedToSuspend

The original returns 0 when breakpoints are disabled and 1 only while a client
UI is up.  A dedicated server never starts a client UI, so a breakpoint can
never be presented and execution must simply continue.
====================
*/
char Scr_AllowBreakpoint(char *pos)
{
    if (scrDebuggerGlob.disableBreakpoints)
        return 0;

    if (pos)
    {
        Com_PrintWarning(CON_CHANNEL_PARSERSCRIPT, "script runtime warning: ignored breakpoint.\n");
        Scr_PrintPrevCodePos(CON_CHANNEL_PARSERSCRIPT, pos, 0);
    }

    return 0;
}

//=============================================================================
// Init / shutdown
//
// These keep the original ordering and the same resulting debugger state, minus
// the UI component Init/Shutdown calls, which cannot be made here: they are
// virtual members of the UI classes that are not part of this build.
//=============================================================================

void Scr_InitDebuggerMain()
{
    if (!scrVarPub.developer)
        return;

    if (scrDebuggerGlob.debugger_inited_main)
        return;

    scrDebuggerGlob.variableBreakpoints =
        (Scr_WatchElementDoubleNode_t **)Hunk_AllocDebugMem(4 * SCR_VARIABLE_BREAKPOINT_COUNT);
    memset((uint8_t *)scrDebuggerGlob.variableBreakpoints, 0, 4 * SCR_VARIABLE_BREAKPOINT_COUNT);

    scrDebuggerGlob.assignHead = 0;
    scrDebuggerGlob.assignHeadCodePos = 0;
    scrDebuggerGlob.disableBreakpoints = 0;

    // UI_ScrollPane::Init / UI_VerticalDivider::Init belong to the windows that
    // are not compiled into this build.
    scrDebuggerGlob.debugger_inited_main = 1;
}

void Scr_ShutdownDebuggerMain()
{
    Scr_OpcodeList_s *opcodeElement;

    if (!scrVarPub.developer || !scrDebuggerGlob.debugger_inited_main)
        return;

    scrDebuggerGlob.debugger_inited_main = 0;

    if (scrDebuggerGlob.variableBreakpoints)
    {
        Hunk_FreeDebugMem(scrDebuggerGlob.variableBreakpoints);
        scrDebuggerGlob.variableBreakpoints = 0;
    }

    while (scrDebuggerGlob.assignHead)
    {
        opcodeElement = scrDebuggerGlob.assignHead->next;
        Hunk_FreeDebugMem(scrDebuggerGlob.assignHead);
        scrDebuggerGlob.assignHead = opcodeElement;
    }

    scrDebuggerGlob.assignHeadCodePos = 0;
}

void Scr_InitDebugger()
{
    if (!scrVarPub.developer || !scrCompilePub.script_loading)
        return;

    if (scrDebuggerGlob.debugger_inited)
        return;

    scrDebuggerGlob.breakpoints = (char *)Hunk_AllocDebugMem(scrCompilePub.programLen);
    memset((uint8_t *)scrDebuggerGlob.breakpoints, SCR_BREAKPOINT_INFO_EMPTY, scrCompilePub.programLen);

    // Scr_ScriptList::Init / Scr_OpenScriptList::Init build the script windows
    // that the source list would be shown in; there is nothing to show.
    scrDebuggerGlob.debugger_inited = 1;
}

void Scr_ShutdownDebugger()
{
    if (!scrVarPub.developer || !scrDebuggerGlob.debugger_inited)
        return;

    scrDebuggerGlob.debugger_inited = 0;

    if (scrDebuggerGlob.breakpoints)
    {
        Hunk_FreeDebugMem(scrDebuggerGlob.breakpoints);
        scrDebuggerGlob.breakpoints = 0;
    }
}

void Scr_InitDebuggerSystem()
{
    if (!scrVarPub.developer)
        return;

    if (scrDebuggerGlob.debugger_inited_system)
        return;

    Scr_InitBreakpoints();

    scrDebuggerGlob.nextBreakpointCodePos = 0;
    scrDebuggerGlob.killThreadCodePos = 0;
    scrDebuggerGlob.breakpointCount = 0;

    // The original raises this while it evaluates watch expressions and lowers
    // it again afterwards; with no watch elements the net effect is 0.
    scrVarPub.evaluate = 0;

    scrDebuggerGlob.assignBreakpointSet = 0;
    scrDebuggerGlob.breakpointPos.bufferIndex = -1;
    scrDebuggerGlob.atBreakpoint = 0;
    scrDebuggerGlob.run_debugger = 0;
    scrDebuggerGlob.gainFocusTime = 0;

    // Scr_ScriptWatch::Init / Scr_ScriptCallStack::Init / LoadScriptPos build
    // the debugger window contents.

    scrDebuggerGlob.debugger_inited_system = 1;
}

void Scr_ShutdownDebuggerSystem(int restart)
{
    if (!scrVarPub.developer)
        return;

    if (scrDebuggerGlob.debugger_inited_system)
    {
        scrDebuggerGlob.debugger_inited_system = 0;
        scrVarPub.evaluate = 0;

        if (scrDebuggerGlob.nextBreakpointCodePos)
            scrDebuggerGlob.nextBreakpointCodePos = 0;
        if (scrDebuggerGlob.killThreadCodePos)
            scrDebuggerGlob.killThreadCodePos = 0;

        // No breakpoint is ever installed, so the count stays at 0 and there is
        // nothing to unbreak here.
        scrDebuggerGlob.breakpointCount = 0;
    }
}

//=============================================================================
// Per-frame update
//=============================================================================

/*
====================
Scr_UpdateDebugger

Called from the server frame loop.  The original returns immediately when the
debugger is not running or when a breakpoint could not be presented; on a
dedicated server the latter is always the case, so this only ever refreshes
debugger data and never suspends.
====================
*/
void Scr_UpdateDebugger()
{
    if (!scrVarPub.developer || !scrDebuggerGlob.debugger_inited_system)
        return;

    if (!Scr_AllowBreakpoint(0))
        return;
}

bool Scr_IgnoreErrors()
{
    return scrDebuggerGlob.disableBreakpoints;
}

void Scr_EnableBreakpoints(bool enable)
{
    scrDebuggerGlob.disableBreakpoints = !enable;
}

/*
====================
Scr_CanDrawScript

The original gates the script overlay on the debugger being up and the watch
list being clean.  Nothing here ever makes the watch list dirty, but the flag is
still consulted so the gating matches the original; Scr_DrawScript below has no
renderer to draw with.
====================
*/
bool Scr_CanDrawScript()
{
    return scrDebuggerGlob.debugger_inited_system && scrDebuggerGlob.scriptWatch.dirty == 0;
}

void Scr_DrawScript()
{
    // Unreachable: Scr_CanDrawScript is always false on a dedicated server.
}

//=============================================================================
// Breakpoint handlers
//
// scr_vm re-dispatches on the table byte, which may be a saved instruction or
// OP_NOP for an explicit script breakpoint. An instruction outside the table
// cannot be resumed through this mechanism.
//=============================================================================

int Scr_HitBreakpoint(VariableValue *top, char *pos, uint32_t localId, int hitBreakpoint)
{
    const char *codePos;
    int opcode;

    codePos = pos - 1;
    opcode = Scr_SavedOriginalOpcode(codePos);

    if (opcode < 0)
    {
        if ((unsigned char)*(pos - 1) == (unsigned char)OP_breakpoint)
        {
            Com_Error(
                ERR_FATAL,
                "Scr_HitBreakpoint: reached OP_breakpoint with no breakpoint installed at %p; "
                "the script code stream is corrupted",
                (const void *)codePos);
        }

        // Reached from the script error path (scr_vm.cpp, after Scr_ShowConsole),
        // where the result is discarded and no breakpoint was involved.
        return 0;
    }

    if (!Scr_AllowBreakpoint(pos))
        return opcode;

    // A real breakpoint was recorded, but a dedicated server has no client UI to
    // present it in, so execution continues on the original opcode.
    return opcode;
}

int Scr_HitAssignmentBreakpoint(VariableValue *top, char *pos, uint32_t localId, int forceBreak)
{
    const char *codePos;
    int opcode;

    codePos = pos - 1;
    opcode = Scr_SavedOriginalOpcode(codePos);

    if (opcode < 0)
    {
        unsigned char current = (unsigned char)*(pos - 1);

        if (current == (unsigned char)OP_assignmentBreakpoint
            || current == (unsigned char)OP_manualAndAssignmentBreakpoint)
        {
            Com_Error(
                ERR_FATAL,
                "Scr_HitAssignmentBreakpoint: reached an assignment breakpoint with no original "
                "opcode recorded at %p; the script code stream is corrupted",
                (const void *)codePos);
        }

        return 0;
    }

    if (!Scr_AllowBreakpoint(pos))
        return opcode;

    return opcode;
}

/*
====================
Scr_HitBuiltinBreakpoint

Only reachable when a builtin carries a breakpoint, and the only writer of
func_table[].breakpointCount in this build sets it to 0 (scr_compiler.cpp,
scr_compiler2.cpp), so this is never called.  It has no bytecode to restore, so
doing nothing is exactly what "no builtin breakpoint hit" means.
====================
*/
void Scr_HitBuiltinBreakpoint(
    VariableValue *top,
    const char *pos,
    uint32_t localId,
    int opcode,
    int builtinIndex,
    uint32_t outparamcount)
{
}

/*
====================
Scr_CheckBreakonNotify

Break-on-notify is driven by watch elements whose expressions the user marked
with breakon.  No watch element exists without the interactive debugger, so
there is nothing to match and the break marker never gets set.
====================
*/
void Scr_CheckBreakonNotify(
    uint32_t notifyListOwnerId,
    uint32_t stringValue,
    VariableValue *top,
    char *pos,
    uint32_t localId)
{
    g_breakonObject = 0;
    g_breakonString = 0;
    g_breakonHit = 0;
}

/*
====================
Scr_DebugKillThread / Scr_DebugTerminateThread

Both exist to clean up breakpoints and debugger stack entries attached to a
thread that is going away.  No breakpoint or debugger stack entry is ever
created here, so there is nothing to clean up.
====================
*/
void Scr_DebugKillThread(uint32_t threadId, const char *codePos)
{
}

void Scr_DebugTerminateThread(int topThread)
{
}

//=============================================================================
// Interactive entry points
//
// These are the ways a user opens the debugger.  There is no console or window
// to open it in, so they report that the feature is unavailable.  Script error
// reporting does not depend on them: errors are printed by the VM before these
// are reached.
//=============================================================================

void Scr_ShowConsole()
{
    // The original raises the in-game console so the player can read the script
    // error.  A dedicated server has no in-game console; the error has already
    // been written to the server log by the time this is called.
}

void Scr_RunDebugger()
{
    Com_PrintWarning(CON_CHANNEL_PARSERSCRIPT,
        "script debugger: no interactive debugger is available on this server.\n");
}

void Scr_RunDebuggerRemote()
{
    Com_PrintWarning(CON_CHANNEL_PARSERSCRIPT,
        "script debugger: the remote script debugger is not available in this build.\n");
}

Scr_WatchElement_s *Scr_DisplayDebugger()
{
    Com_PrintWarning(CON_CHANNEL_PARSERSCRIPT,
        "script debugger: no interactive debugger is available on this server.\n");
    return 0;
}

/*
====================
Scr_MonitorCommand

The "monitor" command adds a watch element to the debugger window.  There is no
debugger window, so the command has nothing to attach the expression to.
====================
*/
void Scr_MonitorCommand(const char *text)
{
    Com_PrintWarning(CON_CHANNEL_PARSERSCRIPT,
        "script debugger: \"monitor\" is unavailable, no debugger window exists on this server.\n");
}

//=============================================================================
// Remote debug socket
//=============================================================================

/*
====================
Scr_UpdateDebugSocket

Drains the remote debug socket, which this build does not have.  Returns 0, the
same answer the original gives when no debug client is connected, so the caller
does not consider a packet arrived.
====================
*/
int Scr_UpdateDebugSocket()
{
    return 0;
}

void Scr_UpdateRemoteDebugger()
{
    // No remote debug client can be attached; see Sys_IsRemoteDebugClient.
}

//=============================================================================
// Assignment breakpoint positions
//=============================================================================

/*
====================
Scr_AddAssignmentPos

Recorded by the compiler for every assignment so that the debugger can later
turn statement positions into assignment breakpoints.  Nothing converts these
into breakpoints in this build, but the positions are still collected exactly
as the original does, because Scr_BreakOnAllAssignmentPos is what chooses which
of them matter and it is driven by the UI.
====================
*/
void Scr_AddAssignmentPos(char *codePos)
{
    Scr_OpcodeList_s *element;

    if (!scrVarPub.developer)
        return;

    if (scrCompilePub.developer_statement != 2 && scrDebuggerGlob.assignHeadCodePos != codePos)
    {
        scrDebuggerGlob.assignHeadCodePos = codePos;
        element = (Scr_OpcodeList_s *)Hunk_AllocDebugMem(sizeof(Scr_OpcodeList_s));
        element->codePos = codePos;
        element->next = scrDebuggerGlob.assignHead;
        scrDebuggerGlob.assignHead = element;
    }
}

/*
====================
Scr_RefToVariable

Called by the expression evaluator for each variable an expression references,
so the debugger knows when to re-evaluate a watch element that depends on it.
It only does anything while a watch element is being registered, which is a UI
operation, so with no debugger the reference is simply not watched.
====================
*/
bool Scr_RefToVariable(uint32_t id, int isObject)
{
    if (!id)
        return 0;

    if (scrDebuggerGlob.removeId)
        return 0;

    if (!scrDebuggerGlob.currentElement)
        return 0;

    if (!scrDebuggerGlob.variableBreakpoints)
        return 0;

    // No watch element is currently being registered (currentElement is never
    // set without the interactive debugger), so there is nothing to record.
    return 0;
}

//=============================================================================
// Watch list entry points used by the VM save/restore path
//
// scr_readwrite.cpp calls these two on scrDebuggerGlob.scriptWatch while it
// saves and restores debugger state.  Both bodies in the original walk the
// elementHead list of watch elements; no watch element can exist without the
// interactive debugger, so the list is always empty and there is nothing to do.
//=============================================================================

#if defined(__linux__) && defined(KISAK_DEDICATED)
void ScrDebuggerWatchStorage::Evaluate()
{
}

void ScrDebuggerWatchStorage::UpdateBreakpoints(bool add)
{
    (void)add;
}
#endif
