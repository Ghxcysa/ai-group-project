/**
 * SampleSelectSystem.h
 * Optimal Sample Selection System — core class declaration
 *
 * Overview:
 *   Select n samples from m candidates, then generate the minimum number of
 *   k-sample groups satisfying combinatorial coverage constraints
 *   (greedy solution to the Set Cover problem).
 *
 * Parameter ranges:
 *   45 ≤ m ≤ 54,  7 ≤ n ≤ 25,  4 ≤ k ≤ 7,  s ≤ j ≤ k,  3 ≤ s
 */

#pragma once

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <string>
#include <algorithm>
#include <random>
#include <chrono>
#include <stdexcept>
#include <filesystem>

// ── Parameter range constants ──────────────────────────────────────────────────
constexpr int M_MIN = 45, M_MAX = 54;
constexpr int N_MIN = 7,  N_MAX = 25;
constexpr int K_MIN = 4,  K_MAX = 7;
constexpr int S_MIN = 3;
constexpr int K_DEFAULT = 6;

// ── Coverage mode ──────────────────────────────────────────────────────────────
enum class CoverMode {
    FULL,     // j = s: j-subset must be wholly contained in some k-group (full coverage)
    PARTIAL   // j > s: at least minCover s-subsets of each j-subset are covered (partial coverage)
};

// ════════════════════════════════════════════════════════════════════════════════
//  SampleSelectSystem — core class for optimal sample selection
// ════════════════════════════════════════════════════════════════════════════════
class SampleSelectSystem {
public:
    // ── Constructor / Destructor ───────────────────────────────────────────────
    /**
     * @brief Constructor — sets all system parameters
     * @param m        Total number of samples (45~54)
     * @param n        Number of samples to select from m (7~25)
     * @param k        Group size (4~7, default 6)
     * @param j        Cover subset size (s ≤ j ≤ k)
     * @param s        Minimum intersection size (3 ≤ s ≤ j)
     * @param minCover Number of s-subsets each j-subset must be covered by (default 1)
     */
    SampleSelectSystem(int m, int n, int k, int j, int s, int minCover = 1);

    // ── Parameter validation ───────────────────────────────────────────────────
    /** Validate all constructor parameters; throws std::invalid_argument if invalid */
    void validateParams() const;

    // ── Number pool management ─────────────────────────────────────────────────
    /**
     * @brief Load m numbers from a pool file to use as the sample pool
     *        (replaces the default 1~m range).
     *        Format: lines starting with # are comments; data lines are comma-separated integers.
     * @param filename Pool file path (e.g. pool_test_m45.txt)
     * @return true if loaded successfully, false if file is missing or data is invalid
     */
    bool loadSamplePool(const std::string& filename);

    /**
     * @brief Write a set of numbers to a pool file (used to generate test files)
     * @param filename  Output file path
     * @param pool      List of numbers (caller guarantees validity)
     * @return true if write succeeded
     */
    static bool saveSamplePool(const std::string& filename,
                               const std::vector<int>& pool);

    /** Print the currently loaded sample pool; shows default 1~m if not loaded */
    void printSamplePool() const;

    /** Return the current sample pool (empty = use default 1~m range) */
    const std::vector<int>& getSamplePool() const { return samplePool_; }

    // ── Sample selection ───────────────────────────────────────────────────────
    /**
     * @brief Randomly select n distinct samples from the pool (or 1~m if no pool loaded);
     *        stores result in samples_
     */
    void randomSamples();

    /**
     * @brief Accept n manually provided samples, validating range and uniqueness
     * @param userInput User-supplied sample list
     * @return true if valid, false if invalid (error message is printed)
     */
    bool inputSamples(const std::vector<int>& userInput);

    /** Print the currently selected n samples */
    void printSamples() const;

    // ── Core algorithm ─────────────────────────────────────────────────────────
    /**
     * @brief Generate the minimum set of k-sample groups satisfying coverage constraints
     *        (greedy set cover)
     *
     * Coverage semantics:
     *   For every j-subset J of the n selected samples, at least minCover
     *   s-subsets of J must be wholly contained in some selected k-group.
     *
     * @return List of generated k-groups (each group is sorted)
     */
    std::vector<std::vector<int>> generateOptimalGroups();

