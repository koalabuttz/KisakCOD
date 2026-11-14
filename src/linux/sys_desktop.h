#pragma once

// Message boxes, web links and starting the other game program for the Linux,
// macOS and Vita builds: what src/win32/win_main.cpp and win_syscon.cpp do with
// MessageBoxA, ShellExecuteA and CreateProcessA.
//
// The game windows (KisakCOD-mp and KisakCOD-sp on Linux and macOS) define
// Sys_ShowDialog and Sys_OpenWebUrl through SDL in
// src/linux/client/mp_client_input.cpp. The dedicated server and the Vita
// have no desktop: src/linux/main.cpp's stand-ins show nothing and open
// nothing, and the callers keep their console messages.
//
// The rules below and Sys_StartProgram are header-only and free of engine
// types so that scripts/tests/test_sys_desktop.py compiles exactly them.

#include <cstddef>
#include <cstring>
#if !defined(__vita__)
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

enum class SysDialogKind
{
    // win_syscon.cpp's Sys_SetErrorText: MB_ICONERROR with one OK button.
    Error,
    // com_playerprofile.cpp's MB_YESNO | MB_ICONINFORMATION questions.
    Question,
};

enum class SysDialogAnswer
{
    // No box was shown: no desktop, dialogs turned off, or another thread.
    NotShown,
    Ok,
    Yes,
    No,
};

// Shows a modal box and waits for the player. `title` and `text` are the
// engine's one-byte (Latin-1) text.
SysDialogAnswer Sys_ShowDialog(SysDialogKind kind, const char *title, const char *text);

// Hands a web address to the desktop's browser. False when nothing took it.
// Sys_OpenURL checks Com_IsWebUrl first (src/qcommon/url_check.h).
bool Sys_OpenWebUrl(const char *url);

// The environment variable that turns every box off: scripted runs set it so a
// fatal error or a question never waits for a click nobody will make.
constexpr const char SYS_NO_DIALOGS_VARIABLE[] = "KISAK_NO_DIALOGS";

// Why no box may be shown, or nullptr when one may.
//   noDialogs    getenv(SYS_NO_DIALOGS_VARIABLE): set to anything but "" or
//                "0" turns boxes off.
//   videoDriver  SDL's video driver, or SDL_VIDEODRIVER before SDL has video.
//                The dummy and offscreen drivers have no screen to show a box
//                on; SDL would still try X11 and wait on a display nobody sees.
//   mainThread   SDL's boxes belong to the thread that runs the window (on
//                macOS another thread's box waits for the main thread, which
//                may be the one blocked).
inline const char *Sys_DialogRefusal(const char *noDialogs, const char *videoDriver, bool mainThread)
{
    if (noDialogs && noDialogs[0] && strcmp(noDialogs, "0") != 0)
        return "turned off by KISAK_NO_DIALOGS";
    if (videoDriver && (strcmp(videoDriver, "dummy") == 0 || strcmp(videoDriver, "offscreen") == 0))
        return "the video driver has no screen";
    if (!mainThread)
        return "not the main thread";
    return nullptr;
}

// Sys_QuitAndStartProcess on Linux and macOS starts a program that sits in the
// same folder as the running one, named without any folder: the other game
// program, never a file a server sent (CL_DownloadsComplete's auto-update
// passes "updates/<name>", a Windows program).
inline bool Sys_IsSiblingProgramName(const char *name)
{
    if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (const char *cursor = name; *cursor; ++cursor)
    {
        const unsigned char c = static_cast<unsigned char>(*cursor);
        if (c == '/' || c == '\\' || c == ':' || c < 0x20 || c == 0x7F)
            return false;
    }
    return true;
}

// True when a folder can be handed to the other game program whole, as
// +set <dvar> "<folder>": it is not empty, and has no '+' or line break, which
// would split the engine's command line, and no quote, which would end the
// argument.
inline bool Sys_FolderCanBePassed(const char *folder)
{
    return folder && folder[0] && !strpbrk(folder, "+\n\r\"");
}

