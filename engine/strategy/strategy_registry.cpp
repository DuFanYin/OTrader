/** Strategy registry; classes register during static init, of the program or a loaded library. */

#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include "strategy/strategy_context.hpp"
#include "utilities/text.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace strategy {

namespace {

struct Entry {
    StrategyFactory factory;
    std::vector<model::Parameter> parameters; // the template's, then the class's
    std::string source;                    // the file it registered from
    std::string library;                   // the library it was loaded from; empty: built in
};

struct Registry {
    std::unordered_map<std::string, Entry> classes;
    std::mutex mtx;
};
auto registry() -> Registry& {
    static Registry r;
    return r;
}

/** A library file as it was when last tried: tried again only once it changes. */
struct Tried {
    std::filesystem::file_time_type modified;
    std::uintmax_t size = 0;
    std::string why; // empty: loaded
};

/** The libraries tried and the directories loaded; one load at a time. */
struct Libraries {
    std::map<std::string, Tried> tried; // by path
    std::vector<std::filesystem::path> directories;
    std::mutex mtx;
};
auto libraries() -> Libraries& {
    static Libraries l;
    return l;
}

/** While a library loads (its classes registering, on the loading thread): its path, and the
 * classes it could not register. */
thread_local const std::string* loading = nullptr;
thread_local std::vector<std::string>* not_registered = nullptr;

constexpr std::string_view kApiMarker = "optra-strategy-api:"; // kStrategyApi's start

/** Why the library cannot be loaded into this program, read from its file before loading it
 * (loading runs its registrations); empty when it can. */
auto api_mismatch(const std::filesystem::path& library) -> std::string {
    // Mapped, not read into a string: a library is a few MB and the stamp a few bytes.
    const int fd = ::open(library.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::format("cannot open it: {}", std::strerror(errno));
    }
    struct stat st{};
    void* mapped = ::fstat(fd, &st) == 0 && st.st_size > 0
                       ? ::mmap(nullptr, static_cast<size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0)
                       : MAP_FAILED;
    ::close(fd);
    if (mapped == MAP_FAILED) {
        return "cannot read it";
    }
    const std::string_view content(static_cast<const char*>(mapped), static_cast<size_t>(st.st_size));
    const size_t at = content.find(kApiMarker);
    const std::string api = at == std::string_view::npos
                                ? std::string{}
                                : std::string(content.substr(at + kApiMarker.size(), 16));
    ::munmap(mapped, static_cast<size_t>(st.st_size));
    if (at == std::string_view::npos) {
        return "not a strategy library (it has no strategy API stamp)";
    }
    if (api != OPTRA_STRATEGY_API) {
        return std::format("built against other strategy headers (its API {}, this program's {}): "
                           "rebuild it, or restart the program on the new build",
                           api, OPTRA_STRATEGY_API);
    }
    return {};
}

/** Loads the library from a copy of its own (deleted once loaded), so a new build of the same file
 * loads beside the old one rather than being taken for it; never unloaded. Empty, or why not. */
auto open_library(const std::filesystem::path& library) -> std::string {
    if (std::string why = api_mismatch(library); !why.empty()) {
        return why;
    }
    static std::atomic<int> copies{0};
    const std::filesystem::path copy =
        std::filesystem::temp_directory_path() /
        std::format("optra-strategy-{}-{}-{}", getpid(), copies++, library.filename().string());
    std::filesystem::copy_file(library, copy, std::filesystem::copy_options::overwrite_existing);
    const std::string path = library.string();
    std::vector<std::string> refused_classes;
    loading = &path;
    not_registered = &refused_classes;
    void* handle = dlopen(copy.c_str(), RTLD_NOW | RTLD_LOCAL);
    loading = nullptr;
    not_registered = nullptr;
    std::filesystem::remove(copy);
    if (handle == nullptr) {
        const char* error = dlerror();
        return error != nullptr ? error : "dlopen failed";
    }
    if (!refused_classes.empty()) {
        return std::format("{} already registered by another library or the program",
                           utilities::join(refused_classes));
    }
    return {};
}

/** The class's entry; throws std::invalid_argument for an unknown class. */
auto entry_of(const std::string& class_name) -> Entry {
    {
        std::scoped_lock lock(registry().mtx);
        if (const auto it = registry().classes.find(class_name); it != registry().classes.end()) {
            return it->second;
        }
    }
    throw std::invalid_argument(
        std::format("Unknown strategy '{}'. Available: {}", class_name,
                    utilities::join(StrategyRegistry::get_all_strategy_class_names())));
}

} // namespace

