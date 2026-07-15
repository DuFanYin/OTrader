#pragma once

/** Base engine (from base_engine.py). */

#include "event.hpp"
#include "object.hpp"
#include "portfolio.hpp"
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace runtime_common {
class MainEngineBase;
void write_log_from_base(MainEngineBase* main, const std::string& msg, int level,
                         const std::string& gateway);
}

namespace utilities {

struct BaseEngine {
    runtime_common::MainEngineBase* main_engine = nullptr;
    std::string engine_name;

    BaseEngine() = default;
    BaseEngine(runtime_common::MainEngineBase* main_engine, std::string engine_name)
        : main_engine(main_engine), engine_name(std::move(engine_name)) {}

    virtual ~BaseEngine() = default;
    virtual void close() {}

    /** Forward log to Main; gateway empty => engine_name. */
    void write_log(const std::string& msg, int level = 20, const std::string& gateway = "") const;

    bool has_main() const { return main_engine != nullptr; }
};

} // namespace utilities
