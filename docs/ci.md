# What hosted CI runs, and when

Hosted runners cost this public repository nothing, but the account runs about
20 jobs at a time (5 on macOS). One full run is about 36 jobs and 150 runner
minutes (Linux 74, Windows 43, macOS 30), so with a dozen pull requests open
the checks of each wait for the others. The rules below keep a pull request's
run to what is likely to tell something new, and leave the full run to master.
`scripts/tests/test_linux_window_workflow.py` (`CiTriggerTests`) pins every one
of them.

| | Pull request | After a merge to master, and manual runs |
| --- | --- | --- |
| Draft pull request | nothing runs | |
| Linux workflow | every job; the window jobs on SDL3 only | every job, SDL2 and SDL3 |
| Windows | Win32 Debug and Release, Vulkan probe, regression checks | also MSVC x64 and MSVC ARM64 |
| macOS | Apple silicon (arm64) | also Intel (x86_64) |
| PS Vita workflow | dedicated server and client, each one job building Debug and RelWithDebInfo, and the port's host tests | the same |
| Upstream patch queue | only when `docs/upstream` or its tool changes: the patches apply, and the patched upstream tree builds with MSVC | the same |

- **Drafts.** No job runs while a pull request is a draft. Marking it ready for
  review starts the checks, and so does every later push. Open a pull request
  as a draft while its work is not ready to be checked, and mark it ready when
  it is; a draft cannot be merged on hosted evidence, because it has none.
- **After-merge platforms.** Windows ARM64, Windows x64 and Intel macOS build
  the same sources as their pull-request neighbours and rarely break alone, so
  they run once per merge. A break there shows on master after the merge:
  whoever merged looks at master's run, and a failure is fixed forward in a new
  pull request. A manual run of a workflow on a branch (`workflow_dispatch`)
  runs everything, for a change that is about one of those platforms.
- **Paths.** A workflow starts only when a changed path can reach it. The
  Windows workflow skips documentation, the Linux, macOS and PS Vita ports' own
  sources and tests, and the test machines' run scripts; the macOS workflow
  skips the PS Vita port's sources. The test machines' run scripts
  (`scripts/runs`, `tests/runs`, `test_perf_run.py`, `test_vita3k_runner.py`)
  start only the one-job **Machine run scripts** workflow (`run-scripts.yaml`);
  on 2026-11-10 a PowerShell-only change had started 26 Linux and PS Vita
  jobs. The filters look at the whole pull request, not the last push.
- **PS Vita.** The port's two build jobs and its host tests are their own
  workflow (`build-kisakcod-vita.yaml`; they were part of the Linux one). A
  change to the port's own files only (`src/vita`, `src/gfx_gxm`,
  `scripts/platform/vita`, `tests/vita_*`, `scripts/tests/test_vita_*.py`,
  `scripts/ci/install-vitasdk.sh`) starts that workflow alone: two jobs
  where a push used to start about thirty Linux jobs that neither compile nor
  read those files. A change to shared source starts both. Each job builds
  Debug and RelWithDebInfo (below) and uploads the RelWithDebInfo `.vpk`; the
  artifacts keep their names (`KisakCOD-dedi-vita-RelWithDebInfo`,
  `KisakCOD-mp-vita-RelWithDebInfo`); fetch them from the run of the **PS Vita server
  and client** workflow (`gh run list --workflow build-kisakcod-vita.yaml`).
  A new host test for the port goes in the Vita client job, and a file a
  Linux job needs must not sit under those paths; `CiTriggerTests` checks
  both.
- **Source scans.** The x86_64 client job's two whole-tree scans
  (`check_32bit_size_literals.py`, `check_32bit_layout_offsets.py`) parse every
  translation unit twice with libclang, about eight minutes each. On a pull
  request they scan only the units the change can reach (`--changed-since
  HEAD^1`, `scripts/tools/changed_units.py`): those whose dependency list in
  the job's own Ninja build holds a changed file. They scan everything when
  the reach is not known (a CMake file, the tools or their allow lists
  changed, a changed header is named in an `#include` but in no compiled
  unit's list, or the Linux workflow changed in a way that can alter the
  scanned build), and always on master. A workflow change counts only in its
  top-level settings other than the triggers, the concurrency group and the
  permissions, and in a job that runs one of the scans; a change to the
  triggers or to any other job does not (`changed_units.py`,
  `WorkflowTests`). On 2026-11-10 PR 213 changed neither, yet the old rule
  ran both scans over the whole tree and its x86_64 client job took 45
  minutes instead of about 10.
