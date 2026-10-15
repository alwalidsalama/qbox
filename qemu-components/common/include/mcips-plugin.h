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
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <sstream>

class McipsPlugin : public LibQemuPlugin
{
    SCP_LOGGER();
    SC_HAS_PROCESS(McipsPlugin);

private:
    struct vCPUTime {
        unsigned int index; /* QEMU vCPU index, the type the plugin API takes */
        /* This plugin holds a pause on this cpu. Tracked because cpu->stop says a cpu is stopped,
         * not who stopped it. In the padding after index, so delta_insn keeps its cond_cb offset. */
        bool paused_by_us;
        uint64_t insn_per_second;
        uint64_t delta_insn;
        sc_core::sc_time cpu_time;
    };

    using sync_window = sc_core::sc_sync_window<sc_core::sc_sync_policy_tlm_quantum>;

    /* Quanta of pacing silence after which a clock master is taken to have stopped driving the
     * clock. Generous: tripping early costs a detached window, a missed stall costs the run. */
    static constexpr unsigned STALLED_CLOCK_TICKS = 50;

    /* TLM global quantum in ns, reused as the instruction quota for the tb_exec_cond_cb
     * immediate. One value for every cpu, so a per-cpu quota is not expressible here. */
    uint64_t m_global_quantum;
    int m_num_vcpus{ 0 };                  // cpu count, refreshed by refresh_num_vcpus()
    void* m_time_handle;                   // handle returned by QEMU to let us control time
    sync_window m_sync_sc;                 // SystemC sync window
    sync_window::window sc_current_window; // last window received from SystemC
    sc_core::sc_time m_quantum;            // copy of the TLM global quantum
    sc_core::sc_time m_qemu_time;          // main QEMU simulation time (only changed under m_mcips_mutex)
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

    /* Idle drift: while no cpu drives the virtual clock the plugin moves SystemC itself, on a
     * host-clock-paced tick. A null m_master_vcpu is what turns it on. */
    cci::cci_param<double> p_idle_drift_quantum_ms;
    gs::async_event m_idle_tick;
    std::thread m_idle_drift_thread;
    std::mutex m_drift_mutex; // only ever guards the tick thread's sleep
    std::condition_variable m_drift_cv;

    /* Bumped whenever QEMU moves the clock or SystemC grants a window. The drift thread watches
     * it to tell a master that is driving the clock from one that only looks like it is. */
    std::atomic<uint64_t> m_progress{ 0 };

public:
    McipsPlugin(const sc_core::sc_module_name& nm, qemu::LibQemu& inst)
        : LibQemuPlugin(nm, inst)
        , m_sync_sc("m_sync_sc")
        , m_global_quantum(0)
        , m_time_handle(nullptr)
        , sc_current_window(sync_window::zero_window)
        , m_quantum(sc_core::SC_ZERO_TIME)
        , m_qemu_time(sc_core::SC_ZERO_TIME)
        , m_master_vcpu(nullptr)
        , m_vcpus_scoreboard(nullptr)
        , p_idle_drift_quantum_ms("idle_drift_quantum_ms", 1.0,
                                  "Realtime tick rate, in milliseconds, at which SystemC is drifted "
                                  "forward while no vCPU is driving the virtual clock")
        , m_idle_tick(false) // never attach-suspending, or a clean end of simulation would hang
    {
        SC_METHOD(idle_tick_method);
        dont_initialize();
        sensitive << m_idle_tick;
    }

    /** @brief The drift tick fired. Empty: being scheduled ahead is what moved SystemC. */
    void idle_tick_method() {}