auto StrategyRegistry::add(const std::string& class_name, StrategyFactory factory,
                           std::span<const model::Parameter> parameters, std::string source) -> bool {
    Entry entry{.factory = std::move(factory),
                .parameters = {},
                .source = std::move(source),
                .library = loading != nullptr ? *loading : std::string{}};
    std::ranges::copy(Strategy::base_parameters(),
                      std::back_inserter(entry.parameters));
    std::ranges::copy(parameters, std::back_inserter(entry.parameters));
    std::scoped_lock lock(registry().mtx);
    if (const auto it = registry().classes.find(class_name);
        loading != nullptr && it != registry().classes.end() &&
        it->second.library != entry.library) {
        not_registered->push_back(class_name);
        return false;
    }
    registry().classes[class_name] = std::move(entry);
    return true;
}

namespace {

/** The library file loaded, unless it is as it was when last tried; a file gone is tried again if it
 * comes back. Under libraries().mtx. */
void try_library(Libraries& l, const std::filesystem::path& file) {
    std::error_code error;
    Tried now{.modified = std::filesystem::last_write_time(file, error), .size = 0, .why = {}};
    if (error) {
        return;
    }
    now.size = std::filesystem::file_size(file, error);
    if (error) {
        return;
    }
    if (const auto it = l.tried.find(file.string());
        it != l.tried.end() && it->second.modified == now.modified && it->second.size == now.size) {
        return;
    }
    try {
        now.why = open_library(file);
    } catch (const std::exception& e) {
        now.why = e.what();
    }
    l.tried[file.string()] = std::move(now);
}

} // namespace

void StrategyRegistry::load(const std::filesystem::path& directory) {
    std::scoped_lock lock(libraries().mtx);
    Libraries& l = libraries();
    if (std::ranges::find(l.directories, directory) == l.directories.end()) {
        l.directories.push_back(directory);
    }
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (const auto& file : std::filesystem::directory_iterator(directory, error)) {
        if (file.is_regular_file() && file.path().extension() == ".so") {
            files.push_back(file.path());
        }
    }
    std::ranges::sort(files);
    for (const std::filesystem::path& file : files) {
        try_library(l, file);
    }
}

void StrategyRegistry::reload() {
    std::vector<std::filesystem::path> directories;
    {
        std::scoped_lock lock(libraries().mtx);
        directories = libraries().directories;
    }
    for (const std::filesystem::path& directory : directories) {
        load(directory);
    }
}

auto StrategyRegistry::beside_executable() -> std::filesystem::path {
    return std::filesystem::read_symlink("/proc/self/exe").parent_path() / "strategies";
}

auto StrategyRegistry::refused() -> std::vector<Refusal> {
    std::scoped_lock lock(libraries().mtx);
    std::vector<Refusal> out;
    for (const auto& [library, tried] : libraries().tried) {
        if (!tried.why.empty()) {
            out.push_back({.library = library, .why = tried.why});
        }
    }
    return out;
}

auto StrategyRegistry::library(const std::string& class_name) -> std::string {
    return entry_of(class_name).library;
}

auto StrategyRegistry::has(const std::string& class_name) -> bool {
    std::scoped_lock lock(registry().mtx);
    return registry().classes.contains(class_name);
}

auto StrategyRegistry::get_all_strategy_class_names() -> std::vector<std::string> {
    std::scoped_lock lock(registry().mtx);
    std::vector<std::string> names;
    names.reserve(registry().classes.size());
    std::ranges::copy(registry().classes | std::views::keys, std::back_inserter(names));
    std::ranges::sort(names);
    return names;
}

auto StrategyRegistry::parameters(const std::string& class_name) -> std::vector<model::Parameter> {
    return entry_of(class_name).parameters;
}

auto StrategyRegistry::source(const std::string& class_name) -> std::string {
    return entry_of(class_name).source;
}

auto StrategyRegistry::resolve_setting(const std::string& class_name, const model::Values& setting)
    -> model::Values {
    return model::resolve(parameters(class_name), setting, class_name);
}

auto StrategyRegistry::create(StrategyContext& context) -> std::unique_ptr<Strategy> {
    return entry_of(context.record().class_name).factory(context);
}

} // namespace strategy
