# QBox

SystemC/QEMU co-simulation framework for virtual platform
development.

## Overview

QBox integrates QEMU into SystemC as a TLM-2.0 model, enabling
cycle-approximate simulation of complete hardware platforms. The
project consists of:

- **libqemu-cxx** -- C++ wrapper around QEMU
- **libqbox** -- SystemC TLM-2.0 integration layer for QEMU
- **Base components** -- SystemC models (routers, memories,
  loaders, exclusive monitor)
- **QEMU device models** -- CPUs, interrupt controllers, UARTs,
  timers, PCI devices, and more
- **Example platforms** -- Reference implementations in `platforms/`

### Supported Architectures

- **ARM:** Cortex-A53/A55/A76/A710, Cortex-M7/M55,
  Cortex-R5/R52, Neoverse-N1/N2
- **RISC-V:** riscv32, riscv64, SiFive X280
- **Hexagon:** Hexagon DSP

## Requirements

Install dependencies using the provided script (supports Ubuntu
20.04/22.04/24.04 and macOS 14/15):

```bash
sudo scripts/install_dependencies.sh
```

## Building

QBox uses [CMake Presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html)
(requires CMake 3.21+) to provide ready-made build configurations:

```bash
cmake --preset gcc
cmake --build --preset gcc --parallel
ctest --preset gcc
```

### Available Presets

| Preset | Compiler | Build Type | Notes |
|--------|----------|------------|-------|
| `gcc` | GCC | Release | Default preset |
| `gcc-debug` | GCC | Debug | |
| `clang` | Clang | Release | Uses `cmake/clang-toolchain.cmake` |
| `clang-debug` | Clang | Debug | Enables `MALLOC_CHECK_=3` |
| `clang-lto` | Clang | Release | Link-time optimization with lld |
| `mac` | Apple Clang | Release | macOS builds |
| `mac-debug` | Apple Clang | Debug | macOS debug builds |

List all presets with `cmake --list-presets`.

Presets can be overridden from the command line:

```bash
cmake --preset gcc -DLIBQEMU_TARGETS="aarch64;riscv64"
```

Create a `CMakeUserPresets.json` file for personal overrides
(git-ignored).

### Manual Configuration

If your CMake version is older than 3.21 or you need full control:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCPM_DOWNLOAD_ALL=ON
cmake --build build --parallel
cmake --install build
```

### CMake Options

| Option | Description |
|--------|-------------|
| `CMAKE_INSTALL_PREFIX` | Install directory for the package and binaries |
| `CMAKE_BUILD_TYPE` | `Debug` or `Release` |
| `LIBQEMU_TARGETS` | Semicolon-separated list of QEMU targets (e.g., `aarch64;riscv64;hexagon`) |
| `CPM_DOWNLOAD_ALL` | Download all dependencies via CPM |
| `CPM_SOURCE_CACHE` | Directory to cache downloaded packages |
| `ENABLE_PYTHON_BINDER` | Enable the PythonBinder component (default: ON). Note: `WITHOUT_PYTHON_BINDER` is deprecated. |
| `GS_ENABLE_LTO` | Enable Link-Time Optimization |
| `UBUNTU_ARCH` | Architecture for Ubuntu platform builds (`aarch64` or `riscv64`) |

### Dependency Management

QBox uses [CPM](https://github.com/cpm-cmake/CPM.cmake) to find
and/or download missing dependencies. CPM uses CMake's standard
`find_package` mechanism to locate installed packages.

**Environment variables:**

- `SYSTEMC_HOME` -- Path to a locally installed SystemC
- `CCI_HOME` -- Path to a locally installed SystemC CCI

**Specifying package locations:**

- `<package>_ROOT` -- Point to a specific installed package
  location
- `CPM_<package>_SOURCE_DIR` -- Use your own source directory
  for a package
- `CMAKE_MODULE_PATH` -- Additional search paths for CMake
  modules

Example using a custom SystemC CCI source:

```bash
cmake -B build -DCPM_SystemCCCI_SOURCE=/path/to/your/cci/source
```

**Offline / cached builds:**

```bash
cmake -B build -DCPM_SOURCE_CACHE=$(pwd)/Packages
```

This populates the `Packages` directory. If a directory named
`Packages` exists at the project root, the build system will
automatically use it as a source cache.

### Testing

```bash
# Run all tests (using preset -- output-on-failure is enabled by default)
ctest --preset gcc

# Run a specific test
ctest --preset gcc -R <test_name>

# Without presets
ctest --test-dir build --output-on-failure
```

### Troubleshooting

**CMake cache issues:** CMake caches compiled modules in
`~/.cmake/`. If you are picking up the wrong version of a
module, it is safe to delete this cache directory.

**Corrupted build directory:** Remove and reconfigure:

```bash
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release
```

**Missing QEMU targets:** Rebuild with the target included:

```bash
cmake -B build -DLIBQEMU_TARGETS="aarch64;riscv64"
```

## MCIPS Plugin (Multi-Core Instructions Per Second Plugin)

The **MCIPS plugin** is a SystemC/C++ component that provides time synchronization between **QEMU** and **SystemC** by leveraging QEMU's TCG (Tiny Code Generator) plugin API. It enables deterministic multi-core simulation with instruction-level timing accuracy.

> **Note:** The MCIPS plugin currently supports **TCG mode only**.

***

### 1. Selecting the MCIPS Time-Sync Strategy from Lua Configuration

To use the plugin, set `time_sync_strategy = "mcips"` inside the `QemuInstance` configuration:

```lua
qemu_inst = {
    moduletype = "QemuInstance";
    args = {"&platform.qemu_inst_mgr", "AARCH64"};
    accel = "tcg",                        -- MCIPS requires TCG mode
    tcg_mode = "MULTI",                   -- Multi-threaded TCG
    sync_policy = "multithread-unconstrained",

    time_sync_strategy = "mcips",         -- Use the MCIPS plugin for time sync

    log_level = 0
}
```

The strategy accepts `"quantum_keeper"` (the default) and `"mcips"`. Setting it to `"quantum_keeper"` (or leaving it unset) uses the traditional quantum keeper synchronization mechanism instead of the MCIPS plugin.

Each CPU can be configured with its own instruction execution rate:

```lua
for i = 0, (ARM_NUM_CPUS - 1) do
    local cpu = {
        moduletype = "cpu_arm_cortexA76";
        args = {"&platform.qemu_inst"};
        mem = {bind = "&router.target_socket"};

        insn_per_second = 100000000;  -- 100 MIPS per CPU
        -- insn_per_second = ((i+1) * 100000000);  -- Different speeds per CPU
    };
    platform["cpu_"..tostring(i)] = cpu;
