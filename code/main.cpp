/**
 * main.cpp
 * Optimal Sample Selection System — main program entry point
 *
 * Contains:
 *   1. Automated test cases (Example 1, Example 5)
 *   2. Interactive menu (parameter input, sample selection, generation, file management)
 *
 * Build (macOS / Linux):
 *   g++ -std=c++17 -O2 -o optimal_sample SampleSelectSystem.cpp ILPSolver.cpp main.cpp
 *   ./optimal_sample
 *
 * Build (Windows MSVC):
 *   cl /std:c++17 /O2 /EHsc SampleSelectSystem.cpp ILPSolver.cpp main.cpp /Fe:optimal_sample.exe
 *   optimal_sample.exe
 */

/** 
 * 
 * @author : Yanboxiang 
 * @file: main file
 *
 **/

#include "SampleSelectSystem.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>      // std::iota
#include <sstream>
#include <string>
#include <vector>

// ── Helper: clear cin error state and discard remaining input on the line ─────
static void clearCin()
{
    std::cin.clear();
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

// ── Helper: safe integer read ─────────────────────────────────────────────────
static bool readInt(int& val, const std::string& prompt)
{
    std::cout << prompt;
    if (!(std::cin >> val)) {
        clearCin();
        std::cerr << "[Input Error] Please enter an integer.\n";
        return false;
    }
    return true;
}

// ════════════════════════════════════════════════════════════════════════════════
// Automated tests: run reference examples from the specification
// ════════════════════════════════════════════════════════════════════════════════

/**
 * @brief Run an example and print/save the result
 * @param m,n,k,j,s,minCover  Parameters
 * @param samples              Preset samples (empty = random)
 * @param label                Example name (used as heading)
 * @param outputDir            DB file output directory
 */
static void runExample(int m, int n, int k, int j, int s, int minCover,
                       const std::vector<int>& samples,
                       const std::string& label,
                       const std::string& outputDir = ".")
{
    std::cout << "\n╔══════════════════════════════════════════════════╗\n";
    std::cout << "║  " << label << "\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n";

    try {
        SampleSelectSystem sys(m, n, k, j, s, minCover);
        sys.printParams();

        // Set samples
        if (samples.empty()) {
            sys.randomSamples();
        } else {
            if (!sys.inputSamples(samples)) {
                std::cerr << "[Example Failed] Invalid sample input.\n";
                return;
            }
        }

        // Generate optimal k-groups
        auto groups = sys.generateOptimalGroups();

        // Print results
        sys.printGroups(groups);

        // Save to DB file
        std::string path = sys.saveToFile(1, groups, outputDir);
        if (!path.empty()) {
            // Verify: reload the saved file
            auto loaded = sys.loadFromFile(path);
            std::cout << ">> Verify: loaded " << loaded.size()
                      << " groups from file, "
                      << (loaded.size() == groups.size() ? "matches generated result ✓" : "does not match ✗")
                      << "\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "[Exception] " << e.what() << "\n";
    }
}

// ════════════════════════════════════════════════════════════════════════════════
// Interactive menu
// ════════════════════════════════════════════════════════════════════════════════

static void printMainMenu(const std::string& currentPoolFile)
{
    std::string poolStatus = currentPoolFile.empty()
        ? "(not loaded, using default 1~m)"
        : "(loaded: " + currentPoolFile + ")";

    std::cout << "\n┌────────── An Optimal Samples Selection System ──────────┐\n";
    std::cout << "│  1. Set parameters and generate optimal k-groups         │\n";
    std::cout << "│  2. List DB files                                         │\n";
    std::cout << "│  3. Read and display a DB file                            │\n";
    std::cout << "│  4. Delete an entire DB file                              │\n";
    std::cout << "│  5. Delete a specific k-group from a DB file              │\n";
    std::cout << "│  6. Run built-in test cases (Example 1 & Example 5)      │\n";
    std::cout << "│  7. Import sample pool file                               │\n";
    std::cout << "│  8. View current pool status                              │\n";
    std::cout << "│  9. ILP exact solver (requires HiGHS)                    │\n";
    std::cout << "│  0. Exit                                                   │\n";
    std::cout << "└──────────────────────────────────────────────────────────┘\n";
    std::cout << "  Pool: " << poolStatus << "\n";
    std::cout << "Select: ";
}

// Menu option 1: enter parameters and generate
// currentPoolFile: if non-empty, load this pool file after constructing sys
static void menuGenerate(const std::string& outputDir,
                         const std::string& currentPoolFile)
{
    int m, n, k, j, s, minCover;

    std::cout << "\n── Parameter Input ───────────────────────────────────────\n";
    std::cout << "  Range: 45≤m≤54, 7≤n≤25, 4≤k≤7, s≤j≤k, s≥3\n\n";

    if (!readInt(m, "  m (total samples, 45~54): "))   return;
    if (!readInt(n, "  n (selected samples, 7~25): "))   return;
    if (!readInt(k, "  k (group size, 4~7, default 6): ")) return;
    if (!readInt(j, "  j (cover subset size): "))        return;
    if (!readInt(s, "  s (min intersection size): "))         return;
    if (!readInt(minCover, "  minCover (min s-subsets covered per j-subset, usually 1): ")) return;

    try {
        SampleSelectSystem sys(m, n, k, j, s, minCover);
        sys.printParams();

        // If a pool file is loaded, try to apply it (loadSamplePool validates m internally)
        if (!currentPoolFile.empty()) {
            std::cout << "  [Pool] Loading: " << currentPoolFile << "\n";
            if (!sys.loadSamplePool(currentPoolFile)) {
                std::cout << "  [Info] Pool load failed, falling back to default 1~m range.\n";
            }
        }

        // Select sample mode
        std::cout << "\n  Sample selection:\n";
        std::cout << "    1. Random from pool"
                  << (currentPoolFile.empty() ? " (default 1~m)" : " (pool loaded)")
                  << "\n";
        std::cout << "    2. Manual input\n";
        std::cout << "  Select: ";
        int mode;
        if (!(std::cin >> mode)) { clearCin(); return; }

        if (mode == 1) {
            sys.randomSamples();
        } else if (mode == 2) {
            std::cout << "  Enter " << n << " samples (1~" << m
                      << ", space separated):\n  ";
            std::vector<int> userInput(n);
            bool ok = true;
            for (int i = 0; i < n; ++i) {
                if (!(std::cin >> userInput[i])) { clearCin(); ok = false; break; }
            }
            if (!ok || !sys.inputSamples(userInput)) return;
        } else {
            std::cout << "[Info] Invalid choice, using random mode.\n";
            sys.randomSamples();
        }

        // Generate
        auto groups = sys.generateOptimalGroups();
        sys.printGroups(groups);

        // Ask whether to save
        std::cout << "\n  Save results to DB file? (1=yes/0=no): ";
        int save;
        if (!(std::cin >> save)) { clearCin(); return; }
        if (save == 1) {
            int runCount;
            if (!readInt(runCount, "  Run index: ")) return;
            sys.saveToFile(runCount, groups, outputDir);
        }

    } catch (const std::exception& e) {
        std::cerr << "[Error] " << e.what() << "\n";
    }
}

// Menu option 7: import pool file
// Returns the chosen file path (empty string on failure)
static std::string menuLoadPool(const std::string& outputDir)
{
    std::cout << "\n── Import Sample Pool File ───────────────────────────────\n";
    std::cout << "  Format: lines starting with # are comments; numbers comma-separated; count must equal m\n";
    std::cout << "  Example: " << outputDir << "/pool_test_m45.txt\n\n";
    std::cout << "  Enter pool filename (or full path): ";
    std::string filename;
    std::cin >> filename;

    // If only a filename was entered, prepend the output directory
    if (filename.find('/') == std::string::npos &&
        filename.find('\\') == std::string::npos) {
        filename = outputDir + "/" + filename;
    }

    // Pre-check the file exists (m validation is done later in menuGenerate)
    std::ifstream probe(filename);
    if (!probe.is_open()) {
        std::cerr << "  [Error] File not found or unreadable: " << filename << "\n";
        return "";
    }
    probe.close();

    std::cout << "  >> Pool file recorded: " << filename << "\n";
    std::cout << "  [Info] Next generation (option 1) will draw n samples from this pool.\n";
    std::cout << "  [Info] Pool count must match m; otherwise falls back to default range.\n";
    return filename;
}

// Menu option 9: ILP exact solver
static void menuILP(const std::string& outputDir,
                    const std::string& currentPoolFile)
{
    int m, n, k, j, s, minCover;

    std::cout << "\n── ILP Exact Solver Parameter Input ─────────────────────\n";
    std::cout << "  Only supports C(n,k) ≤ 10000 (small/medium scale; large falls back to greedy)\n";
    std::cout << "  Range: 45≤m≤54, 7≤n≤25, 4≤k≤7, s≤j≤k, s≥3\n\n";

    if (!readInt(m, "  m (total samples, 45~54): "))    return;
    if (!readInt(n, "  n (selected samples, 7~25): "))    return;
    if (!readInt(k, "  k (group size, 4~7): "))       return;
    if (!readInt(j, "  j (cover subset size): "))         return;
    if (!readInt(s, "  s (min intersection size): "))          return;
    if (!readInt(minCover, "  minCover (min s-subsets per j-subset): ")) return;

    int timeLimitSec = 60;
    std::cout << "  Solver time limit in seconds (default 60, press Enter to use default): ";
    {
        std::string line;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::getline(std::cin, line);
        if (!line.empty()) {
            try { timeLimitSec = std::stoi(line); } catch (...) {}
        }
    }

    try {
        SampleSelectSystem sys(m, n, k, j, s, minCover);
        sys.printParams();

        if (!currentPoolFile.empty()) {
            std::cout << "  [Pool] Loading: " << currentPoolFile << "\n";
            if (!sys.loadSamplePool(currentPoolFile))
                std::cout << "  [Info] Pool load failed, falling back to default 1~m range.\n";
        }

        // Sample selection
        std::cout << "\n  Sample selection:\n";
        std::cout << "    1. Random\n";
        std::cout << "    2. Manual input\n";
        std::cout << "  Select: ";
        int mode;
        if (!(std::cin >> mode)) { clearCin(); return; }

        if (mode == 2) {
            std::cout << "  Enter " << n << " samples (1~" << m
                      << ", space separated):\n  ";
            std::vector<int> userInput(n);
            bool ok = true;
            for (int i = 0; i < n; ++i) {
                if (!(std::cin >> userInput[i])) { clearCin(); ok = false; break; }
            }
            if (!ok || !sys.inputSamples(userInput)) return;
        } else {
            sys.randomSamples();
        }

        // ── Greedy solution ──────────────────────────────────────────────────
        std::cout << "\n── Greedy Result ───────────────────────────────────────\n";
        auto greedyGroups = sys.generateOptimalGroups();
        sys.printGroups(greedyGroups);

        // ── ILP exact solution ───────────────────────────────────────────────
        std::cout << "\n── ILP Exact Result ────────────────────────────────────\n";
        auto ilpGroups = sys.generateOptimalGroupsILP("highs", "./ilp_tmp", timeLimitSec);
        sys.printGroups(ilpGroups);

        // ── Comparison ──────────────────────────────────────────────────────
        std::cout << "\n── Comparison ──────────────────────────────────────────\n";
        std::cout << "  Greedy groups: " << greedyGroups.size() << "\n";
        std::cout << "  ILP groups:    " << ilpGroups.size() << "\n";
        if (ilpGroups.size() < greedyGroups.size()) {
            std::cout << "  => ILP uses "
                      << (greedyGroups.size() - ilpGroups.size())
                      << " fewer group(s) than greedy (optimal solution is better)\n";
        } else if (ilpGroups.size() == greedyGroups.size()) {
            std::cout << "  => ILP and greedy have the same group count (greedy reached optimal)\n";
        } else {
            std::cout << "  => ILP uses more groups (likely timed out, returning feasible solution)\n";
        }

        // Ask whether to save ILP result
        std::cout << "\n  Save ILP result to DB file? (1=yes/0=no): ";
        int save;
        if (!(std::cin >> save)) { clearCin(); return; }
        if (save == 1) {
            int runCount;
            if (!readInt(runCount, "  Run index: ")) return;
            sys.saveToFile(runCount, ilpGroups, outputDir);
        }

    } catch (const std::exception& e) {
        std::cerr << "[Error] " << e.what() << "\n";
    }
}

// Menu option 8: view current pool status
static void menuViewPool(const std::string& currentPoolFile,
                         const std::string& outputDir)
{
    std::cout << "\n── Current Pool Status ───────────────────────────────────\n";
    if (currentPoolFile.empty()) {
        std::cout << "  No pool loaded, using default 1~m range.\n";
        std::cout << "  [Info] Use option 7 to import a pool file, or option 1 to enter samples manually.\n";
    } else {
        std::cout << "  Current pool file: " << currentPoolFile << "\n";
        // Read and display file contents directly (no m validation via sys)
        try {
            std::ifstream ifs(currentPoolFile);
            if (!ifs.is_open()) {
                std::cerr << "  [Error] Cannot read pool file: " << currentPoolFile << "\n";
            } else {
                std::vector<int> nums;
                std::string line;
                while (std::getline(ifs, line)) {
                    if (!line.empty() && line[0] == '#') {
                        std::cout << "  " << line << "\n";  // print comment header
                        continue;
                    }
                    // Parse comma-separated integers
                    std::istringstream iss(line);
                    std::string tok;
                    while (std::getline(iss, tok, ',')) {
                        tok.erase(0, tok.find_first_not_of(" \t\r\n"));
                        tok.erase(tok.find_last_not_of(" \t\r\n") + 1);
                        if (!tok.empty()) {
                            try { nums.push_back(std::stoi(tok)); } catch (...) {}
                        }
                    }
                }
                ifs.close();
                std::cout << "  Numbers (" << nums.size() << " total):\n  ";
                for (int i = 0; i < (int)nums.size(); ++i) {
                    std::cout << std::setw(3) << nums[i];
                    if ((i + 1) % 15 == 0 && i + 1 < (int)nums.size())
                        std::cout << ",\n  ";
                    else if (i + 1 < (int)nums.size())
                        std::cout << ",";
                }
                std::cout << "\n";
            }
        } catch (...) {
            std::cerr << "  [Error] Failed to read pool file.\n";
        }
    }
    std::cout << "  Available pool files (" << outputDir << "/pool_*.txt):\n";
    // List all pool_*.txt files in the directory
    try {
        namespace fs = std::filesystem;
        if (fs::exists(outputDir) && fs::is_directory(outputDir)) {
            bool found = false;
            for (const auto& entry : fs::directory_iterator(outputDir)) {
                std::string name = entry.path().filename().string();
                if (name.substr(0, 5) == "pool_" &&
                    name.size() > 4 &&
                    name.substr(name.size() - 4) == ".txt") {
                    std::cout << "    " << entry.path().string() << "\n";
                    found = true;
                }
            }
            if (!found) std::cout << "    (none)\n";
        }
    } catch (...) {}
}

// Menu option 2: list DB files
static void menuListFiles(const std::string& outputDir)
{
    // Use a temporary system object to call listDBFiles (params are arbitrary, only using utility method)
    try {
        SampleSelectSystem tmp(45, 7, 6, 5, 5);
        auto files = tmp.listDBFiles(outputDir);
        if (files.empty()) {
            std::cout << "  [Info] No DB files found in " << outputDir << ".\n";
        } else {
            std::cout << "  DB files (" << files.size() << " total):\n";
            for (int i = 0; i < (int)files.size(); ++i) {
                std::cout << "  " << (i + 1) << ". " << files[i] << "\n";
            }
        }
    } catch (...) {}
}

// Menu option 3: read and display a DB file
static void menuReadFile(const std::string& outputDir)
{
    std::cout << "  Enter DB filename (or full path): ";
    std::string filename;
    std::cin >> filename;

    // If only a filename, prepend directory
    if (filename.find('/') == std::string::npos &&
        filename.find('\\') == std::string::npos) {
        filename = outputDir + "/" + filename;
    }

    try {
        SampleSelectSystem tmp(45, 7, 6, 5, 5);
        auto groups = tmp.loadFromFile(filename);
        if (!groups.empty()) {
            std::cout << "  File contents (" << groups.size() << " k-groups):\n";
            for (int i = 0; i < (int)groups.size(); ++i) {
                std::cout << "  " << (i + 1) << ". [ ";
                for (int v : groups[i]) std::cout << v << " ";
                std::cout << "]\n";
            }
        }
    } catch (...) {}
}

// Menu option 4: delete an entire file
static void menuDeleteFile(const std::string& outputDir)
{
    std::cout << "  Enter DB filename to delete (or full path): ";
    std::string filename;
    std::cin >> filename;

    if (filename.find('/') == std::string::npos &&
        filename.find('\\') == std::string::npos) {
        filename = outputDir + "/" + filename;
    }

    try {
        SampleSelectSystem tmp(45, 7, 6, 5, 5);
        tmp.deleteFile(filename);
    } catch (...) {}
}

// Menu option 5: delete a specific k-group
static void menuDeleteGroup(const std::string& outputDir)
{
    std::cout << "  Enter DB filename (or full path): ";
    std::string filename;
    std::cin >> filename;

    if (filename.find('/') == std::string::npos &&
        filename.find('\\') == std::string::npos) {
        filename = outputDir + "/" + filename;
    }

    int idx;
    if (!readInt(idx, "  k-group index to delete (1-based): ")) return;

    try {
        SampleSelectSystem tmp(45, 7, 6, 5, 5);
        tmp.deleteGroup(filename, idx - 1); // convert to 0-based
    } catch (...) {}
}

// ════════════════════════════════════════════════════════════════════════════════
// CLI non-interactive mode helpers
// ════════════════════════════════════════════════════════════════════════════════

static std::string jsonEscape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

/** Parse --key=value argument; returns value string or empty string if not found */
static std::string getArg(int argc, char* argv[], const std::string& key)
{
    std::string prefix = key + "=";
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a.size() > prefix.size() && a.substr(0, prefix.size()) == prefix)
            return a.substr(prefix.size());
    }
    return "";
}

