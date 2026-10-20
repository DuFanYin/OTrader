// Screener task: the chain filtered, each kind of strategy generated from it and filtered, the best
// ranked, with model values from the reference vol asked for.
#include "json_io.h"
#include "quant/kernel.h"
#include "screener.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <omp.h>

namespace quant::analytics {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// The request, as the screener reads it.
struct ScreenerParams {
    double spot = 0.0;
    double rate = 0.0;
    double multiplier = 1.0;  // Deribit: 1 contract = 1 BTC / ETH
    ScreenPriceMode price_mode = ScreenPriceMode::Executable;
    bool compute_greeks = true;  // false: keep greeks supplied with the chain
    // Reference volatility for model_value / edge:
    //   "mark" (each option's own iv) | "flat" | "surface" | "heston" | "none"
    std::string model_vol = "mark";
    double model_vol_flat = 0.0;
    std::vector<double> surface_strikes, surface_expiries, surface_ivs;
    HestonParams heston;
    std::vector<ChainOption> chain;
    ScreenStrategyToggles strategies;
    ScreenOptionFilter option_filter;
    ScreenStrategyFilter strategy_filter;
    ScreenRank rank;
};

const char* kind_name(ScreenKind k) {
    switch (k) {
        case ScreenKind::Single:     return "single";
        case ScreenKind::IronCondor: return "iron_condor";
        case ScreenKind::Straddle:   return "straddle";
        case ScreenKind::Strangle:   return "strangle";
        case ScreenKind::ForwardVol: return "forward_vol";
    }
    return "unknown";
}

std::string fmt_strike(double k) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", k);
    return buf;
}

std::string make_label(const StrategyRecord& s, std::span<const ChainOption> chain) {
    const auto& o = [&](int k) -> const ChainOption& { return chain[s.legs[k].option_index]; };
    const std::string dir = s.long_direction ? "LONG" : "SHORT";
    switch (s.kind) {
        case ScreenKind::Single:
            return std::string(s.legs[0].qty > 0 ? "BUY " : "SELL ") + (o(0).is_call ? "C" : "P")
                   + fmt_strike(o(0).strike) + " " + o(0).expiry;
        case ScreenKind::IronCondor:
            return "IC " + dir + " C" + fmt_strike(o(0).strike) + "/" + fmt_strike(o(1).strike)
                   + " P" + fmt_strike(o(2).strike) + "/" + fmt_strike(o(3).strike) + " " + o(0).expiry;
        case ScreenKind::Straddle:
            return "Straddle " + dir + " " + fmt_strike(o(0).strike) + " " + o(0).expiry;
        case ScreenKind::Strangle:
            return "Strangle " + dir + " C" + fmt_strike(o(0).strike) + " P" + fmt_strike(o(1).strike)
                   + " " + o(0).expiry;
        case ScreenKind::ForwardVol:
            return "FwdVol " + dir + " " + (o(0).is_call ? "C" : "P") + fmt_strike(o(0).strike) + " "
                   + o(0).expiry + "->" + o(1).expiry;
    }
    return "";
}

std::vector<double> model_prices(const ScreenerParams& p, std::span<const int> idx) {
    std::vector<double> px;
    if (p.model_vol == "none") return px;
    px.assign(p.chain.size(), kNaN);
    const int n = static_cast<int>(idx.size());
    const bool heston = p.model_vol == "heston";

#pragma omp parallel for schedule(dynamic, 16)
    for (int k = 0; k < n; ++k) {
        const ChainOption& o = p.chain[idx[k]];
        if (heston) {
            if (o.years <= 0.0 || o.forward <= 0.0) continue;
            const double df   = std::exp(-p.rate * o.years);
            const double call =
                heston_call_price(o.forward * df, o.strike, p.rate, o.years, p.heston);
            px[idx[k]] = o.is_call ? call : call - df * (o.forward - o.strike);   // put-call parity
            continue;
        }
        double vol = o.iv;
        if (p.model_vol == "flat") {
            vol = p.model_vol_flat;
        } else if (p.model_vol == "surface") {
            vol = vol_surface_interp(p.surface_strikes, p.surface_expiries, p.surface_ivs,
                                     p.spot, o.strike, o.years);
        }
        const bool priced = vol > 0.0 && o.forward > 0.0 && o.strike > 0.0;  // else none: null
        px[idx[k]] = priced ? black76_price(o.forward, o.strike, p.rate, vol, o.years, o.is_call) : kNaN;
    }
    return px;
}

struct ScreenerParseError { std::string code; };