    /**
     * @brief Drift thread: drift SystemC while no cpu drives the clock, and give the clock up if
     *        its master has stopped driving it. A real thread; see the idle drift members.
     */
    void idle_drift_loop()
    {
        /* Floor the configured rate rather than honour it: a zero or negative tick rate turns
         * the wait below into a spin and pegs a host core. */
        const auto step = std::chrono::microseconds(
            static_cast<int64_t>(std::max(0.01, p_idle_drift_quantum_ms.get_value()) * 1000.0));

        auto last_rt = std::chrono::steady_clock::now();
        uint64_t last_progress = 0;
        unsigned stalled_ticks = 0;

        while (!m_shutdown.load(std::memory_order_acquire)) {
            {
                /* m_drift_mutex guards nothing but this wait, and is dropped before the tick
                 * below, which takes m_mcips_mutex. */
                std::unique_lock<std::mutex> lock(m_drift_mutex);
                m_drift_cv.wait_for(lock, step);
            }
            if (m_shutdown.load(std::memory_order_acquire)) break;

            /* Measure the slice rather than assume it: the drift is by elapsed wall time, so
             * SystemC tracks realtime with no baseline to correct against. */
            const auto now = std::chrono::steady_clock::now();
            const auto slice = std::chrono::duration_cast<std::chrono::microseconds>(now - last_rt);
            last_rt = now;

            /* A stopped master leaves nobody to extend the window, so SystemC suspends on the one
             * it holds and the callback that would re-elect a master never runs again. */
            const uint64_t progress = m_progress.load(std::memory_order_acquire);
            if (progress != last_progress) {
                last_progress = progress;
                stalled_ticks = 0;
            } else if (m_master_vcpu.load(std::memory_order_acquire) && ++stalled_ticks >= STALLED_CLOCK_TICKS) {
                stalled_ticks = 0;

                /* BQL before m_mcips_mutex, the order receive_window_cb() uses. */
                m_inst.plugin_api().qemu_plugin_bql_lock();
                {
                    std::lock_guard<std::mutex> lock(m_mcips_mutex);

                    /* Re-checked under the mutex: the clock may have moved while we took it. */
                    vCPUTime* stalled = master();
                    if (stalled && m_progress.load(std::memory_order_acquire) == last_progress) {
                        SCP_WARN(()) << "cpu_" << stalled->index << " holds the virtual clock but has stopped "
                                     << "driving it; releasing the clock and the sync window";
                        set_master(nullptr); // which is also what turns the drift below on
                        detach_sync_window();
                        catch_up_to_systemc();

                        /* Clock up to SystemC first, so peers wake behind it, then every pause goes
                         * back: only a window callback lifts one, and the window is gone. */
                        for (int i = 0; i < m_num_vcpus; i++) {
                            vCPUTime* vcpu = get_vcpu(i);
                            vcpu->cpu_time = m_qemu_time;
                            request_resume(vcpu);
                        }
                    }
                }
                m_inst.plugin_api().qemu_plugin_bql_unlock();
            }

            /* Read after the stall check, which may just have cleared the master. Shutdown too: a
             * tick issued then would reach an event this thread is about to be joined ahead of. */
            if (!m_shutdown.load(std::memory_order_acquire) && !m_master_vcpu.load(std::memory_order_acquire) &&
                slice.count() > 0) {
                m_idle_tick.notify(sc_core::sc_time(static_cast<double>(slice.count()), sc_core::SC_US));
            }
        }
    }

    /** @brief Publish the clock master. A null master is what turns the idle drift on. */
    void set_master(vCPUTime* vcpu) { m_master_vcpu.store(vcpu, std::memory_order_release); }

    /** @brief The cpu driving the clock, or nullptr. For callers holding m_mcips_mutex. */
    vCPUTime* master() { return m_master_vcpu.load(std::memory_order_relaxed); }