/** Check whether a flag exists (value-less argument, e.g. --cli, --save) */
static bool hasFlag(int argc, char* argv[], const std::string& flag)
{
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == flag) return true;
    return false;
}

/** Parse a comma-separated string into vector<int> */
static std::vector<int> parseCSV(const std::string& s)
{
    std::vector<int> result;
    std::istringstream iss(s);
    std::string tok;
    while (std::getline(iss, tok, ',')) {
        tok.erase(0, tok.find_first_not_of(" \t"));
        tok.erase(tok.find_last_not_of(" \t") + 1);
        if (!tok.empty()) {
            try { result.push_back(std::stoi(tok)); } catch (...) {}
        }
    }
    return result;
}

/**
 * @brief CLI non-interactive mode entry point
 *
 * Usage:
 *   ./optimal_sample --cli --m=45 --n=8 --k=6 --j=6 --s=5 --minCover=1
 *                    [--samples=1,2,3,4,5,6,7,8] [--save] [--run=1]
 *                    [--dbdir=./db] [--poolFile=./db/pool_test_m45.txt]
 *
 * Success output (stdout JSON):
 *   {"success":true,"count":4,"dbFile":"./db/45-8-6-6-5-1-4.txt","groups":[[1,2,3,4,5,6],...]}
 *
 * Failure output:
 *   {"success":false,"error":"<reason>"}
 */
