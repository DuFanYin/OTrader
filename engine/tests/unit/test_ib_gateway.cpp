/**
 * IbGateway's checks before IB is asked: it trades one named paper account (a live one only once
 * root names it), and checks a contract sync against the platform's portfolios first. Pointed at
 * a port nothing listens on: an account it may trade gets as far as reaching for IB.
 */
#include "adapters/ib/engine_gateway_ib.hpp"
#include "adapters/ib/ib_contracts.hpp"
#include "model/portfolios.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

namespace {

/** Nothing listens there: connecting fails at once. */
constexpr int kClosedPort = 1;

struct Gateway {
    Gateway() {
        adapters::IbSettings& s = gateway.default_setting();
        s.host = "127.0.0.1";
        s.port = kClosedPort;
        s.account = "DU1"; // a paper account
    }
    /** connect()'s refusal ("" when it connected). */
    auto connect_error() -> std::string {
        try {
            gateway.connect();
            return "";
        } catch (const std::runtime_error& e) {
            return e.what();
        }
    }
    adapters::IbGateway gateway{nullptr};
};

TEST(IbGateway, TradesOneNamedPaperAccount) {
    Gateway g;
    g.gateway.default_setting().account = "";
    EXPECT_NE(g.connect_error().find("Set IB_ACCOUNT"), std::string::npos); // none named
    g.gateway.default_setting().account = "U1234567";
    EXPECT_NE(g.connect_error().find("not a paper account"), std::string::npos); // a live one
    // Named in a file a user wrote (not root's): still refused.
    const std::string file = ::testing::TempDir() + "optra_live_account";
    std::ofstream(file) << "U1234567\n";
    g.gateway.default_setting().live_account_file = file;
    EXPECT_NE(g.connect_error().find("not a paper account"), std::string::npos);
    std::filesystem::remove(file);

    g.gateway.default_setting().account = "DU2"; // a paper one: on to IB, which is not there
    EXPECT_NE(g.connect_error().find("Cannot reach IB"), std::string::npos);
    EXPECT_FALSE(g.gateway.is_connected());
}

TEST(IbGateway, ASyncIsCheckedBeforeIbIsAsked) {
    Gateway g;
    EXPECT_THROW((void)g.gateway.sync_contracts("QQQ", 7), std::invalid_argument); // no portfolio
    EXPECT_THROW((void)g.gateway.sync_contracts("SPXW", -1), std::invalid_argument);
    EXPECT_THROW((void)g.gateway.sync_contracts("SPXW", 7), std::runtime_error); // not connected
}

TEST(IbGateway, EveryPortfolioIsListedAtIb) {
    for (const model::PortfolioSpec& p : model::kPortfolios) {
        EXPECT_NE(adapters::find_listing(p.underlying), nullptr) << p.name;
        EXPECT_NE(adapters::find_listing(p.hedge), nullptr) << p.name; // what it hedges with
    }
}

} // namespace
