/**
 * MarketDataEngine: load 两段 + finalize；行情 Snapshot → apply_frame；inject_tradier_chain 只填
 * bid/ask/last。
 */

#include "engine_data_tradier.hpp"
#include "../../utilities/event.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <curl/curl.h>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_map>

using json = nlohmann::json;

namespace engines {

namespace {

// Round to 2 decimals (Python round(..., 2) for bid/ask/last)
auto round2(double x) -> double { return std::round(x * 100.0) / 100.0; }

// Parse chain_key "SYMBOL_YYYYMMDD" -> (symbol, date_part). Returns empty if invalid.
auto parse_chain_key(const std::string& chain_key) -> std::pair<std::string, std::string> {
    size_t pos = chain_key.find('_');
    if (pos == std::string::npos || pos + 9 > chain_key.size()) {
        return {"", ""};
    }
    std::string symbol = chain_key.substr(0, pos);
    std::string date_part = chain_key.substr(pos + 1, 8);
    if (date_part.size() != 8U) {
        return {"", ""};
    }
    return {symbol, date_part};
}

// Expiration "YYYYMMDD" -> "YYYY-MM-DD" (Python:
// f"{date_part[:4]}-{date_part[4:6]}-{date_part[6:8]}")
auto expiration_from_date_part(const std::string& date_part) -> std::string {
    if (date_part.size() < 8U) {
        return "";
    }
    return date_part.substr(0, 4) + "-" + date_part.substr(4, 2) + "-" + date_part.substr(6, 2);
}

// Hardcoded: portfolio names to create at startup (option trading_class names). Process only finds;
// unknown names are skipped with warning.
const std::vector<std::string> kPortfolioNamesToCreate = {"SPXW"};
// Underlying symbol prefix -> portfolio name; only listed underlyings are bound.
const std::unordered_map<std::string, std::string> kUnderlyingToPortfolio = {
    {"SPX", "SPXW"},
};

auto portfolio_name_for_underlying(const std::string& symbol_prefix) -> std::string {
    auto it = kUnderlyingToPortfolio.find(symbol_prefix);
    return (it != kUnderlyingToPortfolio.end()) ? it->second : symbol_prefix;
}

// Underlying symbol for quote API (SPX-USD-IND -> SPX). Reverse of portfolio name when chain_key
// uses portfolio name.
auto underlying_symbol_for_quote(const std::string& symbol_part) -> std::string {
    for (const auto& [underlying, portfolio] : kUnderlyingToPortfolio) {
        if (portfolio == symbol_part) {
            return underlying;
        }
    }
    return symbol_part;
}

// Tradier API: base_url 写死，与 Python engine_data.py 一致；token 仅从环境变量 TRADIER_TOKEN 读
const std::string kTradierBaseUrl = "https://api.tradier.com/v1/";

// --- HTTP (libcurl) ---
auto curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
    auto* out = static_cast<std::string*>(userdata);
    size_t total = size * nmemb;
    out->append(ptr, total);
    return total;
}

auto http_get(const std::string& url, const std::string& auth_header) -> std::string {
    std::string body;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return body;
    }
    struct curl_slist* headers = nullptr;
    if (!auth_header.empty()) {
        headers = curl_slist_append(headers, auth_header.c_str());
        headers = curl_slist_append(headers, "Accept: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING,
                     ""); // auto-decompress gzip/deflate (Python requests does this)
    CURLcode res = curl_easy_perform(curl);
    if (headers != nullptr) {
        curl_slist_free_all(headers);
    }
    curl_easy_cleanup(curl);
    return (res == CURLE_OK) ? body : "";
}

// Safe double: Tradier returns null for last/change; nlohmann can throw on null->double
auto json_safe_double(const json& j, const std::string& key, double def) -> double {
    if (!j.contains(key)) {
        return def;
    }
    const json& v = j[key];
    if (v.is_null()) {
        return def;
    }
    try {
        return v.get<double>();
    } catch (...) {
        return def;
    }
}

// --- Tradier JSON parsing (nlohmann) ---
auto parse_tradier_chain_json(const std::string& body) -> std::vector<TradierOptionRaw> {
    std::vector<TradierOptionRaw> out;
    json data = json::parse(body);
    json opts = data.value("options", json::object());
    json opt_arr = opts.value("option", json::array());
    if (!opt_arr.is_array()) {
        if (opt_arr.is_object()) {
            json arr = json::array();
            for (const auto& [_, v] : opt_arr.items()) {
                arr.push_back(v);
            }
            opt_arr = std::move(arr);
        } else {
            return out;
        }
    }
    for (auto& o : opt_arr) {
        TradierOptionRaw raw;
        raw.symbol = o.value("symbol", std::string{});
        raw.root_symbol = o.value("root_symbol", o.value("underlying", std::string{}));
        raw.strike = json_safe_double(o, "strike", 0.0);
        raw.option_type = o.value("option_type", "call");
        raw.contract_size = static_cast<int>(json_safe_double(o, "contract_size", 100.0));
        raw.bid = json_safe_double(o, "bid", 0.0);
        raw.ask = json_safe_double(o, "ask", 0.0);
        raw.last = json_safe_double(o, "last", 0.0);
        raw.volume = json_safe_double(o, "volume", 0.0);
        raw.open_interest = json_safe_double(o, "open_interest", 0.0);
        out.push_back(raw);
    }
    return out;
}

// Underlying quote: {"quotes":{"quote":{...}}}, quote may be object or array of one. Throws on
// parse/structure error.
auto parse_tradier_quote_json(const std::string& body) -> std::pair<double, double> {
    json data = json::parse(body);
    json q = data["quotes"]["quote"];
    if (q.is_array() && !q.empty()) {
        q = q[0];
    }
    double bid = q["bid"].get<double>();
    double ask = q["ask"].get<double>();
    return {bid, ask};
}

} // namespace

