# Kernel Panic System

## Purpose

A kernel panic is a non-recoverable error condition where the kernel determines that it cannot safely continue execution. The panic system:

1. Emits a diagnostic message with the error description, location, and register state.
2. Dumps the call stack (backtrace).
3. Optionally dumps additional diagnostic data (memory map, scheduler state).
4. Halts the current CPU.

## Panic Entry Points

```c
// Primary panic entry — format string + args
void kpanic(const char *fmt, ...) __attribute__((noreturn));

// Assert-style macro — panics if condition is false
#define KASSERT(cond) \
    do { if (!(cond)) kpanic("Assertion failed: %s at %s:%d", #cond, __FILE__, __LINE__); } while(0)

// Explicit BUG marker — used when reaching unreachable code
#define BUG() kpanic("BUG at %s:%d", __FILE__, __LINE__)
```

## Panic Sequence

1. **Disable interrupts** on the current CPU (`cli`).
2. **Emit panic header** to all active log sinks (serial, framebuffer):

   ```
   *** KERNEL PANIC ***
   <message>
   CPU: <n>  RIP: <addr> (<symbol+offset>)
   RAX: <val>  RBX: <val>  RCX: <val>  RDX: <val>
   RSI: <val>  RDI: <val>  RBP: <val>  RSP: <val>
   R8:  <val>  R9:  <val>  R10: <val>  R11: <val>
   R12: <val>  R13: <val>  R14: <val>  R15: <val>
   RFLAGS: <val>  CR2: <val>  CR3: <val>
   ```

3. **Print backtrace**: Walk the stack using RBP frame pointer chain, resolve addresses to symbol names using the kernel symbol table embedded at build time.
4. **Print memory zone summary**: Used/free pages per zone.
5. **Halt**: Execute `hlt` in an infinite loop.

**Note:** There is **no IPI broadcast** to halt other CPUs, and **no per-CPU state dumps** from secondary CPUs. On multiprocessor, secondary CPUs continue running until they hit their own fault or the system is externally reset. This is a known limitation (see finding 7.10 in MEGA_AUDIT.md).

## Symbol Resolution

The kernel binary is linked with a symbol table embedded in `.ksymtab_strings` and `.ksymtab`. During panic, `ksym_lookup(addr)` performs a binary search on the symbol table to find the nearest symbol name and offset from the panic address.

This requires the kernel to be linked with at least function-level symbols (no `-s` strip flag).

## Serial Output Priority

Panic output is written directly to COM1 using the raw port I/O path, bypassing the normal logging system (which may require memory allocation or scheduler access — both unsafe during panic).

## Framebuffer Output

If a framebuffer was configured at boot, the panic message is also rendered to the screen in a distinctive format (white text on red background) using the kernel's emergency framebuffer write path (no driver dependency).

## Related Documents

- [logging.md](logging.md)
- [kernel/interrupt-handling.md](interrupt-handling.md)
- [debugging/serial-diagnostics.md](../debugging/serial-diagnostics.md)
