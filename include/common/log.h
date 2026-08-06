#pragma once

#include <mutex>
#include <ostream>
#include <utility>

namespace personal_cloud {

// All translation units share this mutex because it is an inline variable.
inline std::mutex log_mutex;

template <typename... Parts>
void write_log(std::ostream& output, Parts&&... parts) {
    std::lock_guard lock(log_mutex);
    (output << ... << std::forward<Parts>(parts));
    output.flush();
}

}  // namespace personal_cloud