MarketDataEngine::MarketDataEngine(utilities::MainEngine* main_engine)
    : BaseEngine(main_engine, "MarketData") {}

void MarketDataEngine::write_log(const std::string& msg, int level) {
    if (main_engine != nullptr) {
        main_engine->write_log(msg, level, engine_name);
    }
}

void MarketDataEngine::process_option(const utilities::ContractData& contract) {
    process_contract(contract, true);
}
void MarketDataEngine::ensure_portfolios_created() {
    for (const std::string& name : kPortfolioNamesToCreate) {
        get_or_create_portfolio(name);
    }
}

void MarketDataEngine::process_underlying(const utilities::ContractData& contract) {
    process_contract(contract, false);
}

void MarketDataEngine::process_contract(const utilities::ContractData& contract, bool is_option) {
    contracts_[contract.symbol] = contract;
    size_t pos = contract.symbol.find('-');
    std::string prefix =
        (pos != std::string::npos) ? contract.symbol.substr(0, pos) : contract.symbol;
    std::string portfolio_name =
        is_option ? (contract.trading_class.has_value() && !contract.trading_class->empty()
                         ? *contract.trading_class
                         : prefix)
                  : portfolio_name_for_underlying(prefix);
    utilities::PortfolioData* port = get_portfolio(portfolio_name);
    if (port == nullptr) {
        if (is_option) {
            write_log("Option portfolio \"" + portfolio_name + "\" not created (skip option " +
                          contract.symbol + ").",
                      30);
        } else {
            write_log("Underlying " + contract.symbol + " has no portfolio \"" + portfolio_name +
                          "\" (skip bind).",
                      30);
        }
        return;
    }
    if (is_option) {
        port->add_option(contract);
    } else {
        port->set_underlying(contract);
    }
}

void MarketDataEngine::finalize_all_chains() {
    for (auto& kv : portfolios_) {
        if (kv.second) {
            kv.second->finalize_chains();
        }
    }
}

void MarketDataEngine::subscribe_chains(const std::string& strategy_name,
                                        const std::vector<std::string>& chain_symbols) {
    for (const auto& chain_symbol : chain_symbols) {
        strategy_chains_[strategy_name].insert(chain_symbol);
        size_t i = chain_symbol.find('_');
        std::string portfolio_name =
            (i != std::string::npos) ? chain_symbol.substr(0, i) : chain_symbol;
        active_chains_[portfolio_name].insert(chain_symbol);
    }
    write_log("Strategy " + strategy_name + " subscribed to chains", INFO);
}

