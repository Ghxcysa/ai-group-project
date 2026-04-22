/**
 * SampleSelectSystem.cpp
 * Optimal Sample Selection System — full core class implementation
 */

#include "SampleSelectSystem.h"
#include "ILPSolver.h"

#include <algorithm>
#include <chrono>
#include <cmath>        // std::exp, std::pow
#include <cstdio>       // std::remove
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <queue>        // std::priority_queue (lazy greedy)
#include <unordered_map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ════════════════════════════════════════════════════════════════════════════════
// Constructor & parameter validation
// ════════════════════════════════════════════════════════════════════════════════

SampleSelectSystem::SampleSelectSystem(int m, int n, int k, int j, int s,
                                       int minCover)
    : m_(m), n_(n), k_(k), j_(j), s_(s), minCover_(minCover)
{
    validateParams();
}

void SampleSelectSystem::validateParams() const
{
    // m range
    if (m_ < M_MIN || m_ > M_MAX) {
        throw std::invalid_argument("Parameter m out of range [" +
            std::to_string(M_MIN) + ", " + std::to_string(M_MAX) + "], got: " +
            std::to_string(m_));
    }
    // n range
    if (n_ < N_MIN || n_ > N_MAX) {
        throw std::invalid_argument("Parameter n out of range [" +
            std::to_string(N_MIN) + ", " + std::to_string(N_MAX) + "], got: " +
            std::to_string(n_));
    }
    // n <= m
    if (n_ > m_) {
        throw std::invalid_argument("n cannot exceed m (n=" +
            std::to_string(n_) + ", m=" + std::to_string(m_) + ")");
    }
    // k range
    if (k_ < K_MIN || k_ > K_MAX) {
        throw std::invalid_argument("Parameter k out of range [" +
            std::to_string(K_MIN) + ", " + std::to_string(K_MAX) + "], got: " +
            std::to_string(k_));
    }
    // k <= n
    if (k_ > n_) {
        throw std::invalid_argument("k cannot exceed n (k=" +
            std::to_string(k_) + ", n=" + std::to_string(n_) + ")");
    }
    // j: s <= j <= k
    if (j_ < s_ || j_ > k_) {
        throw std::invalid_argument("Parameter j must satisfy s ≤ j ≤ k (j=" +
            std::to_string(j_) + ", s=" + std::to_string(s_) + ", k=" +
            std::to_string(k_) + ")");
    }
    // s lower bound
    if (s_ < S_MIN) {
        throw std::invalid_argument("Parameter s cannot be less than " +
            std::to_string(S_MIN) + ", got: " + std::to_string(s_));
    }
    // minCover vs C(j,s)
    long long maxCover = C(j_, s_);
    if (minCover_ < 1 || (long long)minCover_ > maxCover) {
        throw std::invalid_argument("minCover must be in [1, C(j,s)=" +
            std::to_string(maxCover) + "], got: " +
            std::to_string(minCover_));
    }
}

// ════════════════════════════════════════════════════════════════════════════════
// Static utility: binomial coefficient C(a,b) via Pascal's triangle
// ════════════════════════════════════════════════════════════════════════════════

long long SampleSelectSystem::C(int a, int b)
{
    // Edge cases
    if (b < 0 || b > a) return 0;
    if (b == 0 || b == a) return 1;
    // Use symmetry to reduce computation
    if (b > a - b) b = a - b;

    // Incremental multiply-divide: C(a,b) = a*(a-1)*...*(a-b+1) / b!
    // Overflow-safe for a ≤ 60 with long long
    long long result = 1;
    for (int i = 0; i < b; ++i) {
        result = result * (a - i) / (i + 1);
    }
    return result;
}

// ════════════════════════════════════════════════════════════════════════════════
// Static utility: enumerate all r-subsets
// ════════════════════════════════════════════════════════════════════════════════

void SampleSelectSystem::enumerateHelper(const std::vector<int>& pool,
                                         int r, int start,
                                         std::vector<int>& current,
                                         std::vector<std::vector<int>>& result)
{
    if ((int)current.size() == r) {
        result.push_back(current);
        return;
    }
    int remaining = r - (int)current.size();
    int poolLeft  = (int)pool.size() - start;
    // Prune: not enough elements left to complete a subset
    if (poolLeft < remaining) return;

    for (int i = start; i < (int)pool.size(); ++i) {
        current.push_back(pool[i]);
        enumerateHelper(pool, r, i + 1, current, result);
        current.pop_back();
    }
}

std::vector<std::vector<int>> SampleSelectSystem::enumerate(
    const std::vector<int>& pool, int r)
{
    std::vector<std::vector<int>> result;
    if (r <= 0 || r > (int)pool.size()) return result;
    result.reserve(static_cast<size_t>(C((int)pool.size(), r)));
    std::vector<int> current;
    current.reserve(r);
    enumerateHelper(pool, r, 0, current, result);
    return result;
}




// ════════════════════════════════════════════════════════════════════════════════
// Static utility: intersection size of two sorted vectors
// ════════════════════════════════════════════════════════════════════════════════

int SampleSelectSystem::intersectSize(const std::vector<int>& a,
                                      const std::vector<int>& b)
{
    int count = 0, i = 0, j = 0;
    while (i < (int)a.size() && j < (int)b.size()) {
        if (a[i] == b[j]) { ++count; ++i; ++j; }
        else if (a[i] < b[j]) ++i;
        else ++j;
    }
    return count;
}

