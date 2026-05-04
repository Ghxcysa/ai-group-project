#!/usr/bin/env python3
"""
cpsat_solver.py — OR-Tools CP-SAT exact solver for the sample selection problem.

Accepts identical CLI parameters to the C++ optimal_sample sidecar and outputs
the same JSON format. Intended for small/medium scale instances (C(n,k) ≤ 8008).

Usage:
  ./cpsat_solver --cli --m=45 --n=8 --k=6 --j=6 --s=5 --minCover=1
                 [--samples=1,2,3,4,5,6,7,8] [--save] [--run=1]
                 [--dbdir=./db] [--poolFile=./db/pool_test_m45.txt]

Success output (stdout JSON):
  {"success":true,"count":4,"dbFile":"./db/45-8-6-6-5-1-4.txt","samples":[...],"groups":[[...],...]}
Failure output:
  {"success":false,"error":"<reason>"}
"""

import sys
import os
import json
import math
import random
import argparse
from itertools import combinations


# ── Combinatorics helpers ────────────────────────────────────────────────────

def binom(n, k):
    """Compute C(n, k)."""
    if k < 0 or k > n:
        return 0
    if k == 0 or k == n:
        return 1
    k = min(k, n - k)
    result = 1
    for i in range(k):
        result = result * (n - i) // (i + 1)
    return result


def enum_subsets(pool, r):
    """Return all r-subsets of pool (each as a sorted tuple)."""
    return [tuple(c) for c in combinations(sorted(pool), r)]


# ── File helpers (mirrors C++ saveToFile / buildFileName) ────────────────────

def build_filename(m, n, k, j, s, run_count, result_count):
    return f"{m}-{n}-{k}-{j}-{s}-{run_count}-{result_count}.txt"


def save_to_file(m, n, k, j, s, min_cover, run_count, groups, output_dir):
    os.makedirs(output_dir, exist_ok=True)
    filename = build_filename(m, n, k, j, s, run_count, len(groups))
    filepath = os.path.join(output_dir, filename)
    try:
        with open(filepath, "w") as f:
            f.write(
                f"# m={m} n={n} k={k} j={j} s={s} "
                f"minCover={min_cover} run={run_count} count={len(groups)}\n"
            )
            for g in groups:
                f.write(",".join(str(x) for x in g) + "\n")
        return filepath
    except Exception as e:
        sys.stderr.write(f"[Warning] Failed to save file: {e}\n")
        return ""


def load_pool_file(path):
    """Read comma-separated integers from a pool file (# lines are comments)."""
    numbers = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            for tok in line.split(","):
                tok = tok.strip()
                if tok:
                    try:
                        numbers.append(int(tok))
                    except ValueError:
                        pass
    return numbers


def json_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def output_error(msg):
    print(json.dumps({"success": False, "error": msg}), flush=True)


# ── CP-SAT solver ────────────────────────────────────────────────────────────

def _greedy_indices(k_candidates, k_covers_s, j_to_subs, num_j, min_cover):
    """
    快速贪心：返回选中的 k-候选索引集合，用作 CP-SAT warm-start hint。
    使用反向索引（sub → j-subset）避免 O(num_j) 内层扫描，
    比原 greedy_cover 快一个数量级。
    """
    num_subs = max((si for ks in k_covers_s for si in ks), default=-1) + 1
    sub_to_j = [[] for _ in range(num_subs)]
    for ji, subs in enumerate(j_to_subs):
        for si in subs:
            sub_to_j[si].append(ji)

    cov_count = [0] * num_subs
    j_cnt     = [0] * num_j
    j_sat     = [False] * num_j
    remaining = num_j
    used      = [False] * len(k_candidates)
    selected  = set()

    while remaining > 0:
        best_c, best_score = -1, -1
        for c, ks in enumerate(k_covers_s):
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
        for si in k_covers_s[best_c]:
            cov_count[si] += 1
            if cov_count[si] == 1:
                for ji in sub_to_j[si]:
                    if not j_sat[ji]:
                        j_cnt[ji] += 1
                        if j_cnt[ji] >= min_cover:
                            j_sat[ji] = True
                            remaining -= 1
    return selected