void MarketDataEngine::unsubscribe_chains(const std::string& strategy_name) {
    auto it = strategy_chains_.find(strategy_name);
    if (it == strategy_chains_.end()) {
        return;
    }
    for (const auto& chain_symbol : it->second) {
        size_t i = chain_symbol.find('_');
        std::string portfolio_name =
            (i != std::string::npos) ? chain_symbol.substr(0, i) : chain_symbol;
        active_chains_[portfolio_name].erase(chain_symbol);
        if (active_chains_[portfolio_name].empty()) {
            active_chains_.erase(portfolio_name);
        }
    }
    strategy_chains_.erase(it);
    write_log("Strategy " + strategy_name + " unsubscribed from all chains", INFO);
}

auto MarketDataEngine::get_or_create_portfolio(const std::string& portfolio_name)
    -> utilities::PortfolioData* {
    auto it = portfolios_.find(portfolio_name);
    if (it != portfolios_.end()) {
        return it->second.get();
    }
    portfolios_[portfolio_name] = std::make_unique<utilities::PortfolioData>(portfolio_name);
    return portfolios_[portfolio_name].get();
}

auto MarketDataEngine::get_portfolio(const std::string& portfolio_name)
    -> utilities::PortfolioData* {
    auto it = portfolios_.find(portfolio_name);
    return (it != portfolios_.end()) ? it->second.get() : nullptr;
}

auto MarketDataEngine::get_all_portfolio_names() const -> std::vector<std::string> {
    std::vector<std::string> out;
    out.reserve(portfolios_.size());
    for (const auto& kv : portfolios_) {
        out.push_back(kv.first);
    }
    return out;
}

auto MarketDataEngine::get_contract(const std::string& symbol) const
    -> const utilities::ContractData* {
    auto it = contracts_.find(symbol);
    return (it != contracts_.end()) ? &it->second : nullptr;
}

auto MarketDataEngine::get_all_contracts() const -> std::vector<utilities::ContractData> {
    std::vector<utilities::ContractData> out;
    out.reserve(contracts_.size());
    for (const auto& kv : contracts_) {
        out.push_back(kv.second);
    }
    return out;
}

void MarketDataEngine::set_tradier_config(std::string base_url, std::string token) {
    tradier_base_url_ = std::move(base_url);
    tradier_token_ = std::move(token);
}

void MarketDataEngine::set_tradier_rate_limit(int requests_per_minute) {
    tradier_requests_per_minute_ = (requests_per_minute > 0) ? requests_per_minute : 60;
}

auto MarketDataEngine::get_fixed_chains_to_query() const -> std::vector<std::string> {
    std::vector<std::string> chains;
    for (const auto& [portfolio_name, chain_set] : active_chains_) {
        for (const auto& chain_key : chain_set) {
            chains.push_back(chain_key);
        }
    }
    return chains;
}

