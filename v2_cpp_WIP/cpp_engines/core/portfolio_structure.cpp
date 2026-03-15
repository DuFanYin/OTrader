/** PortfolioStructure: load_contracts → portfolios + contracts. */

#include "portfolio_structure.hpp"

namespace engines {

namespace {

const std::vector<std::string> kPortfolioNamesToCreate = {"SPXW"};
const std::unordered_map<std::string, std::string> kUnderlyingToPortfolio = {
    {"SPX", "SPXW"},
};

std::string portfolio_name_for_underlying(const std::string& symbol_prefix) {
    auto it = kUnderlyingToPortfolio.find(symbol_prefix);
    return (it != kUnderlyingToPortfolio.end()) ? it->second : symbol_prefix;
}

} // namespace

void PortfolioStructure::ensure_portfolios_created() {
    for (const std::string& name : kPortfolioNamesToCreate) {
        get_or_create_portfolio(name);
    }
}

void PortfolioStructure::ensure_portfolio(const std::string& portfolio_name) {
    get_or_create_portfolio(portfolio_name);
}

void PortfolioStructure::clear() {
    portfolios_.clear();
    contracts_.clear();
}

void PortfolioStructure::process_option(const utilities::ContractData& contract) {
    process_contract(contract, true);
}

void PortfolioStructure::process_underlying(const utilities::ContractData& contract) {
    process_contract(contract, false);
}

void PortfolioStructure::process_option_for_portfolio(const std::string& portfolio_name,
                                                      const utilities::ContractData& contract) {
    contracts_[contract.symbol] = contract;
    utilities::PortfolioData* port = get_portfolio(portfolio_name);
    if (port != nullptr) {
        port->add_option(contract);
    }
}

void PortfolioStructure::process_underlying_for_portfolio(const std::string& portfolio_name,
                                                          const utilities::ContractData& contract) {
    contracts_[contract.symbol] = contract;
    utilities::PortfolioData* port = get_portfolio(portfolio_name);
    if (port != nullptr) {
        port->set_underlying(contract);
    }
}

void PortfolioStructure::process_contract(const utilities::ContractData& contract, bool is_option) {
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
        return;
    }
    if (is_option) {
        port->add_option(contract);
    } else {
        port->set_underlying(contract);
    }
}

void PortfolioStructure::finalize_all_chains() {
    for (auto& kv : portfolios_) {
        if (kv.second) {
            kv.second->finalize_chains();
        }
    }
}

utilities::PortfolioData*
PortfolioStructure::get_or_create_portfolio(const std::string& portfolio_name) {
    auto it = portfolios_.find(portfolio_name);
    if (it != portfolios_.end()) {
        return it->second.get();
    }
    portfolios_[portfolio_name] = std::make_unique<utilities::PortfolioData>(portfolio_name);
    return portfolios_[portfolio_name].get();
}

utilities::PortfolioData* PortfolioStructure::get_portfolio(const std::string& portfolio_name) {
    auto it = portfolios_.find(portfolio_name);
    return (it != portfolios_.end()) ? it->second.get() : nullptr;
}

std::vector<std::string> PortfolioStructure::get_all_portfolio_names() const {
    std::vector<std::string> out;
    out.reserve(portfolios_.size());
    for (const auto& kv : portfolios_) {
        out.push_back(kv.first);
    }
    return out;
}

const utilities::ContractData* PortfolioStructure::get_contract(const std::string& symbol) const {
    auto it = contracts_.find(symbol);
    return (it != contracts_.end()) ? &it->second : nullptr;
}

std::vector<utilities::ContractData> PortfolioStructure::get_all_contracts() const {
    std::vector<utilities::ContractData> out;
    out.reserve(contracts_.size());
    for (const auto& kv : contracts_) {
        out.push_back(kv.second);
    }
    return out;
}

} // namespace engines