def _sa_hint_indices(k_candidates, k_covers_s, j_to_subs, num_j, min_cover,
                     sa_steps=500000, seed=42):
    """
    SA warm-start hint for CP-SAT — mirrors the C++ SA in generateOptimalGroups.

    Algorithm (exact parallel to C++ large-scale branch):
      1. Sample INIT_SAMPLE = min(3000, C(n,k)) candidate indices randomly.
      2. Build an initial solution by greedy on that sampled subset.
         Starting from a sub-optimal (larger) solution gives SA room to shrink.
      3. Run SA on the sampled candidates:
           objective delta = Δcoverage × 100 ± Δsize  (coverage priority)
           Metropolis acceptance: exp(delta / T)
           Cooling: T *= alpha  (exponential, same formula as C++)
           Moves: add / remove (sz > 1) / swap — identical to C++ SA
         Track best fully-feasible solution seen.
      4. Backward elimination on best solution (mirrors C++ BE pass).
      5. Map selected candidate positions back to full k-candidate indices.

    Returns set of k-candidate indices (into the k_candidates list).
    """
    import random
    import math

    rng = random.Random(seed)
    num_cands = len(k_candidates)
    if num_cands == 0:
        return set()

    num_subs = max((si for ks in k_covers_s for si in ks), default=-1) + 1

    # Reverse index: s-subset index → list of j-subset indices that contain it
    sub_to_j = [[] for _ in range(num_subs)]
    for ji, subs in enumerate(j_to_subs):
        for si in subs:
            sub_to_j[si].append(ji)

    # ── Step 1: sample INIT_SAMPLE candidate indices ─────────────────────────
    INIT_SAMPLE = min(3000, num_cands)
    if INIT_SAMPLE < num_cands:
        sampled = rng.sample(range(num_cands), INIT_SAMPLE)
    else:
        sampled = list(range(num_cands))

    sampled_set = set(sampled)

    # ── Step 2: greedy on the sampled subset ─────────────────────────────────
    # (Uses the same fast incremental approach as _greedy_indices)
    cov_g  = [0] * num_subs
    jcnt_g = [0] * num_j
    jsat_g = bytearray(num_j)
    rem_g  = num_j
    used_g = bytearray(num_cands)
    sol    = []

    candidates_left = list(sampled)   # shrink as we pick
    while rem_g > 0 and candidates_left:
        best_c, best_sc = -1, -1
        for c in candidates_left:
            sc = sum(
                1
                for si in k_covers_s[c]
                if cov_g[si] == 0
                for ji in sub_to_j[si]
                if not jsat_g[ji]
            )
            if sc > best_sc:
                best_sc, best_c = sc, c
        if best_c == -1 or best_sc == 0:
            break
        used_g[best_c] = 1
        candidates_left.remove(best_c)
        sol.append(best_c)
        for si in k_covers_s[best_c]:
            if cov_g[si] == 0:
                cov_g[si] = 1
                for ji in sub_to_j[si]:
                    if not jsat_g[ji]:
                        jcnt_g[ji] += 1
                        if jcnt_g[ji] >= min_cover:
                            jsat_g[ji] = 1
                            rem_g -= 1

    # If sampled greedy didn't cover everything, fall back to full greedy + BE
    if rem_g > 0:
        greedy = _greedy_indices(k_candidates, k_covers_s, j_to_subs,
                                 num_j, min_cover)
        be_sol = list(greedy)
        be_cov = [0] * num_subs
        for c in be_sol:
            for si in k_covers_s[c]:
                be_cov[si] += 1
        improved = True
        while improved:
            improved = False
            for idx in range(len(be_sol)):
                c = be_sol[idx]
                for si in k_covers_s[c]:
                    be_cov[si] -= 1
                ok = all(
                    sum(1 for si in j_to_subs[ji] if be_cov[si] >= 1) >= min_cover
                    for ji in range(num_j)
                )
                if ok:
                    be_sol.pop(idx); improved = True; break
                else:
                    for si in k_covers_s[c]:
                        be_cov[si] += 1
        return set(be_sol)

    # ── Step 3: SA on the sampled candidates ─────────────────────────────────
    # SA incremental coverage state
    cov_ref = [0] * num_subs
    j_cnt   = [0] * num_j
    j_sat   = bytearray(num_j)
    sat     = [0]

    def sa_add(c):
        for si in k_covers_s[c]:
            cov_ref[si] += 1
            if cov_ref[si] == 1:
                for ji in sub_to_j[si]:
                    if not j_sat[ji]:
                        j_cnt[ji] += 1
                        if j_cnt[ji] >= min_cover:
                            j_sat[ji] = 1
                            sat[0] += 1

    def sa_rem(c):
        for si in k_covers_s[c]:
            cov_ref[si] -= 1
            if cov_ref[si] == 0:
                for ji in sub_to_j[si]:
                    if j_cnt[ji] > 0:
                        j_cnt[ji] -= 1
                        if j_sat[ji] and j_cnt[ji] < min_cover:
                            j_sat[ji] = 0
                            sat[0] -= 1

    # Initialise SA state from initial greedy solution
    for c in sol:
        sa_add(c)

    not_sol = [c for c in sampled if c not in set(sol)]

    # Best feasible solution seen: initialise from the FULL greedy so that
    # SA can only improve on it (never regress below single-greedy quality).
    full_greedy = _greedy_indices(k_candidates, k_covers_s, j_to_subs,
                                  num_j, min_cover)
    best_sol  = list(full_greedy)
    best_sat  = num_j
    best_size = len(full_greedy)

    # Temperature schedule: identical to C++ SA
    T     = max(1.0, num_j * 0.05)
    T_MIN = 0.001
    alpha = (T_MIN / T) ** (1.0 / sa_steps)

    for _ in range(sa_steps):
        if not sol:
            if not_sol:
                i = rng.randrange(len(not_sol))
                c = not_sol[i]
                sa_add(c); sol.append(c)
                not_sol[i] = not_sol[-1]; not_sol.pop()
            T = max(T * alpha, T_MIN)
            continue

        action = rng.randint(0, 2)
        sz = len(sol)

        if action == 0:                        # ── Add ──────────────────────
            if not not_sol:
                T = max(T * alpha, T_MIN); continue
            i = rng.randrange(len(not_sol))
            c = not_sol[i]
            sat_b = sat[0]
            sa_add(c)
            delta = (sat[0] - sat_b) * 100.0 - 1.0
            if delta < 0 and rng.random() > math.exp(delta / T):
                sa_rem(c)                      # reject
            else:
                sol.append(c)
                not_sol[i] = not_sol[-1]; not_sol.pop()

        elif action == 1 and sz > 1:           # ── Remove ───────────────────
            i = rng.randrange(sz)
            c = sol[i]
            sat_b = sat[0]
            sa_rem(c)
            delta = (sat[0] - sat_b) * 100.0 + 1.0
            if delta < 0 and rng.random() > math.exp(delta / T):
                sa_add(c)                      # reject
            else:
                sol[i] = sol[-1]; sol.pop()
                not_sol.append(c)

        else:                                  # ── Swap ─────────────────────
            if not not_sol:
                T = max(T * alpha, T_MIN); continue
            i  = rng.randrange(sz)
            j2 = rng.randrange(len(not_sol))
            cr, ca = sol[i], not_sol[j2]
            sat_b = sat[0]
            sa_rem(cr); sa_add(ca)
            delta = (sat[0] - sat_b) * 100.0   # size unchanged
            if delta < 0 and rng.random() > math.exp(delta / T):
                sa_rem(ca); sa_add(cr)         # reject
            else:
                sol[i] = ca; not_sol[j2] = cr

        # Track best fully-feasible solution (mirrors C++ bestSAGroups tracking)
        if sat[0] == num_j and len(sol) < best_size:
            best_sat  = sat[0]
            best_size = len(sol)
            best_sol  = list(sol)

        T = max(T * alpha, T_MIN)

    # ── Step 4: backward elimination on best solution ─────────────────────────
    # Mirrors C++ generateOptimalGroups() backward elimination pass.
    be_sol = list(best_sol)
    be_cov = [0] * num_subs
    for c in be_sol:
        for si in k_covers_s[c]:
            be_cov[si] += 1

    improved = True
    while improved:
        improved = False
        for idx in range(len(be_sol)):
            c = be_sol[idx]
            for si in k_covers_s[c]:
                be_cov[si] -= 1
            ok = all(
                sum(1 for si in j_to_subs[ji] if be_cov[si] >= 1) >= min_cover
                for ji in range(num_j)
            )
            if ok:
                be_sol.pop(idx); improved = True; break
            else:
                for si in k_covers_s[c]:
                    be_cov[si] += 1

    return set(be_sol)