    /** @brief Shutdown for the destructor and end_of_simulation(). Idempotent. */
    void shutdown_cleanup()
    {
        if (m_shutdown.load(std::memory_order_seq_cst)) {
            return; // already shut down
        }

        m_shutdown.store(true, std::memory_order_seq_cst); // must be seq_cst before inflight drain
        m_master_vcpu.store(nullptr, std::memory_order_release);

        /* The thread has to be joined before the event it notifies goes away, so wake it out of
         * its sleep rather than wait for the shutdown flag to stop it ticking. */
        m_drift_cv.notify_all();
        if (m_idle_drift_thread.joinable()) {
            m_idle_drift_thread.join();
        }

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

    /** @brief Set up the plugin and register the QEMU callbacks. */
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
        /* Left detached: qemu_plugin_num_vcpus() still reports 0 here, so a window set now would
         * pin SystemC at the end of the first quantum for the whole run. First vcpu_init attaches. */
        detach_sync_window();
        refresh_num_vcpus();
        m_sync_sc.register_sync_cb(std::bind(&McipsPlugin::receive_window_cb, this, std::placeholders::_1));

        /* No cpu exists yet, so nothing can drive the virtual clock, and a null master is what
         * starts the thread drifting. The first vcpu_init takes the role and stops it. */
        m_idle_drift_thread = std::thread(&McipsPlugin::idle_drift_loop, this);

        /* Capture-less lambdas decay to the C function pointer the API expects, and forward
         * through dispatch_userdata: a raw `this` would dangle on a callback after destruction. */
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

    /** @brief This cpu's scoreboard entry. */
    vCPUTime* get_vcpu(unsigned int cpu_index)
    {
        return reinterpret_cast<vCPUTime*>(
            m_inst.plugin_api().qemu_plugin_scoreboard_find(m_vcpus_scoreboard, cpu_index));
    }

    /** @brief Re-read QEMU's cpu count, which only ever grows. @return the count. */
    int refresh_num_vcpus()
    {
        const int n = m_inst.plugin_api().qemu_plugin_num_vcpus();
        if (n > m_num_vcpus) m_num_vcpus = n;
        return m_num_vcpus;
    }

    /* QEMU ground-truth state derivation */

    /** @brief QEMU has this cpu paused (cpu->stopped). */
    bool cpu_stopped(const vCPUTime* vcpu) { return m_inst.plugin_api().qemu_plugin_cpu_stopped(vcpu->index); }
    /** @brief QEMU has this cpu halted (WFI/WFE/powered off). */
    bool cpu_halted(const vCPUTime* vcpu) { return m_inst.plugin_api().qemu_plugin_cpu_halted(vcpu->index); }
    /** @brief A pause is requested on this cpu but not yet acknowledged (cpu->stop). */
    bool cpu_stop_requested(const vCPUTime* vcpu) { return m_inst.plugin_api().qemu_plugin_cpu_stop(vcpu->index); }

    /** @brief Drop a pause record for a cpu seen running again. Cpu's own thread, BQL held. */
    void release_stale_pause(vCPUTime* vcpu)
    {
        /* A reset's resume_all_vcpus() lifts the pause with no callback; left standing, the record
         * would have request_resume() decline the cpu forever. Only here do the two flags agree. */
        if (vcpu->paused_by_us && !cpu_stop_requested(vcpu) && !cpu_stopped(vcpu)) {
            SCP_TRACE(()) << "cpu_" << vcpu->index << " was resumed from outside, dropping our pause";
            vcpu->paused_by_us = false;
        }
    }

    /** @brief Could this cpu still advance the virtual clock, if given the chance? */
    bool cpu_can_drive_clock(const vCPUTime* vcpu)
    {
        if (cpu_halted(vcpu)) return false;
        if (!cpu_stop_requested(vcpu) && !cpu_stopped(vcpu)) return true;
        /* Our own pause does not disqualify it, nor does a global pause, which lasts only until the
         * pairing resume_all_vcpus(). A pause that is neither is not ours to lift. */
        if (vcpu->paused_by_us) return true;
        return m_inst.plugin_api().qemu_plugin_vcpus_globally_paused();
    }

    /** @brief Pull m_qemu_time up to SystemC time if SystemC is ahead. Mutex held. */
    void catch_up_to_systemc()
    {
        const sc_core::sc_time sc_now = sc_core::sc_time_stamp();
        if (sc_now > m_qemu_time) {
            m_qemu_time = sc_now;
            sync_qemu_time_ns();
        }
    }

    /**
     * @brief Pace a cpu that has reached an accounting boundary: pause it if it is ahead, or give
     *        it the clock if nobody holds it. Mutex held.
     */
    void pace_vcpu(vCPUTime* vcpu)
    {
        if (!master()) {
            /* Nobody holds the clock, so this cpu takes it, rebasing onto SystemC and reseeding the
             * window: paced against a stale sc_current_window.to it would pause itself on its way in. */
            catch_up_to_systemc();
            attach_sync_window();
            vcpu->cpu_time = qemu_time_now();
            set_master(vcpu);

            sc_current_window = { m_qemu_time, m_qemu_time + m_quantum };
            set_systemc_window();
            /* Deliberately not paced on the same pass: a pause taken with no master is one nothing
             * lifts, the window that would admit the cpu being detached. */
            return;
        }

        if (!vcpu->paused_by_us && cpu_should_pause(vcpu)) {
            request_pause(vcpu);
        }
    }

    /**
     * @brief Hand the clock to another cpu, or drop it along with the window and every pause.
     *        Mutex held, the outgoing master already stopped. @return the new master, or nullptr.
     */
    vCPUTime* hand_off_clock()
    {
        /* The one way the clock changes hands, and a live master is never displaced. Cleared first
         * so the scan below cannot pick it again. */
        set_master(nullptr);

        for (int i = 0; i < m_num_vcpus; i++) {
            vCPUTime* pick = get_vcpu(i);
            if (!cpu_can_drive_clock(pick)) continue;

            /* Start from the current clock, dropping whatever lead or lag the checkpoint carried. */
            pick->cpu_time = m_qemu_time;
            pick->delta_insn = 0;
            set_master(pick);
            if (!cpu_should_pause(pick)) {
                request_resume(pick); // declines by itself if the pause is not ours
            }
            return pick;
        }

        /* Nothing would ever lift a pause of ours again: the window is detached here, and the stall
         * watch only fires on an elected master. A cpu paused and halted now was skipped above. */
        detach_sync_window();
        for (int i = 0; i < m_num_vcpus; i++) {
            request_resume(get_vcpu(i));
        }
        return nullptr;
    }

    void attach_sync_window()
    {
        if (!m_sync_sc.is_attached()) m_sync_sc.attach();
    }

    void detach_sync_window()
    {
        if (m_sync_sc.is_attached()) m_sync_sc.detach(m_qemu_time);
    }

    /** @brief Set insn/sec for a cpu. @return false if it is not in the scoreboard. */
    bool set_vcpu_insn_per_second(unsigned int cpu_index, uint64_t insn_per_second)
    {
        if (insn_per_second == 0) {
            SCP_FATAL(()) << "insn_per_second must be > 0 (cpu_" << cpu_index << ")";
            return false; /* SC report actions are configurable, so a fatal may not abort */
        }
        if (cpu_index >= static_cast<unsigned int>(refresh_num_vcpus())) {
            return false;
        }

        vCPUTime* vcpu = get_vcpu(cpu_index);
        if (!vcpu) {
            return false;
        }

        vcpu->insn_per_second = insn_per_second;
        return true;
    }

    /** @brief Copy m_qemu_time into the atomic nanosecond field, and record the move. Mutex held. */
    void sync_qemu_time_ns()
    {
        m_qemu_time_ns.store(static_cast<int64_t>(m_qemu_time.to_seconds() * NSEC_IN_ONE_SEC),
                             std::memory_order_release);
        m_progress.fetch_add(1, std::memory_order_release);
    }

    /** @brief Time represented by the in-flight instructions in delta_insn. */
    static sc_core::sc_time cpu_delta_time(const vCPUTime* vcpu)
    {
        sc_assert(vcpu && "cpu_delta_time called with null vCPU");
        return sc_core::sc_time(static_cast<double>(vcpu->delta_insn) / vcpu->insn_per_second, sc_core::SC_SEC);
    }

    /** @brief Time for a cpu: its checkpoint plus its in-flight delta. Mutex held. */
    sc_core::sc_time cpu_time_now(const vCPUTime* vcpu) { return vcpu->cpu_time + cpu_delta_time(vcpu); }

    /** @brief Current QEMU time: the clock plus the master's in-flight delta. */
    sc_core::sc_time qemu_time_now()
    {
        const vCPUTime* m = m_master_vcpu.load(std::memory_order_acquire);
        return m ? m_qemu_time + cpu_delta_time(m) : m_qemu_time;
    }

    /** @brief Pause a cpu for pacing. Caller must credit delta_insn first. */
    void request_pause(vCPUTime* vcpu)
    {
        SCP_TRACE(()) << "request pause cpu_" << vcpu->index;
        vcpu->paused_by_us = true;
        m_inst.plugin_api().qemu_plugin_cpu_request_pause(vcpu->index);
    }

    /**
     * @brief Ask a cpu the plugin paused to resume. Caller holds the BQL then m_mcips_mutex: the
     *        request kicks the target with a bare qemu_cond_broadcast() a waiter could miss.
     */
    void request_resume(vCPUTime* vcpu)
    {
        /* Declines anything the plugin does not own, a resume clearing cpu->stop whoever set it.
         * Nothing is lost by declining a global pause, whose resume restarts every cpu. */
        if (!vcpu->paused_by_us) {
            SCP_TRACE(()) << "resume cpu_" << vcpu->index << " declined, not paused by us";
            return;
        }
        if (m_inst.plugin_api().qemu_plugin_vcpus_globally_paused()) {
            SCP_TRACE(()) << "resume cpu_" << vcpu->index << " declined, global pause in effect";
            return;
        }
        SCP_TRACE(()) << "request resume cpu_" << vcpu->index;
        vcpu->paused_by_us = false;
        m_inst.plugin_api().qemu_plugin_cpu_request_resume(vcpu->index);
    }

    /**
     * @brief Is this cpu far enough ahead of the pacing to be paused? Pure query.
     * @param treat_halted cpu to leave out of the slowest-peer scan
     */
    bool cpu_should_pause(vCPUTime* vcpu, vCPUTime* treat_halted = nullptr)
    {
        const sc_core::sc_time current_qemu_time = qemu_time_now();
        if (vcpu != master()) {
            return cpu_time_now(vcpu) > current_qemu_time; /* a peer is paced against the clock */
        }

        /* Paced against its slowest peer, never itself, which would latch true forever. Only peers
         * that can still drive the clock count: a stalled checkpoint would pin the master. */
        bool found = false;
        sc_core::sc_time min_time = sc_core::SC_ZERO_TIME;
        for (int i = 0; i < m_num_vcpus; i++) {
            vCPUTime* peer = get_vcpu(i);
            if (peer == treat_halted) continue;
            if (!cpu_can_drive_clock(peer)) continue;
            const sc_core::sc_time t = cpu_time_now(peer);
            if (!found || t < min_time) {
                min_time = t;
                found = true;
            }
        }

        /* With no such peer left the window is the only bound, which is what it is for. */
        const sc_core::sc_time threshold = found ? std::min(sc_current_window.to, min_time + m_quantum)
                                                 : sc_current_window.to;
        return current_qemu_time > threshold;
    }

    /**
     * @brief Resume every paused cpu the pacing no longer wants paused. Mutex held.
     * @param skip         cpu to leave alone, normally the caller
     * @param treat_halted passed through to cpu_should_pause()
     */
    void resume_ready_cpus(const vCPUTime* skip = nullptr, vCPUTime* treat_halted = nullptr)
    {
        for (int i = 0; i < m_num_vcpus; i++) {
            vCPUTime* vcpu = get_vcpu(i);
            if (vcpu == skip) continue;
            /* Nothing to resume, which subsumes halted. Also keeps the O(n) pacing query below
             * off the cpus that have no use for it. */
            if (!cpu_stopped(vcpu) && !vcpu->paused_by_us) continue;
            if (!cpu_should_pause(vcpu, treat_halted)) {
                request_resume(vcpu);
            }
        }
    }

    /**
     * @brief Carry the checkpoints of cpus that cannot drive the clock forward with it. Mutex
     *        held, once per quantum, forwards only.
     */
    void rebase_idle_checkpoints()
    {
        /* Such a checkpoint stands still while the clock does not, so the cpu would rejoin as the
         * slowest peer by the whole stretch it sat out. It is not behind on work it never had. */
        for (int i = 0; i < m_num_vcpus; i++) {
            vCPUTime* vcpu = get_vcpu(i);
            if (cpu_can_drive_clock(vcpu)) continue;
            if (vcpu->cpu_time < m_qemu_time) {
                vcpu->cpu_time = m_qemu_time;
            }
        }
    }

    /** @brief Extend the SystemC window to one quantum past the current QEMU time. */
    void set_systemc_window()
    {
        if (!m_sync_sc.is_attached()) {
            SCP_TRACE(()) << "set_systemc_window: window not attached, skipping async_set_window()";
            return;
        }

        const sc_core::sc_time now = qemu_time_now();
        SCP_TRACE(()) << "set_systemc_window: qemu_time_now= " << now << ", sc_current_window= ["
                      << sc_current_window.from << ", " << sc_current_window.to << "]";
        m_sync_sc.async_set_window({ now, now + m_quantum });
    }

    /** @brief SystemC thread: new time window ready; nudge every cpu the new threshold admits. */
    void receive_window_cb(const sync_window::window& sc_w)
    {
        if (m_shutdown.load(std::memory_order_seq_cst)) return;

        m_inflight_cb.fetch_add(1, std::memory_order_seq_cst);
        /* Re-checked after the increment: shutdown may have started in the window between the load
         * above and the increment landing, in which case the drain wait has already passed us. */
        if (!m_shutdown.load(std::memory_order_seq_cst)) {
            /* BQL before the pacing mutex, the order the vCPU callbacks use. The resume below kicks
             * a vCPU via qemu_cond_broadcast(halt_cond), reliable only under the BQL. */
            m_inst.plugin_api().qemu_plugin_bql_lock();
            {
                std::lock_guard<std::mutex> lock(m_mcips_mutex);
                sc_current_window = sc_w;
                m_progress.fetch_add(1, std::memory_order_release);
                rebase_idle_checkpoints();

                /* Revalidate the master: a halt signal or a vm-stop comes with no callback, and such
                 * a master left elected pins the window on a clock that never advances again. */
                vCPUTime* m = master();
                if (m && !cpu_can_drive_clock(m)) {
                    m = hand_off_clock();
                }
                if (!m) {
                    detach_sync_window();
                }

                resume_ready_cpus();

                if (m) {
                    set_systemc_window();
                }
            }
            m_inst.plugin_api().qemu_plugin_bql_unlock();
        }
        m_inflight_cb.fetch_sub(1, std::memory_order_seq_cst);
    }

    /** @brief A cpu came into existence. Called under BQL; takes m_mcips_mutex. */
    void vcpu_init(unsigned int cpu_index)
    {
        if (m_shutdown.load(std::memory_order_acquire)) return;

        std::lock_guard<std::mutex> lock(m_mcips_mutex);
        refresh_num_vcpus();

        vCPUTime* vcpu = get_vcpu(cpu_index);
        vcpu->index = cpu_index;
        if (vcpu->insn_per_second == 0) {
            vcpu->insn_per_second = 1'000'000'000;
        }
        vcpu->delta_insn = 0;
        vcpu->cpu_time = m_qemu_time;
        vcpu->paused_by_us = false;

        /* The first cpu becomes the master and attaches the window end_of_elaboration left
         * detached, having had no cpu yet to extend it with. */
        if (!master()) {
            set_master(vcpu);
            attach_sync_window();
            set_systemc_window();
        }
    }

    /**
     * @brief A cpu ran out its instruction quota, on its own thread: credit the delta, pace,
     *        extend the window. Registered by vcpu_tb_trans() as the conditional TB callback.
     */
    void vcpu_quota_expired(unsigned int cpu_index)
    {
        if (m_shutdown.load(std::memory_order_acquire)) return;

        SCP_TRACE(()) << "vcpu_quota_expired: cpu_" << cpu_index;

        /* BQL before the pacing mutex. This runs from translated code, where the accelerator has
         * released the BQL, so the resumes below must take it or lose the wake they issue. */
        m_inst.plugin_api().qemu_plugin_bql_lock();
        {
            std::lock_guard<std::mutex> lock(m_mcips_mutex);
            vCPUTime* vcpu = get_vcpu(cpu_index);

            /* Reaching a pacing point is proof this cpu is running, so any pause of ours it still
             * carries was lifted by something else. Dropped before the pacing below reads it. */
            release_stale_pause(vcpu);

            const sc_core::sc_time delta = cpu_delta_time(vcpu);
            if (vcpu == master()) {
                m_qemu_time += delta;
                sync_qemu_time_ns();
            }
            vcpu->cpu_time += delta;
            vcpu->delta_insn = 0;

            pace_vcpu(vcpu);
            resume_ready_cpus(vcpu);

            set_systemc_window();
        }
        m_inst.plugin_api().qemu_plugin_bql_unlock();
    }

    /** @brief QEMU translated a block; install the instruction count and the quota check. */
    void vcpu_tb_trans(qemu_plugin_tb* tb)
    {
        const size_t n_insns = m_inst.plugin_api().qemu_plugin_tb_n_insns(tb);
        qemu_plugin_u64 delta_insn = qemu_plugin_scoreboard_u64_in_struct(m_vcpus_scoreboard, vCPUTime, delta_insn);

        m_inst.plugin_api().qemu_plugin_register_vcpu_tb_exec_inline_per_vcpu(tb, QEMU_PLUGIN_INLINE_ADD_U64,
                                                                              delta_insn, n_insns);

        m_inst.plugin_api().qemu_plugin_register_vcpu_tb_exec_cond_cb(
            tb,
            [](unsigned int cpu_index, void* userdata) {
                LibQemuPlugin::dispatch_userdata(
                    userdata, [&](LibQemuPlugin* p) { static_cast<McipsPlugin*>(p)->vcpu_quota_expired(cpu_index); });
            },
            QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_COND_GE, delta_insn, m_global_quantum, handle_as_userdata());
    }

    /** @brief idle_cb: cpu entered QEMU's idle path. */
    void vcpu_idle(unsigned int cpu_index)
    {
        if (m_shutdown.load(std::memory_order_acquire)) return;

        std::lock_guard<std::mutex> lock(m_mcips_mutex);

        vCPUTime* vcpu = get_vcpu(cpu_index);
        const sc_core::sc_time delta = cpu_delta_time(vcpu);
        vcpu->cpu_time += delta;
        vcpu->delta_insn = 0;

        const bool was_master = (vcpu == master());
        if (was_master) {
            m_qemu_time += delta;
            sync_qemu_time_ns();
        }

        /* This cpu is already on its way into QEMU's idle path, so keep its transitional state
         * from holding a stopped clock master paused. */
        resume_ready_cpus(vcpu, vcpu);

        if (!cpu_halted(vcpu)) {
            /* Paused, not halted: it may still run once resumed, so it is not our concern here. */
            SCP_TRACE(()) << "vcpu_idle: cpu_" << cpu_index << " not halted, paused instead, ignoring";
            return;
        }

        if (was_master && !hand_off_clock()) {
            return; /* nobody left to drive the clock; the window is detached */
        }
        set_systemc_window();
    }

    /** @brief resume_cb: cpu left QEMU's idle path. */
    void vcpu_resume(unsigned int cpu_index)
    {
        if (m_shutdown.load(std::memory_order_acquire)) return;

        std::lock_guard<std::mutex> lock(m_mcips_mutex);
        vCPUTime* vcpu = get_vcpu(cpu_index);

        /* Left the idle path, so whatever had it stopped is over. If that was a pause of ours
         * taken away from outside, this is where the record of it goes. */
        release_stale_pause(vcpu);

        if (cpu_halted(vcpu)) {
            /* Still halted, so not about to retire anything: just put its checkpoint back on the
             * clock, and take the window back if the clock was given up while it was away. */
            if (!m_sync_sc.is_attached()) {
                catch_up_to_systemc();
                attach_sync_window();
            }
            vcpu->cpu_time = qemu_time_now();
            if (!master()) {
                set_master(vcpu);
            }
        } else {
            pace_vcpu(vcpu);
        }

        resume_ready_cpus(vcpu);
        set_systemc_window();
    }

    /**
     * @brief A cpu was halted from outside QEMU, by a SystemC halt signal. SystemC thread, once
     *        the halt has reached QEMU; hands the clock on if this was the master.
     */
    void external_halt(unsigned int cpu_index)
    {
        if (m_shutdown.load(std::memory_order_acquire)) return;

        /* BQL before the pacing mutex, the order every other callback here uses. Needed because
         * handing the clock on can involve resuming the cpu it is handed to. */
        m_inst.plugin_api().qemu_plugin_bql_lock();
        {
            std::lock_guard<std::mutex> lock(m_mcips_mutex);
            vCPUTime* vcpu = get_vcpu(cpu_index);
            SCP_TRACE(()) << "external_halt: cpu_" << cpu_index;

            /* Halting the last running cpu is where QEMU stops asking for windows. Any pacing pause
             * is left standing: a halt stops the cpu by its own mechanism. */
            if (vcpu && vcpu == master()) {
                if (hand_off_clock()) {
                    set_systemc_window();
                }
            }
        }
        m_inst.plugin_api().qemu_plugin_bql_unlock();
    }

    /**
     * @brief Simulation time in ns for QEMU's virtual clock. Not a pure read: with no master it
     *        also pulls m_qemu_time, and every checkpoint, up to SystemC time.
     */
    int64_t get_qemu_clock(void* /*userdata*/)
    {
        if (m_shutdown.load(std::memory_order_acquire)) {
            return static_cast<int64_t>(sc_core::sc_time_stamp().to_seconds() * NSEC_IN_ONE_SEC);
        }

        /* Committed master time only: adding the live delta_insn lets a reader count the same delta
         * twice and run the clock backwards, which timer dispatch forbids. At most a quantum late. */
        if (m_master_vcpu.load(std::memory_order_acquire)) {
            return m_qemu_time_ns.load(std::memory_order_acquire);
        }

        std::lock_guard<std::mutex> lock(m_mcips_mutex);
        /* No cpu drives the clock: feed QEMU the SystemC time the idle drift keeps moving, so the
         * iothread can fire due VIRTUAL timers and wake an idle cpu. Forwards only. */
        const sc_core::sc_time sc_now = sc_core::sc_time_stamp();
        if (sc_now > m_qemu_time) {
            const sc_core::sc_time warp = sc_now - m_qemu_time;
            /* Every cpu, not just the paused ones: nothing is retiring instructions, and a
             * checkpoint left behind comes back owing the pacing the whole idle stretch. */
            for (int i = 0; i < m_num_vcpus; i++) {
                get_vcpu(i)->cpu_time += warp;
            }
            catch_up_to_systemc();
        }

        return m_qemu_time_ns.load(std::memory_order_acquire);
    }

    /** @brief JSON status for the monitor. Data only, unlocked, so values may be slightly stale. */
    std::string get_mcips_status_json()
    {
        vCPUTime* m = master();
        std::ostringstream os;

        const uint64_t qemu_time = static_cast<uint64_t>(qemu_time_now().to_seconds() * NSEC_IN_ONE_SEC);
        const uint64_t sc_ns = static_cast<uint64_t>(sc_core::sc_time_stamp().to_seconds() * NSEC_IN_ONE_SEC);

        os << "{" << "\"name\":\"" << name() << "\"," << "\"qemu_time\":\"" << qemu_time << " ns\","
           << "\"qemu_time_ns\":" << qemu_time << "," << "\"sc_time_ns\":" << sc_ns << ","
           << "\"n_cpus\":" << m_num_vcpus << ","
           << "\"master_vcpu_index\":" << (m ? static_cast<int64_t>(m->index) : -1) << "," << "\"vcpus\":[";

        for (int i = 0; i < m_num_vcpus; i++) {
            vCPUTime* vcpu = get_vcpu(i);
            if (i) os << ",";

            const uint64_t ns = static_cast<uint64_t>(vcpu->cpu_time.to_seconds() * NSEC_IN_ONE_SEC);

            os << "{" << "\"index\":" << vcpu->index << ","
               << "\"is_master\":" << ((m && m->index == vcpu->index) ? 1 : 0) << ","
               << "\"insn_per_second\":\"" << vcpu->insn_per_second << "\"," << "\"delta_insn\":\"" << vcpu->delta_insn
               << "\"," << "\"cpu_time_ns\":\"" << ns << "\"," << "\"cpu_time_s\":" << vcpu->cpu_time.to_seconds()
               << "," << "\"q_stopped\":" << (cpu_stopped(vcpu) ? 1 : 0) << ","
               << "\"q_stop\":" << (cpu_stop_requested(vcpu) ? 1 : 0) << ","
               << "\"q_halted\":" << (cpu_halted(vcpu) ? 1 : 0) << ","
               << "\"paused_by_us\":" << (vcpu->paused_by_us ? 1 : 0) << "}";
        }
        os << "]}";

        return os.str();
    }
};

#endif //_LIBQBOX_COMPONENTS_MCIPS_PLUGIN_H