end
```

***

### 2. How the MCIPS Plugin Works

The MCIPS plugin is implemented as a **SystemC module** (`McipsPlugin` in `mcips-plugin.h`) that inherits from the **`LibQemuPlugin`** base class. Here is how it operates:

#### **Thread Model**

| Thread | Entry points |
|---|---|
| QEMU vCPU (BQL held) | `vcpu_init`, `vcpu_idle`, `vcpu_resume`, `vcpu_tb_trans`, `vcpu_quota_expired`, `external_halt` |
| SystemC | `receive_window_cb`, `idle_tick_method` |
| QEMU timer | `get_qemu_clock` (takes `m_mcips_mutex` only when no master is elected) |
| Idle drift (`std::thread`) | `idle_drift_loop` |

**Lock ordering:** `BQL → m_mcips_mutex` (never reversed). The stall release in
`idle_drift_loop()` and `external_halt()` both take the BQL first, because handing the clock on can involve resuming the
vCPU it is handed to.

The drift thread is a real `std::thread` rather than an SC process for two reasons: a suspended
SystemC kernel can only be woken from a foreign thread, and taking the BQL periodically *from* the
SystemC thread was measured to hang roughly 1 run in 8.

#### **Lock-Free Atomics**

| Variable | Ordering | Purpose |
|---|---|---|
| `m_shutdown` | seq_cst | Shutdown flag; checked on all entry paths |
| `m_inflight_cb` | seq_cst | Shutdown drain counter for `receive_window_cb` |
| `m_qemu_time_ns` | release/acquire | Nanosecond shadow of `m_qemu_time` for `get_qemu_clock` |
| `m_master_vcpu` | acquire (lock-free) / relaxed (under mutex) | The vCPU currently driving the virtual clock |
| `m_progress` | release/acquire | Bumped on every clock advance or granted window; the stall detector's liveness signal |

#### **QEMU TCG Plugin Integration**
- **libidlinker Plugin**: Runs QEMU's libidlinker shared library (`libidlinker.so`/`.dylib`)
- **Plugin ID Communication**: libidlinker calls `global_set_cci_param()` to send plugin ID back to QBox; the ID is stored as a `cci::cci_param<uint64_t>` (`m_id`)
- **Event Registration**: Every callback is registered directly via `m_inst.plugin_api().qemu_plugin_register_*_cb(...)` as a capture-less lambda that calls `dispatch_userdata()`, passing `handle_as_userdata()` as the userdata.
- **Callback Dispatch**: Each capture-less lambda decays to the C function pointer QEMU expects and routes the callback to the C++ member function via `dispatch_userdata()`

#### **Instruction-Based Timing Model**
- **Instruction Quota**: Each vCPU runs a global quantum (instruction count limit) before synchronization
- **Delta Tracking**: Tracks `delta_insn` (instructions executed since last sync) per vCPU using QEMU scoreboards
- **Time Calculation**: Converts instruction count to simulation time using `insn_per_second` parameter. `qemu_time_now()` has an overload accepting the master pointer to avoid redundant atomic loads when called under `m_mcips_mutex`.

#### **Clock Master Model**

Time is not a property of the vCPU that happens to be running. One vCPU is elected **clock
master** (`m_master_vcpu`), and only its retired instructions advance `m_qemu_time`; every other
vCPU is a **peer**, paced against that clock by pause and resume requests. `m_qemu_time` is only
ever changed while holding `m_mcips_mutex`.

The two states of the system are therefore "a master is elected, so QEMU drives the virtual clock
itself" and "no master is elected, so nothing inside QEMU moves the clock". Those two always move
together, so the drift is not tracked by a flag of its own: the drift thread reads the master
pointer, and a null master is what tells it SystemC must be driven from outside. Every store goes
through `set_master()`, which is the only writer.

**Pacing rule** (`cpu_should_pause()`): a peer is paused when its own checkpoint has run past the
clock. The master is paced against its slowest peer, capped by the window end, and never against
itself:

```cpp
threshold = slowest_drivable_peer ? min(sc_current_window.to, peer_time + m_quantum)
                                  : sc_current_window.to;
