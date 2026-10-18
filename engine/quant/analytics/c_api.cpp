// API/Adapter layer: ABI wrappers + thread control.
// No JSON parsing/assembly logic in this file.

#include "c_api.h"
#include "engine.h"

#include <chrono>
#include <cstring>
#include <string>
#include <algorithm>
#include <iterator>
#include <string_view>
#include <utility>
#include <span>
#include <omp.h>

namespace {

double now_ms() {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    ) / 1000.0;
}

int copy_out(const std::string& response, char* out, int cap, int* written) {
    if (!out || cap <= 1 || !written) return 2;
    const int need = static_cast<int>(response.size());
    if (need + 1 > cap) { *written = need; return 1; }
    std::memcpy(out, response.c_str(), static_cast<size_t>(need));
    out[need] = '\0';
    *written = need;
    return 0;
}

int respond(std::string (*run)(const std::string&, double), const char* req, char* out, int cap,
            int* written) {
    if (!req) return 2;
    return copy_out(run(req, now_ms()), out, cap, written);
}

using MatrixTask = std::pair<std::string_view, std::string (*)(const std::string&, std::span<const double>, double)>;

constexpr MatrixTask kMatrixTasks[] = {
    {"selection", quant::analytics::run_selection_json},
};

using Task = std::pair<std::string_view, std::string (*)(const std::string&, double)>;

constexpr Task kTasks[] = {
    {"pricing", quant::analytics::run_pricing_json},
    {"pricing_batch", quant::analytics::run_pricing_batch_json},
    {"pricing_batch_grid", quant::analytics::run_pricing_batch_grid_json},
    {"hedging", quant::analytics::run_hedging_json},
    {"scenario", quant::analytics::run_scenario_json},
    {"validation_gate", quant::analytics::run_validation_json},
    {"simulation", quant::analytics::run_simulation_json},
    {"stats", quant::analytics::run_stats_json},
    {"optimize", quant::analytics::run_optimize_json},
    {"ito_check", quant::analytics::run_ito_json},
    {"measure_density", quant::analytics::run_measure_density_json},
    {"measure_compare", quant::analytics::run_measure_compare_json},
    {"pde", quant::analytics::run_pde_json},
    {"vol_surface", quant::analytics::run_vol_surface_json},
    {"greek_surface", quant::analytics::run_greek_surface_json},
    {"lattice", quant::analytics::run_lattice_json},
    {"multi_leg", quant::analytics::run_multi_leg_json},
    {"implied_vol", quant::analytics::run_implied_vol_json},
    {"implied_vol_batch", quant::analytics::run_implied_vol_batch_json},
    {"black_batch", quant::analytics::run_black_batch_json},
    {"heston_calibrate", quant::analytics::run_heston_calibrate_json},
    {"heston_price", quant::analytics::run_heston_price_json},
    {"screener", quant::analytics::run_screener_json},
};

}  // namespace

extern "C" {

int quant_run_json(const char* task, const char* req, char* out, int cap, int* written) {
    if (!task) return 2;
    const auto it = std::ranges::find(kTasks, std::string_view(task), &Task::first);
    if (it == std::end(kTasks)) return 3;
    return respond(it->second, req, out, cap, written);
}

int quant_run_matrix(const char* task, const char* req, const double* values, long long n_values, char* out,
                     int cap, int* written) {
    if (!task || !req || (n_values > 0 && !values) || n_values < 0) return 2;
    const auto it = std::ranges::find(kMatrixTasks, std::string_view(task), &MatrixTask::first);
    if (it == std::end(kMatrixTasks)) return 3;
    const std::span<const double> matrix(values, static_cast<std::size_t>(n_values));
    return copy_out(it->second(req, matrix, now_ms()), out, cap, written);
}

const char* quant_contract_version(void) {
    return quant::analytics::kContractVersion;
}

void quant_set_num_threads(int n_threads) {
    omp_set_num_threads(std::max(1, n_threads));
}

int quant_get_max_threads(void) {
    return omp_get_max_threads();
}

}  // extern "C"
