#include "utilities/log_source.hpp"

namespace utilities {

void LogSource::write_log(const std::string& msg, LogLevel level,
                          const std::string& source) const {
    if (logger == nullptr) {
        return;
    }
    logger->log(level, source.empty() ? source_name : source, msg);
}

} // namespace utilities
