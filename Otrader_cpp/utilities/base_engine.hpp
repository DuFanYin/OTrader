#pragma once

/**
 * C++20 equivalent of utilities/base_engine.py
 * Abstract base for function engines.
 */

#include "event.hpp"
#include <string>

namespace utilities {

struct MainEngine {
    virtual ~MainEngine() = default;
    /** Log intent: live engines call this; gateway empty => "Main". Default no-op. */
    virtual void write_log(const std::string& msg, int level = 20,
                           const std::string& gateway = "") {
        (void)msg;
        (void)level;
        (void)gateway;
    }
    /** Push event into runtime queue; backtest/live Main delegate to their EventEngine. Default
     * no-op. */
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
};

} // namespace utilities