```

Comparing the master to its own checkpoint would latch true forever and wedge it paused with
nobody left able to resume it. With no peer that could drive the clock, the window is the only
bound, which is what the window is for.

**Election** is the scan inside `hand_off_clock()`, the one way the clock changes hands. It looks
for a vCPU that could still advance the clock and resets its checkpoint to the current QEMU time,
dropping whatever lead or lag it carried. A vCPU the plugin itself paused is electable, since it
drives the clock as soon as it is woken, but not one that is also halted, and that is why an
election finding nobody hands every pause back: with no master, nothing would ever lift them.

#### **Pause Ownership**

`cpu->stop` says a vCPU is stopped, not *who* stopped it, and QEMU sets the same flag for its own
global pauses. A resume clears it whoever set it. So inferring ownership from QEMU's flags lets the
pacing resume a vCPU that was stopped for a machine reset, which leaves `pause_all_vcpus()` waiting
for a machine that never stops.

The plugin therefore records its own pauses in `vCPUTime::paused_by_us` and `request_resume()`
declines anything else:

```cpp
if (!vcpu->paused_by_us) return;                                           // not ours to lift
if (m_inst.plugin_api().qemu_plugin_vcpus_globally_paused()) return;       // covers the overlap
```

`qemu_plugin_vcpus_globally_paused()` is a QEMU-side plugin API addition on Qualcomm's QEMU fork;
it reports the nesting depth of `pause_all_vcpus()`. It covers the case the per-vCPU flag cannot:
a pause the plugin really does own, on a vCPU QEMU has since stopped as well. Nothing is lost by
declining, because the pairing `resume_all_vcpus()` restarts every vCPU.

The flag can go stale, since a pause taken away from outside arrives with no callback, so
`release_stale_pause()` clears it wherever the vCPU is seen running. That check is only valid on
the vCPU's own thread while holding the BQL: acknowledging a pause clears `cpu->stop` and then sets
`cpu->stopped` as two separate stores, and a reader without the BQL can catch the gap between them
and see a state indistinguishable from a vCPU nobody stopped at all.

#### **Clock Drivability**

Election, re-validation and slowest-peer selection all ask whether a vCPU *could still advance the
clock* rather than merely whether it is halted (`cpu_can_drive_clock()`):

| vCPU state | Can drive the clock | Why |
|---|---|---|
| Halted (WFI/WFE/powered off) | no | Retires nothing, so the clock would stand still behind it |
| Stopped by something other than the plugin | no | Not the plugin's pause to lift, so electing it parks the clock on a vCPU nobody here can start |
| Paused by the plugin's own pacing | **yes** | Lifted as soon as the window admits it, which is exactly the catching up the master waits on |
| Stopped by a global pause (reset, vm-stop) | **yes** | Lasts only until the pairing `resume_all_vcpus()`; treating it as the clock dying would tear the window down mid-reset |

Counting a vCPU that cannot drive the clock as a peer would pin the master against a checkpoint
that never moves again. Conversely, a vCPU left out of the pacing must not come back owing the time
it sat out, so `receive_window_cb()` carries those checkpoints forward with the clock once
per granted window. Without it, a vCPU rejoining after an idle stretch reappears as the slowest
peer by the whole length of that stretch, and the master pauses itself on the way in.

#### **Translation Block Instrumentation**
```cpp
// For each TB, the plugin:
// 1. Counts instructions inline (atomic increment)
m_inst.plugin_api().qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(
    tb, QEMU_PLUGIN_INLINE_ADD_U64, delta_insn, n_insns);

// 2. Registers conditional callback when quota reached
m_inst.plugin_api().qemu_plugin_register_vcpu_tb_exec_cond_cb(
    tb,
    [](unsigned int cpu_index, void* userdata) {
        LibQemuPlugin::dispatch_userdata(
            userdata, [&](LibQemuPlugin* p) { static_cast<McipsPlugin*>(p)->vcpu_quota_expired(cpu_index); });
    },
    QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_COND_GE, delta_insn, m_global_quantum, handle_as_userdata());
