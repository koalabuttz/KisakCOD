#!/usr/bin/env python3
"""Run the production HTTP state machine against authored local HTTP/TLS servers.

Requires a C++20 compiler, pkg-config/libcurl development files (curl >= 7.85),
and openssl. No game assets or Internet requests are used. Game-facing file path
creation, console output and UI counters are small doubles; transfer, redirects,
TLS verification, files and cancellation use the real platform/libcurl APIs.
"""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import shlex
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time


HARNESS = r'''
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <vector>
#ifdef NDEBUG
#error The HTTP regression probe requires active assertions.
#endif
#ifndef _WIN32
#include <dlfcn.h>
#include <netdb.h>
std::atomic<bool> resolverEntered, resolverReleased;
extern "C" int getaddrinfo(const char* name, const char* service,
                            const struct addrinfo* hints, struct addrinfo** result) {
    if (name && std::strcmp(name,"blocked-dns.invalid")==0) {
        resolverEntered=true;
        while (!resolverReleased) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return EAI_NONAME;
    }
    using Lookup=int (*)(const char*,const char*,const struct addrinfo*,struct addrinfo**);
    static const auto realLookup=reinterpret_cast<Lookup>(dlsym(RTLD_NEXT,"getaddrinfo"));
    assert(realLookup); return realLookup(name,service,hints,result);
}
#endif
#ifndef _WIN32
#define __cdecl
#endif
#define KISAK_MP
constexpr int MAX_OSPATH = 260;
constexpr int CON_CHANNEL_CLIENT = 1;
enum dlStatus_t { DL_CONTINUE, DL_DONE, DL_FAILED };
struct { int cl_downloadSize, cl_downloadCount; } legacyHacks{};
std::string console;
void Com_Printf(int, const char* format, ...) {
    char buffer[1024]; va_list args; va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args); va_end(args);
    console += buffer;
}
int FS_CreatePath(char* name) {
    if (std::strstr(name, "..") || std::strstr(name, "::")) return 1;
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(name).parent_path(), error);
    return error ? 1 : 0;
}
@PRODUCTION@
namespace fs = std::filesystem;
unsigned cases;
std::string base, secure, closedPort, directory, cert, badCert;
void pass(const char* name) { ++cases; std::cout << "PASS " << name << '\n'; }
void env(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value ? value : "");
#else
    if (value) setenv(key, value, 1); else unsetenv(key);
#endif
}
std::string path(const char* name) { return directory + "/" + name + ".tmp"; }
std::string contents(const std::string& name) {
    std::ifstream file(name, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::string payload() {
    std::string result;
    for (unsigned i = 0; i < 65536; ++i) result += char(i % 251);
    return result;
}
int begin(const std::string& output, const std::string& url) {
    return DL_BeginDownload(const_cast<char*>(output.c_str()), const_cast<char*>(url.c_str()));
}
int pump() {
    assert(DL_InProgress());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (DL_InProgress() && std::chrono::steady_clock::now() < deadline) {
        const int status = DL_DownloadLoop();
        if (status != DL_CONTINUE) { assert(!DL_InProgress()); return status; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(!"download did not finish within the local test deadline");
    return DL_FAILED;
}
void success(const char* name, const std::string& url, const std::string& expected) {
    const auto output = path(name);
    assert(begin(output, url)); assert(DL_InProgress());
    assert(pump() == DL_DONE); assert(contents(output) == expected);
    DL_CancelDownload(); assert(fs::exists(output));
    fs::remove(output); pass(name);
}
void failure(const char* name, const std::string& url) {
    const auto output = path(name);
    assert(begin(output, url));
    assert(pump() == DL_FAILED); assert(!fs::exists(output));
    DL_CancelDownload(); DL_CancelDownload(); assert(!DL_InProgress());
    pass(name);
}
int main(int argc, char** argv) {
    assert(argc == 9);
    base=argv[1]; secure=argv[2]; closedPort=argv[3]; directory=argv[4];
    cert=argv[5]; badCert=argv[6]; const std::string mode=argv[7];
    const std::string expectedVersion=argv[8];
    assert(expectedVersion.empty() || expectedVersion == curl_version_info(CURLVERSION_NOW)->version);
    if (mode == "proxy") {
        success("uppercase HTTP_PROXY", "http://codex-download.invalid/payload", payload());
        return 0;
    }
    DL_CancelDownload(); DL_CancelDownload(); assert(!DL_InProgress());
    assert(DL_DownloadLoop() == DL_FAILED); assert(!DL_DLIsMotd());
    auto output=path("invalid");
    assert(!DL_BeginDownload(nullptr, nullptr));
    assert(!begin("", base + "/payload"));
    for (const auto* url : {"", "relative/file.iwd", "file:///etc/passwd", "ftp://localhost/archive.iwd", "gopher://localhost/", "http://"}) {
        assert(!begin(output, url)); assert(!DL_InProgress()); assert(!fs::exists(output));
    }
    assert(!begin(directory + "/" + std::string(260, 'x'), base + "/payload"));
    assert(!begin(directory, base + "/payload")); assert(fs::is_directory(directory));
    pass("invalid names and protocols do not create files");
    assert(DL_ProgressValue(-1)==0 && DL_ProgressValue(0)==0);
    assert(DL_ProgressValue(123)==123);
    assert(DL_ProgressValue((std::numeric_limits<curl_off_t>::max)())==(std::numeric_limits<int>::max)());
    assert(DL_Write(nullptr, (std::numeric_limits<size_t>::max)(), 2, nullptr)==0);
    pass("counter and write-size overflow bounds");

    legacyHacks.cl_downloadCount=999; legacyHacks.cl_downloadSize=1;
    success("binary HTTP download", base+"/payload", payload());
    assert(legacyHacks.cl_downloadCount==65536 && legacyHacks.cl_downloadSize==65536);
    pass("progress updates exact byte counts");
    success("case-insensitive HTTP scheme", "HTTP"+base.substr(4)+"/payload", payload());
    legacyHacks.cl_downloadSize=888;
    success("chunked body", base+"/chunked", payload());
    assert(legacyHacks.cl_downloadCount==65536 && legacyHacks.cl_downloadSize==888);
    pass("unknown content length preserves server size");
    success("redirect with a body", base+"/redirect", payload());
    success("empty 204 body", base+"/empty", "");
    failure("HTTP 404", base+"/missing");
    failure("HTTP 500", base+"/error");
    failure("short content-length body", base+"/truncated");
    failure("redirect loop bounded", base+"/loop");
    failure("redirect without location", base+"/no-location");
    failure("redirect to file scheme denied", base+"/file");
    failure("connection refused", closedPort+"/payload");
    success("URL credentials not forwarded across hosts", base.substr(0,7)+"tester:example@"+base.substr(7)+"/redirect-host", payload());
    assert(console.find("tester") == std::string::npos && console.find("example@") == std::string::npos);
    pass("download diagnostics omit URL credentials");

    output=path("cancel"); assert(begin(output, base+"/slow"));
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while (!legacyHacks.cl_downloadCount && std::chrono::steady_clock::now()<deadline) {
        assert(DL_DownloadLoop()==DL_CONTINUE);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(legacyHacks.cl_downloadCount>0 && DL_InProgress());
    DL_CancelDownload(); assert(!DL_InProgress() && !fs::exists(output));
    DL_CancelDownload(); pass("cancel removes a partial file");
    success("restart after cancel", base+"/payload", payload());
    output=path("replaced"); assert(begin(output,base+"/slow"));
    assert(begin(path("replacement"),base+"/payload"));
    assert(!fs::exists(output)); assert(pump()==DL_DONE);
    assert(contents(path("replacement"))==payload());
    pass("replacing active transfer closes prior handles");

    output=path("stall"); assert(begin(output,base+"/stall"));
    const auto start=std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now()-start<std::chrono::milliseconds(200))
        assert(DL_DownloadLoop()==DL_CONTINUE);
    const auto elapsed=std::chrono::steady_clock::now()-start;
    assert(elapsed<std::chrono::seconds(1));
    DL_CancelDownload(); assert(!fs::exists(output));
    pass("stalled response does not block the frame pump");

#ifndef _WIN32
    output=path("disk-full"); fs::create_symlink("/dev/full",output);
    assert(begin(output,base+"/tiny")); assert(pump()==DL_FAILED);
    assert(!fs::is_symlink(output)); pass("deferred file write failure is not success");
#endif
    env("CURL_CA_BUNDLE", nullptr); env("SSL_CERT_FILE", nullptr);
    failure("untrusted TLS certificate rejected",secure+"/payload");
    env("SSL_CERT_FILE",cert.c_str());
    success("trusted TLS via SSL_CERT_FILE",secure+"/payload",payload());
    auto mismatch=secure; const auto host=mismatch.find("localhost"); assert(host!=std::string::npos);
    mismatch.replace(host,9,"127.0.0.1");
    failure("TLS hostname mismatch rejected",mismatch+"/payload");
    failure("HTTPS downgrade redirect denied",secure+"/downgrade");
    success("HTTP upgrade redirect",base+"/upgrade",payload());
    env("CURL_CA_BUNDLE",badCert.c_str());
    failure("CURL_CA_BUNDLE has precedence",secure+"/payload");
    env("CURL_CA_BUNDLE",cert.c_str()); env("SSL_CERT_FILE",badCert.c_str());
    success("trusted TLS via CURL_CA_BUNDLE",secure+"/payload",payload());
#if !defined(_WIN32) && LIBCURL_VERSION_NUM >= 0x081400
    // Intercept only the authored hostname, never a real external lookup. The
    // resolver is deliberately stuck until two seconds after cancellation.
    output=path("dns-cancel"); resolverEntered=false; resolverReleased=false;
    assert(begin(output,"http://blocked-dns.invalid/payload"));
    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (!resolverEntered && std::chrono::steady_clock::now()<deadline) {
        assert(DL_DownloadLoop()==DL_CONTINUE);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(resolverEntered);
    std::thread releaseResolver([] {
        std::this_thread::sleep_for(std::chrono::seconds(2)); resolverReleased=true;
    });
    const auto cancelStart=std::chrono::steady_clock::now();
    DL_CancelDownload();
    const auto cancelElapsed=std::chrono::steady_clock::now()-cancelStart;
    releaseResolver.join();
    assert(cancelElapsed<std::chrono::milliseconds(250));
    assert(!DL_InProgress() && !fs::exists(output));
    pass("cancel does not join an active DNS resolver");
    success("transfer after DNS cancellation",base+"/payload",payload());
#endif
    DL_CancelDownload();
    std::cout << cases << " HTTP regression cases passed\n";
}
'''


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    http_base = ""
    https_base = ""
    payload = bytes(i % 251 for i in range(65536))

    def log_message(self, *_):
        pass

    def handle(self):
        try:
            super().handle()
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass  # A TLS rejection may close before the first request line.

    def do_GET(self):
        try:
            self.respond()
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            pass  # Expected when the test cancels a transfer.

    def respond(self):
        path = self.path
        if path.startswith("http://codex-download.invalid/"):
            path = "/" + path.split("/", 3)[3]
        if path == "/payload" or path == "/no-auth":
            if path == "/no-auth" and self.headers.get("Authorization"):
                self.send_error(403)
                return
            self.send_response(200)
            self.send_header("Content-Length", str(len(self.payload)))
            self.end_headers()
            self.wfile.write(self.payload)
        elif path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for offset in range(0, len(self.payload), 1024):
                block = self.payload[offset:offset+1024]
                self.wfile.write(f"{len(block):x}\r\n".encode() + block + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif path in {"/redirect", "/loop", "/file", "/no-location", "/upgrade", "/downgrade", "/redirect-host"}:
            locations = {
                "/redirect": "/payload", "/loop": "/loop", "/file": "file:///etc/passwd",
                "/upgrade": self.https_base + "/payload", "/downgrade": self.http_base + "/payload",
                "/redirect-host": self.http_base.replace("127.0.0.1", "localhost") + "/no-auth",
            }
            self.send_response(302)
            if path in locations:
                self.send_header("Location", locations[path])
            self.send_header("Content-Length", "9")
            self.end_headers()
            self.wfile.write(b"NOT A MOD")
        elif path == "/empty":
            self.send_response(204)
            self.end_headers()
        elif path == "/truncated":
            self.send_response(200)
            self.send_header("Content-Length", "1024")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"incomplete")
            self.close_connection = True
        elif path in {"/slow", "/stall"}:
            self.send_response(200)
            self.send_header("Content-Length", str(len(self.payload) * 100))
            self.end_headers()
            if path == "/stall":
                time.sleep(1.5)
                return
            for _ in range(100):
                self.wfile.write(self.payload)
                self.wfile.flush()
                time.sleep(.02)
        elif path == "/tiny":
            self.send_response(200)
            self.send_header("Content-Length", "4")
            self.end_headers()
            self.wfile.write(b"data")
        else:
            self.send_error(500 if path == "/error" else 404)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--curl-version", default="", help="Require an exact libcurl runtime version")
    parser.add_argument("--emit-harness", type=Path, help="Write portable C++ for a native/cross-built runtime probe")
    parser.add_argument("--executable", type=Path, help="Run an already built probe instead of compiling with pkg-config")
    args = parser.parse_args()
    production = (args.source_root / "src/qcommon/dl_main.cpp").read_text()
    # Keep curl/standard headers and the entire production state machine. Only
    # the engine headers are replaced by the game-facing doubles above.
    production = "\n".join(line for line in production.splitlines() if not (
        line.startswith('#include <universal/') or line.startswith('#include <ui_mp/')
        or line in ('#include "dl_main.h"', '#include "qcommon.h"')
    ))
    harness = HARNESS.replace("@PRODUCTION@", production)
    if args.emit_harness:
        args.emit_harness.parent.mkdir(parents=True, exist_ok=True)
        args.emit_harness.write_text(harness)
        return
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    required_tools = ["openssl"] if args.executable else [compiler[0], "pkg-config", "openssl"]
    for command in required_tools:
        if not shutil.which(command):
            parser.error(f"required HTTP regression tool is missing: {command}")
    if args.executable:
        flags = []
    elif os.environ.get("CURL_TEST_CFLAGS") is not None:
        flags = shlex.split(os.environ["CURL_TEST_CFLAGS"] + " " + os.environ["CURL_TEST_LIBS"])
    else:
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "libcurl"], text=True))
    with tempfile.TemporaryDirectory(prefix="kisakcod-http-") as temporary:
        root = Path(temporary)
        cert, key, bad_cert = root / "cert.pem", root / "key.pem", root / "untrusted.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
            "-keyout", str(key), "-out", str(cert),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=unrelated", "-keyout", str(root / "untrusted-key.pem"), "-out", str(bad_cert),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        source = root / "http.cpp"
        executable = args.executable.resolve() if args.executable else root / "http-check"
        source.write_text(harness)
        if not args.executable:
            subprocess.run(compiler + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread", str(source), "-o", str(executable)] + flags + ["-ldl"], check=True)
        http = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        https = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        https.socket = context.wrap_socket(https.socket, server_side=True)
        Handler.http_base = f"http://127.0.0.1:{http.server_port}"
        Handler.https_base = f"https://localhost:{https.server_port}"
        for server in (http, https):
            threading.Thread(target=server.serve_forever, daemon=True).start()
        with socket.socket() as unused:
            unused.bind(("127.0.0.1", 0))
            closed_port = unused.getsockname()[1]
        runtime_env = os.environ.copy()
        # Local fixtures only. Do not alter the parent environment or use its
        # proxy to contact a test server running on the loopback interface.
        runtime_env["NO_PROXY"] = runtime_env["no_proxy"] = "127.0.0.1,localhost,blocked-dns.invalid"
        arguments = [str(executable), Handler.http_base, Handler.https_base, f"http://127.0.0.1:{closed_port}", str(root / "files"), str(cert), str(bad_cert)]
        (root / "files").mkdir()
        try:
            subprocess.run(arguments + ["normal", args.curl_version], env=runtime_env, check=True, timeout=60)
            proxy_env = runtime_env.copy()
            proxy_env.pop("http_proxy", None)
            proxy_env["HTTP_PROXY"] = Handler.http_base
            proxy_env["NO_PROXY"] = proxy_env["no_proxy"] = ""
            subprocess.run(arguments + ["proxy", args.curl_version], env=proxy_env, check=True, timeout=15)
        finally:
            for server in (http, https):
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    main()
