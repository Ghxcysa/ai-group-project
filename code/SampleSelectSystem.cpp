/**
 * SampleSelectSystem.cpp
 * Optimal Sample Selection System — full core class implementation
 */

// Python.h must be included before any standard headers
#ifdef ENABLE_PYTHON_EMBED
#  include <Python.h>
#endif

#if defined(_MSC_VER)
#  include <intrin.h>
#endif

#include "SampleSelectSystem.h"
#include "ILPSolver.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <climits>      // INT_MAX
#include <cmath>        // std::exp, std::pow
#include <cstring>
#include <cstdio>       // std::remove
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>      // std::iota
#include <queue>        // std::priority_queue (lazy greedy)
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

static int popcount64(uint64_t x)
{
#if defined(_MSC_VER)
    return (int)__popcnt64(x);
#else
    return __builtin_popcountll(x);
#endif
}

static bool bitsetSubsetOf(
    const std::vector<uint64_t>& a,
    const std::vector<uint64_t>& b)
{
    for (int i = 0; i < (int)a.size(); ++i) {
        if ((a[i] & ~b[i]) != 0ULL) return false;
    }
    return true;
}

static std::string bitsetKey(const std::vector<uint64_t>& bits)
{
    std::string key;
    key.resize(bits.size() * sizeof(uint64_t));
    if (!bits.empty()) {
        std::memcpy(&key[0], bits.data(), bits.size() * sizeof(uint64_t));
    }
    return key;
}

static bool sortedSubsetOf(const std::vector<int>& a, const std::vector<int>& b)
{
    return std::includes(b.begin(), b.end(), a.begin(), a.end());
}

struct VecHash {
    size_t operator()(const std::vector<int>& v) const {
        size_t h = v.size();
        for (int x : v) {
            h ^= (size_t)x * 2654435761ULL + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};

struct PortfolioSetCoverModel {
    std::vector<std::vector<int>> sets;           // set id -> k-group values
    std::vector<std::vector<int>> setToElements;  // set id -> covered element ids
    std::vector<std::vector<int>> elementToSets;  // element id -> covering set ids
};

static bool isSubsetOf(const std::vector<int>& a, const std::vector<int>& b)
{
    return std::includes(b.begin(), b.end(), a.begin(), a.end());
}

static std::string coverageKey(const std::vector<int>& elements)
{
    std::ostringstream oss;
    for (int e : elements) oss << e << ',';
    return oss.str();
}

static void rebuildElementToSets(PortfolioSetCoverModel& model, int numElements)
{
    model.elementToSets.assign(numElements, {});
    for (int setId = 0; setId < (int)model.setToElements.size(); ++setId) {
        for (int e : model.setToElements[setId]) {
            model.elementToSets[e].push_back(setId);
        }
    }
}

static PortfolioSetCoverModel reduceSetCoverModel(const PortfolioSetCoverModel& input)
{
    if (input.elementToSets.empty()) return input;

    const int numElements = (int)input.elementToSets.size();
    std::vector<bool> removed(input.sets.size(), false);

    // Identical coverage: all costs are 1, so keep one representative.
    std::unordered_map<std::string, int> seen;
    for (int setId = 0; setId < (int)input.sets.size(); ++setId) {
        if (input.setToElements[setId].empty()) {
            removed[setId] = true;
            continue;
        }
        std::string key = coverageKey(input.setToElements[setId]);
        auto [it, inserted] = seen.emplace(key, setId);
        if (!inserted) removed[setId] = true;
    }

    // Dominated sets: if A covers a subset of B, A can be removed. This is
    // quadratic, so keep it to small/medium instances.
    const int DOMINATION_LIMIT = 10000;
    if ((int)input.sets.size() <= DOMINATION_LIMIT) {
        for (int a = 0; a < (int)input.sets.size(); ++a) {
            if (removed[a]) continue;
            for (int b = a + 1; b < (int)input.sets.size(); ++b) {
                if (removed[b]) continue;
                const auto& ea = input.setToElements[a];
                const auto& eb = input.setToElements[b];
                if (ea.size() <= eb.size() && isSubsetOf(ea, eb)) {
                    removed[a] = true;
                    break;
                }
                if (eb.size() <= ea.size() && isSubsetOf(eb, ea)) {
                    removed[b] = true;
                }
            }
        }
    }

    PortfolioSetCoverModel out;
    out.sets.reserve(input.sets.size());
    out.setToElements.reserve(input.setToElements.size());
    for (int setId = 0; setId < (int)input.sets.size(); ++setId) {
        if (removed[setId]) continue;
        out.sets.push_back(input.sets[setId]);
        out.setToElements.push_back(input.setToElements[setId]);
    }
    rebuildElementToSets(out, numElements);
    return out;
}

class PortfolioSetCoverSolution {
public:
    explicit PortfolioSetCoverSolution(const PortfolioSetCoverModel& model)
        : model_(&model),
          coverCount_(model.elementToSets.size(), 0),
          selected_(model.sets.size(), false),
          selectedPos_(model.sets.size(), -1),   // #2: O(1) remove index
          coveredElements_(0)
    {
    }

    bool contains(int setId) const { return selected_[setId]; }
    bool feasible() const { return coveredElements_ == (int)coverCount_.size(); }
    int size() const { return (int)selectedIds_.size(); }
    const std::vector<int>& selectedIds() const { return selectedIds_; }

    int coverCount(int elementId) const { return coverCount_[elementId]; }

    void add(int setId)
    {
        if (selected_[setId]) return;
        selected_[setId] = true;
        selectedPos_[setId] = (int)selectedIds_.size();   // #2: record position
        selectedIds_.push_back(setId);
        for (int e : model_->setToElements[setId]) {
            if (coverCount_[e]++ == 0) ++coveredElements_;
        }
    }

    void remove(int setId)
    {
        if (!selected_[setId]) return;
        selected_[setId] = false;
        // #2: O(1) swap-with-back instead of O(n) linear scan
        int pos  = selectedPos_[setId];
        int last = selectedIds_.back();
        selectedIds_[pos]  = last;
        selectedPos_[last] = pos;
        selectedIds_.pop_back();
        selectedPos_[setId] = -1;
        for (int e : model_->setToElements[setId]) {
            if (--coverCount_[e] == 0) --coveredElements_;
        }
    }

    bool canRemove(int setId) const
    {
        if (!selected_[setId]) return false;
        for (int e : model_->setToElements[setId]) {
            if (coverCount_[e] <= 1) return false;
        }
        return true;
    }

    int privateCount(int setId) const
    {
        int count = 0;
        for (int e : model_->setToElements[setId]) {
            if (coverCount_[e] == 1) ++count;
        }
        return count;
    }

    std::vector<int> uncoveredElements() const
    {
        std::vector<int> result;
        for (int e = 0; e < (int)coverCount_.size(); ++e) {
            if (coverCount_[e] == 0) result.push_back(e);
        }
        return result;
    }

    std::vector<std::vector<int>> groups() const
    {
        std::vector<std::vector<int>> result;
        result.reserve(selectedIds_.size());
        for (int setId : selectedIds_) result.push_back(model_->sets[setId]);
        std::sort(result.begin(), result.end());
        return result;
    }

private:
    const PortfolioSetCoverModel* model_;
    std::vector<int> coverCount_;
    std::vector<bool> selected_;
    std::vector<int> selectedPos_;   // #2: setId → position in selectedIds_ (-1 if absent)
    std::vector<int> selectedIds_;
    int coveredElements_;
};

static void addMandatorySingletonSets(
    const PortfolioSetCoverModel& model,
    PortfolioSetCoverSolution& solution)
{
    for (const auto& coveringSets : model.elementToSets) {
        if (coveringSets.size() == 1) {
            solution.add(coveringSets.front());
        }
    }
}

static double scoreSet(
    const PortfolioSetCoverModel& model,
    const PortfolioSetCoverSolution& solution,
    int setId,
    const std::vector<double>& weights)
{
    double score = 0.0;
    for (int e : model.setToElements[setId]) {
        if (solution.coverCount(e) == 0) score += weights[e];
    }
    return score;
}

static void reversePrune(
    const PortfolioSetCoverModel& model,
    PortfolioSetCoverSolution& solution)
{
    bool improved = true;
    while (improved) {
        improved = false;
        std::vector<int> order = solution.selectedIds();
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            int pa = solution.privateCount(a);
            int pb = solution.privateCount(b);
            if (pa != pb) return pa < pb;
            return model.setToElements[a].size() > model.setToElements[b].size();
        });
        for (int setId : order) {
            if (solution.canRemove(setId)) {
                solution.remove(setId);
                improved = true;
                break;
            }
        }
    }
}

static bool setCoversAllElements(
    const PortfolioSetCoverModel& model,
    int setId,
    const std::vector<int>& required)
{
    const auto& elems = model.setToElements[setId];
    for (int e : required) {
        if (!std::binary_search(elems.begin(), elems.end(), e)) return false;
    }
    return true;
}

static bool trySetReplacementMove(
    const PortfolioSetCoverModel& model,
    PortfolioSetCoverSolution& solution,
    std::mt19937& rng)
{
    auto selected = solution.selectedIds();
    if (selected.empty()) return false;

    std::sort(selected.begin(), selected.end(), [&](int a, int b) {
        int pa = solution.privateCount(a);
        int pb = solution.privateCount(b);
        if (pa != pb) return pa < pb;
        return model.setToElements[a].size() < model.setToElements[b].size();
    });

    const int window = std::min<int>(12, selected.size());
    std::uniform_int_distribution<int> offsetDist(0, window - 1);
    const int offset = offsetDist(rng);

    for (int attempt = 0; attempt < window; ++attempt) {
        int removeId = selected[(offset + attempt) % window];

        std::vector<int> privateElems;
        for (int e : model.setToElements[removeId]) {
            if (solution.coverCount(e) == 1) privateElems.push_back(e);
        }

        if (privateElems.empty()) {
            if (solution.canRemove(removeId)) {
                solution.remove(removeId);
                reversePrune(model, solution);
                return solution.feasible();
            }
            continue;
        }

        int anchor = privateElems.front();
        for (int e : privateElems) {
            if (model.elementToSets[e].size() < model.elementToSets[anchor].size()) {
                anchor = e;
            }
        }

        std::vector<int> replacements;
        for (int cand : model.elementToSets[anchor]) {
            if (cand == removeId || solution.contains(cand)) continue;
            if (setCoversAllElements(model, cand, privateElems)) {
                replacements.push_back(cand);
            }
        }
        if (replacements.empty()) continue;

        std::sort(replacements.begin(), replacements.end(), [&](int a, int b) {
            int reliefA = 0;
            int reliefB = 0;
            for (int e : model.setToElements[a]) {
                if (solution.coverCount(e) == 1) ++reliefA;
            }
            for (int e : model.setToElements[b]) {
                if (solution.coverCount(e) == 1) ++reliefB;
            }
            if (reliefA != reliefB) return reliefA > reliefB;
            return model.setToElements[a].size() > model.setToElements[b].size();
        });

        const int pickWindow = std::min<int>(8, replacements.size());
        std::uniform_int_distribution<int> pickDist(0, pickWindow - 1);
        int addId = replacements[pickDist(rng)];

        solution.remove(removeId);
        solution.add(addId);
        if (solution.feasible()) {
            reversePrune(model, solution);
            return true;
        }

        solution.remove(addId);
        solution.add(removeId);
    }

    return false;
}

static PortfolioSetCoverSolution greedySetCover(
    const PortfolioSetCoverModel& model,
    const std::vector<double>& weights,
    unsigned seed)
{
    PortfolioSetCoverSolution solution(model);
    addMandatorySingletonSets(model, solution);

    using HeapItem = std::tuple<double, int, int>; // score, tie-breaker, set id
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> tbDist(0, INT_MAX);
    std::priority_queue<HeapItem> pq;

    for (int setId = 0; setId < (int)model.sets.size(); ++setId) {
        if (solution.contains(setId)) continue;
        pq.push({scoreSet(model, solution, setId, weights), tbDist(rng), setId});
    }

    while (!solution.feasible() && !pq.empty()) {
        auto [oldScore, tb, setId] = pq.top();
        pq.pop();
        if (solution.contains(setId)) continue;

        double realScore = scoreSet(model, solution, setId, weights);
        if (realScore + 1e-9 < oldScore) {
            pq.push({realScore, tbDist(rng), setId});
            continue;
        }
        if (realScore <= 0.0) break;
        solution.add(setId);
    }

    if (solution.feasible()) reversePrune(model, solution);
    return solution;
}

// #4: Lazy-heap repair — same pattern as greedySetCover.
// Entries are not updated in-place; a stale entry is detected when its cached
// score differs from the recomputed score, and is re-inserted with a fresh score.
// #5: reversePrune is intentionally omitted here; the LNS loop calls it only
// when the repaired solution strictly improves over the current best, avoiding
// the O(size²) prune scan on every repair that does NOT improve.
static PortfolioSetCoverSolution repairWithPenaltyGreedy(
    const PortfolioSetCoverModel& model,
    PortfolioSetCoverSolution solution,
    const std::vector<double>& penalties)
{
    using HeapItem = std::pair<double, int>; // (score, setId)
    std::priority_queue<HeapItem> pq;

    for (int sid = 0; sid < (int)model.sets.size(); ++sid) {
        if (solution.contains(sid)) continue;
        double sc = scoreSet(model, solution, sid, penalties);
        if (sc > 0.0) pq.push({sc, sid});
    }

    while (!solution.feasible() && !pq.empty()) {
        auto [oldScore, sid] = pq.top();
        pq.pop();
        if (solution.contains(sid)) continue;
        double realScore = scoreSet(model, solution, sid, penalties);
        if (realScore + 1e-9 < oldScore) {
            if (realScore > 0.0) pq.push({realScore, sid});
            continue;
        }
        if (realScore <= 0.0) break;
        solution.add(sid);
    }
    return solution;
}

// Optional callback invoked whenever LNS finds a new incumbent strictly better
// than the previous best.  Used by the anytime path to broadcast the improvement
// to the concurrent exact B&B thread without waiting for LNS to finish.
using LnsImprovementCallback =
    std::function<void(const PortfolioSetCoverSolution&)>;
using EliteSeedProvider =
    std::function<bool(PortfolioSetCoverSolution&)>;

