#include "base_engine.hpp"
#include "../runtime/main_engine_base.hpp"

namespace utilities {

void BaseEngine::write_log(const std::string& msg, int level, const std::string& gateway) const {
    if (main_engine == nullptr) {
        return;
    }
    runtime_common::write_log_from_base(
        main_engine, msg, level, gateway.empty() ? engine_name : gateway);
}

} // namespace utilities

namespace runtime_common {

void write_log_from_base(MainEngineBase* main, const std::string& msg, int level,
                         const std::string& gateway) {
    if (main != nullptr) {
        main->write_log(msg, level, gateway);
    }
}

} // namespace runtime_common