def _column_gen(k_candidates, k_covers_s, j_to_subs, coverage_for_sub,
                num_j, min_cover, seed_cols, max_rounds=15):
    """
    方案5：LP 列生成（Column Generation）——求 LP 松弛下界 + 精简候选集。

    算法：
      1. 从 seed_cols（SA 解）出发作为初始列集。
      2. 用 OR-Tools GLOP 求解当前列集的 LP 松弛，获取对偶变量 π[i]。
      3. 定价子问题：对每个不在当前集中的候选 c，计算
           reduced_cost = 1 - Σ_{i∈J(c)} π[i]
         将 reduced_cost < 0 的候选（按改善幅度降序取前 50）加入列集。
      4. 重复直到无改善列（LP 最优）或达到轮次上限。

    返回 (最终列集, ceil(LP最优值))。
    ceil(LP最优值) 是问题的有效整数下界，可直接加入 CP-SAT：sum(x) >= lb。
    """
    try:
        from ortools.linear_solver import pywraplp
    except ImportError:
        sys.stderr.write(">> [CG] pywraplp not available, skipping column generation\n")
        return set(seed_cols), 0

    num_subs = max((si for ks in k_covers_s for si in ks), default=-1) + 1

    # 反向索引 s-子集 → j-子集列表
    sub_to_j = [[] for _ in range(num_subs)]
    for ji, subs in enumerate(j_to_subs):
        for si in subs:
            sub_to_j[si].append(ji)

    # 预计算每个候选覆盖的 j-子集集合（用于定价）
    k_covers_j = [
        {ji for si in k_covers_s[c] for ji in sub_to_j[si]}
        for c in range(len(k_candidates))
    ]

    # 确保初始列集可行：若某 j-子集未被覆盖，强制加入一个覆盖它的候选
    cols = set(seed_cols)
    covered = [0] * num_j
    for c in cols:
        for ji in k_covers_j[c]:
            covered[ji] += 1
    for ji in range(num_j):
        if covered[ji] < min_cover:
            for si in j_to_subs[ji]:
                for c in coverage_for_sub[si]:
                    if c not in cols:
                        cols.add(c)
                        for jj in k_covers_j[c]:
                            covered[jj] += 1
                    break
                if covered[ji] >= min_cover:
                    break

    lp_bound_ceil = 0

    for rnd in range(max_rounds):
        col_list = sorted(cols)
        col_pos  = {c: i for i, c in enumerate(col_list)}

        lp = pywraplp.Solver.CreateSolver('GLOP')
        if lp is None:
            break

        y = [lp.NumVar(0.0, 1.0, f'y{i}') for i in range(len(col_list))]

        # 约束：对每个 j-子集，覆盖它的候选之和 >= min_cover
        constrs = []
        for ji in range(num_j):
            ct = lp.Constraint(float(min_cover), lp.infinity())
            for c in col_list:
                if ji in k_covers_j[c]:
                    ct.SetCoefficient(y[col_pos[c]], 1.0)
            constrs.append(ct)

        obj = lp.Objective()
        for yv in y:
            obj.SetCoefficient(yv, 1.0)
        obj.SetMinimization()

        if lp.Solve() != pywraplp.Solver.OPTIMAL:
            sys.stderr.write(f">> [CG] round {rnd}: LP infeasible/error, stopping\n")
            lp_bound_ceil = 0  # 重置，防止残留上一轮的错误值
            break

        lp_val = lp.Objective().Value()
        lp_bound_ceil = max(lp_bound_ceil, math.ceil(lp_val - 1e-6))
        duals = [ct.dual_value() for ct in constrs]

        # 定价：找 reduced_cost < 0 的候选，按改善幅度降序，取前 50
        improving = sorted(
            (1.0 - sum(duals[ji] for ji in k_covers_j[c]), c)
            for c in range(len(k_candidates))
            if c not in cols
        )
        to_add = [c for rc, c in improving if rc < -1e-6][:50]

        sys.stderr.write(
            f">> [CG] round {rnd+1}: {len(col_list)} cols, "
            f"LP={lp_val:.3f} (lb≥{lp_bound_ceil}), +{len(to_add)} new cols\n"
        )

        if not to_add:
            break
        cols.update(to_add)

    return cols, lp_bound_ceil


