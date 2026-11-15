#!/usr/bin/env python3
"""Check that the remembered dvar and menu lookups give the full lookup's answer.

Dvar_FindVar (src/universal/dvar.cpp) and Menus_FindByName
(src/ui/ui_input_core.cpp) remember what was found last time through each name
pointer, so a caller that passes the same pointer again (a string literal, a
menu expression's operand) skips the walk, and they remember the names that
were not found (src/universal/name_miss_memory.h), so asking again for a dvar
that does not exist or a menu of another context skips it too. This check
compiles the production
lookups on the host, with the production name comparison (q_shared.cpp) and the
production menu list functions (ui_registration.cpp), and compares every answer
with a plain walk over made-up dvars and menus: pointers whose text changes,
names in another case, names that are missing and appear later, lists that are
cleared and refilled in another order, and menus that share a name.

--bench also prints how long a lookup takes with and without the memory.
No game assets are needed.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def block(source: str, start: int) -> str:
    """Return a brace-delimited declaration/function, ignoring quoted braces."""
    opening = source.index("{", start)
    depth = 1
    tokens = re.compile(
        r'''//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[{}]''',
        re.DOTALL,
    )
    for token in tokens.finditer(source, opening + 1):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start:token.end()]
    raise ValueError("Unterminated production declaration")


HARNESS_TOP = r"""
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

typedef uint8_t byte;
typedef uint16_t word;
typedef uint64_t uint64;
typedef long LONG;
#define HIDWORD(x) ((int)((uint64_t)(x) >> 32))
#define LODWORD(x) ((int)(uint64_t)(x))
#define iassert(x) do { if (!(x)) throw std::runtime_error("assert: " #x); } while (0)

void MyAssertHandler(const char*, int, int, const char* format, ...) {
    throw std::runtime_error(std::string("assert: ") + format);
}
enum { ERR_DROP = 1 };
void Com_Error(int, const char* format, ...) { throw std::runtime_error(std::string("Com_Error: ") + format); }
void NET_Sleep(int) {}

struct FastCriticalSection { volatile LONG readCount; volatile LONG writeCount; };
static LONG InterlockedIncrement(volatile LONG* value) { const LONG next = *value + 1; *value = next; return next; }
static LONG InterlockedDecrement(volatile LONG* value) { const LONG next = *value - 1; *value = next; return next; }