```

#### **Per-vCPU State**

There is no plugin-side state enum. A vCPU's run state is read back from QEMU as ground truth
(`cpu_stopped()`, `cpu_halted()`, `cpu_stop_requested()`, wrapping
`qemu_plugin_cpu_stopped()` / `qemu_plugin_cpu_halted()` / `qemu_plugin_cpu_stop()`), and the one
thing QEMU cannot answer, namely who owns a pause, is the single `paused_by_us` bit:

```cpp
struct vCPUTime {
    unsigned int index;          /* QEMU vCPU index, the type the plugin API takes */
    bool paused_by_us;           /* sits in the padding after index, deliberately */
    uint64_t insn_per_second;
    uint64_t delta_insn;
    sc_core::sc_time cpu_time;   /* this vCPU's checkpoint */
};
```

> **Field order is load-bearing.** The `register_vcpu_tb_exec_inline_per_vcpu` and
> `register_vcpu_tb_exec_cond_cb` immediates are built against `delta_insn`'s byte offset, so a new
> small field goes in the padding after `index`, not in front of `delta_insn`.

#### **Idle Drift**

While no vCPU drives the virtual clock, nothing inside QEMU moves it: `get_qemu_clock()` can only
report SystemC time, and SystemC only moves when it has something scheduled. So the clock stops,
the `QEMU_CLOCK_VIRTUAL` timer that would wake an idle vCPU never comes due, and the simulation is
deadlocked with everything waiting on everything else.

A platform carrying a `realtimelimiter` gets the fix for free, since the limiter notifies its own
tick every realtime quantum. But the clock must not depend on an optional module, so the plugin
keeps a tick of its own, modelled on the limiter's and working with or without it:

| Member | Role |
|---|---|
| `m_idle_drift_thread` | Real `std::thread`; sleeps a realtime quantum, ticks, repeats |
| `p_idle_drift_quantum_ms` | Tick rate in ms (default `1.0`); floored, since zero would spin a host core |
| `m_idle_tick` | `gs::async_event`, notified `slice` ahead of now |

The tick is **deliberately empty**. Having been scheduled at a future time is what moves the
kernel forward, and moving the kernel forward is the entire point.

Two properties are easy to get wrong:

- **Paced against the host clock, not free-running.** QEMU's iothread derives its poll timeout from
  the virtual deadline and then sleeps that many real nanoseconds, so virtual time running faster
  than real time overshoots the deadline rather than arriving sooner. Free-running buys nothing and
  makes the clock lurch.
- **The tick is never attach-suspending.** qbox platforms and tests both end when SystemC starves,
  so a tick that held the kernel open would turn every clean end of simulation into a hang. The
  drift raises the floor on how far time may move *while the kernel is running*; if the kernel has
  genuinely finished, it is finished.

The branch also adds `p_MaxRunAhead_ms` to `realtimelimiter`, capping how far it will let SystemC
run ahead of `sc_time_stamp()`. Without it, idle vCPUs let the limiter sprint SystemC far into the
future (observed: virtual time reaching hundreds of thousands of seconds).

#### **Stall Detection**

An elected master can be stopped by something that answers to neither the plugin nor QEMU's idle
path: a machine reset, a halt signal driven from SystemC. Nothing then extends the sync window,
SystemC suspends on the one it is holding, and the per-quantum window callback that would elect a
replacement never runs again. Every thread that could break the cycle is waiting on one that
cannot.

The drift thread therefore ticks unconditionally, whether or not it is drifting, and watches
`m_progress`, bumped whenever QEMU advances the clock or SystemC grants a window, which a working
master does many times per realtime quantum and a stalled one never does again. After
`STALLED_CLOCK_TICKS` (50) quanta of complete silence, the tick gives the clock up:

1. Warps `m_qemu_time` up to SystemC time, so the peers wake up *behind* the clock rather than
   ahead of one that stopped moving a stall ago.
2. Clears the master and detaches the window.
3. Hands back every pause the pacing held. This step is not optional: **only a window callback
   lifts a plugin pause**, and a detached window delivers no callbacks, so a pause not released
   here is released never. The master itself is left alone, since it was stopped by something
   outside the plugin.

The threshold is generous on purpose. Tripping early costs a detached window that the next pacing
point takes back; a real stall going unnoticed costs the whole simulation.

#### **Clock Handover**

`QemuCpu` reports an external halt to the plugin through `McipsSync::on_halt_post()`, which calls
`external_halt()`. Doing it from the halt is the point: halting the last running vCPU is exactly
when QEMU stops asking for windows. If the halted vCPU was the master, `hand_off_clock()` elects a
replacement (waking it if the pacing admits it) or, finding none, detaches the window and hands
back every pause the pacing held, for the same reason the stall release does.

Any pacing pause the plugin holds on a halting vCPU is deliberately **left in place**. A halt is
not a resume; it stops the vCPU by its own mechanism and leaves the pause standing, so forgetting
it would strand the vCPU forever: `request_resume()` would decline it for the rest of the
simulation as a pause the plugin does not own.

On the way back, a vCPU takes an idle clock at its first pacing point (`take_idle_clock()`): it
rebases `m_qemu_time` onto SystemC time, attaches the window, and **seeds `sc_current_window` from
the rebased clock** instead of waiting for the first grant. A stale window would read as already
past its end, so the new master would pause itself on its way in and hand the clock straight back
to nobody. For the same reason a vCPU that has just claimed an idle clock is not put through the
pacing on that pass.

#### **Atomic Time Copy and Clock Monotonicity**
`m_qemu_time_ns` is an atomic copy of `m_qemu_time` in nanoseconds. `get_qemu_clock()` runs on QEMU's timer thread and reads the base time from this shadow (release/acquire) rather than the non-atomic `sc_time m_qemu_time`, since reading the latter off-mutex while vCPU threads write it under the mutex would be a torn read. It is updated by `sync_qemu_time_ns()` every time `m_qemu_time` changes while holding `m_mcips_mutex`.

`get_qemu_clock()` publishes **only committed master time**. Combining `m_qemu_time_ns` with the master's live `delta_insn` is not a coherent snapshot: while a quota is being transferred into `m_qemu_time`, a reader can count the same delta twice or miss it entirely, making the virtual clock run backwards on the next read. QEMU's timer dispatch requires a monotonic clock, so the clock instead advances at MCIPS accounting boundaries, with the delay bounded by one quantum.

It is also not a pure read. With no master elected it takes `m_mcips_mutex` and pulls `m_qemu_time` up to SystemC time, crediting every vCPU's checkpoint with the same warp, so the vCPUs come back onto a clock that did not stand still while they were idle. The clamp is forwards only; the clock is never reported backwards.

#### **Shutdown and Cleanup**

`McipsPlugin` implements deterministic shutdown through multiple guards:

- **`m_shutdown` (atomic bool)**: Set during `end_of_simulation()` and checked at the start of every callback so late calls exit early.
- **`m_inflight_cb` (atomic int)**: Counts how many `receive_window_cb` calls are running right now; shutdown waits for this to reach zero before continuing.
- **Drift thread join**: The master pointer is cleared, then the drift is stopped *and the sleeping thread woken* (`m_drift_cv.notify_all()`) before joining. Clearing the flag alone is not enough: the thread has to be joined before the `gs::async_event` it notifies goes away.
- **`shutdown_bridge()`**: Called from both `end_of_simulation()` and the destructor. Safe to call more than once (the second call does nothing).
- **Detach on shutdown**: If the sync window is still attached, it is detached with the current `m_qemu_time` so SystemC can keep running on its own.

##### Shutdown Double-Check Pattern

`receive_window_cb` uses a double-check pattern to safely interact with
the `m_inflight_cb` counter during shutdown:

```
1. Check m_shutdown → if true, return early (no increment)
2. Increment m_inflight_cb
3. Re-check m_shutdown → if true, decrement and return
4. ... do work ...
5. Decrement m_inflight_cb
```

This prevents a race where `shutdown_cleanup()` finishes draining
`m_inflight_cb` before a concurrent `receive_window_cb` has incremented
it, which would cause a use-after-free. The second check in step 3
catches the case where shutdown started between steps 1 and 2.

***

### 3. Timing Parameters and Synchronization Accuracy

The MCIPS plugin's synchronization behavior is controlled by two critical timing parameters:

#### **quantum_ns Parameter**

The `quantum_ns` parameter defines the global quantum (in nanoseconds) used for time synchronization between SystemC and QEMU. This parameter is set at the platform level:

```lua
platform = {
    quantum_ns = 100000;  -- Recommended: 100 microseconds
    -- ... other platform configuration
}
```

**Impact on Synchronization:**

The `quantum_ns` value controls the trade-off between synchronization accuracy and performance. Smaller values increase synchronization frequency and timing precision at the cost of overhead. Larger values reduce overhead but decrease timing accuracy.

**Recommendation**: Use `quantum_ns = 100000` (100 microseconds) as a starting point.

#### **insn_per_second Parameter**

The `insn_per_second` parameter specifies the instruction execution rate for each vCPU, controlling the simulated CPU performance. This parameter is configured per CPU:

```lua
cpu = {
    moduletype = "cpu_arm_cortexA76";
    insn_per_second = 100000000;  -- 100 MIPS
    -- ... other CPU configuration
}
```

**Impact on CPU Timing:**

This parameter directly determines how CPU time is calculated from instruction counts. The MCIPS plugin converts executed instructions into simulation time using: `cpu_time = (instructions_executed / insn_per_second) * 1e9` nanoseconds.

#### **idle_drift_quantum_ms Parameter**

The realtime tick rate, in milliseconds, at which SystemC is drifted forward while no vCPU is
driving the virtual clock. The plugin is constructed as an SC child of the QEMU instance, so the
parameter's full name is `<platform>.<qemu instance>.mcips_plugin.idle_drift_quantum_ms`. Nested
tables reach it the same way as any other parameter, and it is rarely worth changing:

```lua
qemu_inst = {
    moduletype = "QemuInstance";
    -- ...
    mcips_plugin = { idle_drift_quantum_ms = 1.0 };  -- default
}
```

It bounds how coarsely the virtual clock moves while the machine is idle, which in turn bounds how
late a `QEMU_CLOCK_VIRTUAL` timer can be in waking an idle vCPU. A zero or negative value is
floored rather than honoured, since it would turn the tick thread's wait into a spin and peg a host
core.

***

### 4. Why Disable Quantum Keeper

When MCIPS plugin is enabled, the traditional **quantum keeper mechanism is automatically disabled** in the CPU implementation (`cpu.h`). This is essential because:

- **Quantum Keeper**: Uses wall-clock time and SystemC's quantum-based synchronization
- **MCIPS Plugin**: Uses instruction-count-based time calculation with custom synchronization windows

#### **Time-Sync Strategy Selection**

`QemuCpu` does not branch on `mcips_enabled()` throughout its body. Instead it
owns a `CpuTimeSyncStrategy` (`std::unique_ptr<CpuTimeSyncStrategy> m_time_sync`),
chosen once in the constructor, and delegates every former branch point to it:

```cpp
// In cpu.h - the strategy is chosen once at construction
inline bool mcips_enabled() const { return m_inst.is_mcips_enabled(); }