static PortfolioSetCoverSolution lnsImproveSetCover(
    const PortfolioSetCoverModel& model,
    PortfolioSetCoverSolution best,
    int timeLimitSec,
    LnsImprovementCallback onImprove = nullptr,
    EliteSeedProvider eliteSeedProvider = nullptr)
{
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + std::chrono::seconds(std::max(1, timeLimitSec));

    PortfolioSetCoverSolution globalBest = best;
    const int numElements = (int)model.elementToSets.size();
    std::vector<double> penalties(numElements, 1.0);
    // #3: Time-based seed so different runs explore different regions.
    std::mt19937 rng(20260503u
        ^ (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count()
        ^ (uint32_t)(uintptr_t)(void*)&penalties);
    int iteration = 0;
    int lastImprovedIter = 0;   // #6: track stagnation for adaptive restart

    // ── Build sample-to-sets index for sample-destroy operators ──────────────
    // model.sets[sid] is the k-group (list of sample values).
    // sampleToSets[v] = all set IDs that contain sample value v.
    std::unordered_map<int, std::vector<int>> sampleToSets;
    for (int sid = 0; sid < (int)model.sets.size(); ++sid)
        for (int v : model.sets[sid])
            sampleToSets[v].push_back(sid);
    std::vector<int> allSamples;
    allSamples.reserve(sampleToSets.size());
    for (auto& [v, _] : sampleToSets) allSamples.push_back(v);
    std::sort(allSamples.begin(), allSamples.end());

    // ── Adaptive operator selection ───────────────────────────────────────────
    // op0 = element-destroy  (small neighbourhood, fine-grained)
    // op1 = set-destroy      (remove low-private-count sets, scales to 25% of solution)
    // op2 = sample-destroy   (remove every group containing one sample, large neighbourhood)
    // op3 = double-sample-destroy (two samples at once, very large neighbourhood)
    // op4 = set-replacement  (swap one low-private set for a different covering set)
    // op5 = mega-destroy     (remove 30-40% of solution randomly — escapes deep plateaus)
    //
    // Bias op2/op3/op5 higher at the start because large-neighbourhood moves are most
    // effective for escaping the greedy solution plateau on big instances (n≥13).
    std::array<double, 6> opScore = {1.0, 1.0, 2.0, 1.5, 2.0, 1.5};
    std::array<int, 6>    opTries = {1,   1,   1,   1,   1,   1  };

    // ── A improvement: Simulated Annealing acceptance ─────────────────────────
    // Start at 15% of initial solution size; cool by 0.05% per iteration.
    // High temperature allows escaping the greedy plateau early (accepting
    // temporarily worse solutions); the search becomes purely improving as T→0.
    // Critical for tight covering designs (n≥13) where LNS stalls at ~145 groups.
    double       saTemp    = std::max(1.0, (double)best.size() * 0.15);
    const double saCooling = 0.9995;
    const double saTempMin = 0.05;

    auto selectOp = [&]() -> int {
        double total = 0.0;
        for (int i = 0; i < (int)opScore.size(); ++i)
            total += opScore[i] / opTries[i];
        std::uniform_real_distribution<double> coin(0.0, total);
        double val = coin(rng);
        double acc = 0.0;
        for (int i = 0; i < (int)opScore.size(); ++i) {
            acc += opScore[i] / opTries[i];
            if (val < acc) return i;
        }
        return (int)opScore.size() - 1;
    };

    while (Clock::now() < deadline) {
        // Periodic penalty reset every 200 iterations to escape saturation.
        if (iteration > 0 && iteration % 200 == 0) {
            std::fill(penalties.begin(), penalties.end(), 1.0);
        }

        // #6: Adaptive GRASP fresh-restart — trigger when stagnant for too long.
        // The stagnation threshold adapts to the current best size:
        //   threshold = clamp(best.size()/2, 40, 120)
        // This restarts aggressively early (when best is large and improvement is
        // easy) and becomes patient later (when best is tight and GRASP restarts
        // themselves are expensive for large instances).
        {
            int stagnation = iteration - lastImprovedIter;
            int threshold  = std::max(40, std::min(120, (int)best.size() / 2));
            if (iteration > 0 && stagnation >= threshold) {
                PortfolioSetCoverSolution fresh = best;
                bool fromElite = false;
                if (eliteSeedProvider) {
                    std::uniform_real_distribution<double> eliteCoin(0.0, 1.0);
                    if (eliteCoin(rng) < 0.5) {
                        fromElite = eliteSeedProvider(fresh);
                    }
                }
                if (!fromElite) {
                    std::vector<double> freshWeights(numElements);
                    std::uniform_int_distribution<int> seedDist(0, INT_MAX);
                    // Rotate between three weight strategies for maximum basin diversity:
                    // - Pure random: escapes the penalty basin entirely
                    // - Inverse-degree: always prioritise the rarest (hardest) elements first
                    // - Penalty × wide noise: classic GRASP with significant perturbation
                    int strategy = seedDist(rng) % 3;
                    if (strategy == 0) {
                        // Inverse-degree + small noise: harder elements get higher
                        // weight, producing structurally different solutions from the
                        // penalty-driven strategies while staying near greedy quality.
                        std::uniform_real_distribution<double> noise(0.8, 1.2);
                        for (int e = 0; e < numElements; ++e)
                            freshWeights[e] = noise(rng) /
                                std::max(1, (int)model.elementToSets[e].size());
                    } else if (strategy == 1) {
                        // Inverse-degree: weight ∝ 1/deg(e). Greedy covers bottleneck
                        // elements first, producing structurally different solutions.
                        for (int e = 0; e < numElements; ++e)
                            freshWeights[e] = 1.0 / std::max(1,
                                (int)model.elementToSets[e].size());
                    } else {
                        // Penalty-based with wider multiplicative noise (0.3–3×)
                        // so the relative priority order can flip significantly.
                        std::uniform_real_distribution<double> noise(0.3, 3.0);
                        for (int e = 0; e < numElements; ++e)
                            freshWeights[e] = penalties[e] * noise(rng);
                    }
                    fresh = greedySetCover(model, freshWeights, (unsigned)seedDist(rng));
                }
                if (fresh.feasible()) {
                    int tolerance = std::max(3, (int)globalBest.size() / 12);
                    if (fresh.size() <= globalBest.size() + tolerance) {
                        best = fresh;
                    }
                    if (fresh.size() < globalBest.size()) {
                        globalBest = fresh;
                        if (onImprove) onImprove(globalBest);
                    }
                }
                // Reset even on no improvement to avoid immediate re-trigger.
                lastImprovedIter = iteration;
            }
        }

        PortfolioSetCoverSolution current = best;
        std::vector<int> selected = current.selectedIds();
        if (selected.empty()) break;

        int op = selectOp();
        opTries[op]++;

        if (op == 0) {
            // Element-based destroy: pick a random element, remove ALL groups covering it.
            std::uniform_int_distribution<int> eIdx(0, numElements - 1);
            int chosenElem = eIdx(rng);
            for (int setId : model.elementToSets[chosenElem]) {
                if (current.contains(setId)) current.remove(setId);
            }
        } else if (op == 1) {
            // Set-based destroy: remove sets with fewest private elements first.
            std::sort(selected.begin(), selected.end(), [&](int a, int b) {
                int pa = current.privateCount(a);
                int pb = current.privateCount(b);
                if (pa != pb) return pa < pb;
                return model.setToElements[a].size() < model.setToElements[b].size();
            });
            // Scale destroy size with solution: cycles from 1 up to 25% of
            // current solution.  Previous hard cap of 8 was only 4-8% for
            // large instances (n≥14), too small to escape deep basins.
            int baseStep  = std::max(1, (int)std::sqrt((double)selected.size()));
            int maxRemove = std::max(8, (int)selected.size() / 4); // up to 25%
            int removeCount = 1 + (iteration % baseStep);
            removeCount = std::min(removeCount, std::min(maxRemove, (int)selected.size()));
            for (int i = 0; i < removeCount; ++i) current.remove(selected[i]);
        } else if (op == 2) {
            // Sample-destroy: remove every group in the current solution that contains
            // a randomly chosen sample value.  This restructures one entire "column" of
            // the covering matrix in a single move — much more powerful than element-
            // or set-level destroy for large covering designs where many groups share a
            // sample but cover different j-subsets.
            if (!allSamples.empty()) {
                std::uniform_int_distribution<int> sIdx(0, (int)allSamples.size() - 1);
                int v = allSamples[sIdx(rng)];
                for (int sid : sampleToSets[v])
                    if (current.contains(sid)) current.remove(sid);
            }
        } else if (op == 3) {
            // Double-sample-destroy: pick two distinct samples, remove all groups that
            // contain either one.  Very large neighbourhood; effective when stuck in
            // deep plateaus (typically kicks in after op2 has converged).
            if (allSamples.size() >= 2) {
                std::uniform_int_distribution<int> sIdx(0, (int)allSamples.size() - 1);
                int i1 = sIdx(rng);
                int i2 = sIdx(rng);
                while (i2 == i1) i2 = sIdx(rng);
                for (int v : {allSamples[i1], allSamples[i2]})
                    for (int sid : sampleToSets[v])
                        if (current.contains(sid)) current.remove(sid);
            }
        } else if (op == 4) {
            // Set-replacement: swap a low-private set with a different candidate
            // covering its private elements, then prune. This reaches equal-size
            // neighbourhoods that remove+greedy-repair often maps back to itself.
            if (!trySetReplacementMove(model, current, rng)) {
                ++iteration;
                continue;
            }
        } else {
            // Mega-destroy (op5): remove 30-40% of the solution at random.
            // Used to escape deep plateaus that smaller destroys cannot breach.
            // The large destroy gives greedy-repair freedom to find structurally
            // different solutions even if it temporarily increases solution size.
            int megaCount = std::max(5, (int)(selected.size() * 0.35));
            megaCount = std::min(megaCount, (int)selected.size());
            std::shuffle(selected.begin(), selected.end(), rng);
            for (int i = 0; i < megaCount; ++i)
                current.remove(selected[i]);
        }

        // Penalty increment: rare elements (few covering sets) accumulate faster
        // so the repair greedy is forced to cover them before easy elements.
        // Base += 1 + 50/degree ensures bottleneck elements are prioritised.
        for (int e : current.uncoveredElements()) {
            penalties[e] += 1.0 + 50.0 / std::max(1, (int)model.elementToSets[e].size());
        }

        current = repairWithPenaltyGreedy(model, current, penalties);

        bool improved = false;
        if (current.feasible() && current.size() < best.size()) {
            // #5: reversePrune only on improvement — avoids O(size²) scan on
            // every iteration; only pays cost when we actually have a new best.
            reversePrune(model, current);
            if (current.size() < best.size()) {
                best = current;
                improved = true;
                lastImprovedIter = iteration;   // #6: reset stagnation counter
                if (current.size() < globalBest.size()) {
                    globalBest = current;
                    // Broadcast improvement immediately so concurrent exact B&B can
                    // tighten its budget without waiting for the LNS thread to finish.
                    if (onImprove) onImprove(globalBest);
                }
            }
        } else if (current.feasible() && current.size() > best.size()) {
            // A improvement — SA: accept a worse solution with probability
            // exp(-delta/T).  High T early in the run lets the search climb
            // out of basins; temperature decays toward saTempMin over time.
            if (saTemp > saTempMin) {
                double prob = std::exp(
                    -(double)((int)current.size() - (int)best.size()) / saTemp);
                std::uniform_real_distribution<double> coin(0.0, 1.0);
                if (coin(rng) < prob) best = current;
            }
        } else if (current.feasible()) {
            // Equal quality: accept occasionally to diversify search direction.
            std::uniform_int_distribution<int> pick(0, 4);
            if (pick(rng) == 0) best = current;
        }
        // Cool SA temperature each iteration.
        saTemp = std::max(saTempMin, saTemp * saCooling);
        if (improved) opScore[op] += 1.0;

        ++iteration;
    }

    return globalBest;
}

// ── Simulated Annealing for Set Cover ────────────────────────────────────────
//
// Uses a PENALTY-OBJECTIVE to allow infeasible intermediate states:
//   f(S) = |S| + λ × |uncovered_elements|
//
// This lets the Markov chain traverse "saddle" configurations that are
// infeasible but lead to better feasible neighbourhoods — exactly the moves
// that both LNS (greedy-repair basin) and Tabu Search (feasibility-constrained
// 1-opt) can never accept.
//
// Move: flip a single set's inclusion (O(k) per move, very fast).
// Accept: Metropolis criterion  P = exp(-Δf / T).
// Temperature: time-based exponential decay  T_start → T_min.
// Reheating: restart from best feasible solution after REHEAT_PATIENCE of the
//   budget passes without finding a new best (avoids final-stage freezing).
//
// λ = 2.0: each uncovered element costs as much as 2 extra sets.
// This keeps the chain close to feasible space while still allowing brief
// infeasible excursions when temperature is high.
// ─────────────────────────────────────────────────────────────────────────────
static PortfolioSetCoverSolution simulatedAnnealingSetCover(
    const PortfolioSetCoverModel& model,
    PortfolioSetCoverSolution     initial,
    int                           timeLimitSec,
    LnsImprovementCallback        onImprove = nullptr)
{
    using Clock = std::chrono::steady_clock;
    auto   startTime = Clock::now();
    double totalSec  = (double)std::max(1, timeLimitSec);
    auto   deadline  = startTime + std::chrono::seconds(std::max(1, timeLimitSec));

    const int numSets  = (int)model.sets.size();
    const int numElems = (int)model.elementToSets.size();

    // ── SA hyperparameters ────────────────────────────────────────────────────
    // λ = 4: removing a set with 1 private element costs Δf=+3.
    //   At T_START=0.8 that is accepted with exp(-3/0.8) ≈ 2.3% — selective
    //   but nonzero, allowing the chain to occasionally escape local optima
    //   while mostly staying feasible.
    // Adding a redundant set costs Δf=+1, accepted with exp(-1/0.8) ≈ 29%.
    // This creates new redundancy opportunities without blowing up solution size.
    constexpr double LAMBDA   = 4.0;   // penalty per uncovered element
    constexpr double T_START  = 0.8;   // initial temperature
    constexpr double T_MIN    = 1e-4;  // final temperature
    const double     LOG_COOL = std::log(T_MIN / T_START); // < 0
    constexpr double REHEAT_PATIENCE = 0.10; // reheat after 10% budget without improvement

    // ── Raw state (fast O(k) moves without allocating PortfolioSetCoverSolution) ──
    std::vector<bool> inSol(numSets,  false);
    std::vector<int>  cov  (numElems, 0);
    int solSize   = 0;
    int uncovered = 0;

    // Load initial solution
    for (int sid : initial.selectedIds()) {
        inSol[sid] = true;
        ++solSize;
        for (int e : model.setToElements[sid])
            ++cov[e];
    }
    for (int e = 0; e < numElems; ++e)
        if (cov[e] == 0) ++uncovered;

    // Best feasible solution seen so far
    PortfolioSetCoverSolution best = initial;
    int bestSize = (int)initial.size();

    // ── Helpers ───────────────────────────────────────────────────────────────
    // Rebuild a pruned PortfolioSetCoverSolution from current raw state.
    auto buildPruned = [&]() {
        PortfolioSetCoverSolution sol(model);
        for (int sid = 0; sid < numSets; ++sid)
            if (inSol[sid]) sol.add(sid);
        reversePrune(model, sol);
        return sol;
    };

    // Reload raw state from a PortfolioSetCoverSolution (e.g. after reheating).
    auto loadFrom = [&](const PortfolioSetCoverSolution& sol) {
        std::fill(inSol.begin(), inSol.end(), false);
        std::fill(cov.begin(),   cov.end(),   0);
        solSize   = 0;
        uncovered = 0;
        for (int sid : sol.selectedIds()) {
            inSol[sid] = true;
            ++solSize;
            for (int e : model.setToElements[sid])
                if (++cov[e] == 1) --uncovered;  // newly covered
        }
        // recount: simpler than incremental above if starting from 0
        uncovered = 0;
        for (int e = 0; e < numElems; ++e)
            if (cov[e] == 0) ++uncovered;
    };

    // ── RNG ───────────────────────────────────────────────────────────────────
    std::mt19937 rng(
        20260504u ^ (uint32_t)Clock::now().time_since_epoch().count()
                  ^ (uint32_t)(uintptr_t)(void*)&inSol);
    std::uniform_int_distribution<int>    setDist(0, numSets - 1);
    std::uniform_real_distribution<double> ud(0.0, 1.0);

    double lastImproveFrac = 0.0;

    // ── Main SA loop ──────────────────────────────────────────────────────────
    while (Clock::now() < deadline) {
        double elapsed = std::chrono::duration<double>(
                             Clock::now() - startTime).count();
        double frac = std::min(1.0, elapsed / totalSec);

        // Reheating: restart from best when stuck for REHEAT_PATIENCE of budget
        if (frac - lastImproveFrac > REHEAT_PATIENCE) {
            loadFrom(best);
            lastImproveFrac = frac - 0.005; // small offset to avoid tight loop
        }

        // Time-based exponential temperature decay
        double T = T_START * std::exp(LOG_COOL * frac);

        // ── Sample a random set to flip ───────────────────────────────────────
        int sid = setDist(rng);

        double deltaF;
        if (inSol[sid]) {
            // Removal: size −1, may uncover some elements → +LAMBDA each
            int newUncov = 0;
            for (int e : model.setToElements[sid])
                if (cov[e] == 1) ++newUncov;
            deltaF = -1.0 + LAMBDA * (double)newUncov;
        } else {
            // Addition: size +1, may cover some elements → −LAMBDA each
            int newCov = 0;
            for (int e : model.setToElements[sid])
                if (cov[e] == 0) ++newCov;
            deltaF = 1.0 - LAMBDA * (double)newCov;
        }

        // ── Metropolis acceptance ─────────────────────────────────────────────
        if (deltaF > 0.0 && ud(rng) >= std::exp(-deltaF / T))
            continue; // rejected

        // Apply flip
        if (inSol[sid]) {
            inSol[sid] = false;
            --solSize;
            for (int e : model.setToElements[sid])
                if (--cov[e] == 0) ++uncovered;
        } else {
            inSol[sid] = true;
            ++solSize;
            for (int e : model.setToElements[sid])
                if (++cov[e] == 1) --uncovered;
        }

        // ── Check for new feasible best ───────────────────────────────────────
        if (uncovered == 0 && solSize < bestSize) {
            PortfolioSetCoverSolution cand = buildPruned();
            if ((int)cand.size() < bestSize) {
                best     = cand;
                bestSize = (int)cand.size();
                // Reload pruned solution so we continue from the smaller state
                loadFrom(best);
                lastImproveFrac = frac;
                if (onImprove) onImprove(best);
            }
        }
    }

    return best;
}

// ── Tabu Search for Set Cover ─────────────────────────────────────────────────
// Explores the 1-opt exchange neighbourhood (remove one group G, add one
// replacement B that covers all of G's privately-covered elements) more
// systematically than LNS+SA:
//
//   • Evaluates all valid (G→B) exchanges within a window of the most
//     "exchangeable" groups (lowest private-element count) each iteration.
//   • Scores each exchange by the EXACT net Δ: how many other groups in the
//     current solution become fully redundant after the swap, computable in
//     O(|solution| × |elements-per-group|) without actually doing the swap.
//   • Always applies the best non-tabu move, even if it worsens the solution
//     (aspiration criterion: a tabu move is accepted if it beats the global best).
//   • Periodic random perturbation when stuck beyond a patience threshold.
//
// Avoids the "greedy-repair basin" that LNS falls into: no repair step means
// the neighbourhood is explored by direct substitution, giving access to
// solutions that greedy repair always maps back from.
static PortfolioSetCoverSolution tabuSearchSetCover(
    const PortfolioSetCoverModel& model,
    PortfolioSetCoverSolution     initial,
    int                           timeLimitSec,
    LnsImprovementCallback        onImprove = nullptr)
{
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + std::chrono::seconds(std::max(1, timeLimitSec));

    PortfolioSetCoverSolution current = initial;
    PortfolioSetCoverSolution best    = initial;

    const int numSets  = (int)model.sets.size();
    const int numElems = (int)model.elementToSets.size();

    // Tabu tenure: a set involved in a move is forbidden for this many iterations.
    // Scales with solution size so larger instances cycle less.
    const int tenure = std::max(7, (int)best.size() / 8);

    // tabuUntil[setId] = first iteration index at which setId may appear in a move.
    std::vector<int> tabuUntil(numSets, 0);

    // Per-element coverage-delta scratch arrays (reset per move evaluation).
    // inRemove[e] = 1 if the candidate-to-remove covers element e.
    // inAdd[e]    = 1 if the candidate-to-add    covers element e.
    std::vector<int8_t> inRemove(numElems, 0);
    std::vector<int8_t> inAdd   (numElems, 0);

    int iteration      = 0;
    int noImproveSince = 0;

    std::mt19937 rng(
        20260503u ^ (uint32_t)Clock::now().time_since_epoch().count()
                  ^ (uint32_t)(uintptr_t)(void*)&tabuUntil);

    // Examine this many of the lowest-private-count groups per iteration.
    const int EXAM_WINDOW = std::min(30, (int)initial.size());

    while (Clock::now() < deadline) {

        // ── Perturbation when stuck ───────────────────────────────────────────
        // After `patience` iterations without improvement, apply a random
        // exchange (ignoring tabu) to escape the current attractor.
        {
            int stagnation = iteration - noImproveSince;
            int patience   = std::max(60, (int)best.size() / 2);
            if (iteration > 0 && stagnation >= patience) {
                auto sel = current.selectedIds();
                if (!sel.empty()) {
                    std::uniform_int_distribution<int> rIdx(0, (int)sel.size() - 1);
                    int removeId = sel[rIdx(rng)];

                    std::vector<int> privElems;
                    for (int e : model.setToElements[removeId])
                        if (current.coverCount(e) == 1) privElems.push_back(e);

                    if (privElems.empty()) {
                        current.remove(removeId);
                        reversePrune(model, current);
                        tabuUntil[removeId] = iteration + tenure;
                    } else {
                        // Anchor = rarest private element.
                        int anchorElem = privElems[0];
                        for (int e : privElems)
                            if (model.elementToSets[e].size() <
                                model.elementToSets[anchorElem].size())
                                anchorElem = e;

                        std::vector<int> reps;
                        for (int cand : model.elementToSets[anchorElem]) {
                            if (cand == removeId || current.contains(cand)) continue;
                            if (setCoversAllElements(model, cand, privElems))
                                reps.push_back(cand);
                        }
                        if (!reps.empty()) {
                            std::uniform_int_distribution<int> rp(0, (int)reps.size()-1);
                            int addId = reps[rp(rng)];
                            current.remove(removeId);
                            current.add(addId);
                            if (current.feasible()) {
                                reversePrune(model, current);
                                tabuUntil[removeId] = iteration + tenure;
                                tabuUntil[addId]    = iteration + tenure / 2;
                                if ((int)current.size() < (int)best.size()) {
                                    best = current;
                                    noImproveSince = iteration;
                                    if (onImprove) onImprove(best);
                                }
                            } else {
                                current.remove(addId);
                                current.add(removeId); // undo
                            }
                        }
                    }
                }
                noImproveSince = iteration; // reset after perturbation
                ++iteration;
                continue;
            }
        }

        // ── Sort groups by ascending private count ────────────────────────────
        auto selected = current.selectedIds();
        std::sort(selected.begin(), selected.end(), [&](int a, int b) {
            int pa = current.privateCount(a), pb = current.privateCount(b);
            return (pa != pb) ? (pa < pb)
                              : (model.setToElements[a].size() >
                                 model.setToElements[b].size());
        });

        int examCount = std::min(EXAM_WINDOW, (int)selected.size());

        int bestRemoveId = -1;
        int bestAddId    = -1;
        int bestDelta    = INT_MAX; // negative = improvement (fewer groups)

        // ── Evaluate candidate moves ──────────────────────────────────────────
        for (int i = 0; i < examCount && bestDelta > -2; ++i) {
            int removeId = selected[i];

            std::vector<int> privElems;
            for (int e : model.setToElements[removeId])
                if (current.coverCount(e) == 1) privElems.push_back(e);

            // ── Direct removal (G has no private elements) → delta = -1 ──────
            if (privElems.empty()) {
                bool isTabu = (tabuUntil[removeId] > iteration);
                bool aspire = isTabu && ((int)current.size() - 1 < (int)best.size());
                if (!isTabu || aspire) {
                    bestRemoveId = removeId;
                    bestAddId    = -1;
                    bestDelta    = -1;
                    break; // -1 is the maximum single-move gain; stop searching
                }
                continue;
            }

            // Anchor = rarest private element (minimises replacement candidates to check).
            int anchorElem = privElems[0];
            for (int e : privElems)
                if (model.elementToSets[e].size() <
                    model.elementToSets[anchorElem].size())
                    anchorElem = e;

            // Mark elements covered by removeId for O(1) lookup below.
            for (int e : model.setToElements[removeId]) inRemove[e] = 1;

            // ── Score each replacement candidate ──────────────────────────────
            for (int cand : model.elementToSets[anchorElem]) {
                if (cand == removeId || current.contains(cand)) continue;
                if (!setCoversAllElements(model, cand, privElems)) continue;

                bool isTabu = (tabuUntil[removeId] > iteration ||
                               tabuUntil[cand]     > iteration);

                // Mark elements covered by cand.
                for (int e : model.setToElements[cand]) inAdd[e] = 1;

                // Count groups that become fully redundant after (removeId → cand).
                // Group C is redundant iff for every element e in C:
                //   coverCount(e) - inRemove[e] + inAdd[e] >= 2
                int savedExtra = 0;
                for (int other : selected) {
                    if (other == removeId) continue;
                    bool redundant = true;
                    for (int e : model.setToElements[other]) {
                        int nc = current.coverCount(e) - inRemove[e] + inAdd[e];
                        if (nc < 2) { redundant = false; break; }
                    }
                    if (redundant) ++savedExtra;
                }

                // Net Δ: swap is size-neutral (+0), pruned groups are the gain.
                int delta = -savedExtra;

                bool aspire = isTabu && ((int)current.size() + delta < (int)best.size());
                if (!isTabu || aspire) {
                    if (delta < bestDelta ||
                        (delta == bestDelta && (rng() & 1u))) {
                        bestDelta    = delta;
                        bestRemoveId = removeId;
                        bestAddId    = cand;
                    }
                }

                // Clear inAdd for next candidate.
                for (int e : model.setToElements[cand]) inAdd[e] = 0;
            }

            // Clear inRemove for next group.
            for (int e : model.setToElements[removeId]) inRemove[e] = 0;
        }

        // ── Apply best move ───────────────────────────────────────────────────
        if (bestRemoveId == -1) {
            ++iteration;
            continue;
        }

        current.remove(bestRemoveId);
        tabuUntil[bestRemoveId] = iteration + tenure;

        if (bestAddId >= 0) {
            current.add(bestAddId);
            tabuUntil[bestAddId] = iteration + tenure / 2;
        }

        if (!current.feasible()) {
            // Defensive: undo if infeasible (should not occur with correct checks).
            current.add(bestRemoveId);
            if (bestAddId >= 0) current.remove(bestAddId);
            ++iteration;
            continue;
        }

        reversePrune(model, current);

        if (current.feasible() && (int)current.size() < (int)best.size()) {
            best = current;
            noImproveSince = iteration;
            if (onImprove) onImprove(best);
        }

        ++iteration;
    }

    return best;
}

static int ceilToInt(long double value)
{
    return (int)std::ceil(value - 1e-12L);
}

static int countingLowerBound(int n, int k, int j, int s, int minCover)
{
    long double numerator = (long double)SampleSelectSystem::C(n, j) * minCover;
    long double denominator = 0.0L;

    if (minCover == 1) {
        for (int t = s; t <= std::min(j, k); ++t) {
            denominator += (long double)SampleSelectSystem::C(k, t)
                * SampleSelectSystem::C(n - k, j - t);
        }
    } else {
        denominator = (long double)SampleSelectSystem::C(k, s)
            * SampleSelectSystem::C(n - s, j - s);
    }

    if (denominator <= 0.0L) return 0;
    return std::max(1, ceilToInt(numerator / denominator));
}

// P4 — Schönheim recursive lower bound for covering designs.
// Strictly stronger than the simple counting bound for most parameter sets.
// Formula: D(n,k,t) ≥ ⌈(n/k) · D(n-1, k-1, t-1)⌉ with D(n,k,0)=1 and D(n,k,t)=0
// when k<t.  Only valid for minCover==1 and j==s (standard covering design).
static int schonheimLowerBound(int n, int k, int t)
{
    if (t == 0) return 1;
    if (k < t || n < k) return 0;
    if (n == k) return 1;
    long double inner = (long double)n / (long double)k
                        * (long double)schonheimLowerBound(n - 1, k - 1, t - 1);
    return (int)std::ceil(inner - 1e-9L);
}

// Combined lower bound: max of counting bound and Schönheim bound.
static int combinedLowerBound(int n, int k, int j, int s, int minCover)
{
    int lb = countingLowerBound(n, k, j, s, minCover);
    if (minCover == 1 && j == s) {
        lb = std::max(lb, schonheimLowerBound(n, k, j));
    }
    return lb;
}

static PortfolioSetCoverModel buildStandardSetCoverModel(
    const std::vector<int>& samples,
    int k,
    int j,
    int s)
{
    PortfolioSetCoverModel model;
    model.sets = SampleSelectSystem::enumerate(samples, k);

    const auto jSubsets = SampleSelectSystem::enumerate(samples, j);
    std::unordered_map<std::vector<int>, int, VecHash> elementIndex;
    elementIndex.reserve(jSubsets.size() * 2);
    for (int i = 0; i < (int)jSubsets.size(); ++i) {
        elementIndex[jSubsets[i]] = i;
    }

    model.setToElements.assign(model.sets.size(), {});
    model.elementToSets.assign(jSubsets.size(), {});

    for (int setId = 0; setId < (int)model.sets.size(); ++setId) {
        const auto& kg = model.sets[setId];
        std::vector<int> complement;
        complement.reserve(samples.size() - kg.size());
        for (int v : samples) {
            if (!std::binary_search(kg.begin(), kg.end(), v)) {
                complement.push_back(v);
            }
        }

        for (int t = s; t <= std::min(j, k); ++t) {
            int outside = j - t;
            if (outside < 0 || outside > (int)complement.size()) continue;

            auto inParts = SampleSelectSystem::enumerate(kg, t);
            auto outParts = (outside == 0)
                ? std::vector<std::vector<int>>{std::vector<int>{}}
                : SampleSelectSystem::enumerate(complement, outside);

            for (const auto& in : inParts) {
                for (const auto& out : outParts) {
                    std::vector<int> element = in;
                    element.insert(element.end(), out.begin(), out.end());
                    std::sort(element.begin(), element.end());
                    auto it = elementIndex.find(element);
                    if (it != elementIndex.end()) {
                        model.setToElements[setId].push_back(it->second);
                    }
                }
            }
        }

        auto& elems = model.setToElements[setId];
        std::sort(elems.begin(), elems.end());
        elems.erase(std::unique(elems.begin(), elems.end()), elems.end());
        for (int e : elems) model.elementToSets[e].push_back(setId);
    }

    return model;
}

static SolveReport runHighsExact(
    const SampleSelectSystem& sys,
    const std::vector<std::vector<int>>& incumbent,
    int timeLimitSec,
    const std::string& solverBin,
    int lowerBound)
{
    SolveReport report;
    report.algorithm = "HiGHS ILP";
    report.groups = incumbent;
    report.feasible = !incumbent.empty();
    report.upperBound = (int)incumbent.size();
    report.lowerBound = lowerBound;
    report.timeLimitSec = timeLimitSec;

    const long long nkSize = SampleSelectSystem::C(sys.getN(), sys.getK());
    if (nkSize > 10000) {
        report.algorithm = "HiGHS ILP skipped (candidate limit)";
        return report;
    }

    const auto& samples = sys.getSamples();
    const auto kCandidates = SampleSelectSystem::enumerate(samples, sys.getK());
    const auto jSubsets = SampleSelectSystem::enumerate(samples, sys.getJ());

    std::unordered_map<std::vector<int>, int, VecHash> subIndex;
    std::vector<std::vector<int>> globalSubs;
    for (auto& sub : SampleSelectSystem::enumerate(samples, sys.getS())) {
        subIndex[sub] = (int)globalSubs.size();
        globalSubs.push_back(sub);
    }

    std::vector<std::vector<int>> jToSubs(jSubsets.size());
    for (int i = 0; i < (int)jSubsets.size(); ++i) {
        for (auto& sub : SampleSelectSystem::enumerate(jSubsets[i], sys.getS())) {
            jToSubs[i].push_back(subIndex.at(sub));
        }
    }

    std::vector<std::vector<int>> sCoversList(globalSubs.size());
    for (int c = 0; c < (int)kCandidates.size(); ++c) {
        for (auto& sub : SampleSelectSystem::enumerate(kCandidates[c], sys.getS())) {
            auto it = subIndex.find(sub);
            if (it != subIndex.end()) sCoversList[it->second].push_back(c);
        }
    }

    ILPSolver::Problem prob;
    prob.numK = (int)kCandidates.size();
    prob.numSubs = (int)globalSubs.size();
    prob.numJ = (int)jSubsets.size();
    prob.minCover = sys.getMinCover();
    prob.sCoversList = std::move(sCoversList);
    prob.jToSubs = std::move(jToSubs);
    prob.greedyUpperBound = (int)incumbent.size();

    auto sol = ILPSolver::solve(prob, "./ilp_tmp", solverBin, timeLimitSec);
    if (!sol.feasible) {
        report.algorithm = "Portfolio heuristic (HiGHS timeout/no solution)";
        return report;
    }

    report.groups.clear();
    for (int idx : sol.selectedIndices) {
        report.groups.push_back(kCandidates[idx]);
    }
    std::sort(report.groups.begin(), report.groups.end());
    report.algorithm = sol.optimal ? "HiGHS ILP exact" : "HiGHS ILP feasible";
    report.optimal = sol.optimal;
    report.feasible = true;
    report.upperBound = (int)report.groups.size();
    if (sol.optimal) report.lowerBound = report.upperBound;
    return report;
}

static int popcountMask(uint32_t x)
{
    int c = 0;
    while (x) {
        x &= (x - 1);
        ++c;
    }
    return c;
}

static void enumerateMasksRec(
    int n, int r, int start, uint32_t current, std::vector<uint32_t>& out)
{
    if (r == 0) {
        out.push_back(current);
        return;
    }
    for (int i = start; i <= n - r; ++i) {
        enumerateMasksRec(n, r - 1, i + 1, current | (1u << i), out);
    }
}

static std::vector<uint32_t> enumerateMasks(int n, int r)
{
    std::vector<uint32_t> out;
    if (r < 0 || r > n) return out;
    out.reserve((size_t)SampleSelectSystem::C(n, r));
    enumerateMasksRec(n, r, 0, 0u, out);
    return out;
}

static std::vector<uint32_t> submasksOfMask(uint32_t mask, int r)
{
    std::vector<int> bits;
    bits.reserve(popcountMask(mask));
    for (int i = 0; i < 32; ++i) {
        if (mask & (1u << i)) bits.push_back(i);
    }

    std::vector<uint32_t> out;
    if (r < 0 || r > (int)bits.size()) return out;
    out.reserve((size_t)SampleSelectSystem::C((int)bits.size(), r));

    std::function<void(int, int, uint32_t)> rec = [&](int pos, int need, uint32_t cur) {
        if (need == 0) {
            out.push_back(cur);
            return;
        }
        for (int i = pos; i <= (int)bits.size() - need; ++i) {
            rec(i + 1, need - 1, cur | (1u << bits[i]));
        }
    };
    rec(0, r, 0u);
    return out;
}

static std::vector<int> maskToCanonicalGroup(uint32_t mask)
{
    std::vector<int> group;
    for (int i = 0; i < 32; ++i) {
        if (mask & (1u << i)) group.push_back(i + 1);
    }
    return group;
}

static std::vector<int> maskToSampleGroup(
    uint32_t mask,
    const std::vector<int>& samples)
{
    std::vector<int> group;
    for (int i = 0; i < (int)samples.size(); ++i) {
        if (mask & (1u << i)) group.push_back(samples[i]);
    }
    return group;
}

struct ExactCoverResult {
    bool feasible = false;
    bool optimal = false;
    bool timedOut = false;
    int lowerBound = 0;
    int upperBound = 0;
    double proofTimeSec = 0.0;
    double nodesPerSec = 0.0;
    double reductionRatio = 0.0;
    uint64_t nodes = 0;
    uint64_t ttHits = 0;
    std::vector<uint32_t> selectedMasks;
    std::vector<std::vector<int>> groups;
};

struct ExactOptions {
    int threads = 1;
    // Optional shared state for anytime concurrency: a background LNS thread
    // writes improved incumbents here; the exact solver reads them each budget
    // iteration so it immediately benefits from LNS progress.
    std::atomic<int>*               sharedUB     = nullptr;
    std::mutex*                     sharedMutex  = nullptr;
    std::vector<std::vector<int>>*  sharedGroups = nullptr;
};

class StandardSetCoverExactSolver {
public:
    StandardSetCoverExactSolver(
        int n,
        int k,
        int j,
        int s,
        const std::vector<int>& samples,
        const std::vector<std::vector<int>>& incumbentGroups,
        ExactOptions options = {})
        : n_(n),
          k_(k),
          j_(j),
          s_(s),
          samples_(samples),
          options_(options),
          lowerBound_(combinedLowerBound(n, k, j, s, 1))
    {
        buildModel();
        loadIncumbent(incumbentGroups);
    }

    ExactCoverResult solve(int timeLimitSec)
    {
        using Clock = std::chrono::steady_clock;
        auto start = Clock::now();
        deadline_ = start + std::chrono::seconds(std::max(1, timeLimitSec));

        if (incumbentMasks_.empty()) {
            incumbentMasks_ = kMasks_;
        }

        // C improvement: compute dual-ascent LP relaxation LB once.
        // May be tighter than Schönheim for asymmetric instances; never weaker
        // than the simple counting bound.  Also fills dualPrices_ for D.
        {
            int dualLB = dualAscentLowerBound();
            lowerBound_ = std::max(lowerBound_, dualLB);
        }

        auto uncovered = rootUncovered();
        int uncoveredCount = numElements_;
        bool proved = false;

        // failedStates_ is intentionally NOT cleared between budget iterations:
        // a state that was infeasible at budget B is still infeasible at budget B-1,
        // so old nogoods remain valid and speed up tighter searches.
        while (!timedOut_ && (int)incumbentMasks_.size() > lowerBound_) {
            // Pull in a better incumbent from the concurrent LNS thread if available.
            if (options_.sharedUB && options_.sharedMutex && options_.sharedGroups) {
                int extUB = options_.sharedUB->load(std::memory_order_acquire);
                if (extUB < (int)incumbentMasks_.size()) {
                    std::lock_guard<std::mutex> lk(*options_.sharedMutex);
                    if ((int)options_.sharedGroups->size() == extUB) {
                        loadIncumbent(*options_.sharedGroups);
                        std::cout << ">> [Exact] Incumbent pulled from LNS thread: "
                                  << extUB << " groups\n";
                    }
                }
            }
            if ((int)incumbentMasks_.size() <= lowerBound_) break;

            int budget = (int)incumbentMasks_.size() - 1;
            selectedMasks_.clear();
            std::fill(selected_.begin(), selected_.end(), 0);
            // P5.2: reset incremental degrees — all candidates are now unselected
            for (int e = 0; e < numElements_; ++e)
                elemDecisionDegree_[e] = (int)elementToCandidates_[e].size();
            std::cout << ">> [Exact set-cover] checking budget " << budget
                      << " (current UB=" << incumbentMasks_.size()
                      << ", threads=" << std::max(1, options_.threads) << ")\n";
            if (!decisionSearchRoot(uncovered, uncoveredCount, budget)) {
                if (!timedOut_) proved = true;
                break;
            }
        }
        if ((int)incumbentMasks_.size() <= lowerBound_) {
            proved = true;
        }

        ExactCoverResult result;
        result.feasible = !incumbentMasks_.empty();
        result.optimal = proved && !timedOut_;
        result.timedOut = timedOut_;
        result.lowerBound = result.optimal ? (int)incumbentMasks_.size() : lowerBound_;
        result.upperBound = (int)incumbentMasks_.size();
        result.nodes = nodes_;
        result.ttHits = ttHits_;
        result.reductionRatio = reductionRatio_;
        result.selectedMasks = incumbentMasks_;
        std::sort(result.selectedMasks.begin(), result.selectedMasks.end());
        for (uint32_t mask : result.selectedMasks) {
            result.groups.push_back(maskToSampleGroup(mask, samples_));
        }
        std::sort(result.groups.begin(), result.groups.end());
        result.proofTimeSec =
            std::chrono::duration<double>(Clock::now() - start).count();
        result.nodesPerSec = (result.proofTimeSec > 0.0)
            ? (double)result.nodes / result.proofTimeSec
            : 0.0;
        return result;
    }

private:
    int n_;
    int k_;
    int j_;
    int s_;
    const std::vector<int>& samples_;
    ExactOptions options_;
    int numElements_ = 0;
    int words_ = 0;
    int lowerBound_ = 0;
    int rootMaxCover_ = 1;
    int originalCandidateCount_ = 0;
    int reducedCandidateCount_ = 0;
    double reductionRatio_ = 0.0;

    std::vector<uint32_t> kMasks_;
    std::vector<uint32_t> jMasks_;
    std::vector<int> kIndexByMask_;
    std::vector<int> originalMaskToReducedId_;
    std::vector<std::vector<uint64_t>> setCover_;
    std::vector<std::vector<int>> elementToCandidates_;
    std::vector<std::vector<uint64_t>> coCover_;

    std::vector<uint32_t> incumbentMasks_;
    std::vector<uint32_t> selectedMasks_;
    std::vector<unsigned char> selected_;
    std::vector<unsigned char> forbidden_;
    std::unordered_set<uint64_t> failedStates_;
    static constexpr size_t FAILED_STATE_LIMIT = 500000;

    // P5.2 — Incremental degree tracking.
    // candElements_[c]     : list of j-subset elements covered by candidate c (inverse of elementToCandidates_)
    // elemDecisionDegree_[e]: count of non-selected candidates that cover element e.
    //   Maintained by selectCand()/deselectCand() helpers; used in chooseBranchElementDecision
    //   to replace the O(numElements × candidates × words) hot-path with O(numElements).
    std::vector<std::vector<int>> candElements_;
    std::vector<int>              elemDecisionDegree_;

    // C/D improvement — LP dual prices computed once by dualAscentLowerBound().
    // dualPrices_[e] is a feasible dual variable y_e ≥ 0 satisfying
    //   sum_{e in S_c} y_e ≤ 1  for all candidates c.
    // Used by dualPriceLowerBound() at each B&B node in O(numElements).
    std::vector<double> dualPrices_;

    StandardSetCoverExactSolver(const StandardSetCoverExactSolver& other)
        : n_(other.n_),
          k_(other.k_),
          j_(other.j_),
          s_(other.s_),
          samples_(other.samples_),
          options_(other.options_),
          numElements_(other.numElements_),
          words_(other.words_),
          lowerBound_(other.lowerBound_),
          rootMaxCover_(other.rootMaxCover_),
          originalCandidateCount_(other.originalCandidateCount_),
          reducedCandidateCount_(other.reducedCandidateCount_),
          reductionRatio_(other.reductionRatio_),
          kMasks_(other.kMasks_),
          jMasks_(other.jMasks_),
          kIndexByMask_(other.kIndexByMask_),
          originalMaskToReducedId_(other.originalMaskToReducedId_),
          setCover_(other.setCover_),
          elementToCandidates_(other.elementToCandidates_),
          coCover_(other.coCover_),
          incumbentMasks_(other.incumbentMasks_),
          selectedMasks_(),
          selected_(other.selected_),
          forbidden_(other.forbidden_),
          candElements_(other.candElements_),
          elemDecisionDegree_(other.elemDecisionDegree_),
          dualPrices_(other.dualPrices_),
          deadline_(other.deadline_),
          timedOut_(false),
          nodes_(0),
          ttHits_(0)
    {
    }

    std::chrono::steady_clock::time_point deadline_;
    bool timedOut_ = false;
    uint64_t nodes_ = 0;
    uint64_t ttHits_ = 0;

    static bool bitIsSet(const std::vector<uint64_t>& bits, int idx)
    {
        return (bits[idx >> 6] & (1ULL << (idx & 63))) != 0ULL;
    }

    static void setBit(std::vector<uint64_t>& bits, int idx)
    {
        bits[idx >> 6] |= (1ULL << (idx & 63));
    }

    static int popcountAnd(const std::vector<uint64_t>& a,
                           const std::vector<uint64_t>& b)
    {
        int count = 0;
        for (int i = 0; i < (int)a.size(); ++i) {
            count += popcount64(a[i] & b[i]);
        }
        return count;
    }

    static int popcountBits(const std::vector<uint64_t>& bits)
    {
        int count = 0;
        for (uint64_t x : bits) count += popcount64(x);
        return count;
    }

    static int firstSetBit(const std::vector<uint64_t>& bits)
    {
        for (int w = 0; w < (int)bits.size(); ++w) {
            uint64_t x = bits[w];
            if (!x) continue;
#if defined(_MSC_VER)
            unsigned long idx = 0;
            _BitScanForward64(&idx, x);
            return w * 64 + (int)idx;
#else
            return w * 64 + __builtin_ctzll(x);
#endif
        }
        return -1;
    }

    std::vector<uint64_t> rootUncovered() const
    {
        std::vector<uint64_t> uncovered(words_, ~0ULL);
        if (numElements_ % 64 != 0) {
            uncovered.back() = (1ULL << (numElements_ % 64)) - 1ULL;
        }
        return uncovered;
    }

    void buildModel()
    {
        kMasks_ = enumerateMasks(n_, k_);
        jMasks_ = enumerateMasks(n_, j_);
        numElements_ = (int)jMasks_.size();
        words_ = (numElements_ + 63) / 64;
        originalCandidateCount_ = (int)kMasks_.size();

        kIndexByMask_.assign(1 << n_, -1);
        originalMaskToReducedId_.assign(1 << n_, -1);
        for (int i = 0; i < (int)kMasks_.size(); ++i) {
            kIndexByMask_[kMasks_[i]] = i;
        }

        setCover_.assign(kMasks_.size(), std::vector<uint64_t>(words_, 0ULL));
        elementToCandidates_.assign(numElements_, {});
        rootMaxCover_ = 1;

        for (int c = 0; c < (int)kMasks_.size(); ++c) {
            int coverCount = 0;
            for (int e = 0; e < numElements_; ++e) {
                if (popcountMask(kMasks_[c] & jMasks_[e]) >= s_) {
                    setBit(setCover_[c], e);
                    elementToCandidates_[e].push_back(c);
                    ++coverCount;
                }
            }
            rootMaxCover_ = std::max(rootMaxCover_, coverCount);
        }

        reduceCandidates();
        buildPackingMasksIfCheap();
    }

    void rebuildIndexes()
    {
        std::fill(kIndexByMask_.begin(), kIndexByMask_.end(), -1);
        elementToCandidates_.assign(numElements_, {});
        rootMaxCover_ = 1;

        for (int c = 0; c < (int)kMasks_.size(); ++c) {
            kIndexByMask_[kMasks_[c]] = c;
            int coverCount = 0;
            for (int e = 0; e < numElements_; ++e) {
                if (bitIsSet(setCover_[c], e)) {
                    elementToCandidates_[e].push_back(c);
                    ++coverCount;
                }
            }
            rootMaxCover_ = std::max(rootMaxCover_, coverCount);
        }

        // Build candElements_: inverse of elementToCandidates_ (candidate -> element list)
        candElements_.assign(kMasks_.size(), {});
        for (int e = 0; e < numElements_; ++e)
            for (int c : elementToCandidates_[e])
                candElements_[c].push_back(e);

        // Initialize elemDecisionDegree_: number of non-selected candidates covering each element.
        // At construction / after rebuild, all candidates are unselected.
        elemDecisionDegree_.assign(numElements_, 0);
        for (int e = 0; e < numElements_; ++e)
            elemDecisionDegree_[e] = (int)elementToCandidates_[e].size();
    }

    void reduceCandidates()
    {
        const int m = (int)kMasks_.size();
        std::vector<unsigned char> removed(m, 0);

        std::unordered_map<std::string, int> seen;
        seen.reserve(m * 2);
        for (int i = 0; i < m; ++i) {
            std::string key = bitsetKey(setCover_[i]);
            auto [it, inserted] = seen.emplace(std::move(key), i);
            if (!inserted) removed[i] = 1;
        }

        // Precompute popcount per candidate so domination inner loop can skip
        // pairs that cannot possibly dominate each other (if popcount(a) > popcount(b),
        // a's coverage is strictly larger so a cannot be a subset of b).
        std::vector<int> pc(m);
        for (int i = 0; i < m; ++i) pc[i] = popcountBits(setCover_[i]);

        // Raised threshold from 1.8e8 → 8e8: now covers n=16 (8008² × 30 ≈ 1.9e9 raw
        // but with the popcount early-skip the effective work is ~10× less).
        const long long dominationWork = (long long)m * (long long)m * (long long)words_;
        if (dominationWork <= 800000000LL) {
            for (int a = 0; a < m; ++a) {
                if (removed[a]) continue;
                for (int b = a + 1; b < m; ++b) {
                    if (removed[b]) continue;
                    int ca = pc[a], cb = pc[b];
                    // Popcount early-skip: if ca > cb then setCover_[a] ⊄ setCover_[b].
                    if (ca <= cb && bitsetSubsetOf(setCover_[a], setCover_[b])) {
                        removed[a] = 1;
                        break;
                    }
                    if (cb < ca && bitsetSubsetOf(setCover_[b], setCover_[a])) {
                        removed[b] = 1;
                    }
                }
            }
        } else {
            // For very large instances (n≥17), use a bucket-based heuristic domination:
            // only compare pairs that share the same first set-bit (likely to dominate).
            std::unordered_map<int, std::vector<int>> byFirstBit;
            for (int i = 0; i < m; ++i) {
                if (removed[i]) continue;
                int fb = firstSetBit(setCover_[i]);
                if (fb >= 0) byFirstBit[fb].push_back(i);
            }
            for (auto& [fb, group] : byFirstBit) {
                // Sort by popcount ascending so smaller sets are checked as dominatees first.
                std::sort(group.begin(), group.end(), [&](int a, int b){ return pc[a] < pc[b]; });
                for (int ai = 0; ai < (int)group.size(); ++ai) {
                    int a = group[ai];
                    if (removed[a]) continue;
                    for (int bi = ai + 1; bi < (int)group.size(); ++bi) {
                        int b = group[bi];
                        if (removed[b]) continue;
                        if (pc[a] <= pc[b] && bitsetSubsetOf(setCover_[a], setCover_[b])) {
                            removed[a] = 1; break;
                        }
                    }
                }
            }
        }

        std::vector<uint32_t> oldMasks = kMasks_;
        std::vector<std::vector<uint64_t>> oldCover = setCover_;
        kMasks_.clear();
        setCover_.clear();
        std::vector<int> oldToReduced(m, -1);

        for (int i = 0; i < m; ++i) {
            if (removed[i]) continue;
            oldToReduced[i] = (int)kMasks_.size();
            kMasks_.push_back(oldMasks[i]);
            setCover_.push_back(oldCover[i]);
        }

        for (int i = 0; i < m; ++i) {
            if (oldToReduced[i] >= 0) continue;
            for (int j = 0; j < (int)kMasks_.size(); ++j) {
                if (bitsetSubsetOf(oldCover[i], setCover_[j])) {
                    oldToReduced[i] = j;
                    break;
                }
            }
        }

        std::fill(originalMaskToReducedId_.begin(), originalMaskToReducedId_.end(), -1);
        for (int i = 0; i < m; ++i) {
            int reduced = oldToReduced[i];
            if (reduced >= 0) originalMaskToReducedId_[oldMasks[i]] = reduced;
        }

        reducedCandidateCount_ = (int)kMasks_.size();
        reductionRatio_ = (originalCandidateCount_ > 0)
            ? 1.0 - (double)reducedCandidateCount_ / (double)originalCandidateCount_
            : 0.0;
        rebuildIndexes();

        std::cout << ">> [Exact set-cover] candidate reduction: "
                  << originalCandidateCount_ << " -> "
                  << reducedCandidateCount_
                  << " (" << std::fixed << std::setprecision(1)
                  << reductionRatio_ * 100.0 << "% removed)\n";
    }

    void buildPackingMasksIfCheap()
    {
        long long pairEstimate = 0;
        for (const auto& bits : setCover_) pairEstimate += popcountBits(bits);
        if (pairEstimate * (long long)words_ > 90000000LL) return;

        coCover_.assign(numElements_, std::vector<uint64_t>(words_, 0ULL));
        for (int c = 0; c < (int)setCover_.size(); ++c) {
            for (int e = 0; e < numElements_; ++e) {
                if (!bitIsSet(setCover_[c], e)) continue;
                for (int w = 0; w < words_; ++w) {
                    coCover_[e][w] |= setCover_[c][w];
                }
            }
        }
    }

    // ── C improvement: LP dual lower bound via iterative dual ascent ──────────
    // Constructs a feasible dual solution y_e ≥ 0 satisfying
    //   sum_{e in S_c} y_e ≤ 1  for all candidates c.
    // By LP duality, ceil(sum y_e) is a valid lower bound for the IP optimum.
    //
    // Start from the old safe pricing y_e = 1/maxCov(e), then repeatedly visit
    // elements from rarest to most common and raise y_e by the minimum slack over
    // all candidates covering e. Candidate loads are maintained incrementally, so
    // each pass is O(total incidence).
    //
    // Fills dualPrices_[e] for reuse by dualPriceLowerBound() at each B&B node.
    int dualAscentLowerBound()
    {
        dualPrices_.assign(numElements_, 0.0);
        double total = 0.0;
        for (int e = 0; e < numElements_; ++e) {
            int maxCov = 0;
            for (int c : elementToCandidates_[e]) {
                int cov = (int)candElements_[c].size();
                if (cov > maxCov) maxCov = cov;
            }
            dualPrices_[e] = (maxCov > 0) ? 1.0 / (double)maxCov : 0.0;
            total += dualPrices_[e];
        }

        std::vector<double> candidateLoad(candElements_.size(), 0.0);
        for (int c = 0; c < (int)candElements_.size(); ++c) {
            double load = 0.0;
            for (int e : candElements_[c]) load += dualPrices_[e];
            candidateLoad[c] = load;
        }

        std::vector<int> order(numElements_);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            int da = (int)elementToCandidates_[a].size();
            int db = (int)elementToCandidates_[b].size();
            if (da != db) return da < db;
            return a < b;
        });

        constexpr int MAX_DUAL_ASCENT_ROUNDS = 10;
        int roundsUsed = 0;
        for (int round = 0; round < MAX_DUAL_ASCENT_ROUNDS; ++round) {
            double maxDelta = 0.0;
            for (int e : order) {
                double delta = std::numeric_limits<double>::infinity();
                for (int c : elementToCandidates_[e]) {
                    delta = std::min(delta, 1.0 - candidateLoad[c]);
                }
                if (!std::isfinite(delta) || delta <= 1e-10) continue;
                dualPrices_[e] += delta;
                total += delta;
                maxDelta = std::max(maxDelta, delta);
                for (int c : elementToCandidates_[e]) {
                    candidateLoad[c] += delta;
                }
            }
            roundsUsed = round + 1;
            if (maxDelta <= 1e-8) break;
        }

        int lb = (int)std::ceil(total - 1e-9);
        std::cout << ">> [Dual LB] iterative sum_y = " << std::fixed
                  << std::setprecision(3) << total
                  << "  ->  LB = " << lb
                  << " (rounds=" << roundsUsed << ")\n";
        return lb;
    }

    // ── D improvement: per-node lower bound using pre-computed dual prices ────
    // After fixing some candidates, the remaining uncovered elements still need
    // sum_{e uncovered} y_e units of dual coverage.  Since each additional
    // candidate contributes at most 1 unit (dual constraint), we need at least
    // ceil(sum_{e uncovered} y_e) more candidates — a valid residual LP bound.
    // Runtime O(numElements) — much cheaper than dynamicGainLowerBound.
    int dualPriceLowerBound(const std::vector<uint64_t>& uncovered) const
    {
        if (dualPrices_.empty()) return 0;
        double residual = 0.0;
        for (int e = 0; e < numElements_; ++e) {
            if (bitIsSet(uncovered, e)) residual += dualPrices_[e];
        }
        return (int)std::ceil(residual - 1e-9);
    }

    void loadIncumbent(const std::vector<std::vector<int>>& incumbentGroups)
    {
        std::unordered_map<int, int> samplePos;
        samplePos.reserve(samples_.size() * 2);
        for (int i = 0; i < (int)samples_.size(); ++i) samplePos[samples_[i]] = i;

        std::set<uint32_t> masks;
        for (const auto& group : incumbentGroups) {
            if ((int)group.size() != k_) continue;
            uint32_t mask = 0u;
            bool ok = true;
            for (int v : group) {
                auto it = samplePos.find(v);
                if (it == samplePos.end()) {
                    ok = false;
                    break;
                }
                mask |= (1u << it->second);
            }
            if (ok && popcountMask(mask) == k_) masks.insert(mask);
        }
        std::set<uint32_t> reducedMasks;
        for (uint32_t mask : masks) {
            int reducedId = (mask < originalMaskToReducedId_.size())
                ? originalMaskToReducedId_[mask]
                : -1;
            if (reducedId >= 0) reducedMasks.insert(kMasks_[reducedId]);
        }
        incumbentMasks_.assign(reducedMasks.begin(), reducedMasks.end());
        selected_.assign(kMasks_.size(), 0);
        forbidden_.assign(kMasks_.size(), 0);
    }

    int packingLowerBound(const std::vector<uint64_t>& uncovered) const
    {
        if (coCover_.empty()) return 0;
        std::vector<uint64_t> remaining = uncovered;
        int lb = 0;
        while (true) {
            int e = firstSetBit(remaining);
            if (e < 0 || e >= numElements_) break;
            ++lb;
            for (int w = 0; w < words_; ++w) {
                remaining[w] &= ~coCover_[e][w];
            }
        }
        return lb;
    }

    int dynamicGainLowerBound(const std::vector<uint64_t>& uncovered,
                              int uncoveredCount) const
    {
        std::vector<int> gains;
        gains.reserve(kMasks_.size());
        for (int c = 0; c < (int)kMasks_.size(); ++c) {
            if (selected_[c] || forbidden_[c]) continue;
            int gain = popcountAnd(uncovered, setCover_[c]);
            if (gain > 0) gains.push_back(gain);
        }
        if (gains.empty()) return 1000000000;

        std::sort(gains.begin(), gains.end(), std::greater<int>());
        int covered = 0;
        for (int i = 0; i < (int)gains.size(); ++i) {
            covered += gains[i];
            if (covered >= uncoveredCount) return i + 1;
        }
        return 1000000000;
    }

    int stateLowerBound(const std::vector<uint64_t>& uncovered,
                        int uncoveredCount) const
    {
        if (uncoveredCount <= 0) return 0;
        int lb = std::max(1, (uncoveredCount + rootMaxCover_ - 1) / rootMaxCover_);
        lb = std::max(lb, packingLowerBound(uncovered));
        // D improvement: dual-price bound runs in O(numElements) and is checked
        // before the expensive dynamicGainLowerBound — if it already exceeds the
        // budget, the caller prunes without paying the O(C log C) sort cost.
        lb = std::max(lb, dualPriceLowerBound(uncovered));
        lb = std::max(lb, dynamicGainLowerBound(uncovered, uncoveredCount));
        return lb;
    }

    // Incremental degree maintenance: call when selected_[c] flips 0→1 or 1→0.
    // Decrements/increments elemDecisionDegree_ for every element covered by c.
    void selectCand(int c)
    {
        for (int e : candElements_[c]) --elemDecisionDegree_[e];
    }
    void deselectCand(int c)
    {
        for (int e : candElements_[c]) ++elemDecisionDegree_[e];
    }

    int chooseBranchElement(const std::vector<uint64_t>& uncovered) const
    {
        int best = -1;
        int bestDegree = std::numeric_limits<int>::max();
        for (int e = 0; e < numElements_; ++e) {
            if (!bitIsSet(uncovered, e)) continue;
            int degree = 0;
            for (int c : elementToCandidates_[e]) {
                if (!selected_[c] && !forbidden_[c]) ++degree;
            }
            if (degree == 0) return -2;
            if (degree < bestDegree) {
                best = e;
                bestDegree = degree;
            }
        }
        return best;
    }

    // O(numElements) MFC element selection using precomputed elemDecisionDegree_.
    // elemDecisionDegree_[e] is kept up-to-date by selectCand()/deselectCand().
    // A degree of 0 means no unselected candidate covers this still-uncovered element → prune.
    int chooseBranchElementDecision(const std::vector<uint64_t>& uncovered) const
    {
        int best = -1;
        int bestDegree = std::numeric_limits<int>::max();
        for (int e = 0; e < numElements_; ++e) {
            if (!bitIsSet(uncovered, e)) continue;
            int degree = elemDecisionDegree_[e];
            if (degree == 0) return -2;   // infeasible: no candidate can cover e
            if (degree < bestDegree) {
                best = e;
                bestDegree = degree;
            }
        }
        return best;
    }

    // FNV-1a hash over the uncovered bitset — budget is intentionally excluded.
    // A state infeasible at budget B remains infeasible at any budget B' < B,
    // so budget-independent keys let nogoods persist across iterative-deepening rounds.
    static uint64_t uncoveredHash(const std::vector<uint64_t>& uncovered)
    {
        static constexpr uint64_t FNV_PRIME  = 1099511628211ULL;
        static constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
        uint64_t h = FNV_OFFSET;
        for (uint64_t w : uncovered) {
            h ^= w;
            h *= FNV_PRIME;
        }
        return h;
    }

    void rememberFailed(const std::vector<uint64_t>& uncovered)
    {
        if (failedStates_.size() >= FAILED_STATE_LIMIT) return;
        failedStates_.insert(uncoveredHash(uncovered));
    }

    bool isKnownFailed(const std::vector<uint64_t>& uncovered) const
    {
        if (failedStates_.empty()) return false;
        bool hit = failedStates_.count(uncoveredHash(uncovered)) > 0;
        if (hit) ++const_cast<StandardSetCoverExactSolver*>(this)->ttHits_;
        return hit;
    }

    bool checkTimeout()
    {
        if ((nodes_ & 4095ULL) != 0ULL) return false;
        if (std::chrono::steady_clock::now() <= deadline_) return false;
        timedOut_ = true;
        return true;
    }

    bool applyForcedMoves(
        std::vector<uint64_t>& uncovered,
        int& uncoveredCount,
        int& budget,
        std::vector<int>& applied)
    {
        bool changed = true;
        while (changed) {
            changed = false;
            int forced = -1;

            for (int e = 0; e < numElements_; ++e) {
                if (!bitIsSet(uncovered, e)) continue;
                int available = 0;
                int last = -1;
                for (int c : elementToCandidates_[e]) {
                    if (selected_[c]) continue;
                    int gain = popcountAnd(uncovered, setCover_[c]);
                    if (gain <= 0) continue;
                    ++available;
                    last = c;
                    if (available > 1) break;
                }
                if (available == 0) return false;
                if (available == 1) {
                    forced = last;
                    break;
                }
            }

            if (forced >= 0) {
                if (budget <= 0 || selected_[forced]) return false;
                int gain = popcountAnd(uncovered, setCover_[forced]);
                if (gain <= 0) return false;
                for (int w = 0; w < words_; ++w) uncovered[w] &= ~setCover_[forced][w];
                uncoveredCount -= gain;
                --budget;
                selected_[forced] = 1;
                selectCand(forced);           // P5.2: update incremental degrees
                selectedMasks_.push_back(kMasks_[forced]);
                applied.push_back(forced);
                changed = true;
                if (uncoveredCount == 0) return true;
            }
        }
        return true;
    }

    void undoMoves(const std::vector<int>& applied)
    {
        for (int i = (int)applied.size() - 1; i >= 0; --i) {
            selected_[applied[i]] = 0;
            deselectCand(applied[i]);         // P5.2: restore incremental degrees
            selectedMasks_.pop_back();
        }
    }

    bool decisionSearch(
        std::vector<uint64_t> uncovered,
        int uncoveredCount,
        int budget)
    {
        if (timedOut_) return false;
        ++nodes_;
        if (checkTimeout()) return false;

        if (uncoveredCount == 0) {
            incumbentMasks_ = selectedMasks_;
            std::cout << ">> [Exact set-cover] incumbent improved: "
                      << incumbentMasks_.size() << " groups"
                      << " (nodes=" << nodes_ << ")\n";
            return true;
        }
        if (budget <= 0) return false;
        if (stateLowerBound(uncovered, uncoveredCount) > budget) return false;

        std::vector<int> forced;
        if (!applyForcedMoves(uncovered, uncoveredCount, budget, forced)) {
            undoMoves(forced);
            return false;
        }
        if (uncoveredCount == 0) {
            incumbentMasks_ = selectedMasks_;
            std::cout << ">> [Exact set-cover] incumbent improved: "
                      << incumbentMasks_.size() << " groups"
                      << " (nodes=" << nodes_ << ")\n";
            undoMoves(forced);
            return true;
        }
        if (budget <= 0 || stateLowerBound(uncovered, uncoveredCount) > budget) {
            undoMoves(forced);
            return false;
        }

        if (isKnownFailed(uncovered)) {
            undoMoves(forced);
            return false;
        }

        int branchElement = chooseBranchElementDecision(uncovered);
        if (branchElement < 0) {
            rememberFailed(uncovered);
            undoMoves(forced);
            return false;
        }

        std::vector<std::pair<int, int>> candidates;
        candidates.reserve(elementToCandidates_[branchElement].size());
        for (int c : elementToCandidates_[branchElement]) {
            if (selected_[c]) continue;
            int gain = popcountAnd(uncovered, setCover_[c]);
            if (gain > 0) candidates.push_back({gain, c});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) {
                      if (a.first != b.first) return a.first > b.first;
                      return a.second < b.second;
                  });

        for (auto [gain, c] : candidates) {
            std::vector<uint64_t> next = uncovered;
            for (int w = 0; w < words_; ++w) next[w] &= ~setCover_[c][w];
            selected_[c] = 1;
            selectCand(c);                // P5.2: update incremental degrees
            selectedMasks_.push_back(kMasks_[c]);
            if (decisionSearch(std::move(next), uncoveredCount - gain, budget - 1)) {
                selectedMasks_.pop_back();
                selected_[c] = 0;
                deselectCand(c);          // P5.2: restore incremental degrees
                undoMoves(forced);
                return true;
            }
            selectedMasks_.pop_back();
            selected_[c] = 0;
            deselectCand(c);              // P5.2: restore incremental degrees
            if (timedOut_) {
                undoMoves(forced);
                return false;
            }
        }

        rememberFailed(uncovered);
        undoMoves(forced);
        return false;
    }

    bool decisionSearchRoot(
        const std::vector<uint64_t>& rootUncovered,
        int rootUncoveredCount,
        int budget)
    {
        int threadCount = std::max(1, options_.threads);
        if (threadCount <= 1 || budget <= 1) {
            return decisionSearch(rootUncovered, rootUncoveredCount, budget);
        }

        std::vector<uint64_t> uncovered = rootUncovered;
        int uncoveredCount = rootUncoveredCount;
        int localBudget = budget;
        std::vector<int> forced;
        if (!applyForcedMoves(uncovered, uncoveredCount, localBudget, forced)) {
            undoMoves(forced);
            return false;
        }
        if (uncoveredCount == 0) {
            incumbentMasks_ = selectedMasks_;
            undoMoves(forced);
            return true;
        }
        if (localBudget <= 0 || stateLowerBound(uncovered, uncoveredCount) > localBudget) {
            undoMoves(forced);
            return false;
        }

        int branchElement = chooseBranchElementDecision(uncovered);
        if (branchElement < 0) {
            undoMoves(forced);
            return false;
        }

        std::vector<std::pair<int, int>> candidates;
        candidates.reserve(elementToCandidates_[branchElement].size());
        for (int c : elementToCandidates_[branchElement]) {
            if (selected_[c]) continue;
            int gain = popcountAnd(uncovered, setCover_[c]);
            if (gain > 0) candidates.push_back({gain, c});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) {
                      if (a.first != b.first) return a.first > b.first;
                      return a.second < b.second;
                  });

        if ((int)candidates.size() < 2) {
            bool found = decisionSearch(uncovered, uncoveredCount, localBudget);
            undoMoves(forced);
            return found;
        }

        std::vector<uint32_t> prefix = selectedMasks_;
        // Seed workers with the parent's current TT so they start with inherited nogoods.
        std::unordered_set<uint64_t> seedTT = failedStates_;

        struct BranchResult : ExactCoverResult {
            std::unordered_set<uint64_t> workerTT;
        };

        std::vector<std::future<BranchResult>> futures;
        size_t next = 0;

        auto launchBranch = [&](std::pair<int, int> item) {
            // IMPORTANT: capture seedTT by VALUE to avoid a data race.
            // The main thread updates seedTT after each future.get(), while other
            // workers may still be running — a reference capture would be UB.
            std::unordered_set<uint64_t> ttSnapshot = seedTT;
            return std::async(std::launch::async, [this, uncovered, uncoveredCount,
                                                   localBudget, prefix, item,
                                                   ttSnapshot = std::move(ttSnapshot)]() mutable {
                auto [gain, c] = item;
                StandardSetCoverExactSolver worker(*this);
                worker.options_.threads = 1;
                // Give worker the parent's TT snapshot so it inherits existing nogoods.
                worker.failedStates_ = std::move(ttSnapshot);
                worker.selectedMasks_ = prefix;
                std::fill(worker.selected_.begin(), worker.selected_.end(), 0);
                for (uint32_t mask : prefix) {
                    int idx = worker.kIndexByMask_[mask];
                    if (idx >= 0) worker.selected_[idx] = 1;
                }
                worker.selected_[c] = 1;

                // P5.2: rebuild elemDecisionDegree_ from scratch to match the worker's
                // selected_ state (set directly above without going through selectCand).
                for (int e = 0; e < worker.numElements_; ++e) {
                    int deg = 0;
                    for (int cand : worker.elementToCandidates_[e])
                        if (!worker.selected_[cand]) ++deg;
                    worker.elemDecisionDegree_[e] = deg;
                }

                std::vector<uint64_t> nextUncovered = uncovered;
                for (int w = 0; w < worker.words_; ++w) {
                    nextUncovered[w] &= ~worker.setCover_[c][w];
                }
                worker.selectedMasks_.push_back(worker.kMasks_[c]);

                BranchResult result;
                bool found = worker.decisionSearch(
                    std::move(nextUncovered), uncoveredCount - gain, localBudget - 1);
                result.feasible = found;
                result.timedOut = worker.timedOut_;
                result.nodes = worker.nodes_;
                result.ttHits = worker.ttHits_;
                if (found) {
                    result.selectedMasks = worker.incumbentMasks_;
                    result.upperBound = (int)result.selectedMasks.size();
                }
                // Return worker's TT so parent can absorb new nogoods.
                result.workerTT = std::move(worker.failedStates_);
                return result;
            });
        };

        while (next < candidates.size() || !futures.empty()) {
            while (next < candidates.size() && (int)futures.size() < threadCount) {
                futures.push_back(launchBranch(candidates[next++]));
            }

            auto result = futures.front().get();
            futures.erase(futures.begin());
            nodes_ += result.nodes;
            ttHits_ += result.ttHits;
            if (result.timedOut) timedOut_ = true;

            // Merge worker's TT back into parent and seedTT so subsequent workers
            // benefit from nogoods discovered by completed siblings.
            if (failedStates_.size() < FAILED_STATE_LIMIT) {
                for (uint64_t h : result.workerTT) {
                    if (failedStates_.size() >= FAILED_STATE_LIMIT) break;
                    failedStates_.insert(h);
                }
                seedTT = failedStates_;  // update seed for next worker
            }

            if (result.feasible) {
                incumbentMasks_ = result.selectedMasks;
                std::cout << ">> [Exact set-cover] incumbent improved: "
                          << incumbentMasks_.size() << " groups"
                          << " (parallel root)\n";
                undoMoves(forced);
                return true;
            }
            if (timedOut_) {
                undoMoves(forced);
                return false;
            }
        }

        undoMoves(forced);
        return false;
    }

    void dfs(const std::vector<uint64_t>& uncovered, int uncoveredCount)
    {
        if (timedOut_) return;
        ++nodes_;
        if (checkTimeout()) return;

        if (uncoveredCount == 0) {
            if (selectedMasks_.size() < incumbentMasks_.size()) {
                incumbentMasks_ = selectedMasks_;
                std::cout << ">> [Exact set-cover] incumbent improved: "
                          << incumbentMasks_.size() << " groups"
                          << " (nodes=" << nodes_ << ")\n";
            }
            return;
        }

        if (selectedMasks_.size() + (size_t)stateLowerBound(uncovered, uncoveredCount)
                >= incumbentMasks_.size()) {
            return;
        }

        int branchElement = chooseBranchElement(uncovered);
        if (branchElement < 0) return;

        std::vector<std::pair<int, int>> candidates; // gain, candidate id
        candidates.reserve(elementToCandidates_[branchElement].size());
        for (int c : elementToCandidates_[branchElement]) {
            if (selected_[c] || forbidden_[c]) continue;
            int gain = popcountAnd(uncovered, setCover_[c]);
            if (gain > 0) candidates.push_back({gain, c});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const auto& a, const auto& b) {
                      if (a.first != b.first) return a.first > b.first;
                      return a.second < b.second;
                  });

        std::vector<int> forbiddenHere;
        forbiddenHere.reserve(candidates.size());
        for (auto [gain, c] : candidates) {
            if (timedOut_) return;
            if (!forbidden_[c]) {
                if (selectedMasks_.size() + 1 < incumbentMasks_.size()) {
                    std::vector<uint64_t> next = uncovered;
                    for (int w = 0; w < words_; ++w) next[w] &= ~setCover_[c][w];
                    int nextCount = uncoveredCount - gain;

                    selected_[c] = 1;
                    selectedMasks_.push_back(kMasks_[c]);
                    dfs(next, nextCount);
                    selectedMasks_.pop_back();
                    selected_[c] = 0;
                }

                // Subsequent siblings represent the case where this candidate is
                // excluded, so each feasible solution is assigned to exactly one
                // branch for the selected uncovered element.
                forbidden_[c] = 1;
                forbiddenHere.push_back(c);
            }
        }
        for (int c : forbiddenHere) forbidden_[c] = 0;
    }
};