// ════════════════════════════════════════════════════════════════════════════════
// Sample selection
// ════════════════════════════════════════════════════════════════════════════════

void SampleSelectSystem::randomSamples()
{
    // Use loaded pool if available; otherwise fall back to default 1~m range
    std::vector<int> pool;
    if (!samplePool_.empty()) {
        pool = samplePool_;  // copy pool (shuffle modifies order)
        std::cout << ">> Using loaded sample pool (" << pool.size() << " numbers)\n";
    } else {
        pool.resize(m_);
        for (int i = 0; i < m_; ++i) pool[i] = i + 1;
    }

    // Seed with current timestamp to ensure different results each run
    unsigned seed = static_cast<unsigned>(
        std::chrono::system_clock::now().time_since_epoch().count());
    std::mt19937 rng(seed);
    std::shuffle(pool.begin(), pool.end(), rng);

    samples_.assign(pool.begin(), pool.begin() + n_);
    std::sort(samples_.begin(), samples_.end());

    std::cout << ">> Randomly selected " << n_ << " samples: ";
    for (int v : samples_) std::cout << v << " ";
    std::cout << "\n";
}

bool SampleSelectSystem::inputSamples(const std::vector<int>& userInput)
{
    // Validate count
    if ((int)userInput.size() != n_) {
        std::cerr << "[Error] Input sample count " << userInput.size()
                  << " does not match n=" << n_ << ".\n";
        return false;
    }
    // Validate range and uniqueness
    std::set<int> seen;
    for (int v : userInput) {
        if (v < 1 || v > m_) {
            std::cerr << "[Error] Sample value " << v
                      << " out of range [1, " << m_ << "].\n";
            return false;
        }
        if (!seen.insert(v).second) {
            std::cerr << "[Error] Duplicate sample value " << v << ".\n";
            return false;
        }
    }
    samples_ = userInput;
    std::sort(samples_.begin(), samples_.end());
    std::cout << ">> Accepted " << n_ << " user-provided samples: ";
    for (int v : samples_) std::cout << v << " ";
    std::cout << "\n";
    return true;
}

void SampleSelectSystem::printSamples() const
{
    if (samples_.empty()) {
        std::cout << "[Info] No samples selected yet.\n";
        return;
    }
    std::cout << "Current n=" << n_ << " samples: [ ";
    for (int v : samples_) std::cout << v << " ";
    std::cout << "]\n";
}

// ════════════════════════════════════════════════════════════════════════════════
// Core algorithm: generateOptimalGroups() — three-layer optimized set cover
// ════════════════════════════════════════════════════════════════════════════════
/**
 * Three-layer complexity optimizations:
 *
 * Layer 1 — kCoversS precomputation direction fix (25,300x speedup at n=25)
 *   Old: for each k-candidate, scan C(n,s) global s-subsets → O(C(n,k)×C(n,s))
 *   New: for each k-candidate, enumerate its own C(k,s) s-subsets and hash-lookup index
 *        → O(C(n,k)×C(k,s)); at n=25: 85B → 3.4M
 *
 * Layer 2 — inverted index + incremental update + lazy greedy (480,700x speedup at n=25)
 *   sCoversList[si] = list of j-subsets containing s-subset si
 *   After selecting a k-group, only traverse its C(k,s) s-subsets' sCoversList entries
 *   to update jCnt; lazy priority queue exploits monotone score decrease
 *
 * Layer 3 — simulated annealing (enabled when C(n,k) > 10000)
 *   No full enumeration; random k-groups walk through the solution space
 *   Per-step cost O(C(k,s)×avg_sCoversList), independent of C(n,k)
 */
