#include "../kernel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <omp.h>

namespace quant {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// seq layout: kind (6 bits) | work item (26 bits) | local counter (32 bits)
uint64_t make_seq(ScreenKind kind, uint64_t work, uint64_t local) {
    return (static_cast<uint64_t>(kind) << 58) | ((work & 0x3FFFFFFull) << 32) | (local & 0xFFFFFFFFull);
}

double risk_reward(double gain, double loss) {
    if (std::isnan(gain) || std::isnan(loss)) return kNaN;
    return loss > 0.0 ? gain / loss : kInf;
}

// Options of one expiry, split by side and sorted by strike.
struct ExpiryGroup {
    std::string      expiry;
    double           years   = 0.0;
    double           forward = 0.0;
    std::vector<int> calls;
    std::vector<int> puts;
};

std::vector<ExpiryGroup> group_by_expiry(std::span<const ChainOption> chain, std::span<const int> idx) {
    std::vector<ExpiryGroup> groups;
    std::unordered_map<std::string, size_t> pos;
    for (const int i : idx) {
        const ChainOption& o = chain[i];
        auto [it, inserted] = pos.try_emplace(o.expiry, groups.size());
        if (inserted) groups.push_back({o.expiry, o.years, o.forward, {}, {}});
        (o.is_call ? groups[it->second].calls : groups[it->second].puts).push_back(i);
    }
    auto by_strike = [&](int a, int b) { return chain[a].strike < chain[b].strike; };
    for (auto& g : groups) {
        std::ranges::stable_sort(g.calls, by_strike);
        std::ranges::stable_sort(g.puts, by_strike);
    }
    std::ranges::stable_sort(groups, [](const ExpiryGroup& a, const ExpiryGroup& b) {
        return a.years != b.years ? a.years < b.years : a.expiry < b.expiry;
    });
    return groups;
}

void consider(const ScreenContext& c, StrategyRecord& s, ScreenTopN& out, ScreenCounts& n) {
    ++n.generated;
    if (!screen_evaluate(c, s)) return;
    if (!screen_pass_strategy_filter(*c.filter, s.m)) return;
    ++n.passed;
    out.push(s);
}

StrategyRecord make_record(ScreenKind kind, bool long_dir, std::initializer_list<ScreenLeg> legs) {
    StrategyRecord s;
    s.kind = kind;
    s.long_direction = long_dir;
    for (const auto& l : legs) s.legs[s.n_legs++] = l;
    return s;
}

}  // namespace

// ── Ranking buffer ────────────────────────────────────────────────────────────

ScreenTopN::ScreenTopN(ScreenRank rank) : rank_(rank) {}

static double rank_value(ScreenRankKey key, const ScreenMetrics& m) {
    switch (key) {
        case ScreenRankKey::RR:         return m.rr;
        case ScreenRankKey::Gain:       return m.max_gain;
        case ScreenRankKey::Loss:       return m.max_loss;
        case ScreenRankKey::Cost:       return m.cost;
        case ScreenRankKey::Credit:     return m.credit;
        case ScreenRankKey::Edge:       return m.edge;
        case ScreenRankKey::ForwardVol: return m.forward_vol;
    }
    return kNaN;
}

bool ScreenTopN::better(const StrategyRecord& a, const StrategyRecord& b) const {
    const double va = rank_value(rank_.key, a.m);
    const double vb = rank_value(rank_.key, b.m);
    const bool na = std::isnan(va), nb = std::isnan(vb);
    if (na != nb) return !na;                       // NaN always ranks last
    if (!na && va != vb) return rank_.descending ? va > vb : va < vb;
    return a.seq < b.seq;
}

void ScreenTopN::shrink() {
    const size_t cap = static_cast<size_t>(std::max(rank_.top_n, 0));
    if (buf_.size() <= cap) return;
    auto cmp = [this](const StrategyRecord& a, const StrategyRecord& b) { return better(a, b); };
    std::nth_element(buf_.begin(), buf_.begin() + static_cast<long>(cap), buf_.end(), cmp);
    buf_.resize(cap);
}

void ScreenTopN::push(const StrategyRecord& s) {
    buf_.push_back(s);
    if (buf_.size() >= 2 * static_cast<size_t>(std::max(rank_.top_n, 0)) + 64) shrink();
}

void ScreenTopN::merge(ScreenTopN&& other) {
    buf_.insert(buf_.end(), other.buf_.begin(), other.buf_.end());
    other.buf_.clear();
    shrink();
}

std::vector<StrategyRecord> ScreenTopN::finish() {
    shrink();
    std::ranges::sort(buf_, [this](const StrategyRecord& a, const StrategyRecord& b) { return better(a, b); });
    return std::move(buf_);
}

// ── Metrics ───────────────────────────────────────────────────────────────────