class CoverDesignExactSolver {
public:
    CoverDesignExactSolver(
        int n,
        int k,
        int j,
        int s,
        int minCover,
        const std::vector<int>& samples,
        const std::vector<std::vector<int>>& incumbentGroups)
        : n_(n),
          k_(k),
          j_(j),
          s_(s),
          minCover_(minCover),
          samples_(samples),
          lowerBound_(countingLowerBound(n, k, j, s, minCover))
    {
        buildModel();
        loadIncumbent(incumbentGroups);
        initializeSearchState();
    }

    ExactCoverResult solve(int timeLimitSec)
    {
        using Clock = std::chrono::steady_clock;
        auto start = Clock::now();
        deadline_ = start + std::chrono::seconds(std::max(1, timeLimitSec));

        ExactCoverResult result;
        result.lowerBound = lowerBound_;

        if (incumbentIds_.empty()) {
            // Selecting every candidate is always feasible, but should only be a
            // last-resort bound. Normal callers pass a portfolio incumbent first.
            incumbentIds_.resize(kMasks_.size());
            std::iota(incumbentIds_.begin(), incumbentIds_.end(), 0);
        }

        if ((int)incumbentIds_.size() <= lowerBound_) {
            result.optimal = true;
        } else {
            dfs();
            result.timedOut = timedOut_;
            result.optimal = !timedOut_;
        }

        result.feasible = !incumbentIds_.empty();
        result.upperBound = (int)incumbentIds_.size();
        if (result.optimal) result.lowerBound = result.upperBound;
        result.nodes = nodes_;
        result.reductionRatio = reductionRatio_;

        for (int id : incumbentIds_) {
            result.selectedMasks.push_back(kMasks_[id]);
            result.groups.push_back(maskToSampleGroup(kMasks_[id], samples_));
        }
        std::sort(result.selectedMasks.begin(), result.selectedMasks.end());
        std::sort(result.groups.begin(), result.groups.end());

        result.proofTimeSec =
            std::chrono::duration<double>(Clock::now() - start).count();
        result.nodesPerSec = (result.proofTimeSec > 0.0)
            ? (double)result.nodes / result.proofTimeSec
            : 0.0;
        return result;
    }

private:
    int n_;
    int k_;
    int j_;
    int s_;
    int minCover_;
    const std::vector<int>& samples_;