static void runCliMode(int argc, char* argv[])
{
    auto outputError = [](const std::string& msg) {
        std::cout << "{\"success\":false,\"error\":\""
                  << jsonEscape(msg) << "\"}" << std::flush;
    };

    // ── Parse arguments ───────────────────────────────────────────────────────
    auto mStr        = getArg(argc, argv, "--m");
    auto nStr        = getArg(argc, argv, "--n");
    auto kStr        = getArg(argc, argv, "--k");
    auto jStr        = getArg(argc, argv, "--j");
    auto sStr        = getArg(argc, argv, "--s");
    auto minCoverStr = getArg(argc, argv, "--minCover");
    auto samplesStr  = getArg(argc, argv, "--samples");
    auto runStr      = getArg(argc, argv, "--run");
    auto dbdir       = getArg(argc, argv, "--dbdir");
    auto poolFile    = getArg(argc, argv, "--poolFile");
    bool doSave      = hasFlag(argc, argv, "--save");

    if (mStr.empty() || nStr.empty() || kStr.empty() ||
        jStr.empty() || sStr.empty()) {
        outputError("Missing required parameters: --m --n --k --j --s");
        return;
    }

    int m, n, k, j, s, minCover = 1, runCount = 1;
    try {
        m        = std::stoi(mStr);
        n        = std::stoi(nStr);
        k        = std::stoi(kStr);
        j        = std::stoi(jStr);
        s        = std::stoi(sStr);
        if (!minCoverStr.empty()) minCover = std::stoi(minCoverStr);
        if (!runStr.empty())      runCount = std::stoi(runStr);
    } catch (...) {
        outputError("Parameter parse error: values must be integers");
        return;
    }

    if (dbdir.empty()) dbdir = "./db";

    // ── Construct system object and run ──────────────────────────────────────
    try {
        // Ensure output directory exists
        namespace fs = std::filesystem;
        fs::create_directories(dbdir);

        SampleSelectSystem sys(m, n, k, j, s, minCover);

        // Load pool (optional)
        if (!poolFile.empty()) {
            sys.loadSamplePool(poolFile);
        }

        // Set samples
        if (!samplesStr.empty()) {
            auto samples = parseCSV(samplesStr);
            if ((int)samples.size() != n) {
                outputError("samples count mismatch: expected " + std::to_string(n) +
                            " but got " + std::to_string(samples.size()));
                return;
            }
            if (!sys.inputSamples(samples)) {
                outputError("Invalid samples: out of range or duplicates");
                return;
            }
        } else {
            sys.randomSamples();
        }

        // Generate optimal k-groups
        auto groups = sys.generateOptimalGroups();

        // Save (optional)
        std::string savedFile;
        if (doSave) {
            savedFile = sys.saveToFile(runCount, groups, dbdir);
        }

        // ── Output JSON ───────────────────────────────────────────────────────
        std::cout << "{\"success\":true,\"count\":" << groups.size()
                  << ",\"dbFile\":\"" << jsonEscape(savedFile) << "\""
                  << ",\"samples\":[";
        const auto& samp = sys.getSamples();
        for (int i = 0; i < (int)samp.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << samp[i];
        }
        std::cout << "],\"groups\":[";
        for (int i = 0; i < (int)groups.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << "[";
            for (int jj = 0; jj < (int)groups[i].size(); ++jj) {
                if (jj) std::cout << ",";
                std::cout << groups[i][jj];
            }
            std::cout << "]";
        }
        std::cout << "]}" << std::flush;

    } catch (const std::exception& e) {
        outputError(std::string(e.what()));
    } catch (...) {
        outputError("Unknown error");
    }
}