bool screen_evaluate(const ScreenContext& c, StrategyRecord& s) {
    ScreenMetrics m;
    m.forward_vol = s.m.forward_vol;   // set by the forward-vol generator, kept across evaluation
    const double mult = c.multiplier;
    double model = 0.0, iv_sum = 0.0;
    bool   model_ok = !c.model_px.empty();
    int    iv_n = 0;

    for (int k = 0; k < s.n_legs; ++k) {
        const ScreenLeg&   leg = s.legs[k];
        const ChainOption& o   = c.chain[leg.option_index];
        const double px = screen_fill_price(o, leg.qty, c.price_mode);
        if (std::isnan(px)) return false;
        const double q = leg.qty;
        (q > 0 ? m.debit : m.credit) += px * std::abs(q);   // per unit; multiplier applied below
        m.net_delta += q * o.delta * mult;
        m.net_gamma += q * o.gamma * mult;
        m.net_theta += q * o.theta * mult;
        m.net_vega  += q * o.vega  * mult;
        if (model_ok) {
            const double mp = c.model_px[leg.option_index];
            if (std::isnan(mp)) model_ok = false;
            else model += q * mp * mult;
        }
        if (o.iv > 0.0) { iv_sum += o.iv; ++iv_n; }
    }
    m.debit  *= mult;
    m.credit *= mult;
    m.cost = m.debit - m.credit;
    if (iv_n > 0) m.avg_iv = iv_sum / iv_n;
    if (model_ok) { m.model_value = model; m.edge = model - m.cost; }

    const auto& L = s.legs;
    const auto strike = [&](int k) { return c.chain[L[k].option_index].strike; };
    switch (s.kind) {
        case ScreenKind::Single: {
            const bool is_call = c.chain[L[0].option_index].is_call;
            const double k_notional = strike(0) * mult;
            if (L[0].qty > 0) {
                m.max_gain = is_call ? kInf : k_notional - m.cost;
                m.max_loss = m.cost;
            } else {
                m.max_gain = m.credit;
                m.max_loss = is_call ? kInf : k_notional - m.credit;
            }
            break;
        }
        case ScreenKind::IronCondor: {
            // legs: short call, long call, short put, long put (qty signs flipped when short direction)
            const double wing = std::max(strike(1) - strike(0), strike(2) - strike(3)) * mult;
            if (s.long_direction) {          // credit condor: sell inner, buy outer
                const double net_credit = m.credit - m.debit;
                m.max_gain = net_credit;
                m.max_loss = wing - net_credit;
            } else {                         // reverse condor: buy inner, sell outer
                const double net_debit = m.debit - m.credit;
                m.max_gain = wing - net_debit;
                m.max_loss = net_debit;
            }
            break;
        }
        case ScreenKind::Straddle:
        case ScreenKind::Strangle:
            m.max_gain = s.long_direction ? kInf : m.credit;
            m.max_loss = s.long_direction ? m.cost : kInf;
            break;
        case ScreenKind::ForwardVol:
            // Calendar: long = sell near / buy far. Max gain depends on the far
            // leg's value at the near expiry, so it has no closed form here.
            m.max_gain = s.long_direction ? kNaN : -m.cost;
            m.max_loss = s.long_direction ? m.cost : kNaN;
            break;
    }
    m.rr = risk_reward(m.max_gain, m.max_loss);
    s.m = m;
    return true;
}

bool screen_pass_strategy_filter(const ScreenStrategyFilter& f, const ScreenMetrics& m) {
    if (f.debit.on  && m.debit  > 0.0 && !f.debit.admits(m.debit))   return false;
    if (f.credit.on && m.credit > 0.0 && !f.credit.admits(m.credit)) return false;
    if (!f.gain.admits(m.max_gain) || !f.loss.admits(m.max_loss) || !f.rr.admits(m.rr)) return false;
    if (!f.net_delta.admits(m.net_delta) || !f.net_theta.admits(m.net_theta)
        || !f.net_vega.admits(m.net_vega)) return false;
    if (!std::isnan(m.avg_iv) && !f.iv.admits(m.avg_iv)) return false;
    if (!std::isnan(m.forward_vol) && !f.forward_vol.admits(m.forward_vol)) return false;
    if (!f.edge.admits(m.edge)) return false;
    return true;
}

// ── Generators ────────────────────────────────────────────────────────────────

void screen_generate_single_calls(const ScreenContext& c, std::span<const int> idx, ScreenTopN& out, ScreenCounts& n) {
    const bool lg = c.filter->long_direction;
    uint64_t local = 0;
    for (const auto& g : group_by_expiry(c.chain, idx)) {
        for (const int ci : g.calls) {
            if (!(c.chain[ci].strike > g.forward)) continue;   // OTM vs this expiry's forward
            auto s = make_record(ScreenKind::Single, lg, {{ci, lg ? 1 : -1}});
            s.seq = make_seq(ScreenKind::Single, 0, local++);
            consider(c, s, out, n);
        }
    }
}