    std::vector<uint32_t> kMasks_;
    std::vector<uint32_t> jMasks_;
    std::vector<uint32_t> sMasks_;
    std::vector<int> kIndexByMask_;
    std::vector<int> originalMaskToReducedId_;
    std::vector<int> sIndexByMask_;
    std::vector<std::vector<int>> kToS_;
    std::vector<std::vector<int>> sToK_;
    std::vector<std::vector<int>> jToS_;
    std::vector<std::vector<int>> sToJ_;

    int lowerBound_ = 0;
    int rootMaxGain_ = 1;
    int originalCandidateCount_ = 0;
    int reducedCandidateCount_ = 0;
    double reductionRatio_ = 0.0;
    std::vector<int> incumbentIds_;

    std::vector<int> coverRef_;
    std::vector<int> jCount_;
    std::vector<int> sDeficitWeight_;
    std::vector<unsigned char> selected_;
    std::vector<int> selectedIds_;
    std::vector<int> stamp_;
    int stampToken_ = 1;
    int totalDeficit_ = 0;
    int unsatisfiedJ_ = 0;

    std::chrono::steady_clock::time_point deadline_;
    bool timedOut_ = false;
    uint64_t nodes_ = 0;

    void buildModel()
    {
        const int maskSpace = 1 << n_;
        kIndexByMask_.assign(maskSpace, -1);
        originalMaskToReducedId_.assign(maskSpace, -1);
        sIndexByMask_.assign(maskSpace, -1);

        kMasks_ = enumerateMasks(n_, k_);
        jMasks_ = enumerateMasks(n_, j_);
        sMasks_ = enumerateMasks(n_, s_);
        originalCandidateCount_ = (int)kMasks_.size();

        for (int i = 0; i < (int)kMasks_.size(); ++i) kIndexByMask_[kMasks_[i]] = i;
        for (int i = 0; i < (int)sMasks_.size(); ++i) sIndexByMask_[sMasks_[i]] = i;

        kToS_.assign(kMasks_.size(), {});
        sToK_.assign(sMasks_.size(), {});
        for (int c = 0; c < (int)kMasks_.size(); ++c) {
            for (uint32_t sm : submasksOfMask(kMasks_[c], s_)) {
                int si = sIndexByMask_[sm];
                if (si >= 0) {
                    kToS_[c].push_back(si);
                    sToK_[si].push_back(c);
                }
            }
            std::sort(kToS_[c].begin(), kToS_[c].end());
        }

        jToS_.assign(jMasks_.size(), {});
        sToJ_.assign(sMasks_.size(), {});
        for (int ji = 0; ji < (int)jMasks_.size(); ++ji) {
            for (uint32_t sm : submasksOfMask(jMasks_[ji], s_)) {
                int si = sIndexByMask_[sm];
                if (si >= 0) {
                    jToS_[ji].push_back(si);
                    sToJ_[si].push_back(ji);
                }
            }
        }

        reduceCandidates();
        rootMaxGain_ = estimateRootMaxGain();
        if (rootMaxGain_ <= 0) rootMaxGain_ = 1;
    }

