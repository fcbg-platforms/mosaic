// Hermetic stand-in for the real "python" interpreter AnalysisManager
// launches, used only by tests/test_analysis_manager.cpp (via
// AnalysisManager::set_python_path()). AnalysisManager::launch() only ever
// does `process->start(python, {scriptPath} + args)` -- it never checks
// that `python` is really Python, so a small, fully controllable, plain
// C++ stub lets the tests exercise the real subprocess-launch/stdout-
// stderr-separation/env-passthrough/exit-code path deterministically,
// without depending on a real interpreter being installed anywhere.
//
// Behaviour is driven entirely by MOSAIC_TEST_STUB_* environment
// variables, all under one prefix so they can never collide with anything
// a real analysis script might read:
//
//   MOSAIC_TEST_STUB_SLEEP_MS   - sleep this many ms before printing
//                                 anything (default 0). Lets a test
//                                 deterministically observe "still
//                                 running" instead of racing a near-
//                                 instant real exit.
//   MOSAIC_TEST_STUB_ECHO_ENV   - comma-separated list of env var NAMES;
//                                 for each, prints "ENV:<name>=<value>"
//                                 to stdout ("ENV:<name>=<unset>" if
//                                 absent) -- lets a test confirm a given
//                                 variable actually reached this process.
//   MOSAIC_TEST_STUB_EXIT_CODE  - this process's exit code (default 0).
//
// Always prints one fixed line to stdout and one to stderr, and echoes
// argv[1] (the "script path" AnalysisManager passed) -- see the STDOUT_
// LINE/STDERR_LINE/SCRIPT: prefixes below.

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : fallback;
}

std::vector<std::string> split_csv(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) { out.push_back(item); }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    const int sleepMs = std::atoi(env_or("MOSAIC_TEST_STUB_SLEEP_MS", "0").c_str());
    if (sleepMs > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }

    if (argc > 1) {
        std::cout << "SCRIPT:" << argv[1] << "\n";
    }

    for (const auto& name : split_csv(env_or("MOSAIC_TEST_STUB_ECHO_ENV", ""))) {
        const char* value = std::getenv(name.c_str());
        std::cout << "ENV:" << name << "=" << (value ? value : "<unset>") << "\n";
    }

    std::cout << "STDOUT_LINE" << std::endl;
    std::cerr << "STDERR_LINE" << std::endl;

    return std::atoi(env_or("MOSAIC_TEST_STUB_EXIT_CODE", "0").c_str());
}