void screen_generate_iron_condors(const ScreenContext& c, std::span<const int> idx, ScreenTopN& out, ScreenCounts& n) {
    const bool lg = c.filter->long_direction;
    const int  sgn = lg ? 1 : -1;
    const auto groups = group_by_expiry(c.chain, idx);

    // One work item per (expiry, short call); the inner three loops run inside it.
    std::vector<std::pair<int, int>> work;
    for (int gi = 0; gi < static_cast<int>(groups.size()); ++gi)
        for (int p = 0; p < static_cast<int>(groups[gi].calls.size()); ++p)
            if (c.chain[groups[gi].calls[p]].strike > groups[gi].forward) work.emplace_back(gi, p);

    const int n_work = static_cast<int>(work.size());
#pragma omp parallel
    {
        ScreenTopN   local_top(c.rank);
        ScreenCounts local_n;
#pragma omp for schedule(dynamic, 1) nowait
        for (int w = 0; w < n_work; ++w) {
            const ExpiryGroup& g = groups[work[w].first];
            const int sc = g.calls[work[w].second];
            const double k_sc = c.chain[sc].strike;
            uint64_t local = 0;
            for (const int bc : g.calls) {
                if (!(c.chain[bc].strike > k_sc)) continue;
                for (const int sp : g.puts) {
                    const double k_sp = c.chain[sp].strike;
                    if (!(k_sp < g.forward)) continue;
                    for (const int bp : g.puts) {
                        if (!(c.chain[bp].strike < k_sp)) continue;
                        auto s = make_record(ScreenKind::IronCondor, lg,
                                             {{sc, -sgn}, {bc, sgn}, {sp, -sgn}, {bp, sgn}});
                        s.seq = make_seq(ScreenKind::IronCondor, static_cast<uint64_t>(w), local++);
                        consider(c, s, local_top, local_n);
                    }
                }
            }
        }
#pragma omp critical
        {
            out.merge(std::move(local_top));
            n.generated += local_n.generated;
            n.passed    += local_n.passed;
        }
    }
}

void screen_generate_straddles(const ScreenContext& c, std::span<const int> idx, ScreenTopN& out, ScreenCounts& n) {
    const bool lg = c.filter->long_direction;
    const int  q = lg ? 1 : -1;
    uint64_t local = 0;
    for (const auto& g : group_by_expiry(c.chain, idx)) {
        for (const int ci : g.calls)
            for (const int pi : g.puts) {
                if (c.chain[ci].strike != c.chain[pi].strike) continue;
                auto s = make_record(ScreenKind::Straddle, lg, {{ci, q}, {pi, q}});
                s.seq = make_seq(ScreenKind::Straddle, 0, local++);
                consider(c, s, out, n);
            }
    }
}

void screen_generate_strangles(const ScreenContext& c, std::span<const int> idx, ScreenTopN& out, ScreenCounts& n) {
    const bool lg = c.filter->long_direction;
    const int  q = lg ? 1 : -1;
    uint64_t local = 0;
    for (const auto& g : group_by_expiry(c.chain, idx)) {
        for (const int ci : g.calls) {
            if (!(c.chain[ci].strike > g.forward)) continue;
            for (const int pi : g.puts) {
                if (!(c.chain[pi].strike < g.forward)) continue;
                auto s = make_record(ScreenKind::Strangle, lg, {{ci, q}, {pi, q}});
                s.seq = make_seq(ScreenKind::Strangle, 0, local++);
                consider(c, s, out, n);
            }
        }
    }
}

void screen_generate_forward_vols(const ScreenContext& c, std::span<const int> idx, ScreenTopN& out, ScreenCounts& n) {
    const bool lg = c.filter->long_direction;
    const auto groups = group_by_expiry(c.chain, idx);

    // (is_call, strike) -> first option at that key, per expiry
    std::vector<std::map<std::pair<bool, double>, int>> keyed(groups.size());
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        for (const int i : groups[gi].calls) keyed[gi].try_emplace({true,  c.chain[i].strike}, i);
        for (const int i : groups[gi].puts)  keyed[gi].try_emplace({false, c.chain[i].strike}, i);
    }

    uint64_t local = 0;
    for (size_t a = 0; a < groups.size(); ++a)
        for (size_t b = a + 1; b < groups.size(); ++b)
            for (const auto& [key, near] : keyed[a]) {
                const auto it = keyed[b].find(key);
                if (it == keyed[b].end()) continue;
                const int far = it->second;
                const ChainOption& o1 = c.chain[near];
                const ChainOption& o2 = c.chain[far];
                const double num = o2.iv * o2.iv * o2.years - o1.iv * o1.iv * o1.years;
                const double den = o2.years - o1.years;
                if (!(o1.iv > 0.0 && o2.iv > 0.0) || den <= 0.0 || num <= 0.0) continue;

                auto s = make_record(ScreenKind::ForwardVol, lg, {{near, lg ? -1 : 1}, {far, lg ? 1 : -1}});
                s.m.forward_vol = std::sqrt(num / den);
                s.seq = make_seq(ScreenKind::ForwardVol, 0, local++);
                consider(c, s, out, n);
            }
}

}  // namespace quant