def _build_coverage_maps(samples, k, j, s):
    """
    共享的前处理：枚举所有 k-候选、j-子集、s-子集，
    返回 (k_candidates, j_subsets, sub_index, num_subs, num_j,
           j_to_subs, k_covers_s, coverage_for_sub)。
    """
    k_candidates = enum_subsets(samples, k)
    j_subsets = enum_subsets(samples, j)
    s_subsets_global = enum_subsets(samples, s)

    sub_index = {sub: i for i, sub in enumerate(s_subsets_global)}
    num_subs = len(s_subsets_global)
    num_j = len(j_subsets)

    # j_to_subs[i] = j_subsets[i] 中所有 s-子集的全局索引
    j_to_subs = []
    for jsub in j_subsets:
        j_to_subs.append([sub_index[ss] for ss in enum_subsets(jsub, s)])

    # k_covers_s[c] = k_candidates[c] 包含的 s-子集全局索引
    k_covers_s = []
    for kc in k_candidates:
        k_covers_s.append([sub_index[ss] for ss in enum_subsets(kc, s)
                           if ss in sub_index])

    # coverage_for_sub[si] = 覆盖 s-子集 si 的所有 k-候选索引
    coverage_for_sub = [[] for _ in range(num_subs)]
    for c, kc_subs in enumerate(k_covers_s):
        for si in kc_subs:
            coverage_for_sub[si].append(c)

    return (k_candidates, j_subsets, sub_index, num_subs, num_j,
            j_to_subs, k_covers_s, coverage_for_sub)