void MarketDataEngine::poll_market_data_loop() {
    std::string auth = "Authorization: Bearer " + tradier_token_;
    if (tradier_base_url_.empty() || tradier_token_.empty()) {
        write_log("Tradier config missing (base_url or token); poll loop idle", 30);
        return;
    }
    // Initialise rate-limit window
    tradier_window_start_ = std::chrono::steady_clock::now();
    tradier_requests_used_ = 0;

    auto ensure_quota = [this]() -> void {
        if (tradier_requests_per_minute_ <= 0) {
            return;
        }
        using clock = std::chrono::steady_clock;
        using seconds = std::chrono::seconds;
        auto now = clock::now();
        auto elapsed = now - tradier_window_start_;
        if (elapsed >= seconds(60)) {
            tradier_window_start_ = now;
            tradier_requests_used_ = 0;
        }
        if (tradier_requests_used_ >= tradier_requests_per_minute_) {
            auto sleep_for = seconds(60) - elapsed;
            if (sleep_for > seconds(0)) {
                std::this_thread::sleep_for(sleep_for);
            }
            tradier_window_start_ = clock::now();
            tradier_requests_used_ = 0;
        }
        ++tradier_requests_used_;
    };

    while (started_) {
        std::vector<std::string> chains = get_fixed_chains_to_query();
        if (chains.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        for (const std::string& chain_key : chains) {
            if (!started_) {
                break;
            }
            auto [symbol_part, date_part] = parse_chain_key(chain_key);
            if (symbol_part.empty() || date_part.size() < 8U) {
                std::string msg;
                msg += "poll skip invalid chain_key=";
                msg += chain_key;
                msg += " symbol_part=";
                msg += symbol_part;
                msg += " date_part_len=";
                msg += std::to_string(date_part.size());
                write_log(msg, 30);
                continue;
            }
            // Chains API uses underlying (SPX); Tradier returns SPXW options when symbol=SPX
            std::string api_symbol = underlying_symbol_for_quote(symbol_part);
            std::string expiration = expiration_from_date_part(date_part);
            if (expiration.empty()) {
                continue;
            }
            std::string chain_url = tradier_base_url_;
            chain_url += "markets/options/chains?symbol=";
            chain_url += api_symbol;
            chain_url += "&expiration=";
            chain_url += expiration;

            ensure_quota();
            std::string chain_body = http_get(chain_url, auth);
            if (chain_body.empty()) {
                write_log("chain API response empty url=" + chain_url, 30);
                continue;
            }
            std::vector<TradierOptionRaw> options;
            try {
                options = parse_tradier_chain_json(chain_body);
            } catch (const json::exception& e) {
                write_log("chain JSON parse error: " + std::string(e.what()), 40);
            } catch (const std::exception& e) {
                write_log("chain parse error: " + std::string(e.what()), 40);
            }
            if (options.empty() && chain_body.size() > 10) {
                write_log("chain API returned non-empty body but options_parsed=0 (check JSON "
                          "format or symbol/expiration)",
                          30);
            }

            std::string quote_symbol = underlying_symbol_for_quote(symbol_part);
            std::string quote_url = tradier_base_url_ + "markets/quotes?symbols=" + quote_symbol;

            ensure_quota();
            std::string quote_body = http_get(quote_url, auth);
            double quote_bid = 0.0;
            double quote_ask = 0.0;
            if (!quote_body.empty()) {
                try {
                    auto [b, a] = parse_tradier_quote_json(quote_body);
                    quote_bid = b;
                    quote_ask = a;
                } catch (const std::exception& e) {
                    write_log("underlying quote parse error: " + std::string(e.what()), 40);
                }
            }
            inject_tradier_chain(chain_key, options, quote_bid, quote_ask);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void MarketDataEngine::start_market_data_update() {
    if (tradier_token_.empty()) {
        const char* env_token = getenv("TRADIER_TOKEN");
        if (env_token != nullptr) {
            tradier_token_ = env_token;
        }
    }
    if (tradier_base_url_.empty()) {
        tradier_base_url_ = kTradierBaseUrl;
    }
    started_ = true;
    poll_thread_ = std::thread(&MarketDataEngine::poll_market_data_loop, this);
    write_log("Market data update started (Tradier poll)", INFO);
}

void MarketDataEngine::stop_market_data_update() {
    started_ = false;
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
}

void MarketDataEngine::inject_tradier_chain(const std::string& chain_key,
                                            const std::vector<TradierOptionRaw>& options,
                                            double quote_bid, double quote_ask) {
    auto [symbol_part, date_part] = parse_chain_key(chain_key);
    if (symbol_part.empty() || date_part.empty()) {
        write_log("Invalid chain_key: " + chain_key, 40); // ERROR
        return;
    }
    std::string expiration = expiration_from_date_part(date_part);
    if (expiration.empty()) {
        return;
    }

    std::string portfolio_name = portfolio_name_for_underlying(symbol_part);
    utilities::PortfolioData* portfolio = get_portfolio(portfolio_name);
    if (portfolio == nullptr) {
        write_log("inject skip: no portfolio chain_key=" + chain_key +
                      " symbol_part=" + symbol_part + " portfolio_name=" + portfolio_name,
                  30);
        return;
    }
    const std::vector<utilities::OptionData*>& order = portfolio->option_apply_order();
    const size_t n_opt = order.size();
    std::unordered_map<std::string, size_t> symbol_to_index;
    for (size_t i = 0; i < n_opt; ++i) {
        if (order[i] != nullptr) {
            symbol_to_index[order[i]->symbol] = i;
        }
    }

    utilities::PortfolioSnapshot snapshot;
    snapshot.portfolio_name = portfolio->name;
    snapshot.datetime = std::chrono::system_clock::now();
    snapshot.underlying_bid = round2(quote_bid);
    snapshot.underlying_ask = round2(quote_ask);
    snapshot.underlying_last = (quote_bid > 0.0 && quote_ask > 0.0)
                                   ? round2(0.5 * (quote_bid + quote_ask))
                                   : round2((quote_bid > 0.0) ? quote_bid : quote_ask);
    snapshot.bid.resize(n_opt, 0.0);
    snapshot.ask.resize(n_opt, 0.0);
    snapshot.last.resize(n_opt, 0.0);
    snapshot.delta.resize(n_opt, 0.0);
    snapshot.gamma.resize(n_opt, 0.0);
    snapshot.theta.resize(n_opt, 0.0);
    snapshot.vega.resize(n_opt, 0.0);
    snapshot.iv.resize(n_opt, 0.0);

    // Initialize from current portfolio state (so options not in this chain keep last bid/ask/last)
    if (portfolio->underlying) {
        snapshot.underlying_bid = portfolio->underlying->bid_price;
        snapshot.underlying_ask = portfolio->underlying->ask_price;
        snapshot.underlying_last = portfolio->underlying->mid_price;
    }
    for (size_t i = 0; i < n_opt; ++i) {
        utilities::OptionData* opt = order[i];
        if (opt == nullptr) {
            continue;
        }
        snapshot.bid[i] = opt->bid_price;
        snapshot.ask[i] = opt->ask_price;
        snapshot.last[i] = opt->mid_price;
    }
    // Overwrite with quote when provided
    if (quote_bid > 0.0 || quote_ask > 0.0) {
        snapshot.underlying_bid = round2(quote_bid);
        snapshot.underlying_ask = round2(quote_ask);
        snapshot.underlying_last = (quote_bid > 0.0 && quote_ask > 0.0)
                                       ? round2(0.5 * (quote_bid + quote_ask))
                                       : round2((quote_bid > 0.0) ? quote_bid : quote_ask);
    }
    for (const TradierOptionRaw& opt : options) {
        // OCC e.g. SPXW260302C02800000 -> SPXW-20260302-C-2800.0-100-USD-OPT (match DB format)
        if (opt.symbol.size() < 19U) {
            continue;
        }
        std::string root = opt.symbol.substr(0, 4);
        std::string yy = opt.symbol.substr(4, 2);
        std::string mm = opt.symbol.substr(6, 2);
        std::string dd = opt.symbol.substr(8, 2);
        char opt_type = static_cast<char>(std::toupper(static_cast<unsigned char>(opt.symbol[10])));
        if (opt_type != 'C' && opt_type != 'P') {
            continue;
        }
        int strike_raw = 0;
        try {
            strike_raw = std::stoi(opt.symbol.substr(11, 8));
        } catch (...) {
            continue;
        }
        double strike = strike_raw / 1000.0;
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << strike;
        std::string platform_sym = root;
        platform_sym += "-20";
        platform_sym += yy;
        platform_sym += mm;
        platform_sym += dd;
        platform_sym += "-";
        platform_sym += opt_type;
        platform_sym += "-";
        platform_sym += ss.str();
        platform_sym += "-100-USD-OPT";

        double bid = round2(opt.bid);
        double ask = round2(opt.ask);
        double last = round2(opt.last);
        if (last == 0.0 && (bid != 0.0 || ask != 0.0)) {
            last = (bid != 0.0 && ask != 0.0) ? round2(0.5 * (bid + ask))
                                              : round2((bid != 0.0) ? bid : ask);
        }
        auto it = symbol_to_index.find(platform_sym);
        if (it == symbol_to_index.end()) {
            continue;
        }
        const size_t idx = it->second;
        snapshot.bid[idx] = bid;
        snapshot.ask[idx] = ask;
        snapshot.last[idx] = last;
    }

    if (main_engine != nullptr) {
        main_engine->put_event(
            utilities::Event(utilities::EventType::Snapshot, std::move(snapshot)));
    }
}

} // namespace engines
