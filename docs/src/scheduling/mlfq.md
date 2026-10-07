# MLFQ — Multi-Level Feedback Queue

## Design

The MLFQ scheduler dynamically adjusts thread priority based on observed CPU usage behavior. It is designed to:

- Favor interactive, I/O-bound threads (which naturally stay at high priority levels).
- Gradually demote CPU-bound threads to lower priority levels.
- Prevent starvation via an aging mechanism.
- Require no manual priority tuning for typical workloads.

## Priority Levels

The MLFQ has 8 priority levels (0–7). Level 0 is the highest priority; level 7 is the lowest.

| Level | Time Quantum | Description |
|---|---|---|
| 0 | 5 ms | Interactive / newly created threads |
| 1 | 10 ms | Lightly CPU-bound |
| 2 | 20 ms | Moderately CPU-bound |
| 3 | 40 ms | CPU-bound |
| 4 | 80 ms | Background batch work |
| 5 | 120 ms | Low-priority batch |
| 6 | 160 ms | Very low priority |
| 7 | 200 ms | Lowest priority (system idle tasks) |

Time quanta double at each level. CPU-bound threads are penalized with both lower priority and longer time between scheduling events at lower levels.

## Thread Priority Rules

1. **New thread**: Placed at level 0.
2. **Thread uses full quantum**: Demoted to the next lower level (max: level 7).
3. **Thread blocks before quantum expires** (I/O wait, mutex, etc.): Stays at the current level. Accumulated CPU time for this quantum is reset.
4. **Thread wakes from I/O**: Receives an I/O boost — stays at current level or temporarily moved up one level (see [mlfq-cpu-accounting.md](mlfq-cpu-accounting.md)).
5. **Starvation protection**: Aging periodically boosts threads that have been waiting too long (see [mlfq-aging.md](mlfq-aging.md)).

## Interaction with `nice()`

The `nice` value (range −20 to +19) modifies the effective MLFQ starting level:

- `nice` −20 → starts at level 0, demoted normally.
- `nice` 0 → starts at level 0 (default).
- `nice` +19 → starts at level 3; can still be demoted further.

`nice` does not change demotion rules — it only sets the initial level. A CPU-bound `nice -20` process will eventually be demoted just like any other.

## Scheduling Within a Level

Threads within the same MLFQ level are scheduled in round-robin order. The per-level queue is a FIFO. Newly added threads (including threads boosted from lower levels by aging) are appended to the tail.

## Gaming Prevention

Without protection, a CPU-bound process could game the MLFQ by blocking briefly before its quantum expires, staying at level 0 forever. The OS prevents this using **accumulated CPU time tracking**:

- Each thread tracks total CPU time consumed at its current level (`level_cpu_time`).
- This time is **not reset** when the thread voluntarily blocks.
- Demotion occurs when `level_cpu_time` exceeds the level's quantum, regardless of whether the thread voluntarily yielded or was preempted.

See [mlfq-cpu-accounting.md](mlfq-cpu-accounting.md).

## Configuration

MLFQ parameters are set at compile time (not runtime configurable in the initial implementation):

```c
#define MLFQ_LEVELS         8
#define MLFQ_BASE_QUANTUM   5   // ms, level 0 quantum
// Level N quantum = BASE_QUANTUM << N
```

## Related Documents

- [mlfq-priority-queues.md](mlfq-priority-queues.md)
- [mlfq-aging.md](mlfq-aging.md)
- [mlfq-cpu-accounting.md](mlfq-cpu-accounting.md)
- [overview.md](overview.md)