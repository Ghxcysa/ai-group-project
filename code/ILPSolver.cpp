/**
 * ILPSolver.cpp
 * Integer Linear Programming solver implementation
 *
 * ILP formulation:
 *   Variables: x_c ∈ {0,1} (whether k-candidate c is selected),
 *              z_si ∈ {0,1} (whether s-subset si is covered)
 *   Coverage constraints: z_si - Σ_{c ∈ sCoversList[si]} x_c ≤ 0
 *   Satisfaction constraints: Σ_{si ∈ jToSubs[i]} z_si ≥ minCover
 *   Objective: minimize Σ_c x_c
 */

#include "ILPSolver.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

// ════════════════════════════════════════════════════════════════════════════════
// exportLP: write ILP problem to an LP format file
// ════════════════════════════════════════════════════════════════════════════════

bool ILPSolver::exportLP(const std::string& filePath, const Problem& prob)
{
    std::ofstream ofs(filePath);
    if (!ofs.is_open()) {
        std::cerr << "[ILPSolver] Cannot create LP file: " << filePath << "\n";
        return false;
    }

    // ── Objective: minimize Σ x_c ─────────────────────────────────────────────
    ofs << "Minimize\n obj:";
    for (int c = 0; c < prob.numK; ++c) {
        if (c % 10 == 0) ofs << "\n  ";
        if (c > 0) ofs << " +";
        ofs << " x" << c;
    }
    ofs << "\n\n";

    // ── Constraints ───────────────────────────────────────────────────────────
    ofs << "Subject To\n";

    // Coverage constraints: z_si - Σ_{c ∈ sCoversList[si]} x_c ≤ 0
    for (int si = 0; si < prob.numSubs; ++si) {
        ofs << " cov" << si << ": z" << si;
        for (int c : prob.sCoversList[si]) {
            ofs << " - x" << c;
        }
        ofs << " <= 0\n";
    }

    // Satisfaction constraints: Σ_{si ∈ jToSubs[i]} z_si ≥ minCover
    for (int i = 0; i < prob.numJ; ++i) {
        ofs << " sat" << i << ":";
        bool first = true;
        for (int si : prob.jToSubs[i]) {
            if (!first) ofs << " +";
            ofs << " z" << si;
            first = false;
        }
        if (first) {
            // j-subset has no s-subsets (should not happen in practice, but defensive)
            ofs << " 0";
        }
        ofs << " >= " << prob.minCover << "\n";
    }
    ofs << "\n";

    // ── Variable bounds ────────────────────────────────────────────────────────
    ofs << "Bounds\n";
    for (int c = 0; c < prob.numK; ++c)
        ofs << " 0 <= x" << c << " <= 1\n";
    for (int si = 0; si < prob.numSubs; ++si)
        ofs << " 0 <= z" << si << " <= 1\n";
    ofs << "\n";

    // ── Declare all variables as binary ───────────────────────────────────────
    ofs << "Binary\n";
    for (int c = 0; c < prob.numK; ++c) {
        if (c > 0 && c % 10 == 0) ofs << "\n";
        ofs << " x" << c;
    }
    ofs << "\n";
    for (int si = 0; si < prob.numSubs; ++si) {
        if (si > 0 && si % 10 == 0) ofs << "\n";
        ofs << " z" << si;
    }
    ofs << "\n\nEnd\n";

    ofs.close();
    return true;
}

// ════════════════════════════════════════════════════════════════════════════════
// parseSolutionFile: parse the .sol file produced by HiGHS
// ════════════════════════════════════════════════════════════════════════════════

