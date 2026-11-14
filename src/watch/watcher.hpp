#pragma once

#include <string>
#include <vector>

namespace aoh {

int run_watch_command(int idle_seconds, const std::vector<std::string>& args);
void print_shell_hook(const std::string& shell_name);

} // namespace aoh