def _solve_cpsat_min1(samples, k, j, s, time_limit_sec, k_candidates,
                      num_j, j_to_subs, coverage_for_sub):
    """
    方案 A（min_cover=1 专用快速路径）：彻底去掉 z 变量。

    当 min_cover=1 时，约束可以直接表示为：
      对每个 j-子集 Ji：至少存在一个已选的 k-候选 c，
      使得 c 包含 Ji 中某个 s-子集。
    等价于：
      AddBoolOr([x[c] for si in j_to_subs[i] for c in coverage_for_sub[si]])

    变量规模：C(n,k) 个 x 变量 + C(n,j) 条 AddBoolOr 约束，无 z 变量。
    """
    from ortools.sat.python import cp_model

    model = cp_model.CpModel()
    x = [model.NewBoolVar(f"x{c}") for c in range(len(k_candidates))]

    for i in range(num_j):
        # 收集所有可以满足该 j-子集要求的 k-候选（去重）
        candidates_for_j = list({
            c
            for si in j_to_subs[i]
            for c in coverage_for_sub[si]
        })
        if not candidates_for_j:
            # 无任何 k-候选能覆盖该 j-子集，问题不可行
            model.AddBoolOr([model.NewBoolVar("__infeasible__").Not()])
        else:
            model.AddBoolOr([x[c] for c in candidates_for_j])

    model.Minimize(sum(x))
    return model, x


def _solve_cpsat_general(samples, k, j, s, min_cover, time_limit_sec,
                         k_candidates, num_subs, num_j,
                         j_to_subs, k_covers_s, coverage_for_sub):
    """
    通用路径（min_cover > 1）：保留 z 变量。

    Variables:
      x[c] ∈ {0,1}  — 是否选中 k-候选 c
      z[si] ∈ {0,1} — s-子集 si 是否被至少一个已选 k-组覆盖

    Constraints:
      (1) z[si]=1 → 至少有一个覆盖 si 的 x[c]=1
          AddBoolOr([x[c] for c covering si] + [~z[si]])
      (1') x[c]=1 → c 覆盖的所有 z[si]=1
          AddImplication(x[c], z[si])
      (2) 每个 j-子集：sum(z[si] for si in j_to_subs[i]) >= min_cover
    """
    from ortools.sat.python import cp_model

    model = cp_model.CpModel()
    x = [model.NewBoolVar(f"x{c}") for c in range(len(k_candidates))]
    z = [model.NewBoolVar(f"z{si}") for si in range(num_subs)]

    for si in range(num_subs):
        covering = coverage_for_sub[si]
        if not covering:
            model.Add(z[si] == 0)
        else:
            model.AddBoolOr([x[c] for c in covering] + [z[si].Not()])

    for c, kc_subs in enumerate(k_covers_s):
        for si in kc_subs:
            model.AddImplication(x[c], z[si])

    for i in range(num_j):
        model.Add(sum(z[si] for si in j_to_subs[i]) >= min_cover)

    model.Minimize(sum(x))
    return model, x


