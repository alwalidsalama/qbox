/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All Rights Reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/**
 * @file i2c_multi_slave_backend.h
 * @brief Generic multi-slave I2C backend for the QUPv3 I2C serial-engine model.
 *
 * Models several independent I2C slave devices sharing one bus. Every slave
 * address listed in the `slave_addresses` CCI parameter gets:
 *   - its own biflow socket, named "backend_socket_<addr>", and
 *   - a private 256-byte register file with an auto-incrementing read pointer.
 *
 * This mirrors the QUP master model, which creates one "backend_socket_<addr>"
 * per configured slave address; the platform (or a test) binds each matching
 * pair together, giving an address-routed point-to-point view of the bus.
 *
 * Supported commands (as issued by the QUP I2C master model):
 *   - WRITE      (tlm::TLM_WRITE_COMMAND): data[0] = register offset,
 *                data[1..] = values. Values are stored and the offset is
 *                latched as the current read pointer.
 *   - WRITE_READ (I2C combined transfer, command == 3): data[0] = register
 *                offset; the slave replies with get_data_length() bytes read
 *                from that offset.
 *   - READ       (tlm::TLM_READ_COMMAND): the slave replies with
 *                get_data_length() bytes read from the latched read pointer.
 *
 * Replies are pushed back to the master synchronously over the same biflow
 * socket using force_send(), tagged with the slave's own address so the master
 * can validate/route them.
 */

#ifndef _GS_I2C_MULTI_SLAVE_BACKEND_H_
#define _GS_I2C_MULTI_SLAVE_BACKEND_H_

#include <systemc>
#include <tlm.h>
#include <cci_configuration>
#include <cciutils.h>
#include <scp/report.h>
#include <ports/biflow-socket.h>
#include <module_factory_registery.h>

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class i2c_multi_slave_backend : public sc_core::sc_module
{
    SCP_LOGGER();

    /* I2C combined write-read opcode as forwarded by the QUP master model. */
    static constexpr int I2C_WRITE_READ_CMD = 3;

    struct slave {
        uint8_t addr = 0;
        gs::biflow_socket<i2c_multi_slave_backend>* socket = nullptr;
        std::array<uint8_t, 256> regs{};
        uint8_t ptr = 0;
    };

    std::unordered_map<uint8_t, slave> m_slaves;

public:
    cci::cci_param<std::vector<unsigned int>> p_slave_addresses;

    i2c_multi_slave_backend(sc_core::sc_module_name name)
        : sc_core::sc_module(name)
        , p_slave_addresses("slave_addresses",
                            std::vector<unsigned int>(gs::cci_get_vector<unsigned int>(
                                cci::cci_get_broker(), std::string(sc_module::name()) + ".slave_addresses")),
                            "list of I2C slave addresses answered by this backend; one biflow socket "
                            "'backend_socket_<addr>' is created per entry")
    {
        SCP_TRACE(()) << "constructor";
        for (auto raw : p_slave_addresses.get_value()) {
            uint8_t addr = static_cast<uint8_t>(raw);
            std::string sock_name = "backend_socket_" + std::to_string(raw);
            slave& s = m_slaves[addr];
            s.addr = addr;
            s.socket = new gs::biflow_socket<i2c_multi_slave_backend>(sock_name.c_str());
            s.socket->register_b_transport(this, &i2c_multi_slave_backend::b_transport);
            SCP_DEBUG(())("Created {} for slave 0x{:02x}", sock_name, addr);
        }
    }

    ~i2c_multi_slave_backend()
    {
        for (auto& [addr, s] : m_slaves) delete s.socket;
    }

    /* Return the biflow socket modelling a given slave address (nullptr if the
     * address is not modelled). Used by platforms/tests to bind the master. */
    gs::biflow_socket<i2c_multi_slave_backend>* socket_for(uint8_t addr)
    {
        auto it = m_slaves.find(addr);
        return (it != m_slaves.end()) ? it->second.socket : nullptr;
    }

    /* Allow the master to send to every modelled slave. */
    void end_of_elaboration()
    {
        for (auto& [addr, s] : m_slaves) s.socket->can_receive_any();
    }

    void b_transport(tlm::tlm_generic_payload& txn, sc_core::sc_time& t)
    {
        uint8_t addr = static_cast<uint8_t>(txn.get_address());
        auto it = m_slaves.find(addr);
        if (it == m_slaves.end()) {
            SCP_WARN(())("b_transport: no slave modelled at address 0x{:02x}", addr);
            txn.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
            return;
        }
        slave& s = it->second;
        uint8_t* data = txn.get_data_ptr();
        int cmd = static_cast<int>(txn.get_command());

        if (data == nullptr) {
            SCP_WARN(())("b_transport: null data pointer (slave 0x{:02x})", addr);
            txn.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            return;
        }

        if (cmd == static_cast<int>(tlm::TLM_WRITE_COMMAND)) {
            uint32_t len = txn.get_streaming_width() ? txn.get_streaming_width() : txn.get_data_length();
            if (len >= 1) {
                s.ptr = data[0];
                for (uint32_t i = 1; i < len; i++) {
                    s.regs[static_cast<uint8_t>(s.ptr + (i - 1))] = data[i];
                }
                SCP_DEBUG(())("WRITE slave 0x{:02x} offset 0x{:02x} ({} data byte(s))", addr, s.ptr, len - 1);
            }
            txn.set_response_status(tlm::TLM_OK_RESPONSE);
        } else if (cmd == I2C_WRITE_READ_CMD) {
            uint8_t off = (txn.get_data_length() >= 1) ? data[0] : s.ptr;
            s.ptr = off;
            reply(s, off, txn.get_data_length());
            txn.set_response_status(tlm::TLM_OK_RESPONSE);
        } else if (cmd == static_cast<int>(tlm::TLM_READ_COMMAND)) {
            reply(s, s.ptr, txn.get_data_length());
            txn.set_response_status(tlm::TLM_OK_RESPONSE);
        } else {
            SCP_WARN(())("b_transport: unsupported command {} (slave 0x{:02x})", cmd, addr);
            txn.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
        }
    }

private:
    void reply(slave& s, uint8_t off, uint32_t len)
    {
        if (len == 0) return;
        std::vector<uint8_t> buf(len);
        for (uint32_t i = 0; i < len; i++) buf[i] = s.regs[static_cast<uint8_t>(off + i)];

        tlm::tlm_generic_payload r;
        r.set_address(s.addr); /* the master validates the reply against this address */
        r.set_command(tlm::TLM_WRITE_COMMAND);
        r.set_data_ptr(buf.data());
        r.set_data_length(len);
        r.set_streaming_width(len);
        r.set_response_status(tlm::TLM_OK_RESPONSE);

        /* force_send() is synchronous, so 'buf' stays valid for the whole call. */
        s.socket->force_send(r);
        SCP_DEBUG(())("REPLY slave 0x{:02x} offset 0x{:02x} len {}", s.addr, off, len);
    }
};

extern "C" void module_register();

#endif // _GS_I2C_MULTI_SLAVE_BACKEND_H_