if (mcips_enabled()) {
    m_time_sync = std::make_unique<McipsSync>(*this);          // time driven by the plugin
} else {
    m_time_sync = std::make_unique<QuantumKeeperSync>(*this);  // traditional quantum keeper
}
```

The quantum-keeper machinery itself (`create_quantum_keeper`, `kick_cb`,
`deadline_timer_cb`, `wait_for_work`, `sync_with_kernel`, `prepare_run_cpu`,
`end_of_loop_cb`, the coroutine/watch threads, ...) stays on `QemuCpu`,
unchanged. At each lifecycle/callback point `QemuCpu` calls a strategy hook
(`on_construct`, `on_after_cpu_created`, `on_before_end_of_elaboration`,
`on_halt_pre`/`on_halt_post`, `on_reset_finish`, `on_qk_start`,
`on_arm_deadline`, `get_local_time`, `set_local_time`, ...):

- `QuantumKeeperSync` forwards each hook to the matching `QemuCpu` method — the
  traditional behaviour.
- `McipsSync` overrides only four: `on_end_of_elaboration()` (to register
  `insn_per_second` with the plugin), `on_halt_pre()` / `on_halt_post()` (to
  report an external halt, see below) and `on_destroy()`. Every other hook keeps
  the empty base no-op. The set of hooks it does *not* override documents exactly
  how much of the CPU lifecycle MCIPS participates in.

Strategies hold no state of their own; they reach `QemuCpu`'s members through a
`m_qemu_cpu` back-reference. `QuantumKeeperSync` and `McipsSync` are nested
classes of `QemuCpu`, which grants them access to its private/protected members,
so the carefully-tuned member ordering and run loop are preserved. The abstract
`CpuTimeSyncStrategy` base stays a top-level class.

When MCIPS is enabled the machinery is therefore simply never reached: no
quantum keeper is created, no `end_of_loop_cb`/`kick_cb`/`deadline_timer_cb`
callbacks are registered, no coroutine or external-event watch thread is
spawned, and the quantum-keeper start/stop/sync calls throughout the CPU
lifecycle (constructor, `before_end_of_elaboration`, `start_of_simulation`,
`halt_cb`, `reset_cb`, destructor, ...) become no-ops. `initiator_get_local_time()`
and `initiator_set_local_time()` return `SC_ZERO_TIME` / do nothing via the
`McipsSync` hooks.

#### **sync_policy: How Far a vCPU May Run Ahead**

`sync_policy` selects the quantum keeper a `QemuCpu` runs under
(`tlm_quantumkeeper_factory` in `qk_factory.cc`). The choice is what bounds how
far a vCPU's own time may get in front of SystemC time before the vCPU has to
stop and let the kernel catch up, so it is the knob that decides how closely
the vCPUs stay together:

| `sync_policy` | Run-ahead bound from `sc_time_stamp()` | Thread |
|---|---|---|
| `tlm2` | One quantum, the plain TLM-2 rule | SystemC thread (coroutine) |
| `multithread-quantum` | One quantum, and it only syncs on the boundary | Own thread |
| `multithread` | Two quanta, so SystemC is never left waiting at the boundary | Own thread |
| `multithread-adaptive` | Two quanta, shrinking to half a quantum as the vCPU pulls ahead | Own thread |
| `multithread-rolling` | One quantum, clipped to the next pending SystemC event | Own thread |
| `multithread-unconstrained` | None, `need_sync()` is hardwired to `false` | Own thread |
| `multithread-freerunning` | None, and `sync()` only kicks the kernel awake | Own thread |

The first five keep every vCPU inside a couple of quanta of SystemC and
therefore of each other. The last two never block a vCPU, so time is only kept
together if something else does it: `multithread-unconstrained` is what the
MCIPS example above pairs the plugin with, because under `mcips` the pacing is
the plugin's job and a quantum keeper that also stopped the vCPU would be
fighting it. Under `quantum_keeper` those two policies let one vCPU run its
whole workload while the others sit still.

#### **Halt and Reset with MCIPS**

With `McipsSync` active, `on_reset_finish` is a no-op, so `reset_cb` performs the reset without any
quantum-keeper start/reset or kick event.

`on_halt_pre`/`on_halt_post` are *not* no-ops: they are how the plugin learns that a vCPU has been
stopped from outside QEMU. `on_halt_pre(val)` latches the halt direction in `m_halting`, and
`on_halt_post()` reports it to the plugin:

```cpp
inline void QemuCpu::McipsSync::on_halt_post()
{
    if (!m_halting) return;
    m_qemu_cpu.m_inst.get_mcips_plugin().external_halt(m_qemu_cpu.m_cpu.get_index());
}
```

Only the halt direction needs reporting. An unhalted vCPU starts running and reclaims the clock at
its first pacing point, so there is nothing to tell the plugin on the way back up.

#### **Tracked Async Work**

The CPU uses `make_tracked_async_job()` to wrap async jobs with `m_async_work_outstanding` tracking. The destructor waits (with a 500 ms timeout) for all in-flight jobs to complete before destroying the object, preventing use-after-free when async jobs hold captured references.

### 5. Monitor Support

The MCIPS plugin provides comprehensive **monitoring and debugging support** through the SystemC monitor interface:

#### **Monitor Integration**
```cpp
// In monitor.cc - automatic discovery and registration
void monitor<BUSWIDTH>::end_of_elaboration()
{
    m_qks = find_sc_objects<gs::tlm_quantumkeeper_multithread>();
    m_mcips_plugins = find_sc_objects<McipsPlugin>();  // Auto-discover MCIPS plugins
}
```

#### **Diagnostic JSON**

`get_mcips_status_json()` returns a JSON snapshot of the current state (reads without `m_mcips_mutex`, so values may be slightly out of date):

| Field | Meaning |
|---|---|
| `qemu_time_ns`, `sc_time_ns` | QEMU's virtual clock and SystemC's clock side by side; the gap between them is the drift at a glance |
| `n_cpus`, `master_vcpu_index` | vCPU count, and which one holds the clock (`-1` for none) |
| `vcpus[].is_master` | Which entry is the clock master |
| `vcpus[].index`, `insn_per_second`, `delta_insn` | Identity and instruction accounting |
| `vcpus[].cpu_time_ns`, `cpu_time_s` | This vCPU's checkpoint |
| `vcpus[].q_stopped`, `q_stop`, `q_halted` | QEMU's ground truth: `cpu->stopped`, `cpu->stop`, halted |
| `vcpus[].paused_by_us` | Whether *this plugin* owns the pause, which the three flags above cannot tell you |

The last two rows together are the diagnostic that matters most: a vCPU with `q_stopped=1` and
`paused_by_us=0` is being held by something other than the plugin, and the plugin will not resume
it.

***

### 6. SystemC Synchronization with sc_sync_window

The MCIPS plugin uses **`sc_core::sc_sync_window<sc_sync_policy>`** (defined in `sync_window.h`) for coordinated time advancement between QEMU and SystemC. This is a template class parameterised by a sync policy.

#### **sc_sync_window Architecture**
- **Template Policy**: `sc_sync_window` takes a sync policy as a template (e.g., `sc_sync_policy_tlm_quantum` uses the TLM global quantum, `sc_sync_policy_in_sync` tracks pending work). The policy tells the window how big each step is and whether to stay attached when idle.
- **Single Window**: The MCIPS plugin has one `sc_sync_window<sc_sync_policy_tlm_quantum>` (`m_sync_sc`) that talks to SystemC.
- **Time Windows**: Each window has a `{from, to}` range stored in `sc_current_window`.
- **Sweep/Step Model**: Uses `SC_METHOD` helpers inside: `sweep_helper()` moves time to the start of the next window, `step_helper()` pauses SystemC at the end until a new window comes in.
- **Observer Event**: Uses `sc_ob_event` (or `gs::observer_event` as fallback) to notify at window edges.
- **Callback Registration**: Uses `register_sync_cb()` to get window updates through `receive_window_cb()`.

#### **Attach and Detach**

**Attach** (`m_sync_sc.attach()`):
- Turns on time synchronization between QEMU and SystemC.
- Used when a clock master is elected.
- After attaching, you must call `async_set_window()` to send the first window.

**Detach** (`m_sync_sc.detach(current_time)`):
- Opens the window to `[current_time, max_time]` and stops synchronization.
- Used when no vCPU can drive the clock.
- SystemC can keep running without waiting for QEMU.

> **Important**: `async_set_window()` will crash if the window is not attached. Always attach first.

> **Two traps worth stating plainly.** An *attached* window pins SystemC at the window end, so
> failing to extend it stops SystemC dead. A *detached* window delivers no `receive_window_cb` at
> all **and** lets SystemC free-run unbounded. Since only a window callback lifts a plugin pause,
> detaching while the plugin still holds pauses makes those pauses permanent, which is why the
> stall release hands them all back before it detaches.

#### **Synchronization Flow**
```cpp
// Taking a vacant clock rebases onto SystemC time first: the idle drift moved
// SystemC on while the clock was given up, so a window set from the stale QEMU
// time is one the kernel has already left behind.
void take_idle_clock(vCPUTime* vcpu)
{
    catch_up_to_systemc();        // the idle drift moved SystemC on meanwhile
    attach_sync_window();
    vcpu->cpu_time = qemu_time_now();
    set_master(vcpu);             // which also stops the idle drift

    // Seed the window from the clock just rebased. Leaving the stale window
    // in place reads as "already past the window end", so the new master
    // pauses itself on the way in and the clock is driven by nobody again.
    sc_current_window = { m_qemu_time, m_qemu_time + m_quantum };
    set_systemc_window();
}

