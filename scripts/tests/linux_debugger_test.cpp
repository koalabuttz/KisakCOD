//=============================================================================
// Runtime test for the native Linux dedicated script debugger
// (src/linux/script_debugger.cpp).
//
// The test is linked against the real debugger object file and supplies only the
// engine symbols that object references, so every assertion exercises production
// code rather than a stand-in implementation.
//
// The behaviour under test is what scr_vm.cpp depends on: a breakpoint handler
// must hand back the opcode that belongs at the position the VM is holding.  A
// `breakpoint;` statement compiles to OP_breakpoint without replacing an opcode,
// so the per-position entry keeps the 0x7F filler written at init - and 0x7F is
// OP_NOP, so resuming means carrying on with the next instruction.  A saved
// opcode is returned unchanged.  Only a breakpoint opcode at a position with no
// table entry at all (no table, or outside the compiled program) is a code
// stream error, and that is fatal: the error is recorded and the call does not
// return, exactly as Com_Error behaves in the engine.
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

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Engine symbols the debugger object references
//=============================================================================

scrVarPub_t scrVarPub;
scrCompilePub_t scrCompilePub;

uint32_t g_breakonObject;
uint32_t g_breakonString;
int g_breakonHit;

// Declared in the Windows-only win_net_debug.h, which this build does not use.
int Sys_IsRemoteDebugClient();

static int g_checks;
static int g_failures;

static void Check(bool ok, const char *what)
{
    ++g_checks;
    if (!ok)
        ++g_failures;
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
}

static void CheckEq(int actual, int expected, const char *what)
{
    ++g_checks;
    if (actual != expected)
    {
        ++g_failures;
        printf("  FAIL  %s (got %d, want %d)\n", what, actual, expected);
    }
    else
    {
        printf("  ok    %s\n", what);
    }
}

// Fatal error reporting.  The engine never returns from Com_Error; it reports
// and unwinds.  longjmp reproduces that, so a handler that is supposed to be
// fatal cannot quietly fall through in the test.
static jmp_buf g_fatalJmp;
static bool g_fatalArmed;
static int g_fatalCount;
static char g_fatalMessage[256];

void Com_Error(errorParm_t code, const char *fmt, ...)
{
    va_list ap;

    (void)code;
    ++g_fatalCount;
    va_start(ap, fmt);
    vsnprintf(g_fatalMessage, sizeof(g_fatalMessage), fmt, ap);
    va_end(ap);
    if (!g_fatalArmed)
    {
        printf("  FAIL  unexpected fatal error: %s\n", g_fatalMessage);
        exit(3);
    }
    longjmp(g_fatalJmp, 1);
}

void Com_PrintWarning(int channel, const char *fmt, ...)
{
    (void)channel;
    (void)fmt;
}

void Scr_PrintPrevCodePos(int channel, char *codePos, uint32_t index)
{
    (void)channel;
    (void)codePos;
    (void)index;
}

// Hunk allocations, with the sizes the debugger asks for recorded so the test
// can check they match the documented tables.
static unsigned g_allocCount;
static unsigned g_freeCount;
static uint32_t g_lastAllocSize;

void *Hunk_AllocDebugMem(uint32_t size)
{
    void *p = calloc(1, size ? size : 1);

    if (!p)
    {
        printf("  FAIL  out of memory\n");
        exit(2);
    }
    ++g_allocCount;
    g_lastAllocSize = size;
    return p;
}

void Hunk_FreeDebugMem(void *ptr)
{
    if (ptr)
        ++g_freeCount;
    free(ptr);
}

//=============================================================================
// Test program image
//=============================================================================

static const uint32_t PROGRAM_LEN = 64;
static char g_program[256];

static const uint32_t INS = 8;      // instruction the tests point at
static const uint32_t OUTSIDE = 100; // within g_program, past PROGRAM_LEN

typedef int (*HitFn)(char *pos);