std::vector<std::vector<int>> SampleSelectSystem::generateOptimalGroups()
{
    if (samples_.empty()) {
        throw std::runtime_error("No samples selected. Call randomSamples() or inputSamples() first.");
    }

    // ── 1. Enumerate all j-subsets ───────────────────────────────────────────
    const std::vector<std::vector<int>> jSubsets = enumerate(samples_, j_);
    const int numJ = (int)jSubsets.size();

    std::cout << ">> C(" << n_ << "," << j_ << ") = " << numJ
              << " j-subsets, minCover=" << minCover_
              << "; each j-subset needs " << minCover_
              << " s-subset(s) covered (C(" << j_ << "," << s_ << ")="
              << C(j_, s_) << " total per j-subset)\n";

    // ── 2. Build global s-subset index (unordered_map, O(s) amortized lookup) ─
    std::vector<std::vector<int>> globalSubs;
    std::vector<std::vector<int>> jToSubs(numJ);

    struct VecHash {
        size_t operator()(const std::vector<int>& v) const {
            size_t h = v.size();
            for (int x : v)
                h ^= (size_t)x * 2654435761ULL + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<std::vector<int>, int, VecHash> subIndex;
    for (auto& sub : enumerate(samples_, s_)) {
        if (subIndex.find(sub) == subIndex.end()) {
            subIndex[sub] = (int)globalSubs.size();
            globalSubs.push_back(std::move(sub));
        }
    }
    const int numSubs = (int)globalSubs.size();

    for (int i = 0; i < numJ; ++i)
        for (auto& sub : enumerate(jSubsets[i], s_))
            jToSubs[i].push_back(subIndex.at(sub));

    // ── 3. Inverted index (Layer 2): sCoversList[si] = j-subsets containing s-subset si ─
    std::vector<std::vector<int>> sCoversList(numSubs);
    for (int i = 0; i < numJ; ++i)
        for (int si : jToSubs[i])
            sCoversList[si].push_back(i);

    // ── Helper: k-group → list of its s-subset global indices (used by SA) ───
    auto getKSubIndices = [&](const std::vector<int>& kg) -> std::vector<int> {
        std::vector<int> result;
        for (auto& sub : enumerate(kg, s_)) {
            auto it = subIndex.find(sub);
            if (it != subIndex.end()) result.push_back(it->second);
        }
        return result;
    };

    // ── Sparse scoring function (marginal gain; dirty list avoids O(numJ) init) ─
    std::vector<int> jNewCount(numJ, 0);
    std::vector<int> dirtyList;
    dirtyList.reserve(256);

    auto computeScoreImpl = [&](const std::vector<int>& kSubs,
                                 const std::vector<bool>& cov,
                                 const std::vector<int>&  jC,
                                 const std::vector<bool>& jS) -> int {
        dirtyList.clear();
        for (int si : kSubs) {
            if (cov[si]) continue;
            for (int jIdx : sCoversList[si]) {
                if (jS[jIdx]) continue;
                if (jNewCount[jIdx] == 0) dirtyList.push_back(jIdx);
                ++jNewCount[jIdx];
            }
        }
        int score = 0;
        for (int jIdx : dirtyList) {
            score += std::min(jNewCount[jIdx], minCover_ - jC[jIdx]);
            jNewCount[jIdx] = 0;
        }
        return score;
    };

    // ── Algorithm selection (threshold roughly equals C(n,7) > 10000 when n > 16) ─
    const long long nkSize = C(n_, k_);
    const int THRESHOLD_LARGE = 10000;

    std::vector<std::vector<int>> finalGroups;
    std::mt19937 rng(42u);

    if (nkSize <= THRESHOLD_LARGE) {
        // ════════════════════════════════════════════════════════════════════
        // Small/medium scale: Layer 1 + Layer 2 lazy greedy + random restarts
        // ════════════════════════════════════════════════════════════════════

        // Layer 1 fix: enumerate each k-candidate's own C(k,s) s-subsets
        // O(C(n,k)×C(n,s)) → O(C(n,k)×C(k,s)); 25,300x speedup at n=25
        const std::vector<std::vector<int>> kCandidates = enumerate(samples_, k_);
        std::cout << ">> C(" << n_ << "," << k_ << ") = " << kCandidates.size()
                  << " k-candidate groups (lazy greedy mode)\n";

        std::vector<std::vector<int>> kCoversS(kCandidates.size());
        for (int c = 0; c < (int)kCandidates.size(); ++c)
            for (auto& sub : enumerate(kCandidates[c], s_)) {
                auto it = subIndex.find(sub);
                if (it != subIndex.end()) kCoversS[c].push_back(it->second);
            }

        // Shared greedy state (reset with fill on each restart, no heap alloc)
        std::vector<bool> usedCand(kCandidates.size(), false);
        std::vector<bool> covState(numSubs, false);
        std::vector<int>  jCnt(numJ, 0);
        std::vector<bool> jSat(numJ, false);

        auto scoreForCand = [&](int c) -> int {
            return computeScoreImpl(kCoversS[c], covState, jCnt, jSat);
        };

        // Lazy greedy: score is monotone non-increasing; pop from heap and re-insert
        // with updated score if actual score is lower; tieBreak randomizes ties per restart
        using T3 = std::tuple<int, int, int>;

        auto runLazyGreedy = [&](unsigned seed) -> std::vector<int> {
            std::mt19937 rng_local(seed);
            std::uniform_int_distribution<int> tbDist(0, INT_MAX);

            std::fill(usedCand.begin(), usedCand.end(), false);
            std::fill(covState.begin(), covState.end(), false);
            std::fill(jCnt.begin(),    jCnt.end(),    0);
            std::fill(jSat.begin(),    jSat.end(),    false);
            int remaining = numJ;
            std::vector<int> res;

            // Initial full scoring: Layer 1+2 reduces per-score from
            //   O(C(n,j)×C(j,s)) to O(C(k,s) × avg_sCoversList)
            std::priority_queue<T3> pq;
            for (int c = 0; c < (int)kCandidates.size(); ++c)
                pq.push({scoreForCand(c), tbDist(rng_local), c});

            while (remaining > 0 && !pq.empty()) {
                auto [topScore, tb, c] = pq.top(); pq.pop();
                if (usedCand[c]) continue;

                // Lazy validation: re-insert with updated score if actual score dropped
                int real = scoreForCand(c);
                if (real < topScore) {
                    pq.push({real, tbDist(rng_local), c});
                    continue;
                }
                if (real == 0) break;

                usedCand[c] = true;
                res.push_back(c);

                // Layer 2 incremental update: O(C(k,s) × avg_sCoversList) vs full O(numJ)
                for (int si : kCoversS[c]) {
                    if (covState[si]) continue;
                    covState[si] = true;
                    for (int jIdx : sCoversList[si]) {
                        if (jSat[jIdx]) continue;
                        if (++jCnt[jIdx] >= minCover_) { jSat[jIdx] = true; --remaining; }
                    }
                }
            }
            return res;
        };

        // Restart count adapts to candidate scale
        int numRestarts = ((int)kCandidates.size() <= 200)  ? 500 :
                          ((int)kCandidates.size() <= 2000) ? 50  : 10;
        std::cout << ">> Starting lazy greedy (" << numRestarts << " restarts)...\n";

        std::vector<int> bestIdx;
        for (int r = 0; r < numRestarts; ++r) {
            auto cand = runLazyGreedy(42u + (unsigned)r);
            if (bestIdx.empty() || cand.size() < bestIdx.size())
                bestIdx = std::move(cand);
        }

        std::cout << ">> Greedy phase done (" << numRestarts << " restarts): "
                  << bestIdx.size() << " k-groups\n";

        for (int idx : bestIdx) finalGroups.push_back(kCandidates[idx]);

    } else {
        // ════════════════════════════════════════════════════════════════════
        // Large scale (C(n,k) > 10000): Layer 3 simulated annealing
        // No full enumeration; random k-groups walk through the solution space
        // ════════════════════════════════════════════════════════════════════
        std::cout << ">> C(" << n_ << "," << k_ << ") = " << nkSize
                  << " candidates (above threshold, enabling simulated annealing)\n";

        // SA state: reference counts ensure correctness of incremental updates
        std::vector<int>  covRef(numSubs, 0);   // coverage count per s-subset (ref count)
        std::vector<int>  jCntSA(numJ, 0);      // number of distinct covered s-subsets per j-subset
        std::vector<bool> jSatSA(numJ, false);  // whether each j-subset satisfies minCover
        int satisfiedSA = 0;

        // SA incremental add (returns number of newly satisfied j-subsets)
        auto saAdd = [&](const std::vector<int>& kSubs) -> int {
            int gain = 0;
            for (int si : kSubs)
                if (++covRef[si] == 1)           // 0→1: s-subset newly covered
                    for (int jIdx : sCoversList[si])
                        if (!jSatSA[jIdx] && ++jCntSA[jIdx] == minCover_) {
                            jSatSA[jIdx] = true; ++gain;
                        }
            satisfiedSA += gain;
            return gain;
        };

        // SA incremental remove (returns number of j-subsets that become unsatisfied)
        auto saRemove = [&](const std::vector<int>& kSubs) -> int {
            int loss = 0;
            for (int si : kSubs)
                if (--covRef[si] == 0)           // 1→0: s-subset loses coverage
                    for (int jIdx : sCoversList[si])
                        if (jCntSA[jIdx] > 0) {
                            --jCntSA[jIdx];
                            if (jSatSA[jIdx] && jCntSA[jIdx] < minCover_) {
                                jSatSA[jIdx] = false; ++loss;
                            }
                        }
            satisfiedSA -= loss;
            return loss;
        };

        // Generate a random k-group
        auto randKGroup = [&]() -> std::vector<int> {
            std::vector<int> pool = samples_;
            std::shuffle(pool.begin(), pool.end(), rng);
            pool.resize(k_);
            std::sort(pool.begin(), pool.end());
            return pool;
        };

        // Initial solution: randomly sample INIT_SAMPLE candidates, greedy on the sample set
        const int INIT_SAMPLE = (int)std::min((long long)3000, nkSize);
        std::cout << ">> Building initial solution (sampling " << INIT_SAMPLE << " candidates)...\n";
        {
            std::vector<std::vector<int>> sampledCands, sampledKSubs;
            std::set<std::vector<int>> seen;
            sampledCands.reserve(INIT_SAMPLE);
            sampledKSubs.reserve(INIT_SAMPLE);
            while ((int)sampledCands.size() < INIT_SAMPLE) {
                auto kg = randKGroup();
                if (seen.insert(kg).second) {
                    sampledCands.push_back(kg);
                    sampledKSubs.push_back(getKSubIndices(kg));
                }
            }

            std::vector<bool> covI(numSubs, false);
            std::vector<int>  jCI(numJ, 0);
            std::vector<bool> jSI(numJ, false);
            std::vector<bool> usedI(INIT_SAMPLE, false);
            int remI = numJ;

            while (remI > 0) {
                int bestC = -1, bestS = -1;
                for (int c = 0; c < INIT_SAMPLE; ++c) {
                    if (usedI[c]) continue;
                    int sc = computeScoreImpl(sampledKSubs[c], covI, jCI, jSI);
                    if (sc > bestS) { bestS = sc; bestC = c; }
                }
                if (bestC == -1 || bestS == 0) break;
                usedI[bestC] = true;
                finalGroups.push_back(sampledCands[bestC]);
                for (int si : sampledKSubs[bestC]) {
                    if (covI[si]) continue;
                    covI[si] = true;
                    for (int jIdx : sCoversList[si]) {
                        if (jSI[jIdx]) continue;
                        if (++jCI[jIdx] >= minCover_) { jSI[jIdx] = true; --remI; }
                    }
                }
            }
        }

        // Initialize SA state with the initial solution; cache s-subset indices per group
        std::vector<std::vector<int>> groupKSubs;
        groupKSubs.reserve(finalGroups.size() + 64);
        for (const auto& kg : finalGroups) {
            groupKSubs.push_back(getKSubIndices(kg));
            saAdd(groupKSubs.back());
        }

        std::cout << ">> Initial solution: " << finalGroups.size() << " groups, satisfying "
                  << satisfiedSA << "/" << numJ << " j-subsets\n";

        // SA main loop (exponential cooling)
        // Objective delta = coverage_change×100 ± size_change (100:1 weight, coverage first)
        const int SA_STEPS = 500000;
        double T = std::max(1.0, (double)numJ * 0.05);
        const double T_MIN  = 0.001;
        const double ALPHA  = std::pow(T_MIN / T, 1.0 / SA_STEPS);

        std::uniform_real_distribution<double> uProb(0.0, 1.0);

        std::vector<std::vector<int>> bestSAGroups = finalGroups;
        int bestSatisfied = satisfiedSA;
        int bestSize      = (int)finalGroups.size();

        std::cout << ">> Starting simulated annealing (" << SA_STEPS << " steps)...\n";

        for (int step = 0; step < SA_STEPS; ++step, T *= ALPHA) {
            if (finalGroups.empty()) {
                auto ng = randKGroup(); auto ns = getKSubIndices(ng);
                finalGroups.push_back(ng); groupKSubs.push_back(ns); saAdd(ns);
                continue;
            }

            std::uniform_int_distribution<int> actD(0, 2);
            const int action = actD(rng);
            const int szNow  = (int)finalGroups.size();

            if (action == 0) {
                // Add: insert a random new k-group
                auto ng = randKGroup(); auto ns = getKSubIndices(ng);
                const int satB = satisfiedSA;
                saAdd(ns);
                const double delta = (satisfiedSA - satB) * 100.0 - 1.0;
                if (delta < 0 && uProb(rng) > std::exp(delta / T)) {
                    saRemove(ns);
                } else {
                    finalGroups.push_back(ng);
                    groupKSubs.push_back(std::move(ns));
                }

            } else if (action == 1 && szNow > 1) {
                // Remove: delete a random group
                std::uniform_int_distribution<int> gD(0, szNow - 1);
                const int gi = gD(rng);
                const int satB = satisfiedSA;
                saRemove(groupKSubs[gi]);
                const double delta = (satisfiedSA - satB) * 100.0 + 1.0;
                if (delta < 0 && uProb(rng) > std::exp(delta / T)) {
                    saAdd(groupKSubs[gi]);
                } else {
                    finalGroups.erase(finalGroups.begin() + gi);
                    groupKSubs.erase(groupKSubs.begin() + gi);
                }

            } else {
                // Swap: replace a random group with a new random group
                std::uniform_int_distribution<int> gD(0, szNow - 1);
                const int gi = gD(rng);
                auto ng = randKGroup(); auto ns = getKSubIndices(ng);
                const int satB = satisfiedSA;
                saRemove(groupKSubs[gi]);
                saAdd(ns);
                const double delta = (satisfiedSA - satB) * 100.0;
                if (delta < 0 && uProb(rng) > std::exp(delta / T)) {
                    saRemove(ns);
                    saAdd(groupKSubs[gi]);
                } else {
                    finalGroups[gi] = std::move(ng);
                    groupKSubs[gi]  = std::move(ns);
                }
            }

            // Track historical best
            if (satisfiedSA > bestSatisfied ||
                (satisfiedSA == bestSatisfied && (int)finalGroups.size() < bestSize)) {
                bestSatisfied = satisfiedSA;
                bestSize      = (int)finalGroups.size();
                bestSAGroups  = finalGroups;
            }
        }

        finalGroups = bestSAGroups;
        std::cout << ">> Simulated annealing done: " << finalGroups.size() << " groups, satisfying "
                  << bestSatisfied << "/" << numJ << " j-subsets\n";

        // Fallback: supplemental greedy if SA didn't achieve full coverage
        if (bestSatisfied < numJ) {
            std::cout << ">> SA coverage incomplete, running supplemental greedy...\n";
            std::fill(covRef.begin(), covRef.end(), 0);
            std::fill(jCntSA.begin(), jCntSA.end(), 0);
            std::fill(jSatSA.begin(), jSatSA.end(), false);
            satisfiedSA = 0;
            for (const auto& kg : finalGroups) saAdd(getKSubIndices(kg));

            const int MAX_ATTEMPTS = numJ * 100;
            for (int att = 0; att < MAX_ATTEMPTS && satisfiedSA < numJ; ++att) {
                auto ng = randKGroup(); auto ns = getKSubIndices(ng);
                const int satB = satisfiedSA;
                saAdd(ns);
                if (satisfiedSA > satB) {
                    finalGroups.push_back(ng);
                } else {
                    saRemove(ns);
                }
            }
            std::cout << ">> After supplement: " << finalGroups.size() << " groups\n";
        }
    }

    // ── Backward elimination: try removing each group; keep removal if feasible ─
    std::cout << ">> Starting backward elimination...\n";
    {
        // Pre-cache s-subset indices per group, build covRef_be reference counts
        std::vector<std::vector<int>> fgKSubs;
        fgKSubs.reserve(finalGroups.size());
        std::vector<int> covRef_be(numSubs, 0);
        for (const auto& kg : finalGroups) {
            fgKSubs.push_back(getKSubIndices(kg));
            for (int si : fgKSubs.back()) ++covRef_be[si];
        }

        // Check: after temporarily removing group g, verify all j-subsets still have ≥ minCover covered s-subsets
        auto canRemove = [&](int g) -> bool {
            for (int si : fgKSubs[g]) --covRef_be[si];
            bool ok = true;
            for (int i = 0; i < numJ && ok; ++i) {
                int cnt = 0;
                for (int si : jToSubs[i]) if (covRef_be[si] >= 1) ++cnt;
                if (cnt < minCover_) ok = false;
            }
            for (int si : fgKSubs[g]) ++covRef_be[si];
            return ok;
        };

        bool improved = true;
        while (improved) {
            improved = false;
            for (int g = 0; g < (int)finalGroups.size(); ++g) {
                if (canRemove(g)) {
                    for (int si : fgKSubs[g]) --covRef_be[si];
                    finalGroups.erase(finalGroups.begin() + g);
                    fgKSubs.erase(fgKSubs.begin() + g);
                    improved = true;
                    break;
                }
            }
        }
    }

    std::cout << ">> Backward elimination done: final " << finalGroups.size() << " k-groups\n";
    return finalGroups;
}

// ════════════════════════════════════════════════════════════════════════════════
// Print
// ════════════════════════════════════════════════════════════════════════════════

void SampleSelectSystem::printParams() const
{
    std::cout << "┌─────────────────────────────────────────\n";
    std::cout << "│ An Optimal Samples Selection System\n";
    std::cout << "├─────────────────────────────────────────\n";
    std::cout << "│  m = " << m_ << "  (total samples)\n";
    std::cout << "│  n = " << n_ << "  (selected samples)\n";
    std::cout << "│  k = " << k_ << "  (group size)\n";
    std::cout << "│  j = " << j_ << "  (cover subset size)\n";
    std::cout << "│  s = " << s_ << "  (min intersection size)\n";
    std::cout << "│  minCover = " << minCover_
              << "  (min s-subsets covered per j-subset)\n";
    std::cout << "├─────────────────────────────────────────\n";
    std::cout << "│  C(n,j) = C(" << n_ << "," << j_ << ") = "
              << C(n_, j_) << "  j-subsets\n";
    std::cout << "│  C(j,s) = C(" << j_ << "," << s_ << ") = "
              << C(j_, s_) << "  s-subsets per j-subset\n";
    std::cout << "│  C(n,k) = C(" << n_ << "," << k_ << ") = "
              << C(n_, k_) << "  k-candidates\n";
    std::cout << "└─────────────────────────────────────────\n";
}

void SampleSelectSystem::printGroups(
    const std::vector<std::vector<int>>& groups) const
{
    if (groups.empty()) {
        std::cout << "[Info] Result is empty, no k-groups.\n";
        return;
    }
    std::cout << groups.size() << " k=" << k_ << " sample groups:\n";
    for (int i = 0; i < (int)groups.size(); ++i) {
        std::cout << std::setw(4) << (i + 1) << ". [ ";
        for (int v : groups[i]) std::cout << std::setw(3) << v << " ";
        std::cout << "]\n";
    }
}

// ════════════════════════════════════════════════════════════════════════════════
// File operations
// ════════════════════════════════════════════════════════════════════════════════

std::string SampleSelectSystem::buildFileName(int runCount,
                                              int resultCount) const
{
    return std::to_string(m_) + "-" +
           std::to_string(n_) + "-" +
           std::to_string(k_) + "-" +
           std::to_string(j_) + "-" +
           std::to_string(s_) + "-" +
           std::to_string(runCount) + "-" +
           std::to_string(resultCount) + ".txt";
}

std::string SampleSelectSystem::saveToFile(
    int runCount,
    const std::vector<std::vector<int>>& groups,
    const std::string& outputDir)
{
    if (groups.empty()) {
        std::cerr << "[Warning] Result is empty, nothing written to file.\n";
        return "";
    }

    std::string filename = buildFileName(runCount, (int)groups.size());
    std::string filepath = outputDir + "/" + filename;

    try {
        // Ensure output directory exists
        fs::create_directories(outputDir);

        std::ofstream ofs(filepath);
        if (!ofs.is_open()) {
            throw std::runtime_error("Cannot open file: " + filepath);
        }

        // Write file header comment
        ofs << "# m=" << m_ << " n=" << n_ << " k=" << k_
            << " j=" << j_ << " s=" << s_
            << " minCover=" << minCover_
            << " run=" << runCount
            << " count=" << groups.size() << "\n";

        // Write one k-group per line, comma-separated
        for (const auto& g : groups) {
            for (int i = 0; i < (int)g.size(); ++i) {
                if (i > 0) ofs << ",";
                ofs << g[i];
            }
            ofs << "\n";
        }
        ofs.close();

        std::cout << ">> Results saved to: " << filepath << "\n";
        return filepath;

    } catch (const std::exception& e) {
        std::cerr << "[Error] Failed to save file: " << e.what() << "\n";
        return "";
    }
}

std::vector<int> SampleSelectSystem::parseLine(const std::string& line)
{
    std::vector<int> result;
    if (line.empty() || line[0] == '#') return result; // skip comment lines
    std::istringstream iss(line);
    std::string token;
    while (std::getline(iss, token, ',')) {
        // Trim leading/trailing whitespace
        token.erase(0, token.find_first_not_of(" \t\r\n"));
        token.erase(token.find_last_not_of(" \t\r\n") + 1);
        if (!token.empty()) {
            try {
                result.push_back(std::stoi(token));
            } catch (...) {
                // skip non-numeric tokens
            }
        }
    }
    return result;
}

std::vector<std::vector<int>> SampleSelectSystem::loadFromFile(
    const std::string& filename) const
{
    std::vector<std::vector<int>> groups;
    try {
        std::ifstream ifs(filename);
        if (!ifs.is_open()) {
            throw std::runtime_error("File not found or unreadable: " + filename);
        }
        std::string line;
        while (std::getline(ifs, line)) {
            auto group = parseLine(line);
            if (!group.empty()) {
                groups.push_back(group);
            }
        }
        ifs.close();
        std::cout << ">> Loaded " << groups.size() << " k-groups from " << filename << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[Error] Failed to read file: " << e.what() << "\n";
    }
    return groups;
}

bool SampleSelectSystem::deleteFile(const std::string& filename) const
{
    try {
        if (!fs::exists(filename)) {
            std::cerr << "[Error] File not found: " << filename << "\n";
            return false;
        }
        fs::remove(filename);
        std::cout << ">> Deleted file: " << filename << "\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Error] Failed to delete file: " << e.what() << "\n";
        return false;
    }
}

bool SampleSelectSystem::deleteGroup(const std::string& filename,
                                     int groupIndex) const
{
    try {
        // Read all lines
        std::ifstream ifs(filename);
        if (!ifs.is_open()) {
            throw std::runtime_error("File not found or unreadable: " + filename);
        }

        std::vector<std::string> allLines;
        std::string line;
        while (std::getline(ifs, line)) {
            allLines.push_back(line);
        }
        ifs.close();

        // Separate comment lines from data lines
        std::vector<std::string> commentLines, dataLines;
        for (auto& l : allLines) {
            if (!l.empty() && l[0] == '#') commentLines.push_back(l);
            else if (!parseLine(l).empty())  dataLines.push_back(l);
        }

        if (groupIndex < 0 || groupIndex >= (int)dataLines.size()) {
            throw std::out_of_range("groupIndex=" + std::to_string(groupIndex) +
                " out of range [0, " + std::to_string((int)dataLines.size() - 1) + "]");
        }

        // Remove the specified line
        dataLines.erase(dataLines.begin() + groupIndex);

        // Rewrite the file
        std::ofstream ofs(filename);
        if (!ofs.is_open()) {
            throw std::runtime_error("Cannot write to file: " + filename);
        }
        for (auto& l : commentLines) ofs << l << "\n";
        for (auto& l : dataLines)    ofs << l << "\n";
        ofs.close();

        std::cout << ">> Deleted k-group #" << (groupIndex + 1)
                  << " from " << filename << "\n";
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[Error] Failed to delete k-group: " << e.what() << "\n";
        return false;
    }
}

std::vector<std::string> SampleSelectSystem::listDBFiles(
    const std::string& dir) const
{
    std::vector<std::string> files;
    try {
        if (!fs::exists(dir) || !fs::is_directory(dir)) {
            std::cerr << "[Warning] Directory not found: " << dir << "\n";
            return files;
        }
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                std::string name = entry.path().filename().string();
                // Simple check: ends with .txt
                if (name.size() > 4 &&
                    name.substr(name.size() - 4) == ".txt") {
                    files.push_back(entry.path().string());
                }
            }
        }
        std::sort(files.begin(), files.end());
    } catch (const std::exception& e) {
        std::cerr << "[Error] Failed to list files: " << e.what() << "\n";
    }
    return files;
}

