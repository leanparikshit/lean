/*
Copyright (c) 2013 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include "kernel/kernel_exception.h"
#include "library/io_state.h"

namespace lean {
io_state::io_state(formatter const & fmt):
    m_formatter(fmt),
    m_regular_channel(std::make_shared<stdout_channel>()),
    m_diagnostic_channel(std::make_shared<stderr_channel>()) {
}

io_state::io_state(options const & opts, formatter const & fmt):
    m_options(opts),
    m_formatter(fmt),
    m_regular_channel(std::make_shared<stdout_channel>()),
    m_diagnostic_channel(std::make_shared<stderr_channel>()) {
}

io_state::~io_state() {}

void io_state::set_regular_channel(std::shared_ptr<output_channel> const & out) {
    if (out)
        m_regular_channel = out;
}

void io_state::set_diagnostic_channel(std::shared_ptr<output_channel> const & out) {
    if (out)
        m_diagnostic_channel = out;
}

void io_state::set_options(options const & opts) {
    m_options = opts;
}

void io_state::set_formatter(formatter const & f) {
    m_formatter = f;
}
}