static int HitBreakpoint(char *pos)
{
    return Scr_HitBreakpoint(0, pos, 1, 0);
}

static int HitAssignment(char *pos)
{
    return Scr_HitAssignmentBreakpoint(0, pos, 1, 0);
}

// Run a handler that is expected not to return, and report whether it was fatal.
static bool FatalFrom(HitFn fn, char *pos, const char **message)
{
    g_fatalCount = 0;
    g_fatalMessage[0] = 0;
    g_fatalArmed = true;
    *message = g_fatalMessage;

    if (setjmp(g_fatalJmp) == 0)
    {
        fn(pos);
        g_fatalArmed = false;
        return false;
    }

    g_fatalArmed = false;
    return g_fatalCount == 1;
}

int main()
{
    const char *msg;
    char *savedTable;
    int i;

    printf("script debugger (dedicated) runtime test\n");

    CheckEq((int)sizeof(scrDebuggerGlob_t), 0x2B8, "scrDebuggerGlob_t keeps its documented size");
    CheckEq((int)sizeof(Scr_Breakpoint), 0x1C, "Scr_Breakpoint keeps its documented size");
    CheckEq((int)(sizeof(g_breakpoints) / sizeof(g_breakpoints[0])), 128, "breakpoint slots");
    CheckEq(OP_NOP, 0x7F, "OP_NOP is the 0x7F the breakpoint table is filled with");

    memset(&scrVarPub, 0, sizeof(scrVarPub));
    memset(&scrCompilePub, 0, sizeof(scrCompilePub));
    memset(g_program, 0x5A, sizeof(g_program));   // an ordinary opcode everywhere

    //-------------------------------------------------------------------
    // developer 0: nothing is created
    //-------------------------------------------------------------------
    Scr_InitDebuggerMain();
    Scr_InitDebugger();
    Scr_InitDebuggerSystem();
    Check(!scrDebuggerGlob.debugger_inited_main, "no main state without developer");
    Check(!scrDebuggerGlob.debugger_inited, "no program state without developer");
    Check(!scrDebuggerGlob.debugger_inited_system, "no system state without developer");
    Check(scrDebuggerGlob.variableBreakpoints == 0 && scrDebuggerGlob.breakpoints == 0,
        "no debugger allocation without developer");
    Check(g_breakpointsHead == 0, "breakpoint free list untouched without developer");
    CheckEq((int)g_allocCount, 0, "no hunk allocation without developer");

    //-------------------------------------------------------------------
    // developer 1: the allocations and the starting state
    //-------------------------------------------------------------------
    scrVarPub.developer = 1;
    scrVarPub.programBuffer = g_program;
    scrCompilePub.script_loading = 1;
    scrCompilePub.programLen = PROGRAM_LEN;

    Scr_InitDebuggerMain();
    Check(scrDebuggerGlob.debugger_inited_main, "InitDebuggerMain sets inited_main");
    Check(scrDebuggerGlob.variableBreakpoints != 0, "variable breakpoint table allocated");
    CheckEq((int)g_lastAllocSize, (int)(4 * 98304), "variable breakpoint table is 98304 entries of 4 bytes");
    Check(scrDebuggerGlob.variableBreakpoints[0] == 0, "variable breakpoint table starts zeroed");
    Check(scrDebuggerGlob.assignHead == 0 && scrDebuggerGlob.assignHeadCodePos == 0,
        "assignment position list starts empty");
    Check(!Scr_IgnoreErrors(), "breakpoints enabled after InitDebuggerMain");

    Scr_InitDebugger();
    Check(scrDebuggerGlob.debugger_inited, "InitDebugger sets inited");
    CheckEq((int)g_lastAllocSize, (int)PROGRAM_LEN, "breakpoint table sized to the program");
    for (i = 0; i < (int)PROGRAM_LEN; ++i)
    {
        if ((unsigned char)scrDebuggerGlob.breakpoints[i] != 0x7F)
            break;
    }
    CheckEq(i, (int)PROGRAM_LEN, "every table entry starts at the 0x7F filler");

    Scr_InitDebuggerSystem();
    Check(scrDebuggerGlob.debugger_inited_system, "InitDebuggerSystem sets inited_system");
    Check(g_breakpointsHead == g_breakpoints, "breakpoint free list starts at the first slot");
    {
        int n = 0;
        bool linked = true;
        Scr_Breakpoint *bp;

        for (bp = g_breakpointsHead; bp; bp = bp->next, ++n)
        {
            if (bp != &g_breakpoints[n])
                linked = false;
            else if (n < 127 && bp->next != &g_breakpoints[n + 1])
                linked = false;
        }
        CheckEq(n, 128, "free list spans every breakpoint slot");
        Check(linked, "free list links the slots in order");
    }
    Check(!scrDebuggerGlob.atBreakpoint, "not sitting at a breakpoint");
    CheckEq(scrDebuggerGlob.breakpointPos.bufferIndex, -1, "no source position selected");
    CheckEq(scrDebuggerGlob.breakpointCount, 0, "no breakpoints installed");
    Check(scrDebuggerGlob.nextBreakpointCodePos == 0 && scrDebuggerGlob.killThreadCodePos == 0,
        "no pending step breakpoint or thread kill");
    Check(!scrDebuggerGlob.run_debugger, "debugger not running");
    Check(!scrVarPub.evaluate, "expression evaluation not left enabled");

    // Program state needs a script being loaded.
    Scr_ShutdownDebugger();
    scrCompilePub.script_loading = 0;
    Scr_InitDebugger();
    Check(!scrDebuggerGlob.debugger_inited && scrDebuggerGlob.breakpoints == 0,
        "no breakpoint table while no script is loading");
    scrCompilePub.script_loading = 1;
    Scr_InitDebugger();
    Check(scrDebuggerGlob.debugger_inited && scrDebuggerGlob.breakpoints != 0,
        "breakpoint table returns with the next script");

    //-------------------------------------------------------------------
    // Enable/disable flags the VM and parser consult
    //-------------------------------------------------------------------
    Scr_EnableBreakpoints(false);
    Check(Scr_IgnoreErrors() && scrDebuggerGlob.disableBreakpoints,
        "disabling breakpoints suppresses script errors");
    Scr_EnableBreakpoints(true);
    Check(!Scr_IgnoreErrors() && !scrDebuggerGlob.disableBreakpoints,
        "re-enabling breakpoints restores error reporting");

    Check(Scr_CanDrawScript(), "drawing allowed while the debugger is up");
    scrDebuggerGlob.scriptWatch.dirty = 1;
    Check(!Scr_CanDrawScript(), "drawing blocked while the watch list is dirty");
    scrDebuggerGlob.scriptWatch.dirty = 0;

    //-------------------------------------------------------------------
    // A `breakpoint;` statement: OP_breakpoint in the stream, table entry
    // still at the filler, which is OP_NOP.  Execution continues on OP_NOP.
    //-------------------------------------------------------------------
    g_program[INS] = (char)OP_breakpoint;
    CheckEq((unsigned char)scrDebuggerGlob.breakpoints[INS], 0x7F, "table entry for the statement is untouched");
    g_fatalCount = 0;
    CheckEq(HitBreakpoint(&g_program[INS + 1]), OP_NOP, "breakpoint statement resumes on OP_NOP");
    CheckEq(g_fatalCount, 0, "breakpoint statement is not an error");

    g_program[INS] = (char)OP_assignmentBreakpoint;
    g_fatalCount = 0;
    CheckEq(HitAssignment(&g_program[INS + 1]), OP_NOP, "assignment breakpoint statement resumes on OP_NOP");
    CheckEq(g_fatalCount, 0, "assignment breakpoint statement is not an error");

    g_program[INS] = (char)OP_manualAndAssignmentBreakpoint;
    g_fatalCount = 0;
    CheckEq(HitAssignment(&g_program[INS + 1]), OP_NOP, "manual assignment breakpoint resumes on OP_NOP");
    CheckEq(g_fatalCount, 0, "manual assignment breakpoint is not an error");

    //-------------------------------------------------------------------
    // A breakpoint that did replace an opcode: that opcode comes back
    // unchanged so the VM executes the instruction it stands for.
    //-------------------------------------------------------------------
    g_program[INS] = (char)OP_breakpoint;
    scrDebuggerGlob.breakpoints[INS] = (char)0x5A;
    g_fatalCount = 0;
    CheckEq(HitBreakpoint(&g_program[INS + 1]), 0x5A, "installed breakpoint returns the saved opcode");
    CheckEq(g_fatalCount, 0, "installed breakpoint is not an error");

    scrDebuggerGlob.breakpoints[INS] = (char)OP_NOP;
    CheckEq(HitBreakpoint(&g_program[INS + 1]), OP_NOP, "a saved OP_NOP is returned as OP_NOP");

    g_program[INS] = (char)OP_assignmentBreakpoint;
    scrDebuggerGlob.breakpoints[INS] = (char)0x61;
    g_fatalCount = 0;
    CheckEq(HitAssignment(&g_program[INS + 1]), 0x61, "installed assignment breakpoint returns the saved opcode");
    CheckEq(g_fatalCount, 0, "installed assignment breakpoint is not an error");

    //-------------------------------------------------------------------
    // No table entry to consult: a breakpoint opcode here cannot be resumed,
    // so it is fatal and does not return.
    //-------------------------------------------------------------------
    savedTable = scrDebuggerGlob.breakpoints;
    g_program[INS] = (char)OP_breakpoint;

    scrDebuggerGlob.breakpoints = 0;
    Check(FatalFrom(HitBreakpoint, &g_program[INS + 1], &msg),
        "breakpoint opcode with no table is fatal");
    Check(msg[0] != 0, "the fatal error carries a message");

    scrDebuggerGlob.breakpoints = savedTable;
    scrVarPub.programBuffer = 0;
    Check(FatalFrom(HitBreakpoint, &g_program[INS + 1], &msg),
        "breakpoint opcode with no program buffer is fatal");
    scrVarPub.programBuffer = g_program;

    g_program[OUTSIDE] = (char)OP_breakpoint;
    Check(FatalFrom(HitBreakpoint, &g_program[OUTSIDE + 1], &msg),
        "breakpoint opcode past the end of the program is fatal");

    g_program[INS] = (char)OP_manualAndAssignmentBreakpoint;
    scrDebuggerGlob.breakpoints = 0;
    Check(FatalFrom(HitAssignment, &g_program[INS + 1], &msg),
        "assignment breakpoint opcode with no table is fatal");
    scrDebuggerGlob.breakpoints = savedTable;

    //-------------------------------------------------------------------
    // Script error path: entered after the VM printed an error, at a position
    // with no table entry and no breakpoint opcode.  Nothing to resume, and
    // the caller discards the result, so this must not be an error.
    //-------------------------------------------------------------------
    scrDebuggerGlob.breakpoints = 0;
    g_program[INS] = (char)0x5A;
    g_fatalCount = 0;
    CheckEq(HitBreakpoint(&g_program[INS + 1]), 0, "script error path returns no opcode");
    CheckEq(g_fatalCount, 0, "script error path is not an error");
    CheckEq(HitAssignment(&g_program[INS + 1]), 0, "assignment error path returns no opcode");
    CheckEq(g_fatalCount, 0, "assignment error path is not an error");
    scrDebuggerGlob.breakpoints = savedTable;

    //-------------------------------------------------------------------
    // Assignment positions the compiler collects
    //-------------------------------------------------------------------
    {
        unsigned count = 0;
        Scr_OpcodeList_s *elem;

        Scr_AddAssignmentPos(&g_program[10]);
        Scr_AddAssignmentPos(&g_program[20]);
        Scr_AddAssignmentPos(&g_program[20]);
        for (elem = scrDebuggerGlob.assignHead; elem; elem = elem->next)
            ++count;
        CheckEq((int)count, 2, "assignment positions recorded without duplicates");

        scrCompilePub.developer_statement = 2;
        Scr_AddAssignmentPos(&g_program[30]);
        scrCompilePub.developer_statement = 0;
        count = 0;
        for (elem = scrDebuggerGlob.assignHead; elem; elem = elem->next)
            ++count;
        CheckEq((int)count, 2, "statement mode does not record assignment positions");
    }

    //-------------------------------------------------------------------
    // Break-on-notify leaves no stale match behind
    //-------------------------------------------------------------------
    g_breakonObject = 1234;
    g_breakonString = 5678;
    g_breakonHit = 1;
    Scr_CheckBreakonNotify(0, 0, 0, 0, 0);
    Check(!g_breakonObject && !g_breakonString && !g_breakonHit, "break-on-notify markers cleared");

    //-------------------------------------------------------------------
    // Remote debug socket is not part of this platform
    //-------------------------------------------------------------------
    CheckEq(Sys_IsRemoteDebugClient(), 0, "no remote debug client on this platform");
    CheckEq(Scr_UpdateDebugSocket(), 0, "no debug socket events");

    //-------------------------------------------------------------------
    // Shutdown and re-init must not carry runtime flags over
    //-------------------------------------------------------------------
    scrDebuggerGlob.atBreakpoint = 1;
    scrDebuggerGlob.run_debugger = 1;
    scrDebuggerGlob.breakpointCount = 7;
    scrDebuggerGlob.nextBreakpointCodePos = &g_program[1];

    Scr_ShutdownDebuggerSystem(0);
    Check(!scrDebuggerGlob.debugger_inited_system, "ShutdownDebuggerSystem clears inited_system");
    CheckEq(scrDebuggerGlob.breakpointCount, 0, "shutdown clears the breakpoint count");
    Check(scrDebuggerGlob.nextBreakpointCodePos == 0, "shutdown clears the pending step position");
    Check(!scrVarPub.evaluate, "shutdown leaves evaluation disabled");

    Scr_InitDebuggerSystem();
    Check(scrDebuggerGlob.debugger_inited_system, "the debugger system re-initialises");
    Check(!scrDebuggerGlob.atBreakpoint, "re-init clears the at-breakpoint flag");
    Check(!scrDebuggerGlob.run_debugger, "re-init clears the run-debugger flag");
    CheckEq(scrDebuggerGlob.breakpointPos.bufferIndex, -1, "re-init clears the selected source position");
    Check(g_breakpointsHead == g_breakpoints, "re-init rebuilds the breakpoint free list");

    Scr_EnableBreakpoints(false);
    Scr_ShutdownDebuggerMain();
    Scr_InitDebuggerMain();
    Check(!Scr_IgnoreErrors(), "re-init of the main state re-enables breakpoints");

    //-------------------------------------------------------------------
    // Final shutdown releases everything
    //-------------------------------------------------------------------
    Scr_ShutdownDebugger();
    Check(!scrDebuggerGlob.debugger_inited, "ShutdownDebugger clears inited");
    Check(scrDebuggerGlob.breakpoints == 0, "breakpoint table freed and cleared");

    Scr_ShutdownDebuggerMain();
    Check(!scrDebuggerGlob.debugger_inited_main, "ShutdownDebuggerMain clears inited_main");
    Check(scrDebuggerGlob.variableBreakpoints == 0, "variable breakpoint table freed and cleared");
    Check(scrDebuggerGlob.assignHead == 0 && scrDebuggerGlob.assignHeadCodePos == 0,
        "assignment position list freed and cleared");
    CheckEq((int)g_allocCount, (int)g_freeCount, "every debugger allocation is released");

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
