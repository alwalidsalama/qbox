/*
 * This file is part of libqbox
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. All Rights Reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef _LIBQBOX_COMPONENTS_MCIPS_PLUGIN_H
#define _LIBQBOX_COMPONENTS_MCIPS_PLUGIN_H

#include "libqemu-plugin.h"
#include <sync_window.h>
#include <async_event.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
#include <sstream>

class McipsPlugin : public LibQemuPlugin
{
    SCP_LOGGER();
    SC_HAS_PROCESS(McipsPlugin);

private:
    struct vCPUTime {
        uint64_t index;
        uint64_t insn_per_second;
        uint64_t delta_insn;
        sc_core::sc_time cpu_time;
    };

    uint64_t m_global_quantum; // quantum in nanoseconds; used as the instruction count limit
    int m_num_vcpus{ 0 };      // cached CPU count (set in end_of_elaboration, may grow in vcpu_init)
    void* m_time_handle;       // handle returned by QEMU to let us control time
    sc_core::sc_sync_window<sc_core::sc_sync_policy_tlm_quantum> m_sync_sc; // SystemC sync window
    sc_core::sc_sync_window<sc_core::sc_sync_policy_tlm_quantum>::window
        sc_current_window;        // last window received from SystemC
    sc_core::sc_time m_quantum;   // copy of the TLM global quantum
    sc_core::sc_time m_qemu_time; // main QEMU simulation time (only changed while holding m_mcips_mutex)
    /* CPU currently driving m_qemu_time. Read lock-free by get_qemu_clock() (acquire/release). */
    std::atomic<vCPUTime*> m_master_vcpu;
    qemu_plugin_scoreboard* m_vcpus_scoreboard; // one vCPUTime entry per CPU index

    /* Nanosecond shadow of m_qemu_time; sc_time is not thread-safe. */
    std::atomic<int64_t> m_qemu_time_ns{ 0 };

    /* Shutdown flag; all entry points return immediately when set. */
    std::atomic<bool> m_shutdown{ false };

    /* Counts in-flight receive_window_cb() calls; shutdown waits for zero. */
    std::atomic<int> m_inflight_cb{ 0 };

    /* Protects shared state across vCPU and SystemC threads. */
    std::mutex m_mcips_mutex;