// A vCPU reaching a pacing point with nobody driving the clock takes it over,
// otherwise it is paced.
void take_clock_or_pause(vCPUTime* vcpu)
{
    if (!master()) {
        // Not paced on the same pass: with no master, nothing would lift the
        // pause it had just taken, the window that admits it being detached.
        take_idle_clock(vcpu);
        return;
    }

    if (!vcpu->paused_by_us && cpu_should_pause(vcpu)) {
        vcpu->paused_by_us = true;
        m_inst.plugin_api().qemu_plugin_cpu_request_pause(vcpu->index);
    }
}

// hand_off_clock() is the one way the clock changes hands: clear the master so the
// scan cannot pick it again, take the first vCPU that could still drive the clock,
// and wake it if the pacing admits it. Finding none, the pauses go back too.
vCPUTime* hand_off_clock()
{
    set_master(nullptr);

    for (int i = 0; i < m_num_vcpus; i++) {
        vCPUTime* pick = get_vcpu(i);
        if (!cpu_can_drive_clock(pick)) continue;

        pick->cpu_time = m_qemu_time;      // drop whatever lead or lag it carried
        pick->delta_insn = 0;
        set_master(pick);
        if (!cpu_should_pause(pick)) {
            request_resume(pick);          // a paused pick drives the clock only once woken
        }
        return pick;
    }

    // Nobody left; the idle drift takes over. A vCPU that is paused and halted right
    // now was skipped by the scan above, and with the window detached nothing would
    // ever lift its pause again, so every pause is handed back here.
    detach_sync_window();
    for (int i = 0; i < m_num_vcpus; i++) {
        request_resume(get_vcpu(i));
    }
    return nullptr;
}
```

***

### 7. QEMU TCG Plugin API Integration and Event Registration

The MCIPS plugin leverages QEMU's **TCG Plugin API** through a sophisticated C++/C bridge system:

#### **libidlinker Plugin and ID Communication**
QBox runs QEMU's **libidlinker** plugin to obtain a unique plugin ID:
- **Plugin Loading**: QBox loads `libidlinker.so` (Linux) or `libidlinker.dylib` (macOS) into QEMU
- **ID Generation**: libidlinker generates a unique `qemu_plugin_id_t` for the plugin instance
- **ID Communication**: libidlinker calls the C function `global_set_cci_param(key, plugin_id)` to communicate the ID back to QBox
- **CCI Integration**: The ID is stored as a `cci::cci_param<uint64_t>` and used by the McipsPlugin instance

#### **TCG Plugin API Access**
QBox calls QEMU's plugin API through the auto-generated `LibQemuExports` table, reached via the `LibQemu::plugin_api()` accessor:
- **Auto-generated dispatch table**: Plugin function pointers are populated by libqemu's `exports.py` machinery alongside the rest of the libqemu API.
- **Type Safety**: Plugin types (`qemu_plugin_id_t`, `qemu_plugin_tb`, callback typedefs) come directly from `qemu-plugin.h` via a glib-safe wrapper, so consumers don't need to redeclare anything.

#### **LibQemuPlugin Base Class**

The **LibQemuPlugin** base class (`libqemu-plugin.h`) inherits from `sc_core::sc_module` and holds a reference to `qemu::LibQemu`. It provides:

- **PluginHandle**: Heap-allocated, intentionally leaked at plugin destruction. Every callback passes `handle_as_userdata()` so the dispatch can resolve to the C++ instance with no map lookup. Outliving the plugin lets stale callbacks observe `alive == false` and exit early instead of dereferencing freed memory.
- **Single userdata dispatch**: There is exactly one dispatch path. Subclasses register a capture-less lambda (which decays to the C function pointer the plugin API expects) directly via `m_inst.plugin_api().qemu_plugin_register_*_cb(...)`; the lambda calls `LibQemuPlugin::dispatch_userdata(userdata, body)` to recover the instance. The base class deliberately mirrors no callback signature — only the subclass registration sites do — keeping it decoupled from upstream plugin-API churn.
- **`shutdown_bridge()`**: Stops bridge dispatch and waits for in-flight callbacks to drain. Idempotent — `m_handle->alive`'s exchange is the once-guard. Flips the handle dead, then spins until `m_handle->refcount` reaches zero. The handle itself is intentionally leaked.

#### **Event Registration Process**
The MCIPS plugin registers multiple event types during `end_of_elaboration()`:

```cpp
// Every callback is registered as a capture-less lambda forwarding through
// dispatch_userdata(), with handle_as_userdata() as the userdata.
m_inst.plugin_api().qemu_plugin_register_vcpu_tb_trans_cb(
    m_id,
    [](qemu_plugin_tb* tb, void* userdata) {
        LibQemuPlugin::dispatch_userdata(
            userdata, [&](LibQemuPlugin* p) { static_cast<McipsPlugin*>(p)->vcpu_tb_trans(tb); });
    },
    handle_as_userdata());