    void rebuildKIndexes()
    {
        std::fill(kIndexByMask_.begin(), kIndexByMask_.end(), -1);
        sToK_.assign(sMasks_.size(), {});
        for (int c = 0; c < (int)kMasks_.size(); ++c) {
            kIndexByMask_[kMasks_[c]] = c;
            for (int si : kToS_[c]) sToK_[si].push_back(c);
        }
    }

    void reduceCandidates()
    {
        const int m = (int)kMasks_.size();
        std::vector<unsigned char> removed(m, 0);

        std::unordered_map<std::string, int> seen;
        seen.reserve(m * 2);
        for (int i = 0; i < m; ++i) {
            std::ostringstream key;
            for (int si : kToS_[i]) key << si << ',';
            auto [it, inserted] = seen.emplace(key.str(), i);
            if (!inserted) removed[i] = 1;
        }

        const long long dominationWork = (long long)m * (long long)m
            * (long long)std::max(1, (int)SampleSelectSystem::C(k_, s_));
        if (dominationWork <= 180000000LL) {
            for (int a = 0; a < m; ++a) {
                if (removed[a]) continue;
                for (int b = a + 1; b < m; ++b) {
                    if (removed[b]) continue;
                    if (kToS_[a].size() <= kToS_[b].size()
                            && sortedSubsetOf(kToS_[a], kToS_[b])) {
                        removed[a] = 1;
                        break;
                    }
                    if (kToS_[b].size() <= kToS_[a].size()
                            && sortedSubsetOf(kToS_[b], kToS_[a])) {
                        removed[b] = 1;
                    }
                }
            }
        }

        std::vector<uint32_t> oldMasks = kMasks_;
        std::vector<std::vector<int>> oldKToS = kToS_;
        kMasks_.clear();
        kToS_.clear();
        std::vector<int> oldToReduced(m, -1);

        for (int i = 0; i < m; ++i) {
            if (removed[i]) continue;
            oldToReduced[i] = (int)kMasks_.size();
            kMasks_.push_back(oldMasks[i]);
            kToS_.push_back(oldKToS[i]);
        }

        for (int i = 0; i < m; ++i) {
            if (oldToReduced[i] >= 0) continue;
            for (int j = 0; j < (int)kToS_.size(); ++j) {
                if (sortedSubsetOf(oldKToS[i], kToS_[j])) {
                    oldToReduced[i] = j;
                    break;
                }
            }
        }

        std::fill(originalMaskToReducedId_.begin(), originalMaskToReducedId_.end(), -1);
        for (int i = 0; i < m; ++i) {
            int reduced = oldToReduced[i];
            if (reduced >= 0) originalMaskToReducedId_[oldMasks[i]] = reduced;
        }

        reducedCandidateCount_ = (int)kMasks_.size();
        reductionRatio_ = (originalCandidateCount_ > 0)
            ? 1.0 - (double)reducedCandidateCount_ / (double)originalCandidateCount_
            : 0.0;
        rebuildKIndexes();

        std::cout << ">> [Exact generalized] candidate reduction: "
                  << originalCandidateCount_ << " -> "
                  << reducedCandidateCount_
                  << " (" << std::fixed << std::setprecision(1)
                  << reductionRatio_ * 100.0 << "% removed)\n";
    }

    int estimateRootMaxGain() const
    {
        long long transitionEstimate =
            (long long)kMasks_.size()
            * SampleSelectSystem::C(k_, s_)
            * SampleSelectSystem::C(n_ - s_, j_ - s_);

        // Exact root gain is useful for pruning, but skip the expensive pass on
        // the densest cases and use a safe combinatorial upper bound instead.
        if (transitionEstimate > 60000000LL) {
            long long ub = SampleSelectSystem::C(k_, s_)
                         * SampleSelectSystem::C(n_ - s_, j_ - s_);
            return (int)std::max(1LL, ub);
        }

        std::vector<int> touched;
        std::vector<int> cnt(jMasks_.size(), 0);
        int best = 1;
        for (int c = 0; c < (int)kToS_.size(); ++c) {
            touched.clear();
            for (int si : kToS_[c]) {
                for (int ji : sToJ_[si]) {
                    if (cnt[ji]++ == 0) touched.push_back(ji);
                }
            }
            int gain = 0;
            for (int ji : touched) {
                gain += std::min(minCover_, cnt[ji]);
                cnt[ji] = 0;
            }
            best = std::max(best, gain);
        }
        return best;
    }

    void loadIncumbent(const std::vector<std::vector<int>>& incumbentGroups)
    {
        std::unordered_map<int, int> samplePos;
        samplePos.reserve(samples_.size() * 2);
        for (int i = 0; i < (int)samples_.size(); ++i) samplePos[samples_[i]] = i;

        std::set<int> ids;
        for (const auto& group : incumbentGroups) {
            if ((int)group.size() != k_) continue;
            uint32_t mask = 0u;
            bool ok = true;
            for (int v : group) {
                auto it = samplePos.find(v);
                if (it == samplePos.end()) {
                    ok = false;
                    break;
                }
                mask |= (1u << it->second);
            }
            if (!ok || popcountMask(mask) != k_) continue;
            int id = (mask < originalMaskToReducedId_.size())
                ? originalMaskToReducedId_[mask]
                : -1;
            if (id >= 0) ids.insert(id);
        }
        incumbentIds_.assign(ids.begin(), ids.end());
    }

    void initializeSearchState()
    {
        coverRef_.assign(sMasks_.size(), 0);
        jCount_.assign(jMasks_.size(), 0);
        sDeficitWeight_.assign(sMasks_.size(), 0);
        selected_.assign(kMasks_.size(), 0);
        selectedIds_.clear();
        stamp_.assign(kMasks_.size(), 0);
        totalDeficit_ = (int)jMasks_.size() * minCover_;
        unsatisfiedJ_ = (int)jMasks_.size();

        for (const auto& subs : jToS_) {
            for (int si : subs) sDeficitWeight_[si] += minCover_;
        }
    }

    int stateLowerBound() const
    {
        if (totalDeficit_ <= 0) return 0;
        int lb = std::max(1, (totalDeficit_ + rootMaxGain_ - 1) / rootMaxGain_);

        std::vector<int> gains;
        gains.reserve(kMasks_.size());
        for (int c = 0; c < (int)kMasks_.size(); ++c) {
            if (selected_[c]) continue;
            int gain = candidateScore(c);
            if (gain > 0) gains.push_back(gain);
        }
        if (gains.empty()) return 1000000000;
        std::sort(gains.begin(), gains.end(), std::greater<int>());
        int covered = 0;
        for (int i = 0; i < (int)gains.size(); ++i) {
            covered += gains[i];
            if (covered >= totalDeficit_) {
                lb = std::max(lb, i + 1);
                break;
            }
        }
        if (covered < totalDeficit_) return 1000000000;
        return lb;
    }

    int candidateScore(int candidateId) const
    {
        int score = 0;
        for (int si : kToS_[candidateId]) {
            if (coverRef_[si] == 0) score += std::max(0, sDeficitWeight_[si]);
        }
        return score;
    }

    int chooseBranchJ() const
    {
        int best = -1;
        long long bestPotential = std::numeric_limits<long long>::max();
        int bestDeficit = -1;
        int bestUncovered = std::numeric_limits<int>::max();

        for (int ji = 0; ji < (int)jToS_.size(); ++ji) {
            if (jCount_[ji] >= minCover_) continue;
            int deficit = minCover_ - jCount_[ji];
            int uncovered = 0;
            long long potential = 0;
            for (int si : jToS_[ji]) {
                if (coverRef_[si] != 0) continue;
                ++uncovered;
                potential += (long long)sToK_[si].size();
            }
            if (uncovered < deficit || potential == 0) return -2;
            if (potential < bestPotential ||
                (potential == bestPotential && deficit > bestDeficit) ||
                (potential == bestPotential && deficit == bestDeficit &&
                 uncovered < bestUncovered)) {
                best = ji;
                bestPotential = potential;
                bestDeficit = deficit;
                bestUncovered = uncovered;
            }
        }
        return best;
    }

    std::vector<int> branchCandidates(int branchJ)
    {
        if (++stampToken_ == std::numeric_limits<int>::max()) {
            std::fill(stamp_.begin(), stamp_.end(), 0);
            stampToken_ = 1;
        }

        std::vector<std::pair<int, int>> scored; // score, candidate id
        for (int si : jToS_[branchJ]) {
            if (coverRef_[si] != 0) continue;
            for (int c : sToK_[si]) {
                if (selected_[c] || stamp_[c] == stampToken_) continue;
                stamp_[c] = stampToken_;
                int score = candidateScore(c);
                if (score > 0) scored.push_back({score, c});
            }
        }

        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) {
                      if (a.first != b.first) return a.first > b.first;
                      return a.second < b.second;
                  });

        std::vector<int> out;
        out.reserve(scored.size());
        for (auto [score, c] : scored) out.push_back(c);
        return out;
    }

    void addCandidate(int candidateId)
    {
        selected_[candidateId] = 1;
        selectedIds_.push_back(candidateId);

        for (int si : kToS_[candidateId]) {
            if (coverRef_[si]++ != 0) continue;
            for (int ji : sToJ_[si]) {
                if (jCount_[ji] < minCover_) {
                    --totalDeficit_;
                    for (int otherS : jToS_[ji]) --sDeficitWeight_[otherS];
                    if (jCount_[ji] + 1 == minCover_) --unsatisfiedJ_;
                }
                ++jCount_[ji];
            }
        }
    }

    void removeCandidate(int candidateId)
    {
        for (int si : kToS_[candidateId]) {
            if (--coverRef_[si] != 0) continue;
            for (int ji : sToJ_[si]) {
                --jCount_[ji];
                if (jCount_[ji] < minCover_) {
                    ++totalDeficit_;
                    for (int otherS : jToS_[ji]) ++sDeficitWeight_[otherS];
                    if (jCount_[ji] + 1 == minCover_) ++unsatisfiedJ_;
                }
            }
        }

        selected_[candidateId] = 0;
        selectedIds_.pop_back();
    }

    bool checkTimeout()
    {
        if ((nodes_ & 4095ULL) != 0ULL) return false;
        if (std::chrono::steady_clock::now() <= deadline_) return false;
        timedOut_ = true;
        return true;
    }

    bool applyForcedMoves(std::vector<int>& applied)
    {
        bool changed = true;
        while (changed) {
            changed = false;
            int forcedCandidate = -1;

            for (int ji = 0; ji < (int)jToS_.size(); ++ji) {
                if (jCount_[ji] >= minCover_) continue;
                int deficit = minCover_ - jCount_[ji];
                std::vector<int> availableSis;
                availableSis.reserve(jToS_[ji].size());
                for (int si : jToS_[ji]) {
                    if (coverRef_[si] == 0) availableSis.push_back(si);
                }
                if ((int)availableSis.size() < deficit) return false;
                if ((int)availableSis.size() != deficit) continue;

                for (int si : availableSis) {
                    int availableCandidates = 0;
                    int lastCandidate = -1;
                    for (int c : sToK_[si]) {
                        if (selected_[c]) continue;
                        int score = candidateScore(c);
                        if (score <= 0) continue;
                        ++availableCandidates;
                        lastCandidate = c;
                        if (availableCandidates > 1) break;
                    }
                    if (availableCandidates == 0) return false;
                    if (availableCandidates == 1) {
                        forcedCandidate = lastCandidate;
                        break;
                    }
                }
                if (forcedCandidate >= 0) break;
            }

            if (forcedCandidate >= 0) {
                if (selected_[forcedCandidate]) return false;
                addCandidate(forcedCandidate);
                applied.push_back(forcedCandidate);
                changed = true;
                if (totalDeficit_ <= 0 || unsatisfiedJ_ == 0) return true;
            }
        }
        return true;
    }

    void undoForcedMoves(const std::vector<int>& applied)
    {
        for (int i = (int)applied.size() - 1; i >= 0; --i) {
            removeCandidate(applied[i]);
        }
    }

    void dfs()
    {
        if (timedOut_) return;
        ++nodes_;
        if (checkTimeout()) return;

        std::vector<int> forced;
        if (!applyForcedMoves(forced)) {
            undoForcedMoves(forced);
            return;
        }

        if (totalDeficit_ == 0 || unsatisfiedJ_ == 0) {
            if (selectedIds_.size() < incumbentIds_.size()) {
                incumbentIds_ = selectedIds_;
                std::cout << ">> [Exact] incumbent improved: "
                          << incumbentIds_.size() << " groups"
                          << " (nodes=" << nodes_ << ")\n";
            }
            undoForcedMoves(forced);
            return;
        }

        if (selectedIds_.size() + (size_t)stateLowerBound() >= incumbentIds_.size()) {
            undoForcedMoves(forced);
            return;
        }

        int branchJ = chooseBranchJ();
        if (branchJ < 0) {
            undoForcedMoves(forced);
            return;
        }

        auto candidates = branchCandidates(branchJ);
        for (int c : candidates) {
            if (timedOut_) {
                undoForcedMoves(forced);
                return;
            }
            if (selectedIds_.size() + 1 >= incumbentIds_.size()) continue;
            addCandidate(c);
            dfs();
            removeCandidate(c);
        }
        undoForcedMoves(forced);
    }
};

static std::string cacheKeyFileName(
    int n,
    int k,
    int j,
    int s,
    int minCover)
{
    std::ostringstream oss;
    oss << n << "-" << k << "-" << j << "-" << s << "-"
        << minCover << ".json";
    return oss.str();
}

static std::string solutionCacheFile(
    const std::string& cacheDir,
    const std::string& type,
    int n,
    int k,
    int j,
    int s,
    int minCover)
{
    fs::path path = fs::path(cacheDir) / type / cacheKeyFileName(n, k, j, s, minCover);
    return path.string();
}

static std::vector<std::string> solutionCacheCandidates(
    const std::string& cacheDir,
    const std::string& type,
    int n,
    int k,
    int j,
    int s,
    int minCover)
{
    std::vector<std::string> paths;
    paths.push_back(solutionCacheFile(cacheDir, type, n, k, j, s, minCover));
    if (type == "certified") {
        // Backward compatibility with the earlier flat certified cache layout.
        paths.push_back((fs::path(cacheDir) / cacheKeyFileName(n, k, j, s, minCover)).string());
    }
    return paths;
}

static bool parseCachedGroups(
    const std::string& text,
    std::vector<std::vector<int>>& groups)
{
    size_t key = text.find("\"groups\"");
    if (key == std::string::npos) return false;
    size_t pos = text.find('[', key);
    if (pos == std::string::npos) return false;

    int depth = 0;
    std::vector<int> current;
    for (; pos < text.size(); ++pos) {
        char ch = text[pos];
        if (ch == '[') {
            ++depth;
            if (depth == 2) current.clear();
        } else if (ch == ']') {
            if (depth == 2) {
                if (!current.empty()) groups.push_back(current);
                current.clear();
            }
            --depth;
            if (depth == 0) break;
            if (depth < 0) return false;
        } else if (depth == 2 &&
                   (std::isdigit((unsigned char)ch) || ch == '-')) {
            size_t next = pos + 1;
            while (next < text.size() && std::isdigit((unsigned char)text[next])) ++next;
            current.push_back(std::stoi(text.substr(pos, next - pos)));
            pos = next - 1;
        }
    }
    return !groups.empty();
}

static bool cacheHasParam(
    const std::string& text,
    const std::string& key,
    int value)
{
    std::string needle = "\"" + key + "\":" + std::to_string(value);
    if (text.find(needle) != std::string::npos) return true;
    needle = "\"" + key + "\": " + std::to_string(value);
    return text.find(needle) != std::string::npos;
}

static std::string currentTimestamp()
{
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S%z");
    return oss.str();
}

