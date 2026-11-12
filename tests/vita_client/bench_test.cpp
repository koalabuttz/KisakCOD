// Host checks for the PS Vita bench that fetches its work
// (src/vita/vita_bench.h and vita_bench_client.h), run by
// scripts/tests/test_vita_bench.py.
//
// Without arguments: the formats, SHA-256 and the message codes.
//
//     bench_test start <data folder> <fetched build's path> <fetched 0|1>
//                      <by bench 0|1> <label>
//
// is one process of the game on a card that is the data folder: it does what
// the start-up does with the bench and, for a start of a job's list, what the
// list of starts does (vita_main.cpp): the log named after the start with the
// label in it, the next start's number. Starting the other build is the exit
// code (10 the installed build, 11 the fetched one), so the Python test plays
// the system and runs the next process.

#include <vita/vita_bench_client.h>

#include <cstdio>
#include <cstring>
#include <string>

using namespace kisak::vita_bench;

namespace
{
int g_failures;

#define CHECK(condition)                                                                            \
    do                                                                                              \
    {                                                                                               \
        if (!(condition))                                                                           \
        {                                                                                           \
            printf("FAIL: %s (line %d)\n", #condition, __LINE__);                                   \
            ++g_failures;                                                                           \
        }                                                                                           \
    } while (0)

std::string Sha(const std::string &text)
{
    Sha256 sha;
    // In two pieces, to cross a block's edge both ways.
    sha.Add(text.data(), text.size() / 3);
    sha.Add(text.data() + text.size() / 3, text.size() - text.size() / 3);
    char hex[65];
    sha.Finish(hex);
    return hex;
}

bool JobRefused(const char *text, const char *word)
{
    Job job;
    char error[160];
    if (ParseJob(text, &job, error, sizeof(error)))
        return false;
    return job.none && strstr(error, word) != nullptr;
}

bool ServerRefused(const char *text)
{
    Server server;
    char error[160];
    return !ParseServer(text, &server, error, sizeof(error)) && error[0];
}

int Checks()
{
    CHECK(Sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(Sha(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    {
        // RFC 4231, test case 2.
        Mac mac("Jefe");
        mac.Part("what do ya want for nothing?");
        char hex[65];
        mac.Finish(hex);
        CHECK(!strcmp(hex, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
        // Parts are joined with a newline.
        Mac whole("Jefe"), parts("Jefe");
        whole.Part("ask\n0123");
        parts.Part("ask");
        parts.Part("0123");
        char a[65], b[65];
        whole.Finish(a);
        parts.Finish(b);
        CHECK(!strcmp(a, b) && SameCode(a, b));
        b[10] = b[10] == '0' ? '1' : '0';
        CHECK(!SameCode(a, b) && !SameCode(a, "") && !SameCode("", ""));
    }

    Server server;
    char error[160];
    CHECK(ParseServer("192.168.8.213 28970 0123456789abcdef0123456789abcdef\n", &server, error, sizeof(error)));
    CHECK(!strcmp(server.host, "192.168.8.213") && server.port == 28970 && server.waitSeconds == 0);
    CHECK(!strcmp(server.key, "0123456789abcdef0123456789abcdef"));
    CHECK(ParseServer("10.0.0.2\t80  ABCDEFGHijklmnop wait 600\r\nmore", &server, error, sizeof(error)));
    CHECK(server.port == 80 && server.waitSeconds == 600);
    CHECK(ServerRefused(""));
    CHECK(ServerRefused("laptop.local 80 0123456789abcdef"));
    CHECK(ServerRefused("10.0.0.2.5 80 0123456789abcdef"));
    CHECK(ServerRefused("10.0.0.256 80 0123456789abcdef"));
    CHECK(ServerRefused("10.0.0.2 0 0123456789abcdef"));
    CHECK(ServerRefused("10.0.0.2 65536 0123456789abcdef"));
    CHECK(ServerRefused("10.0.0.2 80 short"));
    CHECK(ServerRefused("10.0.0.2 80 0123456789abcde/"));
    CHECK(ServerRefused("10.0.0.2 80 0123456789abcdef wait"));
    CHECK(ServerRefused("10.0.0.2 80 0123456789abcdef hold 5"));
    CHECK(ServerRefused("10.0.0.2 80 0123456789abcdef wait 86401"));

    const std::string a(64, 'a'), b(64, 'b');
    Job job;
    CHECK(ParseJob("none\n", &job, error, sizeof(error)) && job.none);
    const std::string text = "job 20261112T021500-run-w\r\neboot " + a + " 3414532\nfile runlist-mp.txt " + b +
                             " 120\nfile run-w.txt " + a + " 0\nsend framelog.json\n";
    CHECK(ParseJob(text.c_str(), &job, error, sizeof(error)));
    CHECK(!job.none && !strcmp(job.id, "20261112T021500-run-w") && job.eboot == Eboot::Given);
    CHECK(job.ebootBytes == 3414532 && a == job.ebootSha256 && job.fileCount == 2 && job.sendCount == 1);
    CHECK(job.File("runlist-mp.txt") && job.File("runlist-mp.txt")->bytes == 120 && !job.File("other.txt"));
    CHECK(!strcmp(job.send[0], "framelog.json"));
    CHECK(ParseJob(("job j\nfile runlist-mp.txt " + b + " 1\n").c_str(), &job, error, sizeof(error)));
    CHECK(job.eboot == Eboot::Installed);
    CHECK(JobRefused("", "no \"job"));
    CHECK(JobRefused(("file x.txt " + a + " 1\n").c_str(), "no \"job"));
    CHECK(JobRefused("job a\njob b\n", "line 2"));
    CHECK(JobRefused("job ../a\n", "line 1"));
    CHECK(JobRefused("none\njob a\n", "none"));
    CHECK(JobRefused("job a\nnone\n", "none"));
    CHECK(JobRefused("job a\neboot installed\n", "line 2"));
    CHECK(JobRefused(("job a\neboot " + a + " 0\n").c_str(), "line 2"));
    CHECK(JobRefused(("job a\neboot " + a + " 67108865\n").c_str(), "line 2"));
    CHECK(JobRefused(("job a\neboot " + a.substr(1) + "G 5\n").c_str(), "line 2"));
    CHECK(JobRefused(("job a\neboot " + a + " 5\neboot " + a + " 5\n").c_str(), "line 3"));
    // A job writes plain .txt names into the data folder and nowhere else,
    // and not over the bench's own files.
    for (const char *name : {"../x.txt", "main/x.txt", "x.cfg", "config_mp.cfg", ".txt", "bench.txt",
             "bench-job.txt", ".hidden.txt", "a\\b.txt", "ux0:x.txt"})
        CHECK(JobRefused(("job a\nfile " + std::string(name) + " " + a + " 1\n").c_str(), "line 2"));
    CHECK(JobRefused(("job a\nfile x.txt " + a + " 1048577\n").c_str(), "line 2"));
    CHECK(JobRefused(("job a\nfile x.txt " + a + " 1\nfile x.txt " + a + " 1\n").c_str(), "line 3"));
    CHECK(JobRefused("job a\nsend bench.txt\n", "line 2"));
    CHECK(JobRefused("job a\nsend ../console-mp.log\n", "line 2"));
    CHECK(JobRefused("job a\nsend iw_00.iwd\n", "line 2"));
    CHECK(JobRefused("job a\nrun this\n", "line 2"));
    {
        std::string many = "job a\n";
        for (int i = 0; i < 17; ++i)
            many += "file f" + std::to_string(i) + ".txt " + a + " 1\n";
        CHECK(JobRefused(many.c_str(), "line 18"));
    }

    State state;
    CHECK(ParseState("j-1 run fetched\n", &state) && !strcmp(state.id, "j-1") && !state.send && state.fetched);
    CHECK(ParseState("j-1 send installed\nthe build did not start\n", &state) && state.send && !state.fetched);
    CHECK(!strcmp(state.note, "the build did not start"));
    char written[320];
    FormatState(state, written, sizeof(written));
    CHECK(!strcmp(written, "j-1 send installed\nthe build did not start\n"));
    CHECK(!ParseState("", &state) && !ParseState("j-1 run", &state) && !ParseState("j-1 go fetched", &state));
    CHECK(!ParseState("../j run fetched", &state) && !ParseState("j run other", &state));

    HttpHead head;
    const char answer[] = "HTTP/1.0 200 OK\r\nServer: x\r\ncontent-LENGTH:  12\r\nX-Bench-Mac: abc\r\n\r\nhello";
    CHECK(ParseHttpHead(answer, 20, &head) == 0);
    CHECK(ParseHttpHead(answer, sizeof(answer) - 1, &head) == 1);
    CHECK(head.status == 200 && head.hasLength && head.length == 12 && !strcmp(answer + head.headBytes, "hello"));
    char value[8];
    CHECK(HttpHeaderValue(answer, head.headBytes, "x-bench-mac:", value, sizeof(value)) && !strcmp(value, "abc"));
    CHECK(!HttpHeaderValue(answer, head.headBytes, "x-bench-mac:", value, 3));
    CHECK(!HttpHeaderValue(answer, head.headBytes, "x-other:", value, sizeof(value)));
    const char noLength[] = "HTTP/1.1 404 Not Found\r\n\r\n";
    CHECK(ParseHttpHead(noLength, sizeof(noLength) - 1, &head) == 1 && head.status == 404 && !head.hasLength);
    const char notHttp[] = "SSH-2.0-OpenSSH\r\n\r\n";
    CHECK(ParseHttpHead(notHttp, sizeof(notHttp) - 1, &head) == -1);
    const char badLength[] = "HTTP/1.0 200 OK\r\nContent-Length: 12x\r\n\r\n";
    CHECK(ParseHttpHead(badLength, sizeof(badLength) - 1, &head) == -1);

    if (g_failures)
        return 1;
    printf("vita bench: formats, SHA-256 and codes are right\n");
    return 0;
}

struct HostPlatform final : Platform
{
    bool Network() override
    {
        return !getenv("BENCH_TEST_NO_NETWORK");
    }

    void StartBuild(bool fetched) override
    {
        if (fetched && getenv("BENCH_TEST_FETCHED_DOES_NOT_START"))
            return;
        printf("NEXT %s\n", fetched ? "fetched" : "installed");
        fflush(stdout);
        _exit(fetched ? 11 : 10);
    }

    bool Wait(unsigned) override
    {
        printf("WAIT\n");
        if (getenv("BENCH_TEST_STOP_WAIT"))
            return false;
        usleep(50 * 1000);
        return true;
    }

    void Random(uint8_t *bytes, size_t count) override
    {
        FILE *file = fopen("/dev/urandom", "rb");
        if (!file || fread(bytes, 1, count, file) != count)
            abort();
        fclose(file);
    }
};

std::string ReadFile(const std::string &path)
{
    std::string text;
    if (FILE *file = fopen(path.c_str(), "rb"))
    {
        char block[4096];
        for (size_t got; (got = fread(block, 1, sizeof(block), file)) > 0;)
            text.append(block, got);
        fclose(file);
    }
    return text;
}

void WriteFile(const std::string &path, const std::string &text)
{
    FILE *file = fopen(path.c_str(), "wb");
    if (!file || fwrite(text.data(), 1, text.size(), file) != text.size())
        abort();
    fclose(file);
}

int OneStart(char **arguments)
{
    static HostPlatform platform;
    Paths paths;
    snprintf(paths.data, sizeof(paths.data), "%s", arguments[0]);
    snprintf(paths.eboot, sizeof(paths.eboot), "%s", arguments[1]);
    const bool fetched = atoi(arguments[2]) != 0, byBench = atoi(arguments[3]) != 0;
    const std::string label = arguments[4], data = std::string(arguments[0]) + "/";
    static Bench bench(paths, &platform);
    const Start start = bench.Begin(fetched, byBench);
    printf("%s", bench.Note());
    if (start != Start::OfJob)
    {
        printf("%s\n", start == Start::Quit ? "QUIT" : "ORDINARY");
        return start == Start::Quit ? 2 : 0;
    }
    // The list of starts, as RunListBegin does it.
    using namespace kisak::vita_run;
    const std::string list = ReadFile(data + paths.runList);
    const size_t index = ParseListIndex(ReadFile(data + paths.runListAt).c_str());
    static ListStart listStart;
    size_t count = 0;
    char error[128];
    if (!ParseRunList(list.c_str(), index, &listStart, &count, error, sizeof(error)) || index >= count)
    {
        printf("the list on the card is not one to run: %s\n", error);
        return 4;
    }
    if (index + 1 == count)
    {
        unlink((data + paths.runListDone).c_str());
        rename((data + paths.runList).c_str(), (data + paths.runListDone).c_str());
        unlink((data + paths.runListAt).c_str());
    }
    else
        WriteFile(data + paths.runListAt, std::to_string(index + 1) + "\n");
    WriteFile(data + paths.logStem + "-" + listStart.name + ".log",
        "start " + std::string(listStart.name) + " on " + label + ": " + listStart.commandLine + "\n");
    printf("START %s on %s\n", listStart.name, label.c_str());
    if (getenv("BENCH_TEST_START_FAULTS"))
        return 5;
    bench.End(index + 1 < count, getenv("BENCH_TEST_STOPPED") != nullptr);
    printf("EXIT\n");
    return 3;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 7 && !strcmp(argv[1], "start"))
        return OneStart(argv + 2);
    if (argc != 1)
    {
        printf("usage: bench_test [start <data> <fetched build> <fetched 0|1> <by bench 0|1> <label>]\n");
        return 64;
    }
    return Checks();
}