double jnum_or_nan(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_number()) return std::numeric_limits<double>::quiet_NaN();
    return j[key].get<double>();
}

std::optional<double> jopt_num(const json& j, const char* key) {
    if (!j.contains(key) || j[key].is_null()) return std::nullopt;
    if (!j[key].is_number()) throw ScreenerParseError{std::string("bad_number_") + key};
    return j[key].get<double>();
}

// [lo, hi] -> active range; null -> inactive. Either bound may be null (open-ended).
ScreenRange jrange(const json& j, const char* key) {
    ScreenRange r;
    if (!j.contains(key) || j[key].is_null()) return r;
    const json& a = j[key];
    if (!a.is_array() || a.size() != 2) throw ScreenerParseError{std::string("bad_range_") + key};
    const auto bound = [&](const json& v, double open) {
        if (v.is_null()) return open;
        if (!v.is_number()) throw ScreenerParseError{std::string("bad_range_") + key};
        return v.get<double>();
    };
    r.on = true;
    r.lo = bound(a[0], -std::numeric_limits<double>::infinity());
    r.hi = bound(a[1],  std::numeric_limits<double>::infinity());
    return r;
}

ChainOption parse_chain_option(const json& o) {
    ChainOption c;
    std::string type = jv<std::string>(o, "option_type", "");
    std::ranges::transform(type, type.begin(), [](unsigned char ch) { return std::tolower(ch); });
    if (type != "call" && type != "put") throw ScreenerParseError{"bad_option_type"};
    c.is_call = type == "call";
    c.symbol  = jv<std::string>(o, "symbol", "");
    c.expiry  = jv<std::string>(o, "expiry", "");
    c.strike  = jv<double>(o, "strike", 0.0);
    c.forward = jv<double>(o, "forward", 0.0);
    c.years   = jv<double>(o, "years", 0.0);
    c.bid     = jnum_or_nan(o, "bid");
    c.ask     = jnum_or_nan(o, "ask");
    c.mark    = jv<double>(o, "mark", 0.0);
    c.iv      = jv<double>(o, "iv", 0.0);
    c.volume  = jv<double>(o, "volume", 0.0);
    c.oi      = jv<double>(o, "oi", 0.0);
    c.delta   = jv<double>(o, "delta", 0.0);
    c.gamma   = jv<double>(o, "gamma", 0.0);
    c.theta   = jv<double>(o, "theta", 0.0);
    c.vega    = jv<double>(o, "vega", 0.0);
    if (c.strike <= 0.0) throw ScreenerParseError{"bad_strike"};
    return c;
}

ScreenRankKey parse_rank_key(const std::string& k) {
    if (k == "rr")          return ScreenRankKey::RR;
    if (k == "gain")        return ScreenRankKey::Gain;
    if (k == "loss")        return ScreenRankKey::Loss;
    if (k == "cost")        return ScreenRankKey::Cost;
    if (k == "credit")      return ScreenRankKey::Credit;
    if (k == "edge")        return ScreenRankKey::Edge;
    if (k == "forward_vol") return ScreenRankKey::ForwardVol;
    throw ScreenerParseError{"bad_rank_key"};
}