static bool mapCanonicalGroupsToSamples(
    const SampleSelectSystem& sys,
    const std::vector<std::vector<int>>& canonicalGroups,
    std::vector<std::vector<int>>& mapped)
{
    const auto& samples = sys.getSamples();
    mapped.clear();
    mapped.reserve(canonicalGroups.size());
    for (const auto& group : canonicalGroups) {
        if ((int)group.size() != sys.getK()) return false;
        std::vector<int> actual;
        actual.reserve(group.size());
        std::set<int> seen;
        for (int v : group) {
            if (v < 1 || v > (int)samples.size()) return false;
            if (!seen.insert(v).second) return false;
            actual.push_back(samples[v - 1]);
        }
        std::sort(actual.begin(), actual.end());
        mapped.push_back(std::move(actual));
    }
    std::sort(mapped.begin(), mapped.end());
    return sys.verifyGroups(mapped);
}

static bool canonicalMasksFromGroups(
    const SampleSelectSystem& sys,
    const std::vector<std::vector<int>>& groups,
    std::vector<uint32_t>& masks)
{
    std::unordered_map<int, int> samplePos;
    const auto& samples = sys.getSamples();
    samplePos.reserve(samples.size() * 2);
    for (int i = 0; i < (int)samples.size(); ++i) samplePos[samples[i]] = i;

    std::set<uint32_t> dedup;
    for (const auto& group : groups) {
        if ((int)group.size() != sys.getK()) return false;
        uint32_t mask = 0u;
        for (int v : group) {
            auto it = samplePos.find(v);
            if (it == samplePos.end()) return false;
            mask |= (1u << it->second);
        }
        if (popcountMask(mask) != sys.getK()) return false;
        dedup.insert(mask);
    }
    masks.assign(dedup.begin(), dedup.end());
    return !masks.empty();
}