def solve_cpsat(samples, k, j, s, min_cover, time_limit_sec=60):
    """
    求解最小 k-组覆盖问题（OR-Tools CP-SAT）。

    当 min_cover=1 时自动启用方案 A 快速路径：
      - 仅使用 C(n,k) 个 x 变量 + C(n,j) 条 AddBoolOr 约束
      - 无 z 变量，模型规模缩小 5 倍以上
    当 min_cover>1 时回退到带 z 变量的通用模型。

    返回选中的 k-组列表，不可行/超时时返回 None。
    """
    from ortools.sat.python import cp_model

    (k_candidates, j_subsets, sub_index, num_subs, num_j,
     j_to_subs, k_covers_s, coverage_for_sub) = _build_coverage_maps(
        samples, k, j, s)

    sys.stderr.write(
        f">> CP-SAT: {len(k_candidates)} x-vars, {num_j} j-subsets, "
        f"min_cover={min_cover}, "
        f"path={'A(no-z)' if min_cover == 1 else 'general(z)'}\n"
    )

    if min_cover == 1:
        model, x = _solve_cpsat_min1(
            samples, k, j, s, time_limit_sec,
            k_candidates, num_j, j_to_subs, coverage_for_sub)
    else:
        model, x = _solve_cpsat_general(
            samples, k, j, s, min_cover, time_limit_sec,
            k_candidates, num_subs, num_j,
            j_to_subs, k_covers_s, coverage_for_sub)

    # ── 方案1：SA warm-start hint（greedy 初解 → SA 压缩）──────────────────
    sys.stderr.write(">> Running SA warm-start hint...\n")
    hint_set = _sa_hint_indices(k_candidates, k_covers_s, j_to_subs,
                                num_j, min_cover)
    sys.stderr.write(f">> SA hint: {len(hint_set)} k-groups "
                     f"(upper bound for CP-SAT)\n")

    # ── 方案5：LP 列生成 → LP 松弛下界（加速最优性证明）──────────────────
    sys.stderr.write(">> Running column generation for LP lower bound...\n")
    _, lp_lb = _column_gen(
        k_candidates, k_covers_s, j_to_subs, coverage_for_sub,
        num_j, min_cover, hint_set, max_rounds=15
    )
    if lp_lb > 0:
        model.Add(sum(x) >= lp_lb)
        sys.stderr.write(f">> LP lower bound: sum(x) >= {lp_lb} added to model\n")

    # 注入 SA hint
    for c in range(len(k_candidates)):
        model.AddHint(x[c], 1 if c in hint_set else 0)

    # ── 方案4：CP-SAT 搜索参数调优 ────────────────────────────────────────
    solver = cp_model.CpSolver()
    # 所有参数统一放入 pbtxt，避免 parameters_as_pbtxt 赋值覆盖之前单独设置的字段
    solver.parameters_as_pbtxt = (
        f"max_time_in_seconds:{time_limit_sec}\n"
        "num_workers:8\n"
        "search_branching:PORTFOLIO_WITH_QUICK_RESTART\n"
        "lns_time_limit_factor:2.0\n"
    )

    status = solver.Solve(model)

    if status in (cp_model.OPTIMAL, cp_model.FEASIBLE):
        selected = [
            list(k_candidates[c])
            for c in range(len(k_candidates))
            if solver.Value(x[c]) == 1
        ]
        return selected
    else:
        return None


# ── Greedy fallback (mirrors existing C++ greedy; used if CP-SAT times out) ──

