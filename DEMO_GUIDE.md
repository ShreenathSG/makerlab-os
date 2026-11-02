# Five-minute demonstration

1. Build with `make`, run `./build/makerlab`, and open http://127.0.0.1:8080.
2. Explain the problem: workshop projects share scarce equipment and a technician. Greedy grants can split resources between jobs that cannot finish.
3. Load the safety demo, then point to Max, Held, and Need. Explain that each vector is [Printer, Bench, Technician].
4. Dispatch once. Show the Unsafe event for the higher-priority job and the Granted event for the job holding the printer. Explain the hypothetical deadlock that was avoided.
5. Start and complete the ready Fabrication stage. Show that the printer and technician return to the resource pool and the project enters Inspection.
6. Use Dispatch twice as needed to reserve a complete claim, Start, then Complete. Finish both demo projects.
7. Add a fresh project to demonstrate all three stages from Preparation.
8. Open `src/core.c` to explain Work, Finish, Need, and the safety loop. Explain priority aging and tie-breaking separately.
9. Open `src/server.c` to show four pthread workers, the mutex/condition-variable queue, and the transaction around allocation changes.
10. Run `make test`. Explain that the independent exhaustive checker is a test oracle for 1,000 small states, not a proof of every possible state.

Useful viva distinctions:

- Safe state means at least one possible completion sequence; it does not mean all jobs can run simultaneously.
- Priority determines consideration order; safety can override a higher-priority request.
- The stage model is an application-level analogy. The host OS schedules the real server threads.
- A mutex protects the incoming-connection queue; SQLite transactions protect persisted allocation decisions.
- Docker packages the executable and dependencies. It shares the host kernel (or the Linux VM kernel on desktop Docker), rather than implementing a new operating system.
