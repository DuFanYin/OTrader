#pragma once

/** Centralized configuration constants shared across engines (paths, default mappings). */

#include <string>
#include <unordered_map>
#include <vector>

namespace utilities::config {

// 默认策略配置文件路径（可根据部署需要统一修改）。
inline constexpr const char* kStrategyConfigPath = "Otrader/strategy_config.json";

// 默认需要预创建的组合名称。
inline const std::vector<std::string> kDefaultPortfolioNamesToCreate = {"SPXW"};

// 默认标的前缀到组合名称的映射（例如 SPX → SPXW）。
inline const std::unordered_map<std::string, std::string> kUnderlyingToPortfolio = {
    {"SPX", "SPXW"},
};

} // namespace utilities::config