// ════════════════════════════════════════════════════════════════════════════════
// Number pool management
// ════════════════════════════════════════════════════════════════════════════════

bool SampleSelectSystem::loadSamplePool(const std::string& filename)
{
    try {
        std::ifstream ifs(filename);
        if (!ifs.is_open()) {
            throw std::runtime_error("File not found or unreadable: " + filename);
        }

        // Parse line by line, skip comments, collect all integers
        std::vector<int> loaded;
        std::string line;
        while (std::getline(ifs, line)) {
            auto nums = parseLine(line);  // reuse existing parser
            for (int v : nums) loaded.push_back(v);
        }
        ifs.close();

        // Validate: count must exactly equal m_
        if ((int)loaded.size() != m_) {
            std::cerr << "[Error] Pool file has " << loaded.size()
                      << " numbers, expected m=" << m_ << ".\n";
            return false;
        }

        // Validate: each value in [1, 54]
        for (int v : loaded) {
            if (v < 1 || v > M_MAX) {
                std::cerr << "[Error] Pool file contains invalid number " << v
                          << " (must be in [1, " << M_MAX << "]).\n";
                return false;
            }
        }

        // Validate: no duplicates
        std::set<int> seen(loaded.begin(), loaded.end());
        if ((int)seen.size() != m_) {
            std::cerr << "[Error] Pool file contains duplicate numbers.\n";
            return false;
        }

        // Valid; store in samplePool_ (sorted)
        samplePool_ = loaded;
        std::sort(samplePool_.begin(), samplePool_.end());

        std::cout << ">> Pool loaded successfully (" << filename << "), "
                  << samplePool_.size() << " numbers: ";
        for (int v : samplePool_) std::cout << v << " ";
        std::cout << "\n";
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[Error] Failed to load pool file: " << e.what() << "\n";
        return false;
    }
}