- **Newer pushes.** A newer push to a pull request cancels that pull request's
  older run. A master run is not cancelled once it is in progress: later merges
  wait behind it, and of those only the newest is kept, so master runs neither
  pile up nor get cut off (on 2026-11-10, with a merge every twenty minutes and
  an hour's queue, five master runs in a row were cancelled before they
  finished). The PS Vita workflow goes further: every master run of it
  finishes, so each merge that changes the port keeps its vpk for console runs.
- **One runner for both configurations.** The account's runners go to the
  waiting runs in turn, so under load a push finishes later for every job it
  has, even a one-minute job (measured on 2026-11-10: 27 Linux jobs, 16 of them
  under 2.5 minutes, each waiting 12 to 77 minutes for a runner). So every
  Linux job that took a few minutes per configuration builds and tests Debug
  and then RelWithDebInfo itself: the SDL window, the native D3D9 boundary,
  the D3D9 device probe, the Vulkan window probe, the free camera, world
  surface and world front-end checks, the x86_64, aarch64 and armv7
  dedicated servers, and the PS Vita server and client. A pull request starts 17 Linux jobs, where it started 27.
  Each step that depends on the configuration hands its commands to
  `scripts/ci/each-config.sh` (tested by `test_ci_each_config.py`), which runs
  them for both with `$config` set, keeps going when the first fails, and fails
  the step when either did. A failed step stops the job's later steps, for
  both configurations; the first failure is the one to fix. Steps that do not
  depend on the configuration (installs, fetches, contract tests) now run once.
  A step that was Debug only sets `config=Debug` itself. The native Linux build
  and the long client and census jobs keep one runner per configuration, since
  there two runners save more time than they wait. A new check that takes a
  minute or two per configuration should use the helper instead of a matrix,
  and `assert_both_configurations_in_one_job` in
  `test_linux_window_workflow.py` pins each such job. The macOS dedicated server does the same, one
  job per architecture: GitHub runs five macOS jobs at a time and on 2026-11-10 never started a third of
  the `macos-26` jobs (the Intel pool lost one in twenty; `macos-15` for arm64 was no better in a trial,
  PR 221, closed), so every macOS job saved is one fewer to wait for. A cancelled macOS arm64 job that has no
  log never ran: it is no result, not a failure.
- **Package installs.** Every Linux job installs through `scripts/ci/apt-install.sh`: Ubuntu's main
  archive instead of the runner's slow Azure mirror, packages cached between runs, and the refresh and
  install tried again (up to four attempts, waiting 20, 40 and 60 s) when they fail, since on 2026-11-11
  hosted runners lost name lookups for a minute and six jobs of one run died at `apt-get install` with
  exit 100. A later outage (2026-11-11, about 15:00 to 16:00 UTC) kept `archive.ubuntu.com` and
  `security.ubuntu.com` unresolvable for longer than the four attempts, so the runner's Azure mirror
  stays in `apt-mirrors.txt` as the last choice. That did not help: the run on that commit showed some
  runners resolving no ubuntu.com name at all (the Azure mirror included) while other runners of the same
  run installed fine. So when `archive.ubuntu.com` does not resolve, `apt-install.sh` points the default
  network link at public resolvers (1.1.1.1, 8.8.8.8) before each attempt. Whether that works on a hosted
  runner is unverified until a runner with broken lookups logs "resolves through public resolvers".
  `test_ci_apt_install.py` pins it.

Decided by the project owner on 2026-11-10 (drafts, Windows ARM64, Intel macOS)
and 2026-11-03 (SDL2 after a merge, path filters, cancelling). Both build
configurations stay on every pull request.