static bool loadSolutionCache(
    const SampleSelectSystem& sys,
    const std::string& cacheDir,
    const std::string& type,
    SolveReport& report)
{
    for (const std::string& path : solutionCacheCandidates(
             cacheDir, type, sys.getN(), sys.getK(), sys.getJ(), sys.getS(), sys.getMinCover())) {
        std::ifstream ifs(path);
        if (!ifs.is_open()) continue;

        std::string text((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
        if (type == "certified" &&
            text.find("\"optimal\":true") == std::string::npos &&
            text.find("\"optimal\": true") == std::string::npos) {
            continue;
        }
        if (!cacheHasParam(text, "n", sys.getN()) ||
            !cacheHasParam(text, "k", sys.getK()) ||
            !cacheHasParam(text, "j", sys.getJ()) ||
            !cacheHasParam(text, "s", sys.getS()) ||
            !cacheHasParam(text, "minCover", sys.getMinCover())) {
            continue;
        }

        std::vector<std::vector<int>> canonicalGroups;
        if (!parseCachedGroups(text, canonicalGroups)) continue;

        std::vector<std::vector<int>> mapped;
        if (!mapCanonicalGroupsToSamples(sys, canonicalGroups, mapped)) continue;

        report.groups = std::move(mapped);
        report.feasible = true;
        report.upperBound = (int)report.groups.size();
        report.lowerBound = (type == "certified")
            ? report.upperBound
            : combinedLowerBound(sys.getN(), sys.getK(), sys.getJ(), sys.getS(), sys.getMinCover());
        report.optimal = (type == "certified");
        report.gap = (report.upperBound > 0)
            ? (double)(report.upperBound - report.lowerBound) / (double)report.upperBound
            : 0.0;
        report.algorithm = (type == "certified") ? "Certified cache" : "Incumbent cache";
        report.cacheType = type;
        report.warmStarted = (type == "incumbent");
        report.incumbentCount = (type == "incumbent") ? report.upperBound : 0;
        return true;
    }
    return false;
}

static bool loadCertifiedCache(
    const SampleSelectSystem& sys,
    const std::string& cacheDir,
    SolveReport& report)
{
    return loadSolutionCache(sys, cacheDir, "certified", report);
}

static bool loadIncumbentCache(
    const SampleSelectSystem& sys,
    const std::string& cacheDir,
    SolveReport& report)
{
    return loadSolutionCache(sys, cacheDir, "incumbent", report);
}

static bool writeSolutionCache(
    const std::string& cacheDir,
    const std::string& type,
    int n,
    int k,
    int j,
    int s,
    int minCover,
    const std::vector<uint32_t>& selectedMasks,
    int lowerBound,
    bool optimal,
    double proofTimeSec,
    const std::string& sourceAlgorithm)
{
    try {
        fs::create_directories(fs::path(cacheDir) / type);
    } catch (...) {
        return false;
    }

    std::string path = solutionCacheFile(cacheDir, type, n, k, j, s, minCover);
    std::ofstream ofs(path);
    if (!ofs.is_open()) return false;

    std::string now = currentTimestamp();
    ofs << "{\n";
    ofs << "  \"solverVersion\":\"cover-exact-v2\",\n";
    ofs << "  \"n\":" << n << ",\n";
    ofs << "  \"k\":" << k << ",\n";
    ofs << "  \"j\":" << j << ",\n";
    ofs << "  \"s\":" << s << ",\n";
    ofs << "  \"minCover\":" << minCover << ",\n";
    ofs << "  \"optimal\":" << (optimal ? "true" : "false") << ",\n";
    ofs << "  \"verified\":true,\n";
    ofs << "  \"lowerBound\":" << lowerBound << ",\n";
    ofs << "  \"upperBound\":" << selectedMasks.size() << ",\n";
    ofs << "  \"proofTimeSec\":" << std::fixed << std::setprecision(6)
        << proofTimeSec << ",\n";
    ofs << "  \"createdAt\":\"" << now << "\",\n";
    ofs << "  \"lastImprovedAt\":\"" << now << "\",\n";
    ofs << "  \"sourceAlgorithm\":\"" << sourceAlgorithm << "\",\n";
    ofs << "  \"groups\":[";
    for (int i = 0; i < (int)selectedMasks.size(); ++i) {
        if (i) ofs << ",";
        auto group = maskToCanonicalGroup(selectedMasks[i]);
        ofs << "[";
        for (int p = 0; p < (int)group.size(); ++p) {
            if (p) ofs << ",";
            ofs << group[p];
        }
        ofs << "]";
    }
    ofs << "]\n";
    ofs << "}\n";
    return true;
}

static bool writeCertifiedCache(
    const std::string& cacheDir,
    int n,
    int k,
    int j,
    int s,
    int minCover,
    const std::vector<uint32_t>& selectedMasks,
    double proofTimeSec)
{
    return writeSolutionCache(cacheDir, "certified", n, k, j, s, minCover,
                              selectedMasks, (int)selectedMasks.size(), true,
                              proofTimeSec, "Exact branch-and-bound");
}

static bool writeIncumbentCache(
    const SampleSelectSystem& sys,
    const std::string& cacheDir,
    const SolveReport& report)
{
    if (!report.feasible || report.groups.empty() || !sys.verifyGroups(report.groups)) {
        return false;
    }

    // P3.3 — Write-protection: only overwrite disk if our solution is strictly better
    // than what's already there.  This prevents a timed-out run from clobbering a
    // previous run that had more compute time.
    {
        std::string path = solutionCacheFile(
            cacheDir, "incumbent",
            sys.getN(), sys.getK(), sys.getJ(), sys.getS(), sys.getMinCover());
        std::ifstream existing(path);
        if (existing.is_open()) {
            std::string text((std::istreambuf_iterator<char>(existing)),
                             std::istreambuf_iterator<char>());
            // Extract "upperBound" from existing JSON.
            auto ubPos = text.find("\"upperBound\":");
            if (ubPos != std::string::npos) {
                ubPos += 13; // skip key
                while (ubPos < text.size() && !std::isdigit((unsigned char)text[ubPos]))
                    ++ubPos;
                int existingUB = 0;
                while (ubPos < text.size() && std::isdigit((unsigned char)text[ubPos]))
                    existingUB = existingUB * 10 + (text[ubPos++] - '0');
                if (existingUB > 0 && (int)report.groups.size() >= existingUB) {
                    std::cout << ">> [Cache] Skipping write: disk UB=" << existingUB
                              << " ≤ new UB=" << report.groups.size() << "\n";
                    return false;  // disk is already at least as good
                }
            }
        }
    }

    std::vector<uint32_t> masks;
    if (!canonicalMasksFromGroups(sys, report.groups, masks)) return false;
    return writeSolutionCache(
        cacheDir, "incumbent", sys.getN(), sys.getK(), sys.getJ(),
        sys.getS(), sys.getMinCover(), masks, report.lowerBound, false,
        0.0, report.algorithm);
}

static SolveReport runInternalExact(
    const SampleSelectSystem& sys,
    const std::vector<std::vector<int>>& incumbent,
    int timeLimitSec,
    const std::string& cacheDir,
    bool writeCache,
    int threads)
{
    SolveReport report;
    report.algorithm = "Exact branch-and-bound";
    report.groups = incumbent;
    report.feasible = !incumbent.empty() && sys.verifyGroups(incumbent);
    report.upperBound = (int)incumbent.size();
    report.lowerBound = combinedLowerBound(
        sys.getN(), sys.getK(), sys.getJ(), sys.getS(), sys.getMinCover());
    report.timeLimitSec = timeLimitSec;
    report.threads = std::max(1, threads);

    if (sys.getN() > 16) {
        report.algorithm = "Exact branch-and-bound skipped (n > 16)";
        return report;
    }

    if (sys.getMinCover() == 1) {
        std::cout << ">> [Exact] Building standard set-cover bitset model for n="
                  << sys.getN() << ", k=" << sys.getK()
                  << ", j=" << sys.getJ() << ", s=" << sys.getS() << "\n";

        StandardSetCoverExactSolver solver(
            sys.getN(), sys.getK(), sys.getJ(), sys.getS(),
            sys.getSamples(), incumbent, ExactOptions{std::max(1, threads)});
        auto exact = solver.solve(timeLimitSec);

        if (exact.feasible) {
            report.groups = exact.groups;
            report.feasible = sys.verifyGroups(report.groups);
            report.upperBound = (int)report.groups.size();
        }
        report.optimal = exact.optimal && report.feasible;
        report.lowerBound = exact.lowerBound;
        report.nodes = exact.nodes;
        report.ttHits = exact.ttHits;
        report.nodesPerSec = exact.nodesPerSec;
        report.reductionRatio = exact.reductionRatio;
        report.proofTimeSec = exact.proofTimeSec;
        report.gap = (report.upperBound > 0)
            ? (double)(report.upperBound - report.lowerBound) / (double)report.upperBound
            : 0.0;
        report.algorithm = report.optimal
            ? "Exact branch-and-bound"
            : (exact.timedOut ? "Exact branch-and-bound (timeout)" : "Exact branch-and-bound (no proof)");

        std::cout << ">> [Exact] done: feasible=" << (report.feasible ? "true" : "false")
                  << ", optimal=" << (report.optimal ? "true" : "false")
                  << ", groups=" << report.upperBound
                  << ", lb=" << report.lowerBound
                  << ", nodes=" << exact.nodes
                  << ", time=" << exact.proofTimeSec << "s\n";

        // P4 — Auto-write certified cache whenever we prove optimality, even if the
        // caller did not pass writeCache=true (e.g. the anytime path skips runInternalExact).
        // Also write when UB == Schönheim lower bound even without a full proof,
        // since that is mathematically equivalent to optimality.
        bool schonheimProved = (report.feasible && report.upperBound > 0 &&
                                report.upperBound <= report.lowerBound);
        if ((report.optimal || schonheimProved) && !cacheDir.empty()) {
            // Ensure the certified subdirectory exists.
            try { fs::create_directories(fs::path(cacheDir) / "certified"); } catch (...) {}
            if (writeCertifiedCache(cacheDir, sys.getN(), sys.getK(), sys.getJ(),
                                    sys.getS(), sys.getMinCover(),
                                    exact.selectedMasks, exact.proofTimeSec)) {
                std::cout << ">> [Cache] Certified solution written to disk.\n";
                report.algorithm = "Exact branch-and-bound (certified cached)";
                report.cacheType = "certified";
                report.optimal   = true;
            }
        }

        return report;
    }

    std::cout << ">> [Exact] Building generalized cover-design model for n="
              << sys.getN() << ", k=" << sys.getK()
              << ", j=" << sys.getJ() << ", s=" << sys.getS()
              << ", minCover=" << sys.getMinCover() << "\n";

    CoverDesignExactSolver solver(
        sys.getN(), sys.getK(), sys.getJ(), sys.getS(), sys.getMinCover(),
        sys.getSamples(), incumbent);
    auto exact = solver.solve(timeLimitSec);

    if (exact.feasible) {
        report.groups = exact.groups;
        report.feasible = sys.verifyGroups(report.groups);
        report.upperBound = (int)report.groups.size();
    }
    report.optimal = exact.optimal && report.feasible;
    report.lowerBound = exact.lowerBound;
    report.nodes = exact.nodes;
    report.ttHits = exact.ttHits;
    report.nodesPerSec = exact.nodesPerSec;
    report.reductionRatio = exact.reductionRatio;
    report.proofTimeSec = exact.proofTimeSec;
    report.gap = (report.upperBound > 0)
        ? (double)(report.upperBound - report.lowerBound) / (double)report.upperBound
        : 0.0;
    report.algorithm = report.optimal
        ? "Exact branch-and-bound"
        : (exact.timedOut ? "Exact branch-and-bound (timeout)" : "Exact branch-and-bound (no proof)");

    std::cout << ">> [Exact] done: feasible=" << (report.feasible ? "true" : "false")
              << ", optimal=" << (report.optimal ? "true" : "false")
              << ", groups=" << report.upperBound
              << ", lb=" << report.lowerBound
              << ", nodes=" << exact.nodes
              << ", time=" << exact.proofTimeSec << "s\n";

    if (report.optimal && writeCache) {
        if (writeCertifiedCache(cacheDir, sys.getN(), sys.getK(), sys.getJ(),
                                sys.getS(), sys.getMinCover(),
                                exact.selectedMasks, exact.proofTimeSec)) {
            report.algorithm = "Exact branch-and-bound (cached)";
            report.cacheType = "certified";
        }
    }

    return report;
}

} // namespace

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

void SampleSelectSystem::randomSamples(unsigned seed)
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

    // If seed == 0, use current timestamp (random each run); otherwise deterministic.
    if (seed == 0)
        seed = static_cast<unsigned>(
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
        // C(n,6): n≤9→≤84, n≤11→≤462, n≤13→≤1716, n≤16→≤8008
        int numRestarts = ((int)kCandidates.size() <= 200)  ? 500 :
                          ((int)kCandidates.size() <= 2000) ? 200 :
                          ((int)kCandidates.size() <= 8008) ? 100 : 10;
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
// GRASP: Greedy Randomized Adaptive Search Procedure (large-scale solver)
// ════════════════════════════════════════════════════════════════════════════════
std::vector<std::vector<int>> SampleSelectSystem::generateGRASP(
    int timeLimitSec, double alpha)
{
    if (samples_.empty()) {
        throw std::runtime_error(
            "No samples selected. Call randomSamples() or inputSamples() first.");
    }

    // ── Build coverage data structures (mirrors generateOptimalGroups setup) ──

    const std::vector<std::vector<int>> jSubsets = enumerate(samples_, j_);
    const int numJ = (int)jSubsets.size();

    std::cout << ">> GRASP: C(" << n_ << "," << j_ << ") = " << numJ
              << " j-subsets, minCover=" << minCover_
              << "; C(" << n_ << "," << k_ << ") = " << C(n_, k_) << " k-candidates\n";

    struct VecHash {
        size_t operator()(const std::vector<int>& v) const {
            size_t h = v.size();
            for (int x : v)
                h ^= (size_t)x * 2654435761ULL + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    std::vector<std::vector<int>> globalSubs;
    std::vector<std::vector<int>> jToSubs(numJ);
    std::unordered_map<std::vector<int>, int, VecHash> subIndex;

    for (auto& sub : enumerate(samples_, s_)) {
        if (subIndex.find(sub) == subIndex.end()) {
            subIndex[sub] = (int)globalSubs.size();
            globalSubs.push_back(sub);
        }
    }
    const int numSubs = (int)globalSubs.size();

    for (int i = 0; i < numJ; ++i)
        for (auto& sub : enumerate(jSubsets[i], s_))
            jToSubs[i].push_back(subIndex.at(sub));

    // Inverted index: sCoversList[si] = j-subsets containing s-subset si
    std::vector<std::vector<int>> sCoversList(numSubs);
    for (int i = 0; i < numJ; ++i)
        for (int si : jToSubs[i])
            sCoversList[si].push_back(i);

    // k-group → sorted list of its s-subset global indices
    auto getKSubIndices = [&](const std::vector<int>& kg) -> std::vector<int> {
        std::vector<int> result;
        for (auto& sub : enumerate(kg, s_)) {
            auto it = subIndex.find(sub);
            if (it != subIndex.end()) result.push_back(it->second);
        }
        return result;
    };

    // ── Shared coverage state (reference counts) ──────────────────────────────
    // covRef[si] = number of selected groups covering s-subset si
    // jCnt[i]    = number of covered s-subsets of j-subset i
    // jSat[i]    = jCnt[i] >= minCover_
    std::vector<int>  covRef(numSubs, 0);
    std::vector<int>  jCnt(numJ, 0);
    std::vector<bool> jSat(numJ, false);
    int satisfied = 0;

    auto stateAdd = [&](const std::vector<int>& kSubs) {
        for (int si : kSubs)
            if (++covRef[si] == 1)
                for (int ji : sCoversList[si])
                    if (!jSat[ji] && ++jCnt[ji] == minCover_) {
                        jSat[ji] = true; ++satisfied;
                    }
    };

    auto stateRemove = [&](const std::vector<int>& kSubs) {
        for (int si : kSubs)
            if (--covRef[si] == 0)
                for (int ji : sCoversList[si])
                    if (jSat[ji] && --jCnt[ji] < minCover_) {
                        jSat[ji] = false; --satisfied;
                    }
    };

    auto resetState = [&]() {
        std::fill(covRef.begin(), covRef.end(), 0);
        std::fill(jCnt.begin(),   jCnt.end(),   0);
        std::fill(jSat.begin(),   jSat.end(),   false);
        satisfied = 0;
    };

    // ── Random k-group generator ──────────────────────────────────────────────
    std::mt19937 rng(42u);
    auto randKGroup = [&]() -> std::vector<int> {
        std::vector<int> pool = samples_;
        std::shuffle(pool.begin(), pool.end(), rng);
        pool.resize(k_);
        std::sort(pool.begin(), pool.end());
        return pool;
    };

    // ── Marginal score: how many newly satisfied j-subsets does this group add ─
    // Uses temporary per-call state (does not modify covRef/jCnt/jSat).
    auto marginalScore = [&](const std::vector<int>& kSubs) -> int {
        // Count how many s-subsets in kSubs are currently uncovered (covRef=0)
        // and for each, which j-subsets they help push to satisfaction.
        // We track delta-jCnt temporarily per unique ji.
        std::unordered_map<int, int> delta;
        for (int si : kSubs) {
            if (covRef[si] > 0) continue;          // already covered, no new help
            for (int ji : sCoversList[si]) {
                if (jSat[ji]) continue;             // already fully satisfied
                delta[ji]++;
            }
        }
        int score = 0;
        for (auto& [ji, d] : delta)
            score += std::min(d, minCover_ - jCnt[ji]);
        return score;
    };

    // ── Backward elimination: remove groups that are still redundant ──────────
    auto backwardElim = [&](std::vector<std::vector<int>>& sol,
                            std::vector<std::vector<int>>& solKSubs) {
        bool improved = true;
        while (improved) {
            improved = false;
            // Shuffle order to avoid deterministic bias
            std::vector<int> order(sol.size());
            std::iota(order.begin(), order.end(), 0);
            std::shuffle(order.begin(), order.end(), rng);
            for (int idx : order) {
                if (idx >= (int)sol.size()) continue;
                // Temporarily remove group idx
                stateRemove(solKSubs[idx]);
                if (satisfied == numJ) {
                    // Still fully covered → drop it
                    sol.erase(sol.begin() + idx);
                    solKSubs.erase(solKSubs.begin() + idx);
                    improved = true;
                    break;
                } else {
                    // Restore
                    stateAdd(solKSubs[idx]);
                }
            }
        }
    };

    // ── GRASP construction phase ──────────────────────────────────────────────
    // Sample CAND_POOL candidates at each step; score them; build RCL.
    const int CAND_POOL = std::max(50, std::min(500, (int)C(n_, k_) / 10));

    auto graspConstruct = [&]() -> std::pair<std::vector<std::vector<int>>,
                                              std::vector<std::vector<int>>> {
        resetState();
        std::vector<std::vector<int>> sol, solKSubs;

        while (satisfied < numJ) {
            // Sample a pool of unique random k-groups and score them
            std::vector<std::vector<int>> cands;
            std::vector<std::vector<int>> candsKSubs;
            std::set<std::vector<int>> seen;
            cands.reserve(CAND_POOL);

            int attempts = 0;
            while ((int)cands.size() < CAND_POOL && attempts < CAND_POOL * 4) {
                ++attempts;
                auto kg = randKGroup();
                if (seen.insert(kg).second) {
                    cands.push_back(kg);
                    candsKSubs.push_back(getKSubIndices(kg));
                }
            }

            if (cands.empty()) break;

            // Compute marginal scores
            std::vector<int> scores;
            scores.reserve(cands.size());
            int maxScore = 0, minScore = INT_MAX;
            for (auto& ks : candsKSubs) {
                int sc = marginalScore(ks);
                scores.push_back(sc);
                maxScore = std::max(maxScore, sc);
                minScore = std::min(minScore, sc);
            }

            if (maxScore == 0) {
                // No candidate makes progress; pick a random uncovered helper
                std::uniform_int_distribution<int> pick(0, (int)cands.size() - 1);
                int chosen = pick(rng);
                sol.push_back(cands[chosen]);
                solKSubs.push_back(candsKSubs[chosen]);
                stateAdd(candsKSubs[chosen]);
                continue;
            }

            // Build RCL: candidates with score >= threshold
            double threshold = maxScore - alpha * (maxScore - minScore);
            std::vector<int> rcl;
            for (int c = 0; c < (int)cands.size(); ++c)
                if (scores[c] >= threshold) rcl.push_back(c);

            // Randomly select from RCL
            std::uniform_int_distribution<int> rclPick(0, (int)rcl.size() - 1);
            int chosen = rcl[rclPick(rng)];

            sol.push_back(cands[chosen]);
            solKSubs.push_back(candsKSubs[chosen]);
            stateAdd(candsKSubs[chosen]);
        }
        return {sol, solKSubs};
    };

    // ── Single-swap local search ──────────────────────────────────────────────
    auto singleSwap = [&](std::vector<std::vector<int>>& sol,
                          std::vector<std::vector<int>>& solKSubs) {
        const int MAX_SWAPS = (int)sol.size() * 5;
        for (int attempt = 0; attempt < MAX_SWAPS; ++attempt) {
            if (sol.empty()) break;
            std::uniform_int_distribution<int> gPick(0, (int)sol.size() - 1);
            int gi = gPick(rng);

            // Remove group gi and try to replace with a better random group
            stateRemove(solKSubs[gi]);
            int satBefore = satisfied;

            auto ng = randKGroup();
            auto ns = getKSubIndices(ng);
            stateAdd(ns);
            int satAfter = satisfied;

            if (satAfter >= numJ) {
                // New group achieves full coverage; commit swap
                sol[gi]      = std::move(ng);
                solKSubs[gi] = std::move(ns);
            } else if (satAfter > satBefore) {
                // Partial improvement; commit anyway
                sol[gi]      = std::move(ng);
                solKSubs[gi] = std::move(ns);
            } else {
                // No improvement; revert
                stateRemove(ns);
                stateAdd(solKSubs[gi]);
            }
        }
    };

    // ── Main GRASP loop ───────────────────────────────────────────────────────
    using Clock = std::chrono::steady_clock;
    auto startTime = Clock::now();
    auto elapsed   = [&]() -> double {
        return std::chrono::duration<double>(Clock::now() - startTime).count();
    };

    std::vector<std::vector<int>> best;
    int iter = 0;

    std::cout << ">> Starting GRASP (time limit=" << timeLimitSec
              << "s, alpha=" << alpha << ", cand_pool=" << CAND_POOL << ")...\n";

    while (elapsed() < timeLimitSec) {
        // ① Construction
        auto [sol, solKSubs] = graspConstruct();

        // ② Single-swap local search (keep the state from construction)
        singleSwap(sol, solKSubs);

        // ③ Backward elimination
        backwardElim(sol, solKSubs);

        // ④ Update best
        if (best.empty() || sol.size() < best.size()) {
            best = sol;
            std::cout << ">> GRASP iter " << iter
                      << " (t=" << elapsed() << "s): best=" << best.size()
                      << " groups, satisfied=" << satisfied << "/" << numJ << "\n";
        }

        // Reset global state for next iteration
        resetState();
        ++iter;
    }

    std::cout << ">> GRASP finished after " << iter << " iterations: "
              << best.size() << " k-groups\n";

    // Final backward elimination pass on the best solution
    resetState();
    std::vector<std::vector<int>> bestKSubs;
    for (const auto& kg : best) {
        bestKSubs.push_back(getKSubIndices(kg));
        stateAdd(bestKSubs.back());
    }
    backwardElim(best, bestKSubs);

    std::cout << ">> After final backward elimination: " << best.size() << " k-groups\n";
    return best;
}

// ════════════════════════════════════════════════════════════════════════════════
// Portfolio solver inspired by fontanf/setcoveringsolver
// ════════════════════════════════════════════════════════════════════════════════

SolveReport SampleSelectSystem::solvePortfolio(
    int timeLimitSec,
    const std::string& solverBin,
    bool forceExact,
    bool cacheOnly,
    const std::string& cacheDir,
    bool useIncumbentCache,
    bool incumbentOnly,
    int threads)
{
    if (samples_.empty()) {
        throw std::runtime_error(
            "No samples selected. Call randomSamples() or inputSamples() first.");
    }
    if (timeLimitSec <= 0) timeLimitSec = 120;

    // P3.2 — Adaptive time-limit guidance: for hard instances (n≥15) the default
    // 120s budget is rarely enough to prove optimality.  We warn and internally
    // scale the time limit so the anytime solver at least produces a good UB.
    // We never *reduce* a time limit the caller set explicitly.
    if (n_ >= 15 && timeLimitSec < 300) {
        std::cout << ">> [Advisory] n=" << n_
                  << " is hard (LB=" << combinedLowerBound(n_, k_, j_, s_, minCover_)
                  << "); consider --timeLimit=600 for a near-optimal solution.\n"
                  << ">>   To pre-build the incumbent cache offline, run:\n"
                  << ">>     ./optimal_sample --cli --buildIncumbentCache "
                  << "--n=" << n_ << " --k=" << k_ << " --j=" << j_
                  << " --s=" << s_ << " --m=45 --timeLimit=600 --threads=4\n"
                  << ">>   (running with " << timeLimitSec << "s)\n";
    }

    SolveReport report;
    report.algorithm = "Portfolio";
    report.timeLimitSec = timeLimitSec;
    report.threads = std::max(1, threads);
    report.lowerBound = combinedLowerBound(n_, k_, j_, s_, minCover_);

    const long long nkSize = C(n_, k_);
    std::cout << ">> Portfolio solver: C(" << n_ << "," << k_ << ")=" << nkSize
              << ", minCover=" << minCover_
              << ", lowerBound=" << report.lowerBound << "\n";

    SolveReport incumbentReport;
    bool hasIncumbent = false;

    if (!forceExact && n_ >= 11 && n_ <= 16) {
        if (loadCertifiedCache(*this, cacheDir, report)) {
            report.timeLimitSec = timeLimitSec;
            report.threads = std::max(1, threads);
            std::cout << ">> Certified cache hit: " << report.upperBound
                      << " groups\n";
            return report;
        }
        std::cout << ">> Certified cache miss for "
                  << n_ << "-" << k_ << "-" << j_ << "-" << s_
                  << "-" << minCover_ << "\n";
        if (cacheOnly) {
            report.algorithm = "Certified cache miss";
            report.feasible = false;
            report.optimal = false;
            report.upperBound = 0;
            report.gap = 0.0;
            return report;
        }
        if (useIncumbentCache && loadIncumbentCache(*this, cacheDir, incumbentReport)) {
            hasIncumbent = true;
            incumbentReport.timeLimitSec = timeLimitSec;
            incumbentReport.threads = std::max(1, threads);
            std::cout << ">> Incumbent cache hit: UB="
                      << incumbentReport.upperBound << " groups\n";
            if (incumbentOnly) {
                return incumbentReport;
            }
        } else if (incumbentOnly) {
            report.algorithm = "Incumbent cache miss";
            report.feasible = false;
            report.optimal = false;
            report.cacheType = "none";
            return report;
        }
    }

    if (minCover_ == 1) {
        std::cout << ">> Building standard set-cover model (elements=j-subsets, sets=k-groups)...\n";
        auto model = buildStandardSetCoverModel(samples_, k_, j_, s_);
        const int originalSets = (int)model.sets.size();
        model = reduceSetCoverModel(model);
        std::cout << ">> Reduction: " << originalSets << " -> " << model.sets.size()
                  << " candidate sets, " << model.elementToSets.size()
                  << " elements\n";

        std::vector<double> unitWeights(model.elementToSets.size(), 1.0);
        auto best = greedySetCover(model, unitWeights, 42u);

        std::vector<double> degreeWeights(model.elementToSets.size(), 1.0);
        for (int e = 0; e < (int)degreeWeights.size(); ++e) {
            degreeWeights[e] = 1.0 / std::max(1, (int)model.elementToSets[e].size());
        }
        auto weighted = greedySetCover(model, degreeWeights, 4242u);
        if (weighted.feasible() && (!best.feasible() || weighted.size() < best.size())) {
            best = weighted;
        }

        // Multiple restarts across several weighting strategies for diversity.
        // Threshold raised to 10000 so that n=15 (sets=5005) gets the full 30+15 restart
        // budget instead of falling into the low-coverage 8+5 bracket.
        // Strategy A: unit weights (varying tie-break seed).
        int restarts = (model.sets.size() <= 200)   ? 80 :
                       (model.sets.size() <= 10000)  ? 30 : 8;
        for (int r = 0; r < restarts; ++r) {
            auto cand = greedySetCover(model, unitWeights, 1000u + (unsigned)r);
            if (cand.feasible() && (!best.feasible() || cand.size() < best.size())) {
                best = cand;
            }
        }

        // Strategy B: random soft weights — each element gets weight = uniform(0.5, 2.0).
        // These diversify the greedy ordering so it doesn't always fall into the same basin.
        {
            std::mt19937 rwRng(777u);
            std::uniform_real_distribution<double> rwDist(0.5, 2.0);
            int rwRestarts = (model.sets.size() <= 200)   ? 40 :
                             (model.sets.size() <= 10000)  ? 15 : 5;
            for (int r = 0; r < rwRestarts; ++r) {
                std::vector<double> randWeights(model.elementToSets.size());
                for (auto& w : randWeights) w = rwDist(rwRng);
                auto cand = greedySetCover(model, randWeights, 5000u + (unsigned)r);
                if (cand.feasible() && (!best.feasible() || cand.size() < best.size())) {
                    best = cand;
                }
            }
        }

        // Strategy C: inverse-frequency weights — rare elements get higher priority.
        // Particularly effective for large covering designs where a few j-subsets are
        // covered by very few k-groups (bottleneck elements).
        {
            std::vector<double> invFreqWeights(model.elementToSets.size());
            for (int e = 0; e < (int)invFreqWeights.size(); ++e) {
                invFreqWeights[e] = 1.0 / std::max(1.0,
                    std::sqrt((double)model.elementToSets[e].size()));
            }
            int ifRestarts = (model.sets.size() <= 10000) ? 10 : 3;
            for (int r = 0; r < ifRestarts; ++r) {
                auto cand = greedySetCover(model, invFreqWeights, 9000u + (unsigned)r);
                if (cand.feasible() && (!best.feasible() || cand.size() < best.size())) {
                    best = cand;
                }
            }
        }

        // ── Anytime concurrent mode for n=11..16 ────────────────────────────
        // For the hard covering-design path we launch LNS in a background thread
        // while the exact B&B starts immediately.  They share an atomic UB so the
        // exact solver pulls in any LNS improvement at the start of each budget
        // iteration, giving it the tightest possible pruning bound from day one.
        //
        // For small n (or when nkSize is small), fall through to the old sequential
        // path which is fast enough not to need concurrency.
        if (best.feasible() && (n_ >= 11 && n_ <= 16)) {
            // ── Determine the best warm-start for B&B and LNS ────────────────
            // The incumbent cache (from a previous long run) is often better than
            // the current greedy solution.  If so, we MUST feed it into B&B as the
            // starting incumbent so B&B begins at budget=incumbent-1 instead of
            // wasting every budget from greedy down to incumbent.
            // Without this, a 137-group cache causes B&B to iterate
            //   142→141→140→...→136 before it can try to beat 137 — most of the
            //   time budget is consumed before B&B reaches the interesting level.
            std::vector<std::vector<int>> warmGroups = best.groups();
            int warmUB = (int)best.size();
            if (hasIncumbent && incumbentReport.feasible &&
                incumbentReport.upperBound < warmUB) {
                warmGroups = incumbentReport.groups;
                warmUB     = incumbentReport.upperBound;
                std::cout << ">> [Anytime] Incumbent cache (" << warmUB
                          << " groups) is tighter than greedy (" << (int)best.size()
                          << "); using it as B&B warm-start.\n";
            }

            // Shared state between LNS thread and exact solver.
            std::atomic<int>              lnsSharedUB{warmUB};
            std::mutex                    lnsSharedMutex;
            std::vector<std::vector<int>> lnsSharedGroups = warmGroups;

            // LNS runs for up to 75% of the total budget (capped at 90s).
            // Using the full time rather than just 25% allows it to keep polishing
            // while exact is already searching — the two don't compete for CPU
            // since the user is generally not doing anything else at this point.
            int lnsTime = std::min(timeLimitSec * 3 / 4, 90);
            lnsTime = std::max(lnsTime, 5);

            std::cout << ">> [Anytime] Launching LNS thread for " << lnsTime
                      << "s alongside exact B&B...\n";

            // Publish helper: called both during the LNS run (via callback) and once
            // at the end, so the exact B&B sees each improvement as it happens.
            auto publishLns = [&](const PortfolioSetCoverSolution& sol) {
                if (!sol.feasible()) return;
                int newSz = (int)sol.size();
                int expected = lnsSharedUB.load(std::memory_order_acquire);
                bool published = false;
                while (newSz < expected &&
                       !lnsSharedUB.compare_exchange_weak(
                           expected, newSz, std::memory_order_acq_rel))
                {}
                published = (newSz == lnsSharedUB.load(std::memory_order_acquire)
                             && newSz < expected);
                if (published) {
                    std::lock_guard<std::mutex> lk(lnsSharedMutex);
                    lnsSharedGroups = sol.groups();
                    std::cout << ">> [LNS] Incumbent improved: " << newSz << " groups\n";
                }
            };

            // ── B improvement: Parallel multi-path LNS ────────────────────────
            // Launch lnsWorkerCount LNS threads, each starting from a different
            // greedy seed / weight strategy.  All share publishLns so any
            // improvement is immediately visible to the concurrent B&B.
            //
            // #7: Thread allocation — scale LNS workers with available threads.
            // threads=1   → 1 LNS worker + B&B(1), effectively cooperative serial
            // threads=2   → 1 LNS worker + B&B(1)
            // threads=3   → 2 LNS workers + B&B(1)
            // threads≥4   → 3 LNS workers + B&B(threads-3, min 1)
            // More LNS diversity wins on n=15 where LNS quality is the bottleneck.
            const int lnsWorkerCount = (threads >= 4) ? 3 : (threads >= 3) ? 2 : 1;

            // #1: Build a PortfolioSetCoverSolution from warmGroups so the LNS
            // can start from the best known solution (incumbent cache if tighter
            // than greedy) rather than always from the greedy solution.
            // Build a reverse map: group → set ID in the model.
            PortfolioSetCoverSolution warmSol(model);
            {
                std::unordered_map<std::vector<int>, int, VecHash> groupToSetId;
                groupToSetId.reserve(model.sets.size() * 2);
                for (int sid = 0; sid < (int)model.sets.size(); ++sid)
                    groupToSetId[model.sets[sid]] = sid;
                for (const auto& g : warmGroups) {
                    auto it = groupToSetId.find(g);
                    if (it != groupToSetId.end()) warmSol.add(it->second);
                }
                if (!warmSol.feasible()) {
                    // Fallback: if reconstruction fails use greedy
                    warmSol = best;
                    std::cout << ">> [Anytime] #1 warmSol reconstruction failed; "
                                 "falling back to greedy.\n";
                } else {
                    std::cout << ">> [Anytime] #1 LNS warm-start: " << warmSol.size()
                              << " groups (vs greedy " << best.size() << ")\n";
                }
            }

            std::vector<PortfolioSetCoverSolution> elitePool;
            std::vector<std::string> eliteKeys;
            std::mutex eliteMutex;
            std::atomic<unsigned> elitePickCounter{0};

            auto solutionKey = [](const PortfolioSetCoverSolution& sol) {
                std::ostringstream oss;
                for (const auto& group : sol.groups()) {
                    for (int v : group) oss << v << ',';
                    oss << ';';
                }
                return oss.str();
            };

            auto addElite = [&](const PortfolioSetCoverSolution& sol) {
                if (!sol.feasible()) return;
                std::string key = solutionKey(sol);
                std::lock_guard<std::mutex> lk(eliteMutex);
                if (std::find(eliteKeys.begin(), eliteKeys.end(), key) != eliteKeys.end())
                    return;

                if (elitePool.size() < 5) {
                    elitePool.push_back(sol);
                    eliteKeys.push_back(std::move(key));
                } else {
                    int worst = 0;
                    for (int i = 1; i < (int)elitePool.size(); ++i) {
                        if (elitePool[i].size() > elitePool[worst].size()) worst = i;
                    }
                    if (sol.size() >= elitePool[worst].size()) return;
                    elitePool[worst] = sol;
                    eliteKeys[worst] = std::move(key);
                }

                std::vector<int> order(elitePool.size());
                std::iota(order.begin(), order.end(), 0);
                std::sort(order.begin(), order.end(), [&](int a, int b) {
                    if (elitePool[a].size() != elitePool[b].size())
                        return elitePool[a].size() < elitePool[b].size();
                    return eliteKeys[a] < eliteKeys[b];
                });
                std::vector<PortfolioSetCoverSolution> sortedPool;
                std::vector<std::string> sortedKeys;
                sortedPool.reserve(elitePool.size());
                sortedKeys.reserve(eliteKeys.size());
                for (int idx : order) {
                    sortedPool.push_back(elitePool[idx]);
                    sortedKeys.push_back(std::move(eliteKeys[idx]));
                }
                elitePool = std::move(sortedPool);
                eliteKeys = std::move(sortedKeys);
            };

            auto takeEliteSeed = [&](PortfolioSetCoverSolution& out) -> bool {
                std::lock_guard<std::mutex> lk(eliteMutex);
                if (elitePool.empty()) return false;
                unsigned pick = elitePickCounter.fetch_add(1, std::memory_order_relaxed);
                out = elitePool[pick % elitePool.size()];
                return true;
            };

            addElite(warmSol);
            if (best.feasible()) addElite(best);

            {
                std::mt19937 poolRng(20260504u);
                std::uniform_real_distribution<double> wDist(0.5, 2.0);
                for (int r = 0; r < 3; ++r) {
                    std::vector<double> randWeights(model.elementToSets.size());
                    for (auto& w : randWeights) w = wDist(poolRng);
                    auto cand = greedySetCover(model, randWeights, 15000u + (unsigned)r);
                    addElite(cand);
                }
                std::cout << ">> [Anytime] Elite pool initialized with "
                          << elitePool.size() << " seeds\n";
            }

            // Shared best-LNS result — initialise from warmSol (may beat greedy).
            PortfolioSetCoverSolution lnsBestFinal = warmSol;
            std::mutex lnsBestMutex;

            // Wrapper: publish to B&B AND track the overall best LNS solution.
            auto publishAndTrack = [&](const PortfolioSetCoverSolution& sol) {
                publishLns(sol);
                if (sol.feasible()) {
                    std::lock_guard<std::mutex> lk(lnsBestMutex);
                    if (lnsBestFinal.size() > sol.size()) lnsBestFinal = sol;
                }
                addElite(sol);
            };

            std::vector<std::thread> lnsThreadVec;
            lnsThreadVec.reserve(lnsWorkerCount);
            for (int t = 0; t < lnsWorkerCount; ++t) {
                lnsThreadVec.emplace_back([&, t]() {
                    // All workers start from warmSol.  Workers t>0 also try a
                    // random-weight greedy and take the better of the two.
                    PortfolioSetCoverSolution initSol = warmSol;
                    if (t > 0) {
                        takeEliteSeed(initSol);
                        std::mt19937 wRng(42u + (unsigned)t * 13337u);
                        std::uniform_real_distribution<double> wDist(0.5, 2.0);
                        std::vector<double> randW(model.elementToSets.size());
                        for (auto& w : randW) w = wDist(wRng);
                        auto cand = greedySetCover(model, randW, (unsigned)(t * 7919u));
                        if (cand.feasible() && cand.size() < warmSol.size())
                            initSol = cand;
                    }

                    PortfolioSetCoverSolution result(model);
                    // All workers use LNS with the new large-neighbourhood destroy
                    // operators (op5 mega-destroy + scaled op1).  SA and Tabu proved
                    // less effective than LNS diversity across parallel seeds.
                    result = lnsImproveSetCover(model, initSol, lnsTime,
                                                publishAndTrack, takeEliteSeed);
                    publishAndTrack(result);
                });
            }

            // Exact B&B starts immediately.  report.groups / report.upperBound are
            // initialised with the BEST available warm-start (incumbent cache if
            // tighter than greedy, greedy otherwise).  The B&B solver receives the
            // same warm-start so its first budget = warmUB - 1.
            report.groups = warmGroups;
            report.feasible = true;
            report.upperBound = warmUB;

            ExactOptions exactOpts;
            // #7: Leave enough threads for LNS workers; B&B gets the remainder.
            exactOpts.threads     = std::max(1, threads - lnsWorkerCount);
            exactOpts.sharedUB    = &lnsSharedUB;
            exactOpts.sharedMutex = &lnsSharedMutex;
            exactOpts.sharedGroups = &lnsSharedGroups;

            {
                std::cout << ">> [Exact] Building standard set-cover bitset model for n="
                          << n_ << ", k=" << k_ << ", j=" << j_ << ", s=" << s_ << "\n";
                StandardSetCoverExactSolver solver(
                    n_, k_, j_, s_, samples_, report.groups, exactOpts);
                auto exact = solver.solve(timeLimitSec);
                if (exact.feasible) {
                    report.groups  = exact.groups;
                    report.feasible = verifyGroups(report.groups);
                    report.upperBound = (int)report.groups.size();
                }
                report.optimal      = exact.optimal && report.feasible;
                report.lowerBound   = exact.lowerBound;
                report.nodes        = exact.nodes;
                report.ttHits       = exact.ttHits;
                report.nodesPerSec  = exact.nodesPerSec;
                report.reductionRatio = exact.reductionRatio;
                report.proofTimeSec = exact.proofTimeSec;
                if (exact.timedOut)
                    report.algorithm = "Set-cover portfolio (greedy+LNS anytime, B&B timeout)";
                else
                    report.algorithm = "Set-cover portfolio (greedy+LNS anytime, B&B proved)";
            }

            for (auto& th : lnsThreadVec) th.join();

            // If LNS ended up better than what exact proved, take the LNS result
            // (can happen if exact timed out early and LNS kept running).
            if (lnsBestFinal.feasible() && (int)lnsBestFinal.size() < report.upperBound) {
                report.groups     = lnsBestFinal.groups();
                report.feasible   = verifyGroups(report.groups);
                report.upperBound = (int)report.groups.size();
                report.optimal    = false;
            }

        } else if (best.feasible() && nkSize > 10000) {
            // Large n outside 11..16: sequential LNS only (no exact solver).
            int lnsTime = std::min(timeLimitSec / 4, 30);
            lnsTime = std::max(lnsTime, 5);
            std::cout << ">> Running adaptive LNS for " << lnsTime << "s...\n";
            best = lnsImproveSetCover(model, best, lnsTime);
            report.groups = best.groups();
            report.feasible = best.feasible() && verifyGroups(report.groups);
            report.upperBound = (int)report.groups.size();
            report.algorithm = "Set-cover portfolio (greedy+weighted+LNS)";
        } else {
            report.groups = best.groups();
            report.feasible = best.feasible() && verifyGroups(report.groups);
            report.upperBound = (int)report.groups.size();
            report.algorithm = "Set-cover portfolio (greedy+weighted)";
        }

    } else {
        std::cout << ">> minCover > 1: using generalized coverage heuristic path...\n";
        if (nkSize > 10000) {
            report.groups = generateGRASP(timeLimitSec);
            report.algorithm = "Generalized portfolio (GRASP+prune)";
        } else {
            report.groups = generateOptimalGroups();
            report.algorithm = "Generalized portfolio (lazy greedy+prune)";
        }
        report.feasible = verifyGroups(report.groups);
        report.upperBound = (int)report.groups.size();
    }

    if (hasIncumbent && incumbentReport.feasible &&
        (!report.feasible || incumbentReport.upperBound <= report.upperBound)) {
        std::cout << ">> Warm start replaces heuristic UB: "
                  << report.upperBound << " -> "
                  << incumbentReport.upperBound << "\n";
        report = incumbentReport;
    } else if (hasIncumbent) {
        report.warmStarted = true;
        report.incumbentCount = incumbentReport.upperBound;
        report.cacheType = "incumbent";
    }

    // n=11..16: exact B&B already ran inside the anytime block above.
    // For forceExact on other n (outside 11..16), fall through to runInternalExact.
    if (forceExact && !(n_ >= 11 && n_ <= 16) && report.feasible) {
        auto exact = runInternalExact(
            *this, report.groups, timeLimitSec, cacheDir, true, std::max(1, threads));
        if (exact.feasible
                && (exact.upperBound <= report.upperBound || !report.feasible)) {
            exact.warmStarted = report.warmStarted;
            exact.incumbentCount = report.incumbentCount;
            if (exact.cacheType == "none") exact.cacheType = report.cacheType;
            report = exact;
            report.timeLimitSec = timeLimitSec;
            report.threads = std::max(1, threads);
        }
    }

    // Outside the dedicated n=11..16 path, keep HiGHS as an optional reference.
    if (!report.optimal && !(n_ >= 11 && n_ <= 16)
            && report.feasible && nkSize <= 10000) {
        int exactTimeLimit = std::max(5, timeLimitSec);
        auto exact = runHighsExact(*this, report.groups, exactTimeLimit,
                                   solverBin, report.lowerBound);
        if (exact.feasible
                && (exact.upperBound <= report.upperBound || !report.feasible)) {
            report = exact;
            report.timeLimitSec = exactTimeLimit;
        }
    }

    report.feasible = verifyGroups(report.groups);
    report.upperBound = (int)report.groups.size();
    if (report.optimal) {
        report.lowerBound = report.upperBound;
    } else if (report.upperBound == report.lowerBound && report.feasible) {
        report.optimal = true;
        report.algorithm += " (matched lower bound)";
    }
    report.gap = (report.upperBound > 0)
        ? (double)(report.upperBound - report.lowerBound) / (double)report.upperBound
        : 0.0;

    if (n_ >= 11 && n_ <= 16 && useIncumbentCache && report.feasible) {
        bool shouldWriteIncumbent = !report.optimal;
        if (!hasIncumbent) {
            shouldWriteIncumbent = true;
        } else if (report.upperBound > 0 && report.upperBound < incumbentReport.upperBound) {
            shouldWriteIncumbent = true;
        }
        if (shouldWriteIncumbent && writeIncumbentCache(*this, cacheDir, report)) {
            report.improvedIncumbent = !hasIncumbent ||
                (report.upperBound > 0 && report.upperBound < incumbentReport.upperBound);
            if (!report.optimal && report.cacheType == "none") report.cacheType = "incumbent";
        }
    }

    std::cout << ">> Portfolio done: algorithm=" << report.algorithm
              << ", feasible=" << (report.feasible ? "true" : "false")
              << ", optimal=" << (report.optimal ? "true" : "false")
              << ", groups=" << report.upperBound
              << ", lb=" << report.lowerBound
              << ", gap=" << report.gap << "\n";

    return report;
}

std::vector<std::vector<int>> SampleSelectSystem::generatePortfolio(
    int timeLimitSec,
    const std::string& solverBin)
{
    return solvePortfolio(timeLimitSec, solverBin).groups;
}

bool SampleSelectSystem::verifyGroups(
    const std::vector<std::vector<int>>& groups) const
{
    if (groups.empty()) return false;

    // P5.1 — Bitmask-accelerated verification.
    //
    // Represent every group and every j-subset as a bitmask over samples_ (0-indexed).
    // For each j-subset mask J, we need ≥ minCover_ groups G with popcount(G & J) ≥ s_.
    // This replaces the nested enumerate+includes loop and is ~10-100× faster for n≥14.
    const int n = (int)samples_.size();
    std::unordered_map<int, int> samplePos;
    samplePos.reserve(n * 2);
    for (int i = 0; i < n; ++i) samplePos[samples_[i]] = i;

    // Build group masks (invalid groups are ignored).
    std::vector<uint32_t> gMasks;
    gMasks.reserve(groups.size());
    for (const auto& g : groups) {
        if ((int)g.size() != k_) continue;
        uint32_t m = 0u;
        bool ok = true;
        for (int v : g) {
            auto it = samplePos.find(v);
            if (it == samplePos.end()) { ok = false; break; }
            m |= (1u << it->second);
        }
        if (ok) gMasks.push_back(m);
    }
    if (gMasks.empty()) return false;

    // Enumerate all j-subsets of samples_ as bitmasks and verify each one.
    // We enumerate via a recursive bitmask iteration to avoid building a full vector.
    const int jBits = j_;
    const int sBits = s_;

    // Recursive lambda to iterate all j-subsets of [0..n-1].
    std::function<bool(int, int, uint32_t)> checkAll =
        [&](int start, int remaining, uint32_t acc) -> bool {
        if (remaining == 0) {
            // acc is a j-subset mask.  Count how many groups cover ≥ s_ elements of it.
            int coverCount = 0;
            for (uint32_t gm : gMasks) {
                if (__builtin_popcount(gm & acc) >= sBits) {
                    if (++coverCount >= minCover_) return true;  // j-subset satisfied
                }
            }
            return false;  // j-subset not sufficiently covered
        }
        for (int i = start; i <= n - remaining; ++i) {
            if (!checkAll(i + 1, remaining - 1, acc | (1u << i))) return false;
        }
        return true;
    };

    return checkAll(0, jBits, 0u);
}

// ════════════════════════════════════════════════════════════════════════════════
// Embedded Python CP-SAT solver
// ════════════════════════════════════════════════════════════════════════════════

#ifdef ENABLE_PYTHON_EMBED

// The Python script is compiled into the binary as a string literal.
// Parameters are injected into __main__ before execution; results read back.
static const char* CPSAT_SCRIPT = R"PY(
# 每次执行先清除旧结果，防止多次调用时读到上次的 _groups
_groups = None

import sys, math
from itertools import combinations
try:
    from ortools.sat.python import cp_model as _cpmodel
    _has_ortools = True
except ImportError:
    _has_ortools = False
    print("[CP-SAT] OR-Tools not found; will fall back to greedy.", file=sys.stderr)

def _enum(pool, r):
    return [list(c) for c in combinations(sorted(pool), r)]

def _greedy(k_cands, k_covers, j_to_subs, num_j, min_cover):
    """快速贪心（带反向索引），返回选中的 k-候选索引集合，用作 warm-start hint。"""
    num_subs = max((si for ks in k_covers for si in ks), default=-1) + 1
    sub_to_j = [[] for _ in range(num_subs)]
    for ji, subs in enumerate(j_to_subs):
        for si in subs:
            sub_to_j[si].append(ji)

    cov_count = [0] * num_subs
    j_cnt     = [0] * num_j
    j_sat     = [False] * num_j
    remaining = num_j
    used      = [False] * len(k_cands)
    selected  = set()

    while remaining > 0:
        best_c, best_score = -1, -1
        for c, ks in enumerate(k_covers):
            if used[c]:
                continue
            score = sum(
                1 for si in ks
                if cov_count[si] == 0
                for ji in sub_to_j[si]
                if not j_sat[ji]
            )
            if score > best_score:
                best_score, best_c = score, c
        if best_c == -1 or best_score == 0:
            break
        used[best_c] = True
        selected.add(best_c)
        for si in k_covers[best_c]:
            cov_count[si] += 1
            if cov_count[si] == 1:
                for ji in sub_to_j[si]:
                    if not j_sat[ji]:
                        j_cnt[ji] += 1
                        if j_cnt[ji] >= min_cover:
                            j_sat[ji] = True
                            remaining -= 1
    return selected

def _col_gen(k_cands, k_covers, j_to_subs, num_j, min_cover,
             k_covers_j, cov_for_sub, seed_cols, max_rounds=10):
    """方案5：LP 列生成求 LP 松弛下界，返回 ceil(LP最优值)。"""
    try:
        from ortools.linear_solver import pywraplp
    except ImportError:
        return 0
    cols = set(seed_cols)
    # 确保初始列集可行
    covered = [0] * num_j
    for c in cols:
        for ji in k_covers_j[c]: covered[ji] += 1
    for ji in range(num_j):
        if covered[ji] < min_cover:
            for si in j_to_subs[ji]:
                for c in cov_for_sub[si]:
                    cols.add(c)
                    for jj in k_covers_j[c]: covered[jj] += 1
                    break
                if covered[ji] >= min_cover: break
    lp_lb = 0
    for rnd in range(max_rounds):
        col_list = sorted(cols)
        col_pos  = {c: i for i, c in enumerate(col_list)}
        lp = pywraplp.Solver.CreateSolver('GLOP')
        if not lp: break
        y = [lp.NumVar(0.0, 1.0, f'y{i}') for i in range(len(col_list))]
        constrs = []
        for ji in range(num_j):
            ct = lp.Constraint(float(min_cover), lp.infinity())
            for c in col_list:
                if ji in k_covers_j[c]: ct.SetCoefficient(y[col_pos[c]], 1.0)
            constrs.append(ct)
        obj = lp.Objective()
        for yv in y: obj.SetCoefficient(yv, 1.0)
        obj.SetMinimization()
        if lp.Solve() != pywraplp.Solver.OPTIMAL:
            lp_lb = 0  # 重置，防止残留上一轮的错误值
            break
        lp_val = lp.Objective().Value()
        lp_lb  = max(lp_lb, math.ceil(lp_val - 1e-6))
        duals  = [ct.dual_value() for ct in constrs]
        to_add = sorted(
            (1.0 - sum(duals[ji] for ji in k_covers_j[c]), c)
            for c in range(len(k_cands)) if c not in cols
        )
        new = [c for rc, c in to_add if rc < -1e-6][:50]
        print(f"[CP-SAT][CG] round {rnd+1}: {len(col_list)} cols, "
              f"LP={lp_val:.2f} lb>={lp_lb}, +{len(new)}", file=sys.stderr)
        if not new: break
        cols.update(new)
    return lp_lb

def _solve():
    if not _has_ortools:
        return None

    k_cands = _enum(_samples, _k)
    j_subs  = _enum(_samples, _j)
    s_subs  = _enum(_samples, _s)

    sub_idx     = {tuple(x): i for i, x in enumerate(s_subs)}
    num_subs    = len(s_subs)
    num_j       = len(j_subs)

    j_to_subs = [[sub_idx[tuple(ss)] for ss in _enum(js, _s)] for js in j_subs]
    k_covers  = [[sub_idx[tuple(ss)] for ss in _enum(kc, _s) if tuple(ss) in sub_idx]
                 for kc in k_cands]

    cov_for_sub = [[] for _ in range(num_subs)]
    for c, ks in enumerate(k_covers):
        for si in ks:
            cov_for_sub[si].append(c)

    # 反向索引 s-子集 → j-子集（列生成定价用）
    num_subs_actual = max((si for ks in k_covers for si in ks), default=-1) + 1
    sub_to_j = [[] for _ in range(num_subs_actual)]
    for ji, subs in enumerate(j_to_subs):
        for si in subs: sub_to_j[si].append(ji)
    k_covers_j = [{ji for si in k_covers[c] for ji in sub_to_j[si]}
                  for c in range(len(k_cands))]

    # ── Warm-start hint（SA 优先，greedy 保底）────────────────────────────
    _hint_set = set()
    if '_sa_hint_groups' in globals() and _sa_hint_groups:
        _kc_idx = {tuple(kc): ci for ci, kc in enumerate(k_cands)}
        for _g in _sa_hint_groups:
            _key = tuple(sorted(_g))
            if _key in _kc_idx:
                _hint_set.add(_kc_idx[_key])
        if _hint_set:
            print(f"[CP-SAT] SA hint: {len(_hint_set)} k-groups (upper bound for CP-SAT)", file=sys.stderr)
    if not _hint_set:
        _hint_set = _greedy(k_cands, k_covers, j_to_subs, num_j, _min_cover)
        print(f"[CP-SAT] Greedy hint: {len(_hint_set)} k-groups (upper bound for CP-SAT)", file=sys.stderr)
    hint_set = _hint_set

    mdl = _cpmodel.CpModel()
    x   = [mdl.NewBoolVar(f'x{c}') for c in range(len(k_cands))]

    # ── 方案 A：min_cover=1 专用快速路径（彻底去掉 z 变量）────────────────
    if _min_cover == 1:
        print(f"[CP-SAT] Path A (no-z): {len(k_cands)} x-vars, {num_j} AddBoolOr", file=sys.stderr)
        for i in range(num_j):
            cands = list({c for si in j_to_subs[i] for c in cov_for_sub[si]})
            if not cands:
                mdl.AddBoolOr([mdl.NewBoolVar('__inf__').Not()])
            else:
                mdl.AddBoolOr([x[c] for c in cands])
    else:
        # ── 通用路径（min_cover > 1，保留 z 变量）───────────────────────────
        print(f"[CP-SAT] Path general (z): {len(k_cands)} x-vars, {num_subs} z-vars", file=sys.stderr)
        z = [mdl.NewBoolVar(f'z{si}') for si in range(num_subs)]
        for si in range(num_subs):
            cov = cov_for_sub[si]
            if not cov:
                mdl.Add(z[si] == 0)
            else:
                mdl.AddBoolOr([x[c] for c in cov] + [z[si].Not()])
        for c, ks in enumerate(k_covers):
            for si in ks:
                mdl.AddImplication(x[c], z[si])
        for i in range(num_j):
            mdl.Add(sum(z[si] for si in j_to_subs[i]) >= _min_cover)

    mdl.Minimize(sum(x))

    # ── 方案5：LP 列生成下界约束（加速最优性证明）─────────────────────────
    lp_lb = _col_gen(k_cands, k_covers, j_to_subs, num_j, _min_cover,
                     k_covers_j, cov_for_sub, hint_set, max_rounds=10)
    if lp_lb > 0:
        mdl.Add(sum(x) >= lp_lb)
        print(f"[CP-SAT] LP lower bound: sum(x) >= {lp_lb} added", file=sys.stderr)

    # 注入 hint，让 CP-SAT 从已知可行解出发向下优化
    for c in range(len(k_cands)):
        mdl.AddHint(x[c], 1 if c in hint_set else 0)

    # ── 方案4：CP-SAT 搜索参数调优 ────────────────────────────────────────
    # 所有参数统一放入 pbtxt，避免 parameters_as_pbtxt 赋值覆盖之前单独设置的字段
    slv = _cpmodel.CpSolver()
    slv.parameters_as_pbtxt = (
        f"max_time_in_seconds:{_time_limit}\n"
        "num_workers:8\n"
        "search_branching:PORTFOLIO_WITH_QUICK_RESTART\n"
        "lns_time_limit_factor:2.0\n"
    )
    status = slv.Solve(mdl)

    if status == _cpmodel.OPTIMAL:
        print(f"[CP-SAT] OPTIMAL solution found.", file=sys.stderr)
    elif status == _cpmodel.FEASIBLE:
        print(f"[CP-SAT] FEASIBLE (time limit hit, may not be optimal).", file=sys.stderr)
    else:
        print(f"[CP-SAT] No solution found (status={status}).", file=sys.stderr)
        return None

    return [list(k_cands[c]) for c in range(len(k_cands)) if slv.Value(x[c]) == 1]

_groups = _solve()
)PY";

std::vector<std::vector<int>> SampleSelectSystem::generateCPSAT(int timeLimitSec)
{
    if (samples_.empty()) {
        throw std::runtime_error(
            "No samples selected. Call randomSamples() or inputSamples() first.");
    }

    // Initialise the Python interpreter exactly once per process
    static bool py_init = false;
    if (!py_init) {
        Py_Initialize();
        py_init = true;
    }

    // Get a reference to __main__'s global namespace
    PyObject* main_mod  = PyImport_AddModule("__main__");   // borrowed ref
    PyObject* global_ns = PyModule_GetDict(main_mod);       // borrowed ref

    // ── Inject parameters into __main__ ──────────────────────────────────────

    // _samples  (list of int)
    PyObject* py_samples = PyList_New((Py_ssize_t)samples_.size());
    for (size_t i = 0; i < samples_.size(); ++i)
        PyList_SET_ITEM(py_samples, (Py_ssize_t)i, PyLong_FromLong(samples_[i]));
    PyDict_SetItemString(global_ns, "_samples", py_samples);
    Py_DECREF(py_samples);

    // Scalar parameters
    PyObject* py_k    = PyLong_FromLong(k_);
    PyObject* py_j    = PyLong_FromLong(j_);
    PyObject* py_s    = PyLong_FromLong(s_);
    PyObject* py_mc   = PyLong_FromLong(minCover_);
    PyObject* py_tl   = PyLong_FromLong(timeLimitSec);
    PyDict_SetItemString(global_ns, "_k",          py_k);
    PyDict_SetItemString(global_ns, "_j",          py_j);
    PyDict_SetItemString(global_ns, "_s",          py_s);
    PyDict_SetItemString(global_ns, "_min_cover",  py_mc);
    PyDict_SetItemString(global_ns, "_time_limit", py_tl);
    Py_DECREF(py_k); Py_DECREF(py_j); Py_DECREF(py_s);
    Py_DECREF(py_mc); Py_DECREF(py_tl);

    // ── Run SA/greedy-restarts to generate a warm-start hint for CP-SAT ──────
    // generateOptimalGroups() uses SA for C(n,k)>10000, lazy greedy+restarts
    // otherwise — either way it produces a much tighter upper bound than the
    // single greedy inside the Python script.
    std::cout << ">> Computing SA warm-start hint for CP-SAT...\n";
    {
        auto saGroups = generateOptimalGroups();
        std::cout << ">> SA hint ready: " << saGroups.size() << " k-groups\n";

        PyObject* py_sa = PyList_New((Py_ssize_t)saGroups.size());
        for (size_t i = 0; i < saGroups.size(); ++i) {
            PyObject* pg = PyList_New((Py_ssize_t)saGroups[i].size());
            for (size_t jj = 0; jj < saGroups[i].size(); ++jj)
                PyList_SET_ITEM(pg, (Py_ssize_t)jj,
                                PyLong_FromLong(saGroups[i][jj]));
            PyList_SET_ITEM(py_sa, (Py_ssize_t)i, pg);
        }
        PyDict_SetItemString(global_ns, "_sa_hint_groups", py_sa);
        Py_DECREF(py_sa);
    }

    // ── Execute the embedded Python script ───────────────────────────────────
    std::cout << ">> CP-SAT (embedded Python): C(" << n_ << "," << k_ << ")="
              << C(n_, k_) << " candidates, time_limit=" << timeLimitSec << "s\n";

    PyObject* exec_result = PyRun_String(
        CPSAT_SCRIPT, Py_file_input, global_ns, global_ns);

    if (!exec_result) {
        PyErr_Print();
        std::cerr << "[Warning] CP-SAT Python script threw an exception; "
                     "falling back to greedy.\n";
        return generateOptimalGroups();
    }
    Py_DECREF(exec_result);

    // ── Read back _groups ─────────────────────────────────────────────────────
    PyObject* py_groups = PyDict_GetItemString(global_ns, "_groups"); // borrowed

    if (!py_groups || py_groups == Py_None) {
        std::cerr << "[Warning] CP-SAT returned None (OR-Tools unavailable or "
                     "infeasible); falling back to greedy.\n";
        return generateOptimalGroups();
    }

    std::vector<std::vector<int>> groups;
    Py_ssize_t ng = PyList_Size(py_groups);
    groups.reserve((size_t)ng);
    for (Py_ssize_t i = 0; i < ng; ++i) {
        PyObject* py_group = PyList_GetItem(py_groups, i); // borrowed
        Py_ssize_t gs = PyList_Size(py_group);
        std::vector<int> group;
        group.reserve((size_t)gs);
        for (Py_ssize_t jj = 0; jj < gs; ++jj)
            group.push_back((int)PyLong_AsLong(PyList_GetItem(py_group, jj)));
        groups.push_back(std::move(group));
    }

    std::cout << ">> CP-SAT done: " << groups.size() << " k-groups\n";
    return groups;
}

#else  // ENABLE_PYTHON_EMBED not defined → stub that falls back to greedy

std::vector<std::vector<int>> SampleSelectSystem::generateCPSAT(int /*timeLimitSec*/)
{
    std::cerr << "[Info] CP-SAT not compiled in (ENABLE_PYTHON_EMBED not set); "
                 "using greedy solver.\n";
    return generateOptimalGroups();
}

#endif  // ENABLE_PYTHON_EMBED

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