bool SampleSelectSystem::saveSamplePool(const std::string& filename,
                                        const std::vector<int>& pool)
{
    try {
        // Ensure parent directory exists
        fs::path p(filename);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }

        std::ofstream ofs(filename);
        if (!ofs.is_open()) {
            throw std::runtime_error("Cannot create file: " + filename);
        }

        // Write comment header
        ofs << "# sample_pool m=" << pool.size() << "\n";

        // Write numbers, up to 15 per line, comma-separated
        const int PER_LINE = 15;
        for (int i = 0; i < (int)pool.size(); ++i) {
            if (i > 0) {
                ofs << ",";
                if (i % PER_LINE == 0) ofs << "\n";
            }
            ofs << pool[i];
        }
        ofs << "\n";
        ofs.close();

        std::cout << ">> Pool file generated: " << filename << "\n";
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[Error] Failed to generate pool file: " << e.what() << "\n";
        return false;
    }
}

void SampleSelectSystem::printSamplePool() const
{
    if (samplePool_.empty()) {
        std::cout << ">> No pool loaded, using default range [1, " << m_ << "]\n";
        return;
    }
    std::cout << ">> Current pool (" << samplePool_.size() << " numbers):\n   ";
    for (int i = 0; i < (int)samplePool_.size(); ++i) {
        std::cout << std::setw(3) << samplePool_[i];
        if ((i + 1) % 15 == 0 && i + 1 < (int)samplePool_.size())
            std::cout << "\n   ";
        else if (i + 1 < (int)samplePool_.size())
            std::cout << ",";
    }
    std::cout << "\n";
}

