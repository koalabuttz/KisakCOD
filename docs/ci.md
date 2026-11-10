# What hosted CI runs, and when

Hosted runners cost this public repository nothing, but the account runs about
20 jobs at a time (5 on macOS). One full run is about 43 jobs and 150 runner
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
| PS Vita workflow | dedicated server and client, Debug and RelWithDebInfo, and the port's host tests | the same |
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
  skips the PS Vita port's sources. The filters look at the whole pull request,
  not the last push.
- **PS Vita.** The port's four build jobs and its host tests are their own
  workflow (`build-kisakcod-vita.yaml`; they were part of the Linux one). A
  change to the port's own files only (`src/vita`, `src/gfx_gxm`,
  `scripts/platform/vita`, `tests/vita_*`, `scripts/tests/test_vita_*.py`,
  `scripts/ci/install-vitasdk.sh`) starts that workflow alone: four jobs
  where a push used to start about thirty Linux jobs that neither compile nor
  read those files. A change to shared source starts both. The `.vpk`
  artifacts keep their names; fetch them from the run of the **PS Vita server
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
  the reach is not known (a CMake file, the tools, their allow lists or the
  Linux workflow changed, or a changed header is named in an `#include` but in
  no compiled unit's list), and always on master.
- **Newer pushes.** A newer push to a pull request cancels that pull request's
  older run. A master run is not cancelled once it is in progress: later merges
  wait behind it, and of those only the newest is kept, so master runs neither
  pile up nor get cut off (on 2026-11-10, with a merge every twenty minutes and
  an hour's queue, five master runs in a row were cancelled before they
  finished). The PS Vita workflow goes further: every master run of it
  finishes, so each merge that changes the port keeps its vpk for console runs.
- **One runner for a quick pair.** The account's runners go to the waiting
  runs in turn, so under load a push finishes later for every job it has, even
  a one-minute job (measured on 2026-11-10: 27 Linux jobs, 16 of them under
  2.5 minutes, each waiting 12 to 77 minutes for a runner). The free camera,
  world surface and world front-end checks and the aarch64 and armv7
  dedicated servers therefore run Debug and RelWithDebInfo in one job each,
  one after the other (`scripts/ci/each-config.sh`, tested by
  `test_ci_each_config.py`): 22 Linux jobs on a pull request instead of 27.
  The second configuration still runs when the first fails, and either
  failure fails the job. A new check that takes a minute or two per
  configuration should do the same instead of adding a matrix; a long build
  keeps its matrix, since there one runner per configuration saves time.

Decided by the project owner on 2026-11-10 (drafts, Windows ARM64, Intel macOS)
and 2026-11-03 (SDL2 after a merge, path filters, cancelling). Both build
configurations stay on every pull request.