// ... vcpu_init, vcpu_resume, vcpu_idle registered the same way ...

// Time callback (returns int64_t; SystemC time is the dead-handle fallback)
m_inst.plugin_api().qemu_plugin_register_time_cb(
    m_time_handle,
    [](void* userdata) -> int64_t {
        int64_t result = static_cast<int64_t>(sc_core::sc_time_stamp().to_seconds() * NSEC_IN_ONE_SEC);
        LibQemuPlugin::dispatch_userdata(
            userdata, [&](LibQemuPlugin* p) { result = static_cast<McipsPlugin*>(p)->get_qemu_clock(nullptr); });
        return result;
    },
    handle_as_userdata());
```

### How Everything Works Together

The MCIPS plugin creates a sophisticated multi-layered synchronization system:

1. **QEMU Layer**: libidlinker plugin provides unique ID; TCG instrumentation counts instructions inline
2. **Dispatch Layer**: Capture-less lambdas pass QEMU callbacks to SystemC components through a single `dispatch_userdata()` path; refcounting keeps objects alive during callbacks
3. **Plugin Layer**: McipsPlugin elects a clock master, turns its instruction count into simulation time, paces the other vCPUs against it by pause/resume, and tracks which pauses are its own
4. **Drift Layer**: While no master is elected, a host-paced thread drifts SystemC forward so the virtual clock keeps moving; the same thread watches for a master that has silently stopped driving it
5. **SystemC Layer**: `sc_sync_window` talks to the SystemC kernel through sweep/step methods to move time forward
6. **Monitor Layer**: Provides visibility into the entire system state via JSON diagnostics

***

## Quick Start: Ubuntu Platform (AArch64)

```bash
# Build the rootfs image
cd platforms/ubuntu/fw/
./build_linux_dist_image.sh -s 4G -p xorg,pciutils -a aarch64
cd ../../..