// ════════════════════════════════════════════════════════════════════════════════
// ILP exact solver: generateOptimalGroupsILP()
// ════════════════════════════════════════════════════════════════════════════════

std::vector<std::vector<int>>
SampleSelectSystem::generateOptimalGroupsILP(
    const std::string& solverBin,
    const std::string& workDir,
    int timeLimitSec)
{
    if (samples_.empty()) {
        throw std::runtime_error(
            "No samples selected. Call randomSamples() or inputSamples() first.");
    }

    // ── Step 1: Run greedy to obtain an upper bound ───────────────────────────
    std::cout << ">> [ILP] Running greedy first to get upper bound...\n";
    auto greedyResult = generateOptimalGroups();
    int upperBound = (int)greedyResult.size();
    std::cout << ">> [ILP] Greedy upper bound: " << upperBound << " groups\n";

    // ── Step 2: Enumerate all k-candidates (only for small/medium C(n,k) ≤ 10000) ─
    const long long nkSize = C(n_, k_);
    const int THRESHOLD_LARGE = 10000;

    if (nkSize > THRESHOLD_LARGE) {
        std::cout << ">> [ILP] C(" << n_ << "," << k_ << ")=" << nkSize
                  << " exceeds threshold " << THRESHOLD_LARGE
                  << ". ILP only supports small/medium scale; returning greedy result.\n";
        return greedyResult;
    }

    const std::vector<std::vector<int>> kCandidates = enumerate(samples_, k_);
    if (kCandidates.empty()) return greedyResult;

    // ── Step 3: Rebuild data structures needed by ILP (same logic as generateOptimalGroups)
    const std::vector<std::vector<int>> jSubsets = enumerate(samples_, j_);
    const int numJ = (int)jSubsets.size();

    // Global s-subset index
    struct VecHash {
        size_t operator()(const std::vector<int>& v) const {
            size_t h = v.size();
            for (int x : v)
                h ^= (size_t)x * 2654435761ULL + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    std::unordered_map<std::vector<int>, int, VecHash> subIndex;
    std::vector<std::vector<int>> globalSubs;
    for (auto& sub : enumerate(samples_, s_)) {
        if (subIndex.find(sub) == subIndex.end()) {
            subIndex[sub] = (int)globalSubs.size();
            globalSubs.push_back(sub);
        }
    }
    const int numSubs = (int)globalSubs.size();

    // jToSubs[i] = list of global s-subset indices within j-subset i
    std::vector<std::vector<int>> jToSubs(numJ);
    for (int i = 0; i < numJ; ++i)
        for (auto& sub : enumerate(jSubsets[i], s_))
            jToSubs[i].push_back(subIndex.at(sub));

    // sCoversList[si] = list of k-candidate indices containing s-subset si (inverted index)
    std::vector<std::vector<int>> sCoversList(numSubs);
    for (int c = 0; c < (int)kCandidates.size(); ++c)
        for (auto& sub : enumerate(kCandidates[c], s_)) {
            auto it = subIndex.find(sub);
            if (it != subIndex.end())
                sCoversList[it->second].push_back(c);
        }

    // ── Step 4: Construct ILPSolver::Problem ──────────────────────────────────
    ILPSolver::Problem prob;
    prob.numK              = (int)kCandidates.size();
    prob.numSubs           = numSubs;
    prob.numJ              = numJ;
    prob.minCover          = minCover_;
    prob.sCoversList       = sCoversList;
    prob.jToSubs           = jToSubs;
    prob.greedyUpperBound  = upperBound;

    std::cout << ">> [ILP] Variables: " << prob.numK << " (x) + "
              << prob.numSubs << " (z) = " << (prob.numK + prob.numSubs) << "\n";
    std::cout << ">> [ILP] Constraints: " << prob.numSubs << " (coverage) + "
              << prob.numJ << " (satisfaction) = " << (prob.numSubs + prob.numJ) << "\n";

    // ── Step 5: Invoke ILP solver ─────────────────────────────────────────────
    auto sol = ILPSolver::solve(prob, workDir, solverBin, timeLimitSec);

    if (!sol.feasible) {
        std::cout << ">> [ILP] No feasible solution found (" << sol.statusMsg
                  << "); returning greedy result.\n";
        return greedyResult;
    }

    // ── Step 6: Convert selected indices to k-groups ──────────────────────────
    std::vector<std::vector<int>> optGroups;
    optGroups.reserve(sol.selectedIndices.size());
    for (int idx : sol.selectedIndices)
        optGroups.push_back(kCandidates[idx]);

    std::cout << ">> [ILP] " << (sol.optimal ? "Optimal" : "Feasible")
              << " solution: " << optGroups.size() << " groups"
              << " (greedy: " << upperBound << " groups)\n";

    return optGroups;
}
