#pragma once

/** BaseEngine: what every engine has, a name and the log it writes to under that name. Not a
 * runtime interface: nothing holds an engine as a BaseEngine; each one closes as itself. */

#include "log_level.hpp"
#include "logger.hpp"
#include <string>
#include <utility>

namespace utilities {

struct BaseEngine {
    /** Where write_log goes; null: nowhere (an engine on its own, as in a test). */
    Logger* logger = nullptr;
    std::string engine_name;

    BaseEngine() = default;
    BaseEngine(Logger* logger, std::string engine_name)
        : logger(logger), engine_name(std::move(engine_name)) {}

    /** Log msg as source; source empty => engine_name. */
    void write_log(const std::string& msg, utilities::LogLevel level = utilities::INFO,
                   const std::string& source = "") const;
};

} // namespace utilities
