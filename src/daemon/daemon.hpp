#pragma once

#include "common/config.hpp"
#include <string>

namespace aoh {

// Runs the background daemon (IPC server + HTTP settings UI + Process exit watcher + Tray/Wait loop)
int run_daemon(bool open_ui_on_start = false);

// IPC client helper for CLI commands
bool send_ipc_command(int port, const std::string& cmd, std::string& reply);

} // namespace aoh