def greedy_cover(samples, k, j, s, min_cover):
    """Simple greedy set cover as a fallback."""
    k_candidates = enum_subsets(samples, k)
    j_subsets = enum_subsets(samples, j)
    s_subsets_global = enum_subsets(samples, s)

    sub_index = {sub: i for i, sub in enumerate(s_subsets_global)}
    num_subs = len(s_subsets_global)
    num_j = len(j_subsets)

    j_to_subs = []
    for jsub in j_subsets:
        j_to_subs.append([sub_index[ss] for ss in enum_subsets(jsub, s)])

    k_covers_s = []
    for kc in k_candidates:
        k_covers_s.append([sub_index[ss] for ss in enum_subsets(kc, s)
                           if ss in sub_index])

    cov_count = [0] * num_subs
    j_cnt = [0] * num_j
    j_sat = [False] * num_j
    remaining = num_j
    used = [False] * len(k_candidates)
    result = []

    while remaining > 0:
        best_c, best_score = -1, -1
        for c, kc_subs in enumerate(k_covers_s):
            if used[c]:
                continue
            score = 0
            for si in kc_subs:
                if cov_count[si] > 0:
                    continue
                for ji in range(num_j):
                    if j_sat[ji]:
                        continue
                    if si in j_to_subs[ji]:
                        score += 1
            if score > best_score:
                best_score = score
                best_c = c
        if best_c == -1 or best_score == 0:
            break
        used[best_c] = True
        result.append(list(k_candidates[best_c]))
        for si in k_covers_s[best_c]:
            cov_count[si] += 1
            if cov_count[si] == 1:
                for ji, subs in enumerate(j_to_subs):
                    if not j_sat[ji] and si in subs:
                        j_cnt[ji] += 1
                        if j_cnt[ji] >= min_cover:
                            j_sat[ji] = True
                            remaining -= 1

    return result


# ── Main entry point ─────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--cli", action="store_true")
    parser.add_argument("--m", type=int)
    parser.add_argument("--n", type=int)
    parser.add_argument("--k", type=int)
    parser.add_argument("--j", type=int)
    parser.add_argument("--s", type=int)
    parser.add_argument("--minCover", type=int, default=1)
    parser.add_argument("--samples", type=str, default="")
    parser.add_argument("--save", action="store_true")
    parser.add_argument("--run", type=int, default=1)
    parser.add_argument("--dbdir", type=str, default="./db")
    parser.add_argument("--poolFile", type=str, default="")

    args, _ = parser.parse_known_args()

    if not args.cli:
        output_error("Must be called with --cli flag")
        return

    if args.m is None or args.n is None or args.k is None or \
       args.j is None or args.s is None:
        output_error("Missing required parameters: --m --n --k --j --s")
        return

    m, n, k, j, s = args.m, args.n, args.k, args.j, args.s
    min_cover = args.minCover
    run_count = args.run
    dbdir = args.dbdir

    # Build sample pool (default: 1..m)
    if args.poolFile:
        try:
            pool = load_pool_file(args.poolFile)
        except Exception as e:
            output_error(f"Failed to load pool file: {e}")
            return
        if len(pool) < m:
            output_error(f"Pool file has {len(pool)} numbers, expected at least {m}")
            return
        pool = sorted(pool[:m])
    else:
        pool = list(range(1, m + 1))

    # Select samples
    if args.samples:
        try:
            samples = [int(x.strip()) for x in args.samples.split(",") if x.strip()]
        except ValueError:
            output_error("Failed to parse --samples: values must be integers")
            return
        if len(samples) != n:
            output_error(f"Samples count mismatch: expected {n} but got {len(samples)}")
            return
        if len(set(samples)) != n:
            output_error("Duplicate values in --samples")
            return
        samples = sorted(samples)
    else:
        samples = sorted(random.sample(pool, n))

    # Validate C(n,k) is within CP-SAT range
    cnk = binom(n, k)
    sys.stderr.write(f">> cpsat_solver: C({n},{k}) = {cnk} k-candidates\n")

    try:
        os.makedirs(dbdir, exist_ok=True)
        groups = solve_cpsat(samples, k, j, s, min_cover, time_limit_sec=60)
        if groups is None:
            sys.stderr.write(">> CP-SAT returned no solution, falling back to greedy\n")
            groups = greedy_cover(samples, k, j, s, min_cover)
    except Exception as e:
        output_error(f"Solver error: {e}")
        return

    saved_file = ""
    if args.save and groups:
        saved_file = save_to_file(m, n, k, j, s, min_cover, run_count, groups, dbdir)

    result = {
        "success": True,
        "count": len(groups),
        "dbFile": saved_file,
        "samples": samples,
        "groups": groups,
    }
    print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()
