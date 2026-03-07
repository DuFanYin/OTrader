#pragma once

/** Base engine (from base_engine.py). */

#include "event.hpp"
#include <string>

namespace utilities {

struct MainEngine {
    virtual ~MainEngine() = default;
    /** Log intent; gateway empty => "Main". */
    virtual void write_log(const std::string& msg, int level = 20,
                           const std::string& gateway = "") {
        (void)msg;
        (void)level;
        (void)gateway;
    }
    /** Push event to runtime queue. */
    virtual void put_event(const Event& e) { (void)e; }
};

struct BaseEngine {
    MainEngine* main_engine = nullptr;
    std::string engine_name;

    BaseEngine() = default;
    BaseEngine(MainEngine* main_engine, std::string engine_name)
        : main_engine(main_engine), engine_name(std::move(engine_name)) {}

    virtual ~BaseEngine() = default;
    virtual void close() {}

    /** Forward log to Main; gateway empty => engine_name. */
    void write_log(const std::string& msg, int level = 20, const std::string& gateway = "") const {
        if (main_engine != nullptr) {
            main_engine->write_log(msg, level, gateway.empty() ? engine_name : gateway);
        }
    }

    bool has_main() const { return main_engine != nullptr; }
};

} // namespace utilities
