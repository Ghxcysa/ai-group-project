/**
 * ILPSolver.h
 * Integer Linear Programming solver interface
 *
 * Models the set cover problem as a 0-1 ILP and finds the optimal solution
 * via an external HiGHS solver.
 */

#pragma once

#include <string>
#include <vector>

class ILPSolver {
public:
    // ── Problem description ────────────────────────────────────────────────────
    struct Problem {
        int numK;       ///< Number of k-candidate groups (x variables)
        int numSubs;    ///< Number of global s-subsets (z variables)
        int numJ;       ///< Number of j-subsets
        int minCover;   ///< Lower bound on s-subsets that must be covered per j-subset

        /// sCoversList[si] = list of k-candidate indices that cover s-subset si
        std::vector<std::vector<int>> sCoversList;
        /// jToSubs[i] = list of global s-subset indices within j-subset i
        std::vector<std::vector<int>> jToSubs;

        /// Greedy upper bound (0 = do not pass an upper-bound constraint)
        int greedyUpperBound = 0;
    };

    // ── Solution ───────────────────────────────────────────────────────────────
    struct Solution {
        bool optimal  = false;  ///< true = globally optimal solution found
        bool feasible = false;  ///< true = at least one feasible solution found (may be non-optimal on timeout)
        int  optSize  = 0;      ///< Number of selected k-candidate groups
        std::vector<int> selectedIndices; ///< Indices of selected k-candidates (0-based)
        std::string statusMsg;            ///< HiGHS status description
    };

    /**
     * @brief Write the problem to an LP format file
     * @param filePath  Output file path
     * @param prob      Problem description
     * @return true if write succeeded
     */
    static bool exportLP(const std::string& filePath, const Problem& prob);

    /**
     * @brief Invoke HiGHS subprocess to solve the ILP
     * @param prob          Problem description
     * @param workDir       Directory for LP/SOL temporary files (default ".")
     * @param solverBin     Path to the highs executable (default "highs")
     * @param timeLimitSec  Solver time limit in seconds (default 60)
     * @return Solution result
     */
    static Solution solve(const Problem& prob,
                          const std::string& workDir      = ".",
                          const std::string& solverBin    = "highs",
                          int                timeLimitSec = 60);

    /**
     * @brief Parse a HiGHS .sol output file and extract the selected k-candidate indices
     * @param solPath  Path to the .sol file
     * @param numK     Total number of k-candidates (used to determine variable name range)
     * @param numSubs  Total number of s-subsets (used to determine variable name range)
     * @return Solution result with selectedIndices populated
     */
    static Solution parseSolutionFile(const std::string& solPath,
                                      int numK, int numSubs);
};
