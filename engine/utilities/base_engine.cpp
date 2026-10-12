#include "base_engine.hpp"

namespace utilities {

void BaseEngine::write_log(const std::string& msg, utilities::LogLevel level,
                           const std::string& source) const {
    if (logger == nullptr) {
        return;
    }
    LogData record;
    record.source = source.empty() ? engine_name : source;
    record.msg = msg;
    record.level = level;
    logger->log(record);
}

} // namespace utilities
