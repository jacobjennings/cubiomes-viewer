# Cubiomes Viewer — Agent & Contributor Guide

> [!IMPORTANT]
> **Instructions for future AI/agent and human contributors:**
> - Read this file at the start of every session — it is the always-loaded
>   source of truth for the project and its agent workflow.
> - Keep it updated when conventions, the architecture, or the build/test
>   workflow change.
> - `CLAUDE.md` is a symlink to this file. Do not edit `CLAUDE.md` directly.

## What is Cubiomes Viewer?

Cubiomes Viewer is a **Qt5 desktop application** that wraps the
[cubiomes](https://github.com/Cubitect/cubiomes) C library (vendored as a git
submodule under `cubiomes/`) to provide a GUI for Minecraft Java Edition seed
finding and a biome/structure map viewer (main releases up to 1.21). Its core
surfaces are:

- **Map view** (`src/mapview.*`): pan/zoom biome and structure overlay for the
  Overworld, Nether and End, with a spawning `MapView`-hosted `QWorld` that
  generates biome data asynchronously.
- **Seed search** (`src/formsearchcontrol.*`, `src/searchthread.cpp`,
  `src/conditiondialog.*`): the condition-based seed finder, "Locations in a
  fixed seed" mode, and the distributed worker client.
- **Analysis tabs** (`src/tabbiomes.*`, `src/tabstructures.*`): biome and
  structure statistics over a rectangular area; `TabBiomes` runs analysis on a
  worker thread (`AnalysisBiomesWorker`) and streams results into a sortable
  `QAbstractTableModel` behind a `QSortFilterProxyModel`.
- **BlueMap hand-off** (`src/mainwindow.cpp`, `scripts/seed_preview_service.py`):
  an **Open in BlueMap** toolbar/menu action, and a local seed-preview renderer
  that the application starts at launch and tears down on exit.