ILPSolver::Solution ILPSolver::parseSolutionFile(const std::string& solPath,
                                                  int numK, int /*numSubs*/)
{
    Solution sol;
    std::ifstream ifs(solPath);
    if (!ifs.is_open()) {
        sol.statusMsg = "Cannot open solution file: " + solPath;
        return sol;
    }

    std::string line;
    bool inColumns = false;
    bool foundOptimal = false;
    bool foundFeasible = false;

    while (std::getline(ifs, line)) {
        // Check solver status line
        if (line.find("Model   status") != std::string::npos ||
            line.find("Model status") != std::string::npos) {
            if (line.find("Optimal") != std::string::npos) {
                foundOptimal  = true;
                foundFeasible = true;
            } else if (line.find("Feasible") != std::string::npos ||
                       line.find("feasible") != std::string::npos) {
                foundFeasible = true;
            }
        }

        // Enter/exit the Columns section
        if (line == "Columns") { inColumns = true; continue; }
        if (inColumns && !line.empty() && line[0] != ' ' && line[0] != '\t') {
            inColumns = false;
        }

        if (!inColumns) continue;

        // Parse lines of the form "x{N}  {value}  ..."
        // HiGHS sol format: variable name followed by whitespace and value (0 or 1)
        std::istringstream iss(line);
        std::string varName;
        double value = 0.0;
        if (!(iss >> varName >> value)) continue;

        if (varName.size() < 2 || varName[0] != 'x') continue;

        // Parse subscript
        bool allDigit = true;
        for (size_t i = 1; i < varName.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(varName[i])))
                { allDigit = false; break; }
        if (!allDigit) continue;

        int idx = std::stoi(varName.substr(1));
        if (idx < 0 || idx >= numK) continue;

        if (value > 0.5) {  // selected
            sol.selectedIndices.push_back(idx);
        }
    }
    ifs.close();

    sol.feasible = foundFeasible || !sol.selectedIndices.empty();
    sol.optimal  = foundOptimal;
    sol.optSize  = (int)sol.selectedIndices.size();

    if (!sol.statusMsg.empty()) {
        // Error message already set; keep it
    } else if (sol.optimal) {
        sol.statusMsg = "Optimal";
    } else if (sol.feasible) {
        sol.statusMsg = "Feasible (time limit reached)";
    } else {
        sol.statusMsg = "No feasible solution found";
    }

    return sol;
}

// ════════════════════════════════════════════════════════════════════════════════
// solve: invoke HiGHS subprocess to solve the ILP
// ════════════════════════════════════════════════════════════════════════════════

ILPSolver::Solution ILPSolver::solve(const Problem& prob,
                                     const std::string& workDir,
                                     const std::string& solverBin,
                                     int timeLimitSec)
{
    Solution sol;

    // ── 1. Ensure working directory exists ────────────────────────────────────
    {
        std::string mkdirCmd = "mkdir -p \"" + workDir + "\"";
        std::system(mkdirCmd.c_str());
    }

    // ── 2. Generate timestamped temporary filenames ───────────────────────────
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::string lpFile  = workDir + "/cover_" + std::to_string(now) + ".lp";
    std::string solFile = workDir + "/cover_" + std::to_string(now) + ".sol";

    // ── 3. Write LP file ──────────────────────────────────────────────────────
    if (!exportLP(lpFile, prob)) {
        sol.statusMsg = "Failed to write LP file";
        return sol;
    }
    std::cout << ">> [ILP] LP file generated: " << lpFile << "\n";

    // ── 4. Build HiGHS invocation command ────────────────────────────────────
    // HiGHS CLI options:
    //   --solution_file <path>  specify solution output file
    //   --time_limit <sec>      solver time limit
    std::string cmd = "\"" + solverBin + "\""
                    + " \"" + lpFile + "\""
                    + " --solution_file \"" + solFile + "\""
                    + " --time_limit " + std::to_string(timeLimitSec)
                    + " > /dev/null 2>&1";

    std::cout << ">> [ILP] Calling HiGHS (time limit " << timeLimitSec << "s)...\n";

    int ret = std::system(cmd.c_str());

    // ── 5. Parse solution file ────────────────────────────────────────────────
    {
        std::ifstream probe(solFile);
        if (!probe.is_open()) {
            // HiGHS may not be installed, or LP is infeasible
            if (ret != 0) {
                sol.statusMsg = "HiGHS invocation failed (exit code " + std::to_string(ret) +
                                "). Please ensure highs is installed (brew install highs)";
                std::cerr << ">> [ILP] " << sol.statusMsg << "\n";
            } else {
                sol.statusMsg = "Solution file not generated";
                std::cerr << ">> [ILP] " << sol.statusMsg << "\n";
            }
        } else {
            probe.close();
            sol = parseSolutionFile(solFile, prob.numK, prob.numSubs);
        }
    }

    // ── 6. Clean up temporary files ───────────────────────────────────────────
    std::remove(lpFile.c_str());
    std::remove(solFile.c_str());

    return sol;
}