// The menu structures as far as the lookup and the list functions use them.
struct menuDef_t { struct { const char* name; } window; };
struct UILocalVarContext { int inits, shutdowns; };
struct UiContext {
    int localClientNum;
    menuDef_t* Menus[640];
    int menuCount;
    menuDef_t* menuStack[16];
    int openMenuCount;
    UILocalVarContext localVars;
};
void UILocalVar_Init(UILocalVarContext* vars) { ++vars->inits; }
void UILocalVar_Shutdown(UILocalVarContext* vars) { ++vars->shutdowns; }
"""

HARNESS_BOTTOM = r"""
static void check(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

// ---- dvars ----

static std::deque<dvar_s> g_dvarStorage;
static std::deque<std::string> g_dvarNames;

static dvar_s* addDvar(const std::string& name) {
    dvar_s* dvar = &g_dvarStorage.emplace_back();
    dvar->name = g_dvarNames.emplace_back(name).c_str();
    const int hash = generateHashValue(dvar->name);
    s_dvarRegistrations.fetch_add(1);  // as Dvar_RegisterNew does (checked in the script)
    dvar->hashNext = dvarHashTable[hash];
    dvarHashTable[hash] = dvar;
    return dvar;
}

// The lookup without the memory: the chain walk alone.
static dvar_s* walkDvars(const char* name) {
    for (dvar_s* var = dvarHashTable[generateHashValue(name)]; var; var = var->hashNext)
        if (!I_stricmp(name, var->name))
            return var;
    return nullptr;
}

static void findDvar(const char* name) {
    dvar_s* want = walkDvars(name);
    dvar_s* got = Dvar_FindMalleableVar(name);
    check(got == want, std::string("Dvar_FindVar differs for ") + name);
    check(g_dvarCritSect.readCount == 0, "the dvar read count was not given back");
}

static int rememberedDvars() {
    int count = 0;
    for (auto& slot : s_dvarFoundByPointer)
        count += slot.load() != nullptr;
    return count;
}

static std::string anyCase(std::mt19937& rng, std::string name) {
    for (char& c : name)
        if (rng() % 3 == 0)
            c = (rng() & 1) ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
    return name;
}

static void checkDvars(std::mt19937& rng) {
    // Fixed cases first.
    dvar_s* fov = addDvar("cg_fov");
    char buffer[64] = "cg_fov";
    findDvar("cg_fov");
    findDvar(buffer);
    check(Dvar_FindMalleableVar(buffer) == fov, "cg_fov was not found");
    strcpy(buffer, "CG_FOV");  // the same pointer, another case
    findDvar(buffer);
    check(Dvar_FindMalleableVar(buffer) == fov, "CG_FOV was not found");
    strcpy(buffer, "cg_fovScale");  // the same pointer, a dvar that is missing
    findDvar(buffer);
    check(!Dvar_FindMalleableVar(buffer), "a missing dvar was found");
    findDvar(buffer);  // asked again: the remembered miss
    check(s_dvarMisses.Used() >= 1, "a missing dvar was not remembered");
    dvar_s* scale = addDvar("cg_fovScale");  // ... and appears later
    findDvar(buffer);
    check(Dvar_FindMalleableVar(buffer) == scale, "a dvar added after a miss was not found");
    strcpy(buffer, "cg_fo");  // a prefix of a remembered name
    findDvar(buffer);
    strcpy(buffer, "");
    findDvar(buffer);
    bool stopped = false;
    try { Dvar_FindMalleableVar(nullptr); } catch (const std::runtime_error&) { stopped = true; }
    check(stopped, "a null name must stop as before");
    g_dvarCritSect.readCount = 0;  // the made-up Com_Error threw past the release

    // Many dvars and many pointers, some of which change their text.
    std::vector<std::string> names;
    for (int i = 0; i < 2500; ++i)
        names.push_back("dvar_" + std::to_string(rng() % 100000) + (i % 7 ? "_x" : "") + std::to_string(i)
                        + (i % 11 ? "" : std::string(30 + i % 20, 'n')));  // some too long to remember as missing
    for (int i = 0; i < 2000; ++i)
        addDvar(names[i]);  // the last 500 stay missing for now
    std::vector<std::vector<char>> pointers(600, std::vector<char>(96));
    auto refill = [&](std::vector<char>& p) {
        const std::string text = anyCase(rng, names[rng() % names.size()]);
        strcpy(p.data(), text.c_str());
    };
    for (auto& p : pointers) refill(p);
    for (int step = 0; step < 200000; ++step) {
        auto& p = pointers[rng() % pointers.size()];
        if (rng() % 50 == 0) refill(p);
        if (step == 100000)
            for (int i = 2000; i < 2500; ++i) addDvar(names[i]);
        findDvar(p.data());
    }
    check(rememberedDvars() > 300, "the dvar memory is hardly used; audit the check");
    check(s_dvarMisses.Used() > 60, "the memory of missing dvars is hardly used; audit the check");

    // Dvar_Shutdown: every dvar goes, and the same names come back as others.
    memset(dvarHashTable, 0, sizeof(dvarHashTable));
    Dvar_ForgetFoundByPointer();
    check(rememberedDvars() == 0, "dvars were still remembered after the shutdown");
    for (auto& p : pointers) findDvar(p.data());
    for (int i = 0; i < 2500; ++i) addDvar(names[i]);
    for (auto& p : pointers) findDvar(p.data());
}

// ---- menus ----

static menuDef_t* walkMenus(const UiContext* dc, const char* name) {
    for (int i = 0; i < dc->menuCount; ++i)
        if (!I_stricmp(dc->Menus[i]->window.name, name))
            return dc->Menus[i];
    return nullptr;
}

static void findMenu(const UiContext* dc, const char* name) {
    check(Menus_FindByName(dc, name) == walkMenus(dc, name), std::string("Menus_FindByName differs for ") + name);
}

static void checkMenus(std::mt19937& rng) {
    static UiContext contexts[2];
    UiContext* dc = &contexts[0];
    UI_SetupMenuContext(dc, nullptr);
    UI_SetupMenuContext(&contexts[1], nullptr);

    // Fixed cases first: two menus share the name "a".
    static menuDef_t x{{"x"}}, a1{{"a"}}, a2{{"A"}}, b{{"b"}};
    const char* nameA = "a";
    findMenu(dc, nameA);  // an empty list
    UI_AppendMenuReference(dc, &x);
    UI_AppendMenuReference(dc, &a1);
    findMenu(dc, nameA);
    check(Menus_FindByName(dc, nameA) == &a1, "menu a was not found");
    findMenu(&contexts[1], nameA);  // the other context has no such menu
    // Cleared whole, as CG_Init does, and refilled so that the remembered place
    // holds a menu with the name again but another one comes first.
    memset(dc, 0, sizeof(*dc));
    findMenu(dc, nameA);
    UI_AppendMenuReference(dc, &a2);
    UI_AppendMenuReference(dc, &a1);
    check(Menus_FindByName(dc, nameA) == &a2, "the first menu named a must be found after a refill");
    const char* nameB = "B";
    findMenu(dc, nameB);  // not there yet
    findMenu(dc, nameB);  // the remembered miss
    check(!Menus_FindByName(dc, nameB), "a missing menu was found");
    UI_AppendMenuReference(dc, &b);
    check(Menus_FindByName(dc, nameB) == &b, "a menu added after a miss was not found");
    findMenu(dc, "B");
    UI_ShutdownOwnedMenuContext(dc);
    findMenu(dc, nameA);
    UI_SetupMenuContext(dc, nullptr);
    // A test fixture may write a short list by hand, with no call that counts
    // a change (tests/linux_window/menu_script_test.cpp does). The list's
    // length is part of what a remembered miss is held against, so a menu put
    // into an empty list this way is found.
    findMenu(dc, nameA);
    check(!Menus_FindByName(dc, nameA), "an empty list has no menu a");
    dc->Menus[0] = &a1;
    dc->menuCount = 1;
    check(Menus_FindByName(dc, nameA) == &a1, "a menu written into an empty list by hand was not found");
    UI_SetupMenuContext(dc, nullptr);

    // Random lists in both contexts.
    std::vector<std::string> names;
    for (int i = 0; i < 12; ++i) names.push_back("menu_" + std::to_string(i));
    std::deque<std::string> menuNames;
    std::deque<menuDef_t> menus;
    for (int i = 0; i < 900; ++i) {
        menuNames.push_back(anyCase(rng, names[rng() % names.size()]));  // names repeat
        menus.push_back(menuDef_t{{menuNames.back().c_str()}});
    }
    // Far more name pointers than the memory has places: every place is shared
    // by several pointers and by both contexts, wherever the addresses fall.
    std::vector<std::vector<char>> pointers(1500, std::vector<char>(32));
    auto refill = [&](std::vector<char>& p) {
        const std::string text = rng() % 10 ? anyCase(rng, names[rng() % names.size()]) : "no_such_menu";
        strcpy(p.data(), text.c_str());
    };
    for (auto& p : pointers) refill(p);
    for (int step = 0; step < 300000; ++step) {
        dc = &contexts[rng() % 2];
        const unsigned op = rng() % 1000;
        if (op < 30) {
            if (!UI_AppendMenuReference(dc, &menus[rng() % menus.size()]))
                check(dc->menuCount == 640, "an append was refused below 640 menus");
        } else if (op < 32) {
            memset(dc, 0, sizeof(*dc));
        } else if (op < 34) {
            UI_ShutdownOwnedMenuContext(dc);
            UI_SetupMenuContext(dc, nullptr);
        } else if (op < 50) {
            refill(pointers[rng() % pointers.size()]);
        } else {
            findMenu(dc, pointers[rng() % pointers.size()].data());
        }
    }
    int remembered = 0;
    for (auto& slot : s_menuFoundByPointer) remembered += slot.load() != 0;
    check(remembered > 40, "the menu memory is hardly used; audit the check");
    check(s_menuMisses.Used() > 20, "the memory of missing menus is hardly used; audit the check");

    // A fifth context has no number and is always walked; the two above and
    // the two below took four.
    static UiContext more[3];
    for (UiContext& context : more) {
        UI_SetupMenuContext(&context, nullptr);
        for (int i = 0; i < 20; ++i) UI_AppendMenuReference(&context, &menus[rng() % menus.size()]);
    }
    check(Menus_ContextNumber(&more[0]) == 2 && Menus_ContextNumber(&more[1]) == 3
          && Menus_ContextNumber(&more[2]) == -1 && Menus_ContextNumber(&contexts[1]) == 1,
          "the contexts did not get the numbers 0 to 3 and then none");
    for (int step = 0; step < 20000; ++step)
        findMenu(&more[rng() % 3], pointers[rng() % pointers.size()].data());

    // Past 2^20 changes the count no longer fits an entry: nothing is
    // remembered or used from then on.
    g_uiMenuListChanges.store((1u << 20) - 2);
    for (int step = 0; step < 4000; ++step) {
        dc = &contexts[rng() % 2];
        if (step % 1000 == 999) {
            UI_SetupMenuContext(dc, nullptr);  // crosses 2^20 on the second time
            for (int i = 0; i < 30; ++i) UI_AppendMenuReference(dc, &menus[rng() % menus.size()]);
        }
        findMenu(dc, pointers[rng() % pointers.size()].data());
    }
    check(g_uiMenuListChanges.load() > (1u << 20), "the change count did not pass 2^20");
    uint32_t before[256];
    for (int i = 0; i < 256; ++i) before[i] = s_menuFoundByPointer[i].load();
    for (int step = 0; step < 4000; ++step)
        findMenu(&contexts[rng() % 2], pointers[rng() % pointers.size()].data());
    for (int i = 0; i < 256; ++i)
        check(s_menuFoundByPointer[i].load() == before[i], "a place was remembered past 2^20 changes");
}

// ---- the memory of misses under several threads ----

static void checkMissMemoryThreads() {
    // Name i is only ever remembered with stamp 7 * i + 1. Whatever the
    // threads do to each other's entries, no other pair may ever be reported.
    static NameMissMemory<4> memory;  // 16 entries for 200 names: constant overwriting
    static std::vector<std::string> names;
    // Half of the names differ only in their last bytes, in the text's last word.
    for (int i = 0; i < 200; ++i)
        names.push_back(i % 2 ? std::string(NAME_MISS_TEXT_BYTES - 4, 'p') + std::to_string(100 + i)
                              : "name_" + std::to_string(i * 7919) + std::string(i % 30, 'x'));
    std::atomic<int> wrong{0};
    std::atomic<long> hits{0};
    auto work = [&](unsigned seed) {
        std::mt19937 rng(seed);
        for (int step = 0; step < 400000; ++step) {
            const int i = (int)(rng() % names.size());
            const uint32_t right = 7u * (uint32_t)i + 1;
            if (rng() % 3 == 0) {
                memory.Remember(names[i].c_str(), right);
            } else {
                const int j = (int)(rng() % names.size());
                if (j != i && memory.Has(names[i].c_str(), 7u * (uint32_t)j + 1)) ++wrong;
                // Another name asked under this name's stamp, as if the two
                // texts were taken for one.
                if (j != i && memory.Has(names[j].c_str(), right)) ++wrong;
                if (memory.Has(names[i].c_str(), right)) ++hits;
            }
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) threads.emplace_back(work, 1000 + t);
    for (auto& thread : threads) thread.join();
    check(wrong.load() == 0, "a miss was reported under a stamp it was never remembered with");
    check(hits.load() > 1000, "the threads hardly ever found a remembered miss; audit the check");
    const std::string tooLong(NAME_MISS_TEXT_BYTES, 'z'), longest(NAME_MISS_TEXT_BYTES - 1, 'z');
    memory.Remember(tooLong.c_str(), 5);
    memory.Remember(longest.c_str(), 5);
    check(!memory.Has(tooLong.c_str(), 5) && memory.Has(longest.c_str(), 5) && !memory.Has(longest.c_str(), 6)
          && !memory.Has(nullptr, 5), "the longest name that fits, the first that does not, or a null name");
}

// ---- timing (--bench) ----

template <typename Find> static double nsEach(Find find, const std::vector<const char*>& names) {
    uintptr_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    const int rounds = 20000;
    for (int round = 0; round < rounds; ++round)
        for (const char* name : names)
            sink += (uintptr_t)find(name);
    const auto end = std::chrono::steady_clock::now();
    if (!sink) std::printf("(nothing found)\n");
    return std::chrono::duration<double, std::nano>(end - start).count() / (rounds * names.size());
}

template <typename Find> static double nsEachMiss(Find find, const std::vector<const char*>& names) {
    uintptr_t sink = 0;
    const auto start = std::chrono::steady_clock::now();
    const int rounds = 20000;
    for (int round = 0; round < rounds; ++round)
        for (const char* name : names)
            sink += (uintptr_t)find(name);
    const auto end = std::chrono::steady_clock::now();
    if (sink) std::printf("(a missing name was found)\n");
    return std::chrono::duration<double, std::nano>(end - start).count() / (rounds * names.size());
}

static void bench() {
    std::vector<const char*> dvarNames, menuNames;
    for (size_t i = 0; i < g_dvarNames.size() && dvarNames.size() < 60; i += 37)
        dvarNames.push_back(g_dvarNames[i].c_str());
    std::printf("bench: Dvar_FindVar over %zu dvars: %.1f ns each walking the chain, %.1f ns each remembered\n",
                g_dvarNames.size(), nsEach(walkDvars, dvarNames), nsEach(Dvar_FindMalleableVar, dvarNames));
    static std::deque<std::string> missing;
    std::vector<const char*> missingNames;
    for (int i = 0; i < 20; ++i) missingNames.push_back(missing.emplace_back("ui_not_a_dvar_" + std::to_string(i)).c_str());
    std::printf("bench: a missing dvar: %.1f ns each walking the chain, %.1f ns each remembered\n",
                nsEachMiss(walkDvars, missingNames), nsEachMiss(Dvar_FindMalleableVar, missingNames));
    static UiContext dc;
    static std::deque<std::string> names;
    static std::deque<menuDef_t> menus;
    // Start the menu memory afresh: the check above used up the four context
    // numbers and took the change count past its limit.
    for (auto& context : s_menuContexts) context.store(nullptr);
    for (auto& slot : s_menuFoundByPointer) slot.store(0);
    g_uiMenuListChanges.store(0);
    UI_SetupMenuContext(&dc, nullptr);
    for (int i = 0; i < 300; ++i) {
        names.push_back("menu_number_" + std::to_string(i));
        menus.push_back(menuDef_t{{names.back().c_str()}});
        UI_AppendMenuReference(&dc, &menus.back());
    }
    for (int i : {20, 150, 280}) menuNames.push_back(names[i].c_str());
    std::vector<const char*> missingMenus = {"scoreboard_elsewhere", "another_context_menu", "third_missing_menu"};
    std::printf("bench: a missing menu among 300: %.1f ns each walking the list, %.1f ns each remembered\n",
                nsEachMiss([](const char* name) { return walkMenus(&dc, name); }, missingMenus),
                nsEachMiss([](const char* name) { return Menus_FindByName(&dc, name); }, missingMenus));
    std::printf("bench: Menus_FindByName over 300 menus: %.1f ns each walking the list, %.1f ns each remembered\n",
                nsEach([](const char* name) { return walkMenus(&dc, name); }, menuNames),
                nsEach([](const char* name) { return Menus_FindByName(&dc, name); }, menuNames));
}

int main(int argc, char** argv) {
    try {
        std::mt19937 rng(20261114);
        checkDvars(rng);
        std::printf("PASS: Dvar_FindVar gives the chain walk's dvar through pointers that change, any case, "
                    "late dvars and a shutdown\n");
        checkMenus(rng);
        std::printf("PASS: Menus_FindByName gives the first menu with the name through cleared and refilled "
                    "lists and shared names\n");
        checkMissMemoryThreads();
        std::printf("PASS: a remembered miss is only ever reported for its own text and stamp, with four threads "
                    "writing\n");
        if (argc > 1 && !strcmp(argv[1], "--bench"))
            bench();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "name pointer lookup check failed: %s\n", error.what());
        return 1;
    }
    return 0;
}
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--sanitize", action="store_true", help="run with address and undefined-behavior sanitizers")
    parser.add_argument("--bench", action="store_true", help="also print the time per lookup")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    q_shared_h = (root / "src/universal/q_shared.h").read_text()
    q_shared = (root / "src/universal/q_shared.cpp").read_text()
    dvar_cpp = (root / "src/universal/dvar.cpp").read_text()
    miss_h = (root / "src/universal/name_miss_memory.h").read_text()
    registration_h = (root / "src/ui/ui_registration.h").read_text()
    registration = (root / "src/ui/ui_registration.cpp").read_text()
    input_core = (root / "src/ui/ui_input_core.cpp").read_text()

    types = block(q_shared_h, q_shared_h.index("enum DvarType")) + ";\n"
    start = q_shared_h.index("union DvarValue")
    end = q_shared_h.index("using dvar_t = dvar_s;", start)
    types += q_shared_h[start:end] + "using dvar_t = dvar_s;\n"

    # Dvar_Shutdown is too tangled to compile here; it must forget under the
    # write lock, which the harness then does by hand.
    shutdown = block(dvar_cpp, dvar_cpp.index("void Dvar_Shutdown()"))
    if not re.search(r"Dvar_ForgetFoundByPointer\(\);\s*Sys_UnlockWrite\(&g_dvarCritSect\);", shutdown):
        raise SystemExit("Dvar_Shutdown no longer forgets the remembered dvars before it unlocks")

    # A remembered miss holds until a dvar is registered: Dvar_RegisterNew is
    # the one place a dvar enters the table, and it must count just before.
    if len(re.findall(r"dvarHashTable\[[^\]]*\]\s*=[^=]", dvar_cpp)) != 1 or not re.search(
            r"s_dvarRegistrations\.fetch_add\(1, std::memory_order_relaxed\);\s*"
            r"dvar->hashNext = dvarHashTable\[HashValue\];\s*dvarHashTable\[HashValue\] = dvar;", dvar_cpp):
        raise SystemExit("a dvar enters the hash table without counting a registration first")

    # Menus_FindByName trusts that a menu list only changes through
    # ui_registration.cpp (which counts each change) or by clearing a whole
    # context. Any other write in the engine must be looked at.
    write = re.compile(r"(?:->|\.)Menus\[[^\]]*\]\s*=[^=]|(?:->|\.)menuCount\s*(?:=[^=]|\+\+|--|\+=|-=)|(?:\+\+|--)\s*[\w.>-]*(?:->|\.)menuCount\b")
    known = {
        # Not a menu list: the decompiler's name for a field after uiDC.
        ("src/ui_mp/ui_main_mp.cpp", "dc[1].Menus[56] = (menuDef_t *)(dc[1].Menus[56] == 0);"),
        # The text menu loader's own MenuList, not a context.
        ("src/ui/ui_shared_obj.cpp", "g_load_0.menuList.menus[g_load_0.menuList.menuCount++] = menu;"),
    }
    for path in sorted((root / "src").rglob("*.cpp")) + sorted((root / "src").rglob("*.h")):
        relative = path.relative_to(root).as_posix()
        if relative == "src/ui/ui_registration.cpp":
            continue
        for line in path.read_text(errors="replace").splitlines():
            if write.search(line) and (relative, line.strip()) not in known:
                raise SystemExit(f"{relative} changes a menu list outside ui_registration.cpp: {line.strip()}")

    start = dvar_cpp.index("static std::atomic<dvar_s *> s_dvarFoundByPointer")
    end = dvar_cpp.index("const dvar_s *Dvar_FindVar(const char *dvarName)", start)
    start_h = registration_h.index("inline std::atomic<uint32_t> g_uiMenuListChanges;")
    start_menu = input_core.index("static std::atomic<const UiContext *> s_menuContexts")
    end_menu = input_core.index("void Menus_CloseByName(", start_menu)
    production = [
        "bool I_isupper(int c);\nint I_strnicmp(const char* s0, const char* s1, int n);",
        block(q_shared, q_shared.index("int I_stricmp(const char* s0, const char* s1)")),
        block(q_shared, q_shared.index("int I_strnicmp(const char* s0, const char* s1, int n)")),
        block(q_shared, q_shared.index("bool I_isupper(int c)")),
        types,
        miss_h[miss_h.index("inline constexpr size_t NAME_MISS_TEXT_WORDS"):],
        "static FastCriticalSection g_dvarCritSect;\nstatic dvar_s* dvarHashTable[0x100];",
        block(dvar_cpp, dvar_cpp.index("static int generateHashValue(const char* fname)")),
        dvar_cpp[start:end],
        block(registration_h, start_h),
        block(registration, registration.index("void UI_SetupMenuContext(")),
        block(registration, registration.index("bool UI_AppendMenuReference(")),
        block(registration, registration.index("void UI_ShutdownOwnedMenuContext(")),
        input_core[start_menu:end_menu],
    ]
    program = HARNESS_TOP + "\n\n".join(production) + HARNESS_BOTTOM

    flags = ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"]
    with tempfile.TemporaryDirectory(prefix="kisakcod-name-pointer-lookups-") as temp:
        cpp, executable = Path(temp) / "name_pointer_lookups.cpp", Path(temp) / "name_pointer_lookups"
        cpp.write_text(program)
        subprocess.run(shlex.split(args.cxx) + flags + [str(cpp), "-o", str(executable)], check=True)
        subprocess.run([str(executable)] + (["--bench"] if args.bench else []), check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