// The game and home folders this program started with (fs_basepath and
// fs_homepath once Com_Init has read the command line), which
// Sys_QuitAndStartProcess hands to the program it starts.
//
// They are kept here, and not read from the dvars when the other program is
// started, because a game server sets a client's dvars by name and retail's
// settings stay settable (docs/server-set-dvars.md): after a visit to a server
// the dvars may name a folder the server chose, and the other program would
// load its game data from there.
class SysStartFolders
{
public:
    // Keeps both folders, the first time only. A folder that cannot be passed
    // whole or does not fit is kept as "none", and the other program then finds
    // its own.
    void Remember(const char *basePath, const char *homePath)
    {
        if (remembered_)
            return;
        remembered_ = true;
        Keep(basePath, basePath_);
        Keep(homePath, homePath_);
    }

    bool Remembered() const { return remembered_; }
    // nullptr when there is none to pass.
    const char *BasePath() const { return basePath_[0] ? basePath_ : nullptr; }
    const char *HomePath() const { return homePath_[0] ? homePath_ : nullptr; }

private:
    // Shorter than sys_exitCmdLine (1024), which has to hold both.
    static constexpr size_t kFolderSize = 448;

    static void Keep(const char *folder, char (&kept)[kFolderSize])
    {
        kept[0] = '\0';
        if (!Sys_FolderCanBePassed(folder))
            return;
        const size_t length = strlen(folder);
        if (length < kFolderSize)
            memcpy(kept, folder, length + 1);
    }

    bool remembered_ = false;
    char basePath_[kFolderSize] = {};
    char homePath_[kFolderSize] = {};
};

// Splits the parameters Sys_QuitAndStartProcess was given into arguments, as
// the engine's own tokenizer reads a Windows command line: whitespace ends an
// argument and a pair of double quotes groups one, quotes removed. Writes the
// arguments one after another into `storage`, each ended by '\0', and their
// starts into `arguments`. Returns how many, or -1 when they do not fit.
inline int Sys_SplitParameters(const char *parameters, char *storage, size_t storageSize,
                               const char **arguments, int maxArguments)
{
    if (!parameters)
        return 0;
    size_t used = 0;
    int count = 0;
    const char *cursor = parameters;
    for (;;)
    {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r')
            ++cursor;
        if (!*cursor)
            return count;
        if (count >= maxArguments)
            return -1;
        arguments[count++] = storage + used;
        bool quoted = false;
        while (*cursor && (quoted || (*cursor != ' ' && *cursor != '\t' && *cursor != '\n' && *cursor != '\r')))
        {
            if (*cursor == '"')
            {
                quoted = !quoted;
                ++cursor;
                continue;
            }
            if (used + 1 >= storageSize)
                return -1;
            storage[used++] = *cursor++;
        }
        if (used >= storageSize)
            return -1;
        storage[used++] = '\0';
    }
}

#if !defined(__vita__)
// win_main.cpp's CreateProcessA for Sys_Quit: starts `program` with
// `arguments` (argv, program first, ended by nullptr) and does not wait for
// it. The new program runs on its own while this one finishes quitting: its
// input is /dev/null, so it never waits on this terminal, and it inherits no
// open file but output and errors (not this process's sockets, devices or
// logs). Returns the new process, or -1 with errno set when it could not be
// made; a program that fails to start exits with 127.
inline pid_t Sys_StartProgram(const char *program, const char *const *arguments)
{
    long openMax = sysconf(_SC_OPEN_MAX);
    if (openMax < 0 || openMax > 65536)
        openMax = 65536;

    const pid_t child = fork();
    if (child == 0)
    {
        // Only async-signal-safe calls between fork and exec: this process has
        // other threads, whose locks the child would inherit held.
        const int devNull = open("/dev/null", O_RDONLY);
        if (devNull >= 0)
        {
            dup2(devNull, STDIN_FILENO);
            if (devNull != STDIN_FILENO)
                close(devNull);
        }
        for (int fd = 3; fd < openMax; ++fd)
            close(fd);
        execv(program, const_cast<char *const *>(arguments));
        _exit(127);
    }
    return child;
}
#endif