// ════════════════════════════════════════════════════════════════════════════════
// Main function
// ════════════════════════════════════════════════════════════════════════════════

int main(int argc, char* argv[])
{
    // ── CLI non-interactive mode (invoked by Tauri sidecar) ───────────────────
    if (hasFlag(argc, argv, "--cli")) {
        runCliMode(argc, argv);
        return 0;
    }

    const std::string OUTPUT_DIR = "./db";  // unified DB file output directory

    // ── Generate test pool file on startup ────────────────────────────────────
    // Creates a pool file containing 1~45 for direct import by users
    {
        std::vector<int> testPool(45);
        std::iota(testPool.begin(), testPool.end(), 1);  // fill with 1,2,...,45
        SampleSelectSystem::saveSamplePool(OUTPUT_DIR + "/pool_test_m45.txt", testPool);
    }

    // ── Built-in test cases ───────────────────────────────────────────────────
    // Example 1: m=45, n=7, k=6, j=5, s=5, minCover=1 → expected 6 groups
    runExample(45, 7, 6, 5, 5, 1,
               {1, 2, 3, 4, 5, 6, 7},
               "Example 1: m=45, n=7, k=6, j=5, s=5, minCover=1 (expected 6 groups)",
               OUTPUT_DIR);

    // Example 5: m=45, n=8, k=6, j=6, s=5, minCover=1 → expected 4 groups
    runExample(45, 8, 6, 6, 5, 1,
               {1, 2, 3, 4, 5, 6, 7, 8},
               "Example 5: m=45, n=8, k=6, j=6, s=5, minCover=1 (expected 4 groups)",
               OUTPUT_DIR);

    // Extra: Example 2 (n=8, j=5, s=5, expected 12 groups)
    runExample(45, 8, 6, 5, 5, 1,
               {1, 2, 3, 4, 5, 6, 7, 8},
               "Example 2: m=45, n=8, k=6, j=5, s=5, minCover=1 (expected 12 groups)",
               OUTPUT_DIR);

    // Extra: Example 6 (n=8, j=6, s=5, minCover=4, expected 10 groups)
    runExample(45, 8, 6, 6, 5, 4,
               {1, 2, 3, 4, 5, 6, 7, 8},
               "Example 6: m=45, n=8, k=6, j=6, s=5, minCover=4 (expected 10 groups)",
               OUTPUT_DIR);

    // ── Interactive menu ──────────────────────────────────────────────────────
    std::cout << "\n\n";
    std::cout << "══════════════════════════════════════════════════════════\n";
    std::cout << "  Automated tests complete. Entering interactive mode...\n";
    std::cout << "  Test pool file generated: " << OUTPUT_DIR << "/pool_test_m45.txt\n";
    std::cout << "══════════════════════════════════════════════════════════\n";

    std::string currentPoolFile;  // currently loaded pool file path (empty = none)

    int choice = -1;
    while (choice != 0) {
        printMainMenu(currentPoolFile);
        if (!(std::cin >> choice)) {
            clearCin();
            continue;
        }

        switch (choice) {
            case 1:
                menuGenerate(OUTPUT_DIR, currentPoolFile);
                break;
            case 2:
                menuListFiles(OUTPUT_DIR);
                break;
            case 3:
                menuReadFile(OUTPUT_DIR);
                break;
            case 4:
                menuDeleteFile(OUTPUT_DIR);
                break;
            case 5:
                menuDeleteGroup(OUTPUT_DIR);
                break;
            case 6:
                // Run built-in tests again (2nd run, runCount=2)
                runExample(45, 7, 6, 5, 5, 1,
                           {1, 2, 3, 4, 5, 6, 7},
                           "Example 1 (2nd run)",
                           OUTPUT_DIR);
                runExample(45, 8, 6, 6, 5, 1,
                           {1, 2, 3, 4, 5, 6, 7, 8},
                           "Example 5 (2nd run)",
                           OUTPUT_DIR);
                break;
            case 7: {
                // Import pool file
                std::string loaded = menuLoadPool(OUTPUT_DIR);
                if (!loaded.empty()) {
                    currentPoolFile = loaded;
                }
                break;
            }
            case 8:
                // View current pool status
                menuViewPool(currentPoolFile, OUTPUT_DIR);
                break;
            case 9:
                // ILP exact solver
                menuILP(OUTPUT_DIR, currentPoolFile);
                break;
            case 0:
                std::cout << ">> Exiting. Goodbye!\n";
                break;
            default:
                std::cout << "  [Info] Invalid choice, please try again.\n";
                break;
        }
    }

    return 0;
}
