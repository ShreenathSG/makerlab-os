# Verification record

Date: 2 November 2026. Platform: macOS arm64, Apple Clang 21, system SQLite.

- C11 build with `-Wall -Wextra -Wpedantic`: passed without compiler warnings.
- C safety engine: known safe/unsafe cases, invalid allocations/dimensions, empty state, priority aging/ties, and saturation: passed.
- 1,000 deterministic randomized small matrices checked against an independent exhaustive completion-order search: passed.
- 10 black-box HTTP integration tests against the compiled C executable: passed. These cover the full three-stage workflow, persisted state after restart, unsafe-request rejection, lower-priority safe grant, resource release, invalid transitions, active-capacity limit, concurrent dispatch (32 calls from 12 clients), actual aging order, input validation/escaping, cross-origin rejection, static assets, malformed JSON, and duplicate Content-Length rejection.
- Browser: dashboard connects to C; loading the safety demo and dispatching visibly logs the unsafe J002 request, grants the technician to J001, and displays [1,0,1] held with safe order J001 → J002.

The test harness is Python; the running application and algorithms are C. Test databases are temporary and do not alter the demonstration database.

Docker configuration is included but a container image build/run has not been verified. No physical workshop or performance improvement is claimed. Tests exercise classroom scenarios, not production-grade HTTP robustness, sustained endurance, or real-time response guarantees.
