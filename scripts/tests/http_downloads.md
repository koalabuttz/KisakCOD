# HTTP map/mod download checks

The game uses pinned libcurl 8.23.0 for HTTP and HTTPS redirects from game
servers. The dependency is static and uses Schannel on Windows, so it adds no
runtime DLL. Native Windows trust uses the system certificate store. Explicit
`CURL_CA_BUNDLE` or `SSL_CERT_FILE` bundles and proxy environment settings are
preserved; certificate and hostname verification stay enabled.

The downloader pumps libcurl's multi interface once per client frame. A shared
multi handle retains a bounded connection cache and resolver pool for the process
lifetime: cancelling an easy handle must not join a slow OS DNS lookup. Each
transfer still closes its easy handle and file on cancellation or completion.
Only a completed 2xx response and a successful file close return `DL_DONE`.
The client then atomically installs the temporary file before acknowledging
success. Initialization, transfer, and finalization failures restore the relative
names required by the existing UDP fallback. `cl_wwwDownload 0` is enforced
locally, including for disconnected/browser redirects.

## Host checks

Requirements: a C++20 compiler, pkg-config, libcurl development files (7.85 or
newer), Python 3, and `openssl` for ephemeral test certificates.

```sh
python3 scripts/tests/test_http_downloads.py
python3 scripts/tests/test_http_download_callers.py
```

The first fixture uses real local HTTP/TLS servers and the complete production
state machine with small game-facing file/UI/console doubles. It covers binary
and chunked bodies, progress, redirects, status errors, truncated responses,
cancellation/restart, file errors, nonblocking pumping, TLS trust/hostname
checks, HTTPS downgrade refusal, credential forwarding, and uppercase
`HTTP_PROXY`. On Linux with libcurl 8.20+, it also injects an intentionally
blocked DNS lookup and checks that cancellation returns before that lookup ends.
The supplied CMake probe uses the exact pinned dependency.

The caller fixture extracts the production parser and completion handler. It
checks reliable commands, disabled downloads, relative UDP fallback names,
disconnected downloads, final-file replacement/failure, and legacy MOTD routing.
It does not require libcurl or game assets.

## Native Windows Schannel check

From a Visual Studio development environment, with Python and `openssl` on
`PATH` (Git for Windows supplies OpenSSL):

```powershell
cmake -S scripts/tests/http -B build-http-check -G "Visual Studio 17 2022" -A Win32
cmake --build build-http-check --config Release
python scripts/tests/test_http_downloads.py --executable build-http-check/Release/kisakcod-http-runtime-check.exe --curl-version 8.23.0
```

This executes the real Schannel backend against trusted and untrusted local TLS
certificates. Linux-only `/dev/full` and resolver-interposition cases are omitted.
The native probe forces assertions on in Release as well as Debug.

`--emit-harness /path/http-runtime-check.cpp` writes the portable C++ probe
without requiring a host compiler or libcurl. `--executable` runs an existing
probe while Python owns the local servers. Tests use authored data and temporary
certificates/files; they do not download game content or contact external hosts.

The fixtures verify the HTTP subsystem and caller state transitions. Full
client/server network protocol integration and third-party server configuration
still require a game session.

The build copies curl's `COPYING` notice beside each executable as
`curl-COPYING.txt`. Retain it when redistributing the executable.