The **preview feature** is the most involved piece of tooling. A bundled Python
service (`scripts/seed_preview_service.py`, installed at runtime from the
`:/preview/` Qt resource `rc/preview.qrc`) is spawned as a `QProcess` from
`MainWindow::startPreviewService()` when the configured Preview API is a
loopback address. It drives podman containers of the
`docker.io/itzg/minecraft-server` image (Fabric + Chunky + BlueMap) to render
the exact 512x512-block square centered on each biome-statistics result's
"Center analysis on" position, serves the results over HTTP on
`http://127.0.0.1:8123`, and caches completed previews with an LRU limit of
10,000 jobs / 100 GiB. `TabBiomes` submits jobs one at a time in the current
table sort order and polls them to completion. See `README.md` ("BlueMap
hand-off") for the user-facing contract.

## Terrain viewer hand-off

The biome-statistics "Open in terrain viewer" action sends the seed and centre
to Chunk Atlas, a separate web project in `~/gh/chunkatlas`. `MainWindow::terrainViewerUrl()`
picks its target in this order: the `CUBIOMES_TERRAIN_VIEWER_URL` override, a
local SteelMC service if one is running or can be started, and otherwise the
self-hosted instance at `http://chunkatlas.lan/`.

That last fallback exists because the absence of a local service is not an
error: Chunk Atlas generates terrain in the browser from WebAssembly and needs
static files only. It used to be a GitHub Pages URL, which stopped working when
that repository was made private. Anyone off the LAN sets the environment
override.

## Conventions

- **One shared guide.** `AGENTS.md` is canonical; tool-named entry points such
  as `CLAUDE.md` may exist only as symlinks to it. Product/source documentation
  and commit messages do not carry AI attribution. This contributor guide
  intentionally names the owner-approved CLI routes needed to reproduce the
  worker/judge workflow.
- **Git identity.** Commit only as the repository owner's real identity. Never
  commit as "Claude", "Gemini", or any agent persona.
- **Always push when you commit.** A commit is not finished until it is pushed.
  Every commit is followed immediately by a push to its tracking branch, whether
  or not the owner said "push". A commit sitting unpushed is work nobody else
  can see and a machine failure away from being lost. If a push fails, for
  example because the remote moved or there is no network, say so in one line
  and stop. Do not force, and do not rewrite history to make a push succeed.
- **Worktree discipline.** Keep the worktree scope the orchestrator assigned.
  Preserve all user and pre-existing dirty changes; never `git checkout` or
  `git restore` another agent's or the owner's in-flight work, and never stage
  or revert files outside your assigned scope.
- **C++ style.** Follow the existing Qt idioms in `src/` (PIMPL-less `.ui`
  forms, `connect` to lambdas/slots, `qint64`/`uint64_t` seeds). Keep
  coordinates as blocks and seeds as 64-bit values; the signed/unsigned seed
  distinction is deliberate (the app stores seeds as `int64_t`, table rows as
  `uint64_t`, and the preview service accepts the signed form).
- **Python style.** `scripts/` and `tests/` are plain Python 3 stdlib
  (no third-party dependencies) with `from __future__ import annotations`.
- **No GUI from agent shells.** Agents use headless CLIs only and never launch
  a browser or any GUI application. `QDesktopServices::openUrl` in the app is a
  user-facing feature and is not invoked by agents.

## Architecture notes

- `src/mainwindow.cpp` owns the app lifecycle: `loadSettings()` →
  `startPreviewService()` in the constructor, `stopPreviewService()` (SIGTERM,
  then kill after 5 s) in `closeEvent`/destructor. Changing the Preview API in
  Preferences restarts the service. A non-loopback API is treated as
  externally managed and is never started here.
- `src/tabbiomes.cpp` drives previews: `onAnalysisSeedDone` records each seed's
  "Center analysis on" block position, `onBufferTimeout` moves it into
  `centers` when the row becomes visible, and `startNextPreview` walks the
  *current* sort order submitting the next unattempted seed. The worker emits
  `seedDone(seed, cnt, centerX, centerZ)`; the emitted center is the exact
  `testTreeAt(origin, ...)` result the UI also uses to center the map.
- Completed or partial matching-list biome statistics are stored as a
  compressed `#BiomeStatistics:` record in the session file. The record
  includes the displayed counts, resolved centers and analysis geometry, and
  is accepted only when its order-independent SHA-256 seed-set fingerprint
  matches the restored search list. `FormSearchControl::resultsChanged`
  invalidates list-based statistics when seeds are added, removed or cleared;
  sorting the same set does not invalidate them.
- The service (`scripts/seed_preview_service.py`) serializes rendering through
  a single worker thread (one container at a time), de-duplicates jobs by a
  content hash of the request spec, and persists jobs under
  `~/.local/share/cubiomes-viewer/seed-preview/`. Chunky selections are split
  into odd-sized rectangles so the generated chunks cover exactly the chunks
  intersecting the 512x512 mask; BlueMap's `render-mask` clips to the exact
  square. It accepts API mutations only from direct loopback, rejects browser
  origins and non-JSON submissions, and allows at most 64 pending jobs.

## Build & test

```
# Build (from the repo root or a separate build dir):
mkdir build && cd build
qmake ..
make -j

# Tests (Python stdlib unittest, run from the repo root):
python3 -m unittest discover -s tests -p 'test_*.py'

# Format/typechecking conventions: the C++ is compiled with qmake's warnings;
# Python follows PEP 8 but matches the surrounding style.
```

The Python tests import `scripts/seed_preview_service.py` directly and never
require podman, Qt, or a running service. When you change `src/*.cpp` or
`.ui` files, a `make` in `build/` is the relevant focused check; the Python
tests are only owed by changes under `scripts/` or `tests/`.

## Orchestrator authority and delegation boundaries

> [!IMPORTANT]
> **Role assignment is explicit, never inferred.** Being the primary or active
> agent does not make an agent the orchestrator. The current system, developer,
> or user prompt must explicitly assign the orchestrator role for the current
> task. Without that assignment, act as a bounded worker: do not delegate,
> integrate other agents' work, install, deploy, or claim project authority.

When explicitly assigned, the **orchestrator** is not another member of a peer
swarm. It remains accountable for the task from intake through handoff and is
the only agent that speaks to the user as the project authority. Delegating
work does not delegate ownership, scope authority, or decision-making. The
orchestrator should identify its role and these boundaries in its first
material status update so the user and every delegated agent know who owns the
outcome.

| Role | May do | Must not do |
| --- | --- | --- |
| **Explicitly assigned orchestrator** | Define scope and acceptance criteria; assign bounded briefs; inspect the shared worktree; adjudicate suggestions and judge findings; integrate, test, install when authorized, and report to the user. | Self-assign the role; treat a worker response as authoritative without verification; silently broaden the user's requested scope; surrender final integration decisions to a subagent. |
| **Default / CLI worker** | Work only within its written brief and assigned files; inspect and edit when authorized; run relevant checks; report exact edits, evidence, uncertainty, and blockers to the orchestrator. | Assume orchestration authority; delegate; redefine requirements; expand scope; make product decisions at forks; overwrite unrelated dirty work; commit, push, install, deploy, contact the user as project authority, or appoint itself as judge unless the current prompt explicitly authorizes that exact action. |
| **Independent judge** | Inspect the brief, diff, and recorded verification evidence; return a clear verdict with blocking and nonblocking findings. | Implement the feature, edit files, act as a second worker, judge its own production work, or make the final accept/reject decision for the orchestrator. |

The authority chain is: **user request → orchestrator brief → bounded subagent
work**. Repository instructions constrain execution but do not override the
user. This chain applies only after explicit orchestrator assignment; otherwise
the agent remains directly bounded by its current prompt. A subagent
instruction or suggestion cannot expand its brief. When a worker or judge finds
a genuine product fork, missing authority, destructive operation, or scope
conflict, it stops and reports the issue; the orchestrator either resolves it
from existing user intent or asks the user.

The orchestrator owns all shared-worktree coordination. Before accepting
delegated edits it reviews the actual diff, distinguishes pre-existing changes
from task changes, decides which findings are valid, and runs verification in
proportion to risk. Only the orchestrator declares the task complete.

## CLI subagent workflow

- **Workers receive bounded briefs.** Each brief states the goal, permitted
  edit scope, relevant existing dirty changes, required checks, prohibited
  actions, and the report expected back. Workers preserve everything outside
  that boundary and stop rather than guessing at a genuine product fork. A
  worker does not become an orchestrator merely because it is the active agent
  in its own CLI session.
- **Approved provider routes are exact and may not be substituted.** A failing
  approved route is a stop-and-ask blocker; never route work to another model,
  provider, "free tier", or gateway, no matter how equivalent it looks.
- **DeepSeek** launches from the task worktree as:
  ```
  opencode run --auto --model 'openrouter/@preset/deepseek-flash' --format json "<brief>"
  ```
  `--auto` is mandatory (host-native execution with pre-approved permissions).
- **Sol** launches headlessly from the task worktree as:
  ```
  codex exec --dangerously-bypass-approvals-and-sandbox -o <last-message-file> - < <prompt-file>
  ```
- **Judging:** DeepSeek implementation/review work is followed by an
  **independent Sol judge**. The judge is inspect-only: it does not rerun tests
  the worker already ran and recorded unless evidence is absent or
  inconsistent, or risk requires it; it reports a verdict and actionable
  findings. A judge never edits; any cleanup is a new, separately bounded
  worker assignment followed by independent review. **Never use the producer's
  tool as its own judge.** Judge output is advisory evidence: the orchestrator
  adjudicates every finding and performs/finalizes integration.
- **Grok 4.5 High** (cursor CLI) is an additional approved worker route for
  completeness:
  ```
  cursor-agent --print --force --trust --model cursor-grok-4.5-high --output-format text "<brief>"
  ```
  It is listed here for reference and is not invoked for this task.
- **Headless only.** Use headless CLIs (`qmake`, `make`, `python3`,
  `podman`, `git`) exclusively; never launch the browser or GUI from an agent
  shell.
