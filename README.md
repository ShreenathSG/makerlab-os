# MakerLabOS

A campus workshop management website with a **C11 backend**. It applies priority scheduling with aging and Banker's Algorithm to fabrication projects that share a 3D printer, workbenches, and a technician.

Students submit a project. The workshop operator reserves resources, starts each stage, and marks it complete. The dashboard shows who holds each resource, the remaining claim, a safe completion order, and the decision history.

## Stack

- C11: HTTP server, business logic, priority scheduler, Banker safety algorithm.
- POSIX sockets and four pthread workers; a bounded connection queue protected by a mutex and condition variable.
- SQLite: projects, allocation counts, stage timestamps, event history, JSON request validation.
- HTML, CSS, vanilla JavaScript: responsive browser dashboard, no frontend build step.
- Dockerfile and Compose: prepared for the container deployment stage.

Python is used **only for black-box integration tests**, not for the application server or OS algorithms. There are no pip or npm runtime dependencies.

## Run locally

Requirements: a C compiler, Make, and SQLite development headers/library with JSON functions (SQLite 3.38+ is recommended). macOS Xcode Command Line Tools supply the local build tools. On Debian/Ubuntu:

```sh
sudo apt-get update
sudo apt-get install build-essential libsqlite3-dev
```

From this repository's root:

```sh
make
./build/makerlab
```

Open **http://127.0.0.1:8080**. If that port is occupied:

```sh
./build/makerlab --port 8086
```

The database is created at `data/makerlab-c.db`. Restarting the server preserves projects and event history. Stop with Ctrl+C. Run from the repository root so the default `static/` directory is found. An alternate database's parent directory must already exist:

```sh
./build/makerlab --port 8090 --db /tmp/makerlab-demo.db --static static
```

Windows: use WSL with the Debian/Ubuntu commands above, or Docker Desktop. The native implementation uses POSIX sockets/pthreads; MSVC is not supported.

## Workflow

| Stage | Maximum resource claim [Printer, Bench, Technician] |
| --- | --- |
| Preparation | [0, 1, 1] |
| Fabrication | [1, 0, 1] |
| Inspection | [0, 1, 1] |

Capacity is **[1, 2, 1]**. Each project has one active stage at a time.

1. Choose **New project**, enter a title/team and priority.
2. Click **Dispatch next**. Each click reserves at most one resource unit safely.
3. Once all required units are held, the job becomes **Ready**.
4. Click **Start stage**, then **Complete stage** when the work is done.
5. Completion releases every held unit and queues the next stage. Inspection completion closes the project.

State transitions: `Waiting → Reserved → Ready → Running → Waiting (next stage) / Completed`. Cancellation releases all held resources. Completed/cancelled projects remain in History. Service is manual, non-preemptive, and not timed automatically.

## Reproduce the safety demonstration

Use an empty active queue and click **Load safety demo**. The demo intentionally starts two projects at Fabrication; Preparation is skipped for this teaching fixture.

- J001 has priority 2 and already holds the only printer: allocation [1, 0, 0].
- J002 has priority 9 and holds nothing: allocation [0, 0, 0].
- Both have a maximum claim [1, 0, 1]. The technician is free.

Click **Dispatch next**. The dispatcher considers J002 first. Granting it the technician would leave J001 waiting for the technician and J002 waiting for the printer. The tentative state has no safe completion order, so the request is **rejected and logged**. The dispatcher then grants the technician to J001. The safe order is J001, then J002.

If there are previous records, IDs will differ. Loading the demo never deletes existing history and is refused while any projects are active. The trace shows deadlock **avoidance**: the unsafe allocation is never committed.

## OS concepts and implementation

| Concept | Implementation |
| --- | --- |
| Logical jobs and states | Each current project stage has a claim, allocation, priority, and lifecycle state. |
| Priority with aging | `effective = base + floor(max(0, now - queued_at) / 60)`. Ties use queue time, then ID. Aging resets at each new stage. |
| Banker's Algorithm | Validate tentative request against remaining claim/availability; copy available units into Work; repeatedly find a stage whose Need ≤ Work; release its hypothetical allocation; grant only if all stages can finish. |
| Shared-resource synchronization | Allocation reads, safety checks, writes, and event logging occur inside one SQLite `BEGIN IMMEDIATE` transaction. Competing writers serialize. |
| Actual OS threads | Four pthreads handle requests; a mutex and condition variable coordinate the 64-slot producer/consumer connection queue. |
| Process and file I/O | The OS runs the C server process; sockets carry HTTP and SQLite persists data in files. |

The Banker core is isolated in `src/core.c` and can be tested independently of HTTP and SQLite. The UI reads projects, matrices, and history inside one database read transaction for a consistent snapshot.

The application's jobs are **not kernel processes**. Application priority does not change OS thread priority. Resource occupancy is not CPU utilization. A safe order proves a possible completion order for current stage claims, assuming operators eventually complete or cancel jobs. Aging improves priority competition but does not guarantee a maximum wait.

## Tests

```sh
make test
```

This runs the C core tests plus Python standard-library HTTP tests that launch the **compiled C server** with disposable databases. See [TEST_REPORT.md](TEST_REPORT.md) for actual verification and limitations.

## Docker (prepared for the next stage)

```sh
docker compose up --build -d
```

Open http://127.0.0.1:8080. The container runs the C executable as an unprivileged user, with a named volume for SQLite and a health check. The multi-stage Dockerfile compiles C and runs the C core tests during the image build. The runtime image does not use Python.

```sh
docker compose logs -f
docker compose down
```

`docker compose down` keeps the named data volume. This version includes Docker configuration; a successful container deployment has not been verified in the development environment.

## Project layout

```text
src/core.h, core.c     Banker safety and priority scheduling
src/server.c           HTTP, pthread workers, SQLite, workflow
static/                Browser interface
tests/core_test.c      C algorithm tests and exhaustive oracle
 tests/integration.py  Black-box tests of the compiled server
Makefile               Build, run, test, clean
Dockerfile             C build and minimal runtime
compose.yaml           Local port, persistent database volume
```

## Scope

This is a local, single-operator educational application with at most 100 active projects. There is no login/role system, physical machine control, calendar booking, or real-time deadline guarantee. Keep it local: the server accepts localhost Host headers, the default listener is loopback, and Compose publishes only on loopback. Public hosting would require authentication, TLS, a production HTTP stack and further security/load testing. GitHub stores the source; GitHub Pages cannot execute this C backend.

## References

- Silberschatz, Galvin and Gagne, *Operating System Concepts*, 10th ed.: scheduling, synchronization, deadlock avoidance.
- [SQLite transactions](https://www.sqlite.org/lang_transaction.html): write serialization and read snapshots.
- [SQLite JSON functions](https://www.sqlite.org/json1.html): request parsing and validation.
- [POSIX pthreads](https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/pthread.h.html): worker synchronization primitives.