ScreenerParams parse_screener_params(const json& j) {
    ScreenerParams p;
    p.spot           = jv<double>(j, "spot", 0.0);
    p.rate           = jv<double>(j, "rate", 0.0);
    p.multiplier     = jv<double>(j, "multiplier", 1.0);
    p.compute_greeks = jv<bool>(j, "compute_greeks", true);
    if (p.multiplier <= 0.0) throw ScreenerParseError{"bad_multiplier"};

    const auto price_mode = jv<std::string>(j, "price_mode", "executable");
    if (price_mode == "executable")  p.price_mode = ScreenPriceMode::Executable;
    else if (price_mode == "mid")    p.price_mode = ScreenPriceMode::Mid;
    else throw ScreenerParseError{"bad_price_mode"};

    p.model_vol = jv<std::string>(j, "model_vol", "mark");
    if (p.model_vol == "flat") {
        p.model_vol_flat = jv<double>(j, "model_vol_flat", 0.0);
        if (p.model_vol_flat <= 0.0) throw ScreenerParseError{"model_vol_flat_required"};
    } else if (p.model_vol == "surface") {
        const json s = j.value("surface", json::object());
        for (const char* k : {"strikes", "expiries", "ivs"})
            if (!s.contains(k) || !s[k].is_array()) throw ScreenerParseError{"surface_arrays_required"};
        p.surface_strikes  = s["strikes"].get<std::vector<double>>();
        p.surface_expiries = s["expiries"].get<std::vector<double>>();
        p.surface_ivs      = s["ivs"].get<std::vector<double>>();
        if (p.surface_strikes.empty() || p.surface_strikes.size() != p.surface_expiries.size()
            || p.surface_strikes.size() != p.surface_ivs.size() || p.spot <= 0.0)
            throw ScreenerParseError{"bad_surface"};
    } else if (p.model_vol == "heston") {
        const json h = j.value("heston", json::object());
        p.heston.v0    = jv<double>(h, "v0",    p.heston.v0);
        p.heston.kappa = jv<double>(h, "kappa", p.heston.kappa);
        p.heston.theta = jv<double>(h, "theta", p.heston.theta);
        p.heston.xi    = jv<double>(h, "xi",    p.heston.xi);
        p.heston.rho   = jv<double>(h, "rho",   p.heston.rho);
    } else if (p.model_vol != "mark" && p.model_vol != "none") {
        throw ScreenerParseError{"bad_model_vol"};
    }

    if (!j["chain"].is_array()) throw ScreenerParseError{"chain_must_be_array"};
    p.chain.reserve(j["chain"].size());
    for (const auto& o : j["chain"]) p.chain.push_back(parse_chain_option(o));

    const json st = j.value("strategies", json::object());
    p.strategies.single_calls = jv<bool>(st, "single_calls", false);
    p.strategies.iron_condors = jv<bool>(st, "iron_condors", false);
    p.strategies.straddles    = jv<bool>(st, "straddles",    false);
    p.strategies.strangles    = jv<bool>(st, "strangles",    false);
    p.strategies.forward_vols = jv<bool>(st, "forward_vols", false);

    const json of = j.value("option_filter", json::object());
    auto& f = p.option_filter;
    f.min_volume             = jopt_num(of, "min_volume");
    f.min_oi                 = jopt_num(of, "min_oi");
    f.min_price              = jopt_num(of, "min_price");
    f.max_bid_ask_spread     = jopt_num(of, "max_bid_ask_spread");
    f.max_bid_ask_spread_pct = jopt_num(of, "max_bid_ask_spread_pct");
    if (of.contains("expiry") && of["expiry"].is_string()) f.expiry = of["expiry"].get<std::string>();
    f.days_to_expiry    = jrange(of, "days_to_expiry_range");
    f.volume_ratio      = jrange(of, "volume_ratio_range");
    f.moneyness         = jrange(of, "moneyness_range");
    f.require_two_sided = jv<bool>(of, "require_two_sided", true);

    const json sf_ = j.value("strategy_filter", json::object());
    auto& g = p.strategy_filter;
    const auto direction = jv<std::string>(sf_, "direction", "LONG");
    if (direction != "LONG" && direction != "SHORT") throw ScreenerParseError{"bad_direction"};
    g.long_direction = direction == "LONG";
    g.debit       = jrange(sf_, "debit_range");
    g.credit      = jrange(sf_, "credit_range");
    g.gain        = jrange(sf_, "potential_gain_range");
    g.loss        = jrange(sf_, "potential_loss_range");
    g.rr          = jrange(sf_, "rr_range");
    g.net_delta   = jrange(sf_, "net_delta_range");
    g.net_theta   = jrange(sf_, "net_theta_range");
    g.net_vega    = jrange(sf_, "net_vega_range");
    g.iv          = jrange(sf_, "iv_range");
    g.forward_vol = jrange(sf_, "forward_vol_range");
    g.edge        = jrange(sf_, "edge_range");

    const json rk = j.value("rank", json::object());
    p.rank.key = parse_rank_key(jv<std::string>(rk, "key", "rr"));
    // Natural order: smallest loss / cost first, largest of everything else first.
    const bool natural_desc = p.rank.key != ScreenRankKey::Loss && p.rank.key != ScreenRankKey::Cost;
    p.rank.descending = jv<bool>(rk, "descending", natural_desc);
    p.rank.top_n      = std::clamp(jv<int>(rk, "top_n", 20), 1, 500);
    return p;
}