public:
    McipsPlugin(const sc_core::sc_module_name& nm, qemu::LibQemu& inst)
        : LibQemuPlugin(nm, inst)
        , m_sync_sc("m_sync_sc")
        , m_global_quantum(0)
        , m_time_handle(nullptr)
        , sc_current_window(sc_core::sc_sync_window<sc_core::sc_sync_policy_tlm_quantum>::zero_window)
        , m_quantum(sc_core::SC_ZERO_TIME)
        , m_qemu_time(sc_core::SC_ZERO_TIME)
        , m_master_vcpu(nullptr)
        , m_vcpus_scoreboard(nullptr)
    {
    }

    /**
     * @brief Shared shutdown logic used by both the destructor and end_of_simulation().
     *
     * Stops the idle pump, marks shutdown, clears the master CPU, waits for
     * any in-flight receive_window_cb to finish, and detaches the sync window.
     * Safe to call more than once (subsequent calls are no-ops).
     */
    void shutdown_cleanup()
    {
        if (m_shutdown.load(std::memory_order_seq_cst)) {
            return; // already shut down
        }

        m_shutdown.store(true, std::memory_order_seq_cst); // must be seq_cst before inflight drain
        m_master_vcpu.store(nullptr, std::memory_order_release);

        shutdown_bridge();

        /* Wait for any in-flight receive_window_cb() to finish. */
        while (m_inflight_cb.load(std::memory_order_seq_cst) > 0) {
            std::this_thread::yield();
        }

        detach_sync_window();
    }

    ~McipsPlugin() { shutdown_cleanup(); }

    /** @brief Called by SystemC at end of simulation; stops all QEMU callbacks. */
    void end_of_simulation() override { shutdown_cleanup(); }

    /** @brief Set up the plugin, register QEMU callbacks, and send the first sync window. */
    void end_of_elaboration() override
    {
        LibQemuPlugin::end_of_elaboration();
        m_vcpus_scoreboard = m_inst.plugin_api().qemu_plugin_scoreboard_new(sizeof(vCPUTime));
        sc_assert(m_vcpus_scoreboard);

        m_time_handle = const_cast<void*>(m_inst.plugin_api().qemu_plugin_request_time_control(m_id));
        sc_assert(m_time_handle);

        m_quantum = tlm_utils::tlm_quantumkeeper::get_global_quantum();
        m_global_quantum = static_cast<uint64_t>(std::floor(m_quantum.to_seconds() * NSEC_IN_ONE_SEC));

        sc_current_window = { sc_core::SC_ZERO_TIME, m_quantum };
        if (m_sync_sc.is_attached()) {
            m_sync_sc.async_set_window(sc_current_window);
        } else {
            SCP_FATAL(()) << "Window must be attached before calling async_set_window()";
            sc_assert(false);
        }
        m_num_vcpus = m_inst.plugin_api().qemu_plugin_num_vcpus();
        m_sync_sc.register_sync_cb(std::bind(&McipsPlugin::receive_window_cb, this, std::placeholders::_1));

        /* Every callback is registered directly with the plugin API as a
         * capture-less lambda (which decays to the C function pointer the API
         * expects). Each forwards through LibQemuPlugin::dispatch_userdata for
         * shutdown synchronization against the leaked PluginHandle, a raw
         * `this` would dangle once this SystemC module is destroyed while QEMU
         * threads still have callbacks in flight. */
        m_inst.plugin_api().qemu_plugin_register_vcpu_tb_trans_cb(
            m_id,
            [](qemu_plugin_tb* tb, void* userdata) {
                LibQemuPlugin::dispatch_userdata(
                    userdata, [&](LibQemuPlugin* p) { static_cast<McipsPlugin*>(p)->vcpu_tb_trans(tb); });
            },
            handle_as_userdata());
        m_inst.plugin_api().qemu_plugin_register_vcpu_init_cb(
            m_id,
            [](unsigned int cpu_index, void* userdata) {
                LibQemuPlugin::dispatch_userdata(
                    userdata, [&](LibQemuPlugin* p) { static_cast<McipsPlugin*>(p)->vcpu_init(cpu_index); });
            },
            handle_as_userdata());
        m_inst.plugin_api().qemu_plugin_register_vcpu_resume_cb(
            m_id,
            [](unsigned int cpu_index, void* userdata) {
                LibQemuPlugin::dispatch_userdata(
                    userdata, [&](LibQemuPlugin* p) { static_cast<McipsPlugin*>(p)->vcpu_resume(cpu_index); });
            },
            handle_as_userdata());
        m_inst.plugin_api().qemu_plugin_register_vcpu_idle_cb(
            m_id,
            [](unsigned int cpu_index, void* userdata) {
                LibQemuPlugin::dispatch_userdata(
                    userdata, [&](LibQemuPlugin* p) { static_cast<McipsPlugin*>(p)->vcpu_idle(cpu_index); });
            },
            handle_as_userdata());
        m_inst.plugin_api().qemu_plugin_register_time_cb(
            m_time_handle,
            [](void* userdata) -> int64_t {
                int64_t result = static_cast<int64_t>(sc_core::sc_time_stamp().to_seconds() * NSEC_IN_ONE_SEC);
                LibQemuPlugin::dispatch_userdata(userdata, [&](LibQemuPlugin* p) {
                    result = static_cast<McipsPlugin*>(p)->get_qemu_clock(nullptr);
                });
                return result;
            },
            handle_as_userdata());
    }

    /** @brief Helper function to get vCPU time structure from scoreboard */
    vCPUTime* get_vcpu(unsigned int cpu_index)
    {
        return reinterpret_cast<vCPUTime*>(
            m_inst.plugin_api().qemu_plugin_scoreboard_find(m_vcpus_scoreboard, cpu_index));
    }

    /* QEMU ground-truth state derivation */

    /** @brief QEMU has this vCPU paused (cpu->stopped). */
    bool cpu_stopped(const vCPUTime* vcpu)
    {
        return m_inst.plugin_api().qemu_plugin_cpu_stopped(static_cast<unsigned int>(vcpu->index));
    }
    /** @brief QEMU has this vCPU halted (WFI/WFE/powered off). */
    bool cpu_halted(const vCPUTime* vcpu)
    {
        return m_inst.plugin_api().qemu_plugin_cpu_halted(static_cast<unsigned int>(vcpu->index));
    }
    /**
     * @brief We asked QEMU to pause this vCPU and it has not taken the stop yet (cpu->stop).
     */
    bool cpu_pause_pending(const vCPUTime* vcpu)
    {
        return m_inst.plugin_api().qemu_plugin_cpu_stop(static_cast<unsigned int>(vcpu->index));
    }
    /** @brief Paused or about to be: not a candidate to run, and a candidate to resume. */
    bool cpu_paused_or_pending(const vCPUTime* vcpu) { return cpu_stopped(vcpu) || cpu_pause_pending(vcpu); }

    void attach_sync_window()
    {
        if (!m_sync_sc.is_attached()) m_sync_sc.attach();
    }

    void detach_sync_window()
    {
        if (m_sync_sc.is_attached()) m_sync_sc.detach(m_qemu_time);
    }

    /** @brief Set insn/sec for a vCPU. @return false if vCPU not found in scoreboard. */
    bool set_vcpu_insn_per_second(unsigned int cpu_index, uint64_t insn_per_second)
    {
        if (insn_per_second == 0) {
            SCP_FATAL(()) << "insn_per_second must be > 0 (cpu_" << cpu_index << ")";
            return false;
        }

        const unsigned int n = static_cast<unsigned int>(m_inst.plugin_api().qemu_plugin_num_vcpus());
        if (cpu_index >= n) {
            return false;
        }
        if (n > static_cast<unsigned int>(m_num_vcpus)) m_num_vcpus = static_cast<int>(n);

        vCPUTime* vcpu = get_vcpu(cpu_index);
        if (!vcpu) {
            return false;
        }

        vcpu->insn_per_second = insn_per_second;
        return true;
    }

    /** @brief Copy m_qemu_time into the atomic nanosecond field. Must hold m_mcips_mutex. */
    void sync_qemu_time_ns()
    {
        m_qemu_time_ns.store(static_cast<int64_t>(m_qemu_time.to_seconds() * NSEC_IN_ONE_SEC),
                             std::memory_order_release);
    }

    /** @brief Time represented by the in-flight instructions in delta_insn. */
    static sc_core::sc_time cpu_delta_time(const vCPUTime* vcpu)
    {
        sc_assert(vcpu && "cpu_delta_time called with null vCPU");
        return sc_core::sc_time(static_cast<double>(vcpu->delta_insn) / vcpu->insn_per_second, sc_core::SC_SEC);
    }

    /** @brief Time for a CPU (base + in-flight delta); 0 delta for non-running. Holds m_mcips_mutex. */
    sc_core::sc_time cpu_time_now(const vCPUTime* vcpu)
    {
        sc_assert(vcpu && "cpu_time_now called with null vCPU");
        return vcpu->cpu_time + cpu_delta_time(vcpu);
    }

    /** @brief Current QEMU time (base + master CPU's in-flight delta). */
    sc_core::sc_time qemu_time_now() { return qemu_time_now(m_master_vcpu.load(std::memory_order_acquire)); }

    /** @brief Overload for callers that already have the master pointer. */
    sc_core::sc_time qemu_time_now(const vCPUTime* master)
    {
        if (!master) return m_qemu_time;
        return m_qemu_time + cpu_delta_time(master);
    }

    /**
     * @brief Elect a new master (clock-source) cpu, or nullptr if nobody qualifies. Mutex held.
     *
     * Only call this when there is no master: a live one is never displaced, not even once it becomes
     * paused, since churning the clock source is worse than waiting for it to come back. Only its
     * owner clears it, in vcpu_idle, when it genuinely halts.
     *
     * A halted cpu is skipped: it retires nothing, so the clock would stand still. A paused/pending
     * cpu is elected too, but it drives the clock only once woken, so the caller resumes it (see
     * vcpu_idle).
     *
     * The new master starts from the current QEMU time, dropping whatever lead or lag its old
     * checkpoint carried.
     */
    vCPUTime* select_new_master_vcpu()
    {
        for (int i = 0; i < m_num_vcpus; i++) {
            vCPUTime* vcpu = get_vcpu(i);
            if (cpu_halted(vcpu)) continue;

            vcpu->cpu_time = m_qemu_time;
            vcpu->delta_insn = 0;
            m_master_vcpu.store(vcpu, std::memory_order_release);
            return vcpu;
        }

        m_master_vcpu.store(nullptr, std::memory_order_release);
        return nullptr;
    }

    /** @brief Pause a vCPU for pacing. Caller must credit delta_insn first. */
    void request_pause(vCPUTime* vcpu)
    {
        SCP_WARN(()) << "request pause cpu_" << vcpu->index;
        m_inst.plugin_api().qemu_plugin_cpu_request_pause(static_cast<unsigned int>(vcpu->index));
    }

    /**
     * @brief Ask a paused vCPU to resume. m_mcips_mutex held; any-thread-safe.
     */
    void request_resume(vCPUTime* vcpu)
    {
        SCP_WARN(()) << "request resume cpu_" << vcpu->index;
        m_inst.plugin_api().qemu_plugin_cpu_request_resume(static_cast<unsigned int>(vcpu->index));
    }

    /**
     * @brief Is this cpu ahead of the pacing threshold and should be paused? Pure query.
     *
     * A peer is paced against the clock; the master is paced against its slowest peer (capped by the
     * window), never against itself, it drives the clock, so comparing it to its own checkpoint
     * latches true forever and wedges it paused with nobody able to resume it. With every peer halted
     * the window is the only bound, which is what it is for.
     */
    bool cpu_should_pause(vCPUTime* vcpu)
    {
        auto* master = m_master_vcpu.load(std::memory_order_relaxed);
        const sc_core::sc_time current_qemu_time = qemu_time_now(master);
        if (vcpu != master) {
            const sc_core::sc_time vcpu_time = cpu_time_now(vcpu);
            return vcpu_time > current_qemu_time;
        }

        vCPUTime* slowest = nullptr;
        sc_core::sc_time min_time = sc_core::SC_ZERO_TIME;
        for (int i = 0; i < m_num_vcpus; i++) {
            auto* current_cpu = get_vcpu(i);
            if (cpu_halted(current_cpu)) continue;
            const sc_core::sc_time t = cpu_time_now(current_cpu);
            if (!slowest || t < min_time) {
                min_time = t;
                slowest = current_cpu;
            }
        }

        const sc_core::sc_time threshold = slowest ? std::min(sc_current_window.to, cpu_time_now(slowest) + m_quantum)
                                                   : sc_current_window.to;
        return current_qemu_time > threshold;
    }

    /** @brief Set the SystemC synchronization window from current QEMU time (or a custom window). */
    void set_systemc_window(
        const sc_core::sc_sync_window<sc_core::sc_sync_policy_tlm_quantum>::window* custom_window = nullptr)
    {
        SCP_WARN(()) << "set_systemc_window";
        if (m_sync_sc.is_attached()) {
            if (custom_window) {
                SCP_WARN(()) << "set_systemc_window::custom_window.from= " << custom_window->from
                             << ", custom_window.to= " << custom_window->to;
                m_sync_sc.async_set_window(*custom_window);
            } else {
                sc_core::sc_time current_qemu_time = qemu_time_now();
                SCP_WARN(()) << "set_systemc_window::qemu_cpu_time_now = " << current_qemu_time
                             << ", sc_current_window.from= " << sc_current_window.from
                             << ", sc_current_window.to= " << sc_current_window.to;
                m_sync_sc.async_set_window({ current_qemu_time, (current_qemu_time + m_quantum) });
            }
        } else {
            SCP_WARN(()) << "set_systemc_window: window not attached, skipping async_set_window()";
        }
    }

    /** @brief SystemC thread: new time window ready; nudge every cpu the new threshold admits. */
    void receive_window_cb(const sc_core::sc_sync_window<sc_core::sc_sync_policy_tlm_quantum>::window& sc_w)
    {
        if (m_shutdown.load(std::memory_order_seq_cst)) return;

        m_inflight_cb.fetch_add(1, std::memory_order_seq_cst);
        /* Re-checked after the increment: shutdown may have started in the window between the load
         * above and the increment landing, in which case the drain wait has already passed us. */
        if (!m_shutdown.load(std::memory_order_seq_cst)) {
            /* Take the BQL before the pacing mutex (same order the vCPU callbacks already use). The
             * resume below kicks a vCPU via qemu_cond_broadcast(halt_cond); per cpus.c that wake is
             * only reliable while holding the BQL. This callback runs on the SystemC thread, which
             * does NOT otherwise hold the BQL, so without this a resume requested here (the only way
             * to wake a sole stopped master once every peer is halted) is lost against the master
             * sitting in qemu_cond_wait(halt_cond, &bql) -> deadlock. */
            m_inst.lock_iothread();
            {
                std::lock_guard<std::mutex> lock(m_mcips_mutex);
                sc_current_window = sc_w;

                for (int i = 0; i < m_num_vcpus; i++) {
                    auto* vcpu = get_vcpu(i);
                    if (!cpu_paused_or_pending(vcpu)) continue;
                    if (!cpu_should_pause(vcpu)) {
                        request_resume(vcpu);
                    }
                }

                if (m_master_vcpu.load(std::memory_order_relaxed)) {
                    set_systemc_window();
                }
            }
            m_inst.unlock_iothread();
        }
        m_inflight_cb.fetch_sub(1, std::memory_order_seq_cst);
    }

    /** @brief Called under BQL before simulation starts; does not hold m_mcips_mutex. */
    void vcpu_init(unsigned int cpu_index)
    {
        if (m_shutdown.load(std::memory_order_acquire)) return;

        const int current = m_inst.plugin_api().qemu_plugin_num_vcpus();
        if (current > m_num_vcpus) m_num_vcpus = current;

        vCPUTime* vcpu = get_vcpu(cpu_index);
        vcpu->index = cpu_index;
        if (vcpu->insn_per_second == 0) {
            vcpu->insn_per_second = 1'000'000'000;
        }
        vcpu->delta_insn = 0;
        vcpu->cpu_time = sc_core::SC_ZERO_TIME;

        if (m_master_vcpu.load(std::memory_order_relaxed) == nullptr) {
            m_master_vcpu.store(vcpu, std::memory_order_release);
        }
    }

    /** @brief vCPU's own thread, quota exhausted: credit its delta, pace it, refresh the window. */
    void cpu_end_delta_quota(vCPUTime* vcpu)
    {
        SCP_WARN(()) << "cpu_end_delta_quota: cpu_" << vcpu->index;

        std::lock_guard<std::mutex> lock(m_mcips_mutex);

        const sc_core::sc_time delta = cpu_delta_time(vcpu);
        if (vcpu == m_master_vcpu.load(std::memory_order_relaxed)) {
            m_qemu_time += delta;
            sync_qemu_time_ns();
        }
        vcpu->cpu_time += delta;
        vcpu->delta_insn = 0;

        if (!cpu_pause_pending(vcpu) && cpu_should_pause(vcpu)) {
            request_pause(vcpu);
        }
        for (int i = 0; i < m_num_vcpus; i++) {
            auto* other = get_vcpu(i);
            if (other == vcpu) continue;
            if (!cpu_paused_or_pending(other)) continue; /* nothing to resume (subsumes halted) */
            if (!cpu_should_pause(other)) {
                request_resume(other);
            }
        }

        set_systemc_window();
    }

    /** @brief Called when a CPU has run at least global_quantum instructions. */
    void vcpu_tb_exec_cond(unsigned int cpu_index, void* /*udata*/)
    {
        if (m_shutdown.load(std::memory_order_acquire)) return;

        vCPUTime* vcpu = get_vcpu(cpu_index);
        /* The classification is re-checked under the mutex in cpu_end_delta_quota. */
        cpu_end_delta_quota(vcpu);
    }

    /** @brief Called when QEMU translates a block; installs instruction counting + quota check. */
    void vcpu_tb_trans(qemu_plugin_tb* tb)
    {
        const size_t n_insns = m_inst.plugin_api().qemu_plugin_tb_n_insns(tb);
        qemu_plugin_u64 delta_insn = qemu_plugin_scoreboard_u64_in_struct(m_vcpus_scoreboard, vCPUTime, delta_insn);

        m_inst.plugin_api().qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(tb, QEMU_PLUGIN_INLINE_ADD_U64,
                                                                              delta_insn, n_insns);

        m_inst.plugin_api().qemu_plugin_register_vcpu_tb_exec_cond_cb(
            tb,
            [](unsigned int cpu_index, void* userdata) {
                LibQemuPlugin::dispatch_userdata(userdata, [&](LibQemuPlugin* p) {
                    static_cast<McipsPlugin*>(p)->vcpu_tb_exec_cond(cpu_index, userdata);
                });
            },
            QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_COND_GE, delta_insn, m_global_quantum, handle_as_userdata());
    }

    /**
     * @brief idle_cb: cpu entered QEMU's idle path.
     */
    void vcpu_idle(unsigned int cpu_index)
    {
        if (m_shutdown.load(std::memory_order_acquire)) return;

        std::lock_guard<std::mutex> lock(m_mcips_mutex);

        vCPUTime* vcpu = get_vcpu(cpu_index);
        const sc_core::sc_time delta = cpu_delta_time(vcpu);
        vcpu->cpu_time += delta;
        vcpu->delta_insn = 0;

        auto* master = m_master_vcpu.load(std::memory_order_relaxed);
        if (vcpu == master) {
            m_qemu_time += delta;
            sync_qemu_time_ns();
        }

        for (int i = 0; i < m_num_vcpus; i++) {
            auto* other = get_vcpu(i);
            if (other == vcpu) continue;
            if (!cpu_paused_or_pending(other)) continue; /* nothing to resume (subsumes halted) */
            if (!cpu_should_pause(other)) {
                request_resume(other);
            }
        }
        if (!cpu_halted(vcpu)) {
            /* Paused, not halted: it may still run once resumed, so it is not our concern here. */
            SCP_WARN(()) << "vcpu_idle: cpu_" << cpu_index << " not halted -> paused, ignoring";
            return;
        }

        if (vcpu == master) {
            master = select_new_master_vcpu();
            if (!master) {
                detach_sync_window();
                return;
            }
            /* A paused pick drives the clock only once woken; resume it unless pacing says it is
             * ahead (safe: caller is halted). */
            if (cpu_paused_or_pending(master) && !cpu_should_pause(master)) request_resume(master);
        }
        set_systemc_window();
    }

    /**
     * @brief resume_cb: cpu left QEMU's idle path.
     */
    void vcpu_resume(unsigned int cpu_index)
    {
        if (m_shutdown.load(std::memory_order_acquire)) return;

        std::lock_guard<std::mutex> lock(m_mcips_mutex);
        vCPUTime* vcpu = get_vcpu(cpu_index);

        if (cpu_halted(vcpu)) {
            if (!m_sync_sc.is_attached()) {
                const sc_core::sc_time sc_now = sc_core::sc_time_stamp();
                if (sc_now > m_qemu_time) {
                    m_qemu_time = sc_now;
                    sync_qemu_time_ns();
                }
                attach_sync_window();
            }
            vcpu->cpu_time = qemu_time_now();

            if (!m_master_vcpu.load(std::memory_order_relaxed)) {
                m_master_vcpu.store(vcpu, std::memory_order_release);
            }

        } else if (!cpu_pause_pending(vcpu) && cpu_should_pause(vcpu)) {
            request_pause(vcpu);
        }

        for (int i = 0; i < m_num_vcpus; i++) {
            auto* other = get_vcpu(i);
            if (other == vcpu) continue;
            if (!cpu_paused_or_pending(other)) continue; /* nothing to resume (subsumes halted) */
            if (!cpu_should_pause(other)) {
                request_resume(other);
            }
        }
        set_systemc_window();
    }

    /** @brief Current sim time in ns for QEMU's iothread; lock-free (atomic shadow + master delta). */
    int64_t get_qemu_clock(void* /*userdata*/)
    {
        // SCP_WARN(()) << "get_qemu_clock";
        if (m_shutdown.load(std::memory_order_acquire)) {
            return static_cast<int64_t>(sc_core::sc_time_stamp().to_seconds() * NSEC_IN_ONE_SEC);
        }

        auto master = m_master_vcpu.load(std::memory_order_acquire);
        int64_t qemu_time = m_qemu_time_ns.load(std::memory_order_acquire);

        if (!master) {
            std::lock_guard<std::mutex> lock(m_mcips_mutex);
            /* No cpu is driving the clock: feed QEMU SystemC time. The rtl advances SystemC while we
             * are detached, so this is what keeps the clock moving and lets the iothread fire due
             * VIRTUAL timers to wake an idle cpu. Never report backwards: only clamp forwards. */
            const int64_t systemc_time_now = static_cast<int64_t>(sc_core::sc_time_stamp().to_seconds() *
                                                                  NSEC_IN_ONE_SEC);
            if (systemc_time_now > qemu_time) {
                const int64_t warp_time = systemc_time_now - qemu_time;
                for (int i = 0; i < m_num_vcpus; i++) {
                    vCPUTime* vcpu = get_vcpu(i);
                    if (cpu_paused_or_pending(vcpu)) {
                        vcpu->cpu_time += sc_core::sc_time(static_cast<double>(warp_time), sc_core::SC_NS);
                    }
                }
                qemu_time += warp_time;
                m_qemu_time += sc_core::sc_time(static_cast<double>(warp_time), sc_core::SC_NS);
                sync_qemu_time_ns();
            }
        } else {
            // delta_insn / insn_per_second are race-free to read here.
            qemu_time += static_cast<int64_t>(cpu_delta_time(master).to_seconds() * NSEC_IN_ONE_SEC);
        }

        return qemu_time;
    }

    /** @brief JSON status for the monitor (data-only, unlocked — values may be slightly stale). */
    std::string get_mcips_status_json()
    {
        auto master = m_master_vcpu.load(std::memory_order_relaxed);
        std::ostringstream os;

        const uint64_t qemu_time = static_cast<uint64_t>(qemu_time_now().to_seconds() * NSEC_IN_ONE_SEC);
        const uint64_t sc_ns = static_cast<uint64_t>(sc_core::sc_time_stamp().to_seconds() * NSEC_IN_ONE_SEC);

        os << "{" << "\"name\":\"" << name() << "\"," << "\"qemu_time\":\"" << qemu_time << " ns\","
           << "\"qemu_time_ns\":" << qemu_time << "," << "\"sc_time_ns\":" << sc_ns << ","
           << "\"n_cpus\":" << m_num_vcpus << ","
           << "\"master_vcpu_index\":" << (master ? static_cast<int64_t>(master->index) : -1) << "," << "\"vcpus\":[";

        bool first = true;
        for (int i = 0; i < m_num_vcpus; i++) {
            auto* vcpu = get_vcpu(i);
            if (!vcpu) continue;

            if (!first) os << ",";
            first = false;

            const uint64_t ns = static_cast<uint64_t>(vcpu->cpu_time.to_seconds() * NSEC_IN_ONE_SEC);
            const bool q_stopped = cpu_stopped(vcpu);
            const bool q_halted = cpu_halted(vcpu);
            const bool q_stop = cpu_pause_pending(vcpu);

            os << "{" << "\"index\":" << vcpu->index << ","
               << "\"is_master\":" << ((master && master->index == vcpu->index) ? 1 : 0) << ","
               << "\"insn_per_second\":\"" << vcpu->insn_per_second << "\"," << "\"delta_insn\":\"" << vcpu->delta_insn
               << "\"," << "\"cpu_time_ns\":\"" << ns << "\"," << "\"cpu_time_s\":" << vcpu->cpu_time.to_seconds()
               << "," << "\"q_stopped\":" << (q_stopped ? 1 : 0) << ","
               << "\"q_stop\":" << (q_stop ? 1 : 0) << "," << "\"q_halted\":" << (q_halted ? 1 : 0) << "}";
        }
        os << "]}";

        return os.str();
    }
};

#endif //_LIBQBOX_COMPONENTS_MCIPS_PLUGIN_H
