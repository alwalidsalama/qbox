/*
 * This file is part of libqbox
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. All Rights Reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifndef GS_I2C_TLM_COMMAND_H
#define GS_I2C_TLM_COMMAND_H

#include <tlm>

namespace gs {

/*
 * QUP I2C commands transported through a biflow socket use standard TLM
 * commands. TLM_IGNORE_COMMAND marks a combined write/read transaction.
 */
inline constexpr auto i2c_write_read_command = tlm::TLM_IGNORE_COMMAND;

/** Marks a QUP transaction that uses its first byte as an offset. */
class i2c_offset_extension : public tlm::tlm_extension<i2c_offset_extension>
{
public:
    explicit i2c_offset_extension(bool uses_offset = false): uses_offset(uses_offset) {}

    bool uses_offset;

    tlm::tlm_extension_base* clone() const override { return new i2c_offset_extension(*this); }
    void copy_from(const tlm::tlm_extension_base& extension) override
    {
        uses_offset = static_cast<const i2c_offset_extension&>(extension).uses_offset;
    }
};

} // namespace gs

#endif // GS_I2C_TLM_COMMAND_H