json to_output(const StrategyRecord& s, const ScreenContext& c, int rank) {
    json legs = json::array();
    for (int k = 0; k < s.n_legs; ++k) {
        const ScreenLeg& leg = s.legs[k];
        const ChainOption& o = c.chain[leg.option_index];
        legs.push_back({{"symbol", o.symbol}, {"option_type", o.is_call ? "call" : "put"}, {"expiry", o.expiry},
                        {"strike", o.strike}, {"years", o.years}, {"forward", o.forward}, {"qty", leg.qty},
                        {"fill_price", screen_fill_price(o, leg.qty, c.price_mode)}, {"mark", o.mark},
                        {"iv", o.iv},
                        {"model_price", c.model_px.empty() ? kNaN : c.model_px[leg.option_index]},
                        {"delta", o.delta}, {"gamma", o.gamma}, {"theta", o.theta}, {"vega", o.vega}});
    }
    const ScreenMetrics& m = s.m;
    // Non-finite values (unbounded gain/loss, missing model) serialize as null.
    return {{"rank", rank}, {"kind", kind_name(s.kind)}, {"direction", s.long_direction ? "LONG" : "SHORT"},
            {"label", make_label(s, c.chain)}, {"legs", std::move(legs)},
            {"debit", m.debit}, {"credit", m.credit}, {"cost", m.cost},
            {"max_gain", m.max_gain}, {"max_loss", m.max_loss}, {"rr", m.rr},
            {"max_gain_unbounded", std::isinf(m.max_gain)}, {"max_loss_unbounded", std::isinf(m.max_loss)},
            {"net_delta", m.net_delta}, {"net_gamma", m.net_gamma},
            {"net_theta", m.net_theta}, {"net_vega", m.net_vega},
            {"avg_iv", m.avg_iv}, {"model_value", m.model_value}, {"edge", m.edge},
            {"forward_vol", m.forward_vol}};
}

}  // namespace

std::string run_screener_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"spot", "chain"}, "missing_required_screener_fields");
    if (!req) return req.error();
    ScreenerParams p;
    try {
        p = parse_screener_params(*req);
    } catch (const ScreenerParseError& e) {
        return err_response(e.code);
    } catch (const json::exception&) {
        return err_response("bad_screener_request");
    }
    const auto t0 = std::chrono::steady_clock::now();
    const int n_in = static_cast<int>(p.chain.size());
    const std::vector<int> idx = screen_filter_options(p.chain, p.option_filter);
    if (p.compute_greeks) screen_compute_greeks(p.chain, idx, p.rate);
    const std::vector<double> model_px = model_prices(p, idx);

    const ScreenContext ctx{
        .chain = p.chain,
        .model_px = model_px,
        .multiplier = p.multiplier,
        .price_mode = p.price_mode,
        .filter = &p.strategy_filter,
        .rank = p.rank,
    };

    using Generator = void (*)(const ScreenContext&, std::span<const int>, ScreenTopN&, ScreenCounts&);
    const std::pair<bool, std::pair<ScreenKind, Generator>> plan[] = {
        {p.strategies.single_calls, {ScreenKind::Single, screen_generate_single_calls}},
        {p.strategies.iron_condors, {ScreenKind::IronCondor, screen_generate_iron_condors}},
        {p.strategies.straddles, {ScreenKind::Straddle, screen_generate_straddles}},
        {p.strategies.strangles, {ScreenKind::Strangle, screen_generate_strangles}},
        {p.strategies.forward_vols, {ScreenKind::ForwardVol, screen_generate_forward_vols}},
    };
    ScreenTopN top(p.rank);
    json by_kind = json::object();
    long long generated = 0, passed = 0;
    for (const auto& [enabled, entry] : plan) {
        if (!enabled) continue;
        ScreenTopN kind_top(p.rank);
        ScreenCounts counts;
        entry.second(ctx, idx, kind_top, counts);
        top.merge(std::move(kind_top));
        by_kind[kind_name(entry.first)] = {{"generated", counts.generated}, {"passed", counts.passed}};
        generated += counts.generated;
        passed += counts.passed;
    }
    json strategies = json::array();
    for (const auto& s : top.finish()) strategies.push_back(to_output(s, ctx, static_cast<int>(strategies.size()) + 1));
    const double engine_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    json summary = {
        {"n_options_in", n_in},
        {"n_options_after_filter", static_cast<int>(idx.size())},
        {"n_generated", generated},
        {"n_passed", passed},
        {"n_returned", static_cast<int>(strategies.size())},
        {"by_kind", std::move(by_kind)},
        {"rank_key", jv<std::string>(req->value("rank", json::object()), "key", "rr")},
        {"rank_descending", p.rank.descending},
        {"model_vol", p.model_vol},
        {"price_mode", p.price_mode == ScreenPriceMode::Mid ? "mid" : "executable"},
        {"multiplier", p.multiplier},
        {"engine_runtime_ms", engine_ms},
    };
    return ok_response(std::move(summary), {{"strategies", std::move(strategies)}}, t0_ms);
}

}  // namespace quant::analytics
