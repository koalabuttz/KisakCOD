// Runs the production CL_Setenv_f from the full client startup translation unit.
// Only command input and console output are supplied by this harness.
#include <client_mp/client_mp.h>
#include <qcommon/cmd.h>
#include <universal/assertive.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::vector<std::string> arguments;
static std::string printed;
static int checks, failures;
int Cmd_Argc() { return static_cast<int>(arguments.size()); }
const char *Cmd_Argv(int index) {
    if (index < 0 || static_cast<size_t>(index) >= arguments.size()) std::abort();
    return arguments[index].c_str();
}
void Com_Printf(int, const char *format, ...) {
    char buffer[4096];
    va_list args; va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args); va_end(args);
    printed += buffer;
}
void MyAssertHandler(const char *, int, int, const char *, ...) { std::abort(); }
void Com_Error(errorParm_t, const char *, ...) { std::abort(); }
static void check(bool pass, const char *label) {
    ++checks; failures += !pass;
    std::printf("%s: %s\n", pass ? "PASS" : "FAIL", label);
}
static void invoke(std::vector<std::string> args) {
    arguments = std::move(args); printed.clear(); CL_Setenv_f();
}
static void overwriteStack() {
    volatile unsigned char bytes[8192];
    for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = 0xA5;
}
static bool valueIs(const char *name, const std::string &expected) {
    const char *value = std::getenv(name);
    return value && value == expected;
}
int main() {
    // Private names only, no account/configuration environment is inspected.
    const char *key = "KISAK_CLIENT_PLATFORM_TEST_VALUE";
    unsetenv(key);
    invoke({"setenv", key});
    check(printed == std::string(key) + " undefined\n", "query reports absent private variable");
    invoke({"setenv", key, "alpha", "beta"});
    arguments.clear(); overwriteStack();
    check(valueIs(key, "alpha beta "), "environment copies stack buffer and preserves argument spacing");
    invoke({"setenv", key});
    check(printed == std::string(key) + "=alpha beta \n", "query reads copied value through production command");
    invoke({"setenv", key, "replacement"}); overwriteStack();
    check(valueIs(key, "replacement "), "assignment replaces existing value");
    invoke({"setenv", key, ""}); overwriteStack();
    check(valueIs(key, " "), "empty argument preserves existing trailing-space command semantics");
    invoke({"setenv", std::string(key) + "=prefix", "tail"}); overwriteStack();
    check(valueIs(key, "prefix=tail "), "first equals separates name from value like Windows putenv");
    invoke({"setenv", key, std::string(4096, 'z')}); overwriteStack();
    check(valueIs(key, std::string(1023 - std::strlen(key) - 1, 'z')),
          "long values obey original bounded command buffer");
    std::string before(std::getenv(key));
    invoke({"setenv", std::string(2048, 'n'), "unused"});
    check(valueIs(key, before), "truncated assignment without equals leaves existing variable alone");
    std::string longKey = "KISAK_CLIENT_PLATFORM_TEST_";
    longKey.resize(1022, 'n');
    setenv(longKey.c_str(), "seed", 1);
    invoke({"setenv", longKey, "does-not-fit"});
    check(std::getenv(longKey.c_str()) == nullptr,
          "name filling buffer leaves NAME= and removes variable like Windows putenv");
    unsetenv(longKey.c_str());
    invoke({"setenv"});
    check(printed.empty() && valueIs(key, before), "bare command does not mutate environment");
    unsetenv(key);
    std::printf("-- %d checks, %d failures --\n", checks, failures);
    return failures ? 1 : 0;
}
