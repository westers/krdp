// SPDX-FileCopyrightText: 2026 Steve Westers
// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

// Built with -fexceptions (see src/CMakeLists.txt) so the handler can rethrow the active
// exception to read it; the rest of the tree is -fno-exceptions.

#include "TerminateHandler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <exception>
#include <typeinfo>
#include <unistd.h>

namespace
{
char g_program[64] = "farside";

void terminateHandler()
{
    char message[768];
    int length = std::snprintf(message, sizeof(message), "farside: %s: std::terminate: ", g_program);
    auto append = [&](const char *text) {
        if (length < int(sizeof(message)) - 1) {
            length += std::snprintf(message + length, sizeof(message) - size_t(length), "%s", text);
        }
    };
    if (const auto active = std::current_exception()) {
        try {
            std::rethrow_exception(active);
        } catch (const std::exception &error) {
            int status = 0;
            char *demangled = abi::__cxa_demangle(typeid(error).name(), nullptr, nullptr, &status);
            append(status == 0 && demangled ? demangled : typeid(error).name());
            std::free(demangled);
            append(": ");
            append(error.what());
        } catch (...) {
            append("unknown exception");
        }
    } else {
        append("no active exception");
    }
    append("\n");
    [[maybe_unused]] const auto written = ::write(2, message, size_t(length));
    // abort(), not _exit(): the core (where the host's policy keeps one) must survive.
    std::abort();
}
}

namespace KRdp
{

void installTerminateHandler(const char *program)
{
    if (program) {
        std::snprintf(g_program, sizeof(g_program), "%s", program);
    }
    std::set_terminate(terminateHandler);
}

}
