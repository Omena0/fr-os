# NUMA Policies

## Overview

Non-Uniform Memory Access (NUMA) awareness allows the allocator to prefer physical memory from the NUMA node closest to the requesting CPU, reducing memory access latency.

On QEMU with a single virtual machine, all vCPUs typically share a single NUMA node (homogeneous memory). NUMA policy code is present and functional but is effectively a no-op on single-node QEMU configurations. It becomes active on physical multi-socket hardware or multi-node QEMU configurations.

## NUMA Topology

NUMA topology is discovered at boot via ACPI SRAT (System Resource Affinity Table):

- Each CPU is assigned to a NUMA node.
- Each physical memory range is assigned to a NUMA node.
- The PMM tracks which `struct page` belongs to which node.

```c
struct numa_node {
    uint32_t          node_id;
    cpumask_t         cpu_mask;   // CPUs local to this node
    phys_addr_t       mem_start;
    phys_addr_t       mem_end;
    struct buddy_zone zones[3];   // per-zone buddy free lists for this node
};
```

## Allocation Policy

The default allocation policy is **local first**: allocate from the NUMA node closest to the current CPU. If the local node is out of memory: fall back to the nearest neighboring node (determined by NUMA distance matrix from ACPI SLIT table).

```c
struct page *pmm_alloc_page(gfp_t flags) {
    int node = cpu_to_node(smp_processor_id());
    struct page *p = node_alloc_page(&numa_nodes[node], flags);
    if (p) return p;
    // Fallback: try other nodes in order of increasing distance
    for each node n in order of distance from current node:
        p = node_alloc_page(&numa_nodes[n], flags);
        if (p) return p;
    return NULL;  // out of memory
}
```

## Per-Process NUMA Policy

Processes may set a per-VMA NUMA policy via the `mbind()` syscall (planned):

| Policy | Behavior |
|---|---|
| `MPOL_DEFAULT` | Local node first (default) |
| `MPOL_BIND` | Allocate only from specified node set |
| `MPOL_PREFERRED` | Prefer specified node, fall back to local |
| `MPOL_INTERLEAVE` | Round-robin across specified nodes |

`mbind()` is applied per VMA. Different VMAs of the same process can have different policies.

## Thread and CPU Affinity Interaction

NUMA policy and CPU affinity interact:

- A thread pinned to CPUs on NUMA node 0 (via `sched_setaffinity`) should have its heap memory allocated from node 0.
- Mismatch (CPU on node 0, memory on node 1) is detectable via `numastat`-equivalent tracing and is reported as remote access count in performance counters.

## NUMA and SLAB

The SLAB allocator uses per-CPU magazines and a global per-cache pool. For NUMA systems, the pool is split per-node: each NUMA node has its own slab pool. Objects allocated from the SLAB on node 0 come from node 0's physical pages, reducing cross-node accesses.

## Per-CPU Page Cache and NUMA

The per-CPU page cache (see [per-cpu-caches.md](per-cpu-caches.md)) is automatically NUMA-aware: it is refilled from the local NUMA node's buddy allocator. Pages in the cache are always from the local node.

## Related Documents

- [physical-allocator.md](physical-allocator.md)
- [per-cpu-caches.md](per-cpu-caches.md)
- [buddy-allocator.md](buddy-allocator.md)
- [scheduling/cpu-affinity.md](../scheduling/cpu-affinity.md)