# Build QBox with AArch64 target
cmake -B build -DLIBQEMU_TARGETS=aarch64
cmake --build build --parallel

# Run the virtual platform
./build/platforms/platforms-vp -l platforms/ubuntu/conf_aarch64.lua
```

## Documentation

- **[Configuration](docs/configuration.md)** -- CCI parameters,
  Lua configuration, YAML, ConfigurableBroker

### Component Libraries

- **[libqbox](docs/libqbox.md)** -- QEMU/SystemC integration,
  CPUs, interrupt controllers, UARTs, VNC, parallelism
- **[libqemu-cxx](docs/libqemu-cxx.md)** -- Low-level QEMU C++
  wrapper
- **[libgssync](docs/libgssync.md)** -- Synchronization policies
  and suspend/unsuspend interface
- **[libgsutils](docs/libgsutils.md)** -- CCI utilities and TLM
  port types
- **[Base components](docs/base-components.md)** -- Router,
  memory, loader, memory dumper
- **[Extra components](docs/extra-components.md)** -- GPEX, NVME,
  OpenCores Ethernet
- **[PythonBinder](docs/python-binder.md)** -- Python integration
  for SystemC models
- **[Networking](docs/networking.md)** -- SystemC Ethernet MAC
  setup and host configuration
- **[Character backends](docs/backends.md)** -- stdio, socket,
  and file backends for UART I/O
- **[Monitor](docs/monitor.md)** -- Web-based simulation
  monitoring interface

### Platforms

- **[Ubuntu](docs/platforms/ubuntu.md)** -- Ubuntu Linux platform
  (AArch64 and RISC-V 64)

### Tutorials and Examples

- **[hello-qbox](examples/hello-qbox/)** -- Step-by-step
  tutorial that walks you through building a minimal AArch64
  virtual platform (Cortex-A53, RAM, UART) from scratch

## C++ Standard

QBox requires C++17 and is compatible with SystemC versions from
SystemC 2.3.1a.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

See [LICENSE](LICENSE).