    /**
     * @brief ILP exact solver: guaranteed minimum k-groups (globally optimal)
     *        Runs greedy first to obtain an upper bound, then solves the
     *        0-1 integer linear program via HiGHS.
     * @param solverBin    Path to HiGHS executable (default "highs")
     * @param workDir      Temporary directory for LP/SOL files (default "./ilp_tmp")
     * @param timeLimitSec Solver time limit in seconds (falls back to greedy on timeout)
     * @return Optimal k-group list (each group is sorted)
     */
    std::vector<std::vector<int>> generateOptimalGroupsILP(
        const std::string& solverBin    = "highs",
        const std::string& workDir      = "./ilp_tmp",
        int                timeLimitSec = 60);

    // ── Result operations ──────────────────────────────────────────────────────
    /** Print k-group list to the console */
    void printGroups(const std::vector<std::vector<int>>& groups) const;

    /**
     * @brief Save k-group results to a DB file (text format, one group per line, comma-separated)
     * @param runCount    Run index used for file naming
     * @param groups      k-group list to save
     * @param outputDir   Output directory (default: current directory)
     * @return Generated filename, or empty string on failure
     */
    std::string saveToFile(int runCount,
                           const std::vector<std::vector<int>>& groups,
                           const std::string& outputDir = ".");

    /**
     * @brief Load k-group list from a DB file
     * @param filename  DB file path
     * @return k-group list; returns empty list if file is missing or malformed
     */
    std::vector<std::vector<int>> loadFromFile(const std::string& filename) const;

    /**
     * @brief Delete an entire DB file
     * @param filename  DB file path
     * @return true if deletion succeeded
     */
    bool deleteFile(const std::string& filename) const;

    /**
     * @brief Delete a k-group at the specified index (0-based) from a DB file
     * @param filename   DB file path
     * @param groupIndex Row index to delete (0-based)
     * @return true if operation succeeded
     */
    bool deleteGroup(const std::string& filename, int groupIndex) const;

    /**
     * @brief List all DB files in the specified directory matching the naming convention
     * @param dir  Directory path (default: current directory)
     * @return List of DB file paths
     */
    std::vector<std::string> listDBFiles(const std::string& dir = ".") const;

    // ── Static utility methods ─────────────────────────────────────────────────
    /**
     * @brief Compute binomial coefficient C(a, b) = a! / (b! * (a-b)!)
     *        Uses Pascal's triangle precomputation; supports a ≤ 60, overflow-safe.
     */
    static long long C(int a, int b);

    /**
     * @brief Enumerate all subsets of size r from pool
     * @param pool  Element pool (sorted)
     * @param r     Subset size
     * @return All r-subsets (each subset is sorted)
     */
    static std::vector<std::vector<int>> enumerate(
        const std::vector<int>& pool, int r);

    /**
     * @brief Compute the intersection size of two sorted vectors
     */
    static int intersectSize(const std::vector<int>& a,
                             const std::vector<int>& b);

    // ── Getters ────────────────────────────────────────────────────────────────
    int getM() const { return m_; }
    int getN() const { return n_; }
    int getK() const { return k_; }
    int getJ() const { return j_; }
    int getS() const { return s_; }
    int getMinCover() const { return minCover_; }
    const std::vector<int>& getSamples() const { return samples_; }

    /** Print current system parameters */
    void printParams() const;

private:
    // ── Private member variables ───────────────────────────────────────────────
    int m_;         ///< Total number of samples
    int n_;         ///< Number of selected samples
    int k_;         ///< Group size
    int j_;         ///< Cover subset size
    int s_;         ///< Minimum intersection size
    int minCover_;  ///< Required s-subset coverage count per j-subset

    std::vector<int> samples_;     ///< Currently selected n samples
    std::vector<int> samplePool_;  ///< Imported number pool; empty = use default 1~m range

    // ── Private utility methods ────────────────────────────────────────────────
    /** Recursive enumeration helper */
    static void enumerateHelper(const std::vector<int>& pool,
                                int r, int start,
                                std::vector<int>& current,
                                std::vector<std::vector<int>>& result);

    /** Build DB filename from current parameters */
    std::string buildFileName(int runCount, int resultCount) const;

    /** Parse a comma-separated line of numbers into vector<int> */
    static std::vector<int> parseLine(const std::string& line);
};
