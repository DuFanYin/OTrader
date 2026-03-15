#pragma once

/** Base engine (from base_engine.py). */

#include "event.hpp"
#include <string>
#include <utility>

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
    /** Rvalue overload: override to move into queue/pool instead of copy. */
    virtual void put_event(Event&& e) { put_event(e); }
    /** Snapshot pool (live): override to return pooled slot; fill and put_event(Snapshot, p). */
    virtual PortfolioSnapshot* acquire_snapshot() { return nullptr; }
    /** Order/Trade pools: override to return pooled slot; fill and put_event(Order/Trade, p). */
    virtual OrderData* acquire_order() { return nullptr; }
    virtual TradeData* acquire_trade() { return nullptr; }
    /** Lvalue overload: move into queue to avoid copy. */
    virtual void put_event(Event& e) { put_event(std::move(e)); }
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
