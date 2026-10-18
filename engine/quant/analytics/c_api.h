#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// ── Tasks ─────────────────────────────────────────────────────────────────────
// Runs `task` (pricing, screener, ...: the table in c_api.cpp) on a JSON request. Returns 0 on success,
// 1 if the buffer is too small (out_written = required size), 2 on bad args, 3 for no such task.
int quant_run_json(const char* task, const char* request_json, char* out_response_json, int out_capacity,
                   int* out_written);
// The same for a task on a matrix of doubles (selection: trials x periods, row by row) passed beside the request.
int quant_run_matrix(const char* task, const char* request_json, const double* values, long long n_values,
                     char* out_response_json, int out_capacity, int* out_written);

// ── Version and thread control ────────────────────────────────────────────────
const char* quant_contract_version(void);   // kContractVersion: the shape of every response
void quant_set_num_threads(int n_threads);
int  quant_get_max_threads(void);

#ifdef __cplusplus
}
#endif
