import { useState, useCallback, useMemo } from "react";
import { Info } from "lucide-react";
import CogIcon from "@/assets/icons/line-md--cog-loop.svg?react";
import ConfirmIcon from "@/assets/icons/line-md--confirm-circle-twotone.svg?react";
import CloseCircleIcon from "@/assets/icons/line-md--close-circle.svg?react";
import DownloadIcon from "@/assets/icons/line-md--download-loop.svg?react";
import PlayIcon from "@/assets/icons/line-md--play.svg?react";
import RegenerateIcon from "@/assets/icons/line-md--switch.svg?react";
import ExternalLinkIcon from "@/assets/icons/line-md--external-link-rounded.svg?react";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Card, CardContent, CardHeader, CardTitle, CardDescription } from "@/components/ui/card";
import { Badge } from "@/components/ui/badge";
import { ScrollArea } from "@/components/ui/scroll-area";
import { Separator } from "@/components/ui/separator";
import GroupTable from "@/components/GroupTable";
import { runGenerate, listPools, type GenerateResult, type PoolFileInfo } from "@/lib/tauri";
import { useEffect } from "react";

// ── Algorithm info derived from n ────────────────────────────────────────────

function binom(n: number, k: number): number {
  if (k < 0 || k > n) return 0;
  k = Math.min(k, n - k);
  let r = 1;
  for (let i = 0; i < k; i++) r = (r * (n - i)) / (i + 1);
  return Math.round(r);
}

interface AlgoInfo {
  name: string;
  label: string;
  description: string;
  color: string; // tailwind bg class for the badge
}

function getAlgoInfo(n: number, k: number, minCover: number): AlgoInfo {
  if (minCover === 1) {
    return {
      name: "Portfolio",
      label: "Set-cover Portfolio",
      description: `Greedy variants + row-weighted LNS · C(${n},${k}) = ${binom(n, k)} · certified cache/exact for n=11..16`,
      color: "bg-indigo-100 text-indigo-800 dark:bg-indigo-900/40 dark:text-indigo-300",
    };
  }
  return {
    name: "Generalized Portfolio",
    label: "Generalized Portfolio",
    description: `minCover=${minCover} · generalized coverage · C(${n},${k}) = ${binom(n, k)} · certified cache/exact for n=11..16`,
    color: "bg-blue-100 text-blue-800 dark:bg-blue-900/40 dark:text-blue-300",
  };
}

interface ParamField {
  key: keyof Params;
  label: string;
  min: number;
  max: number;
  hint: string;
}

interface Params {
  m: number;
  n: number;
  k: number;
  j: number;
  s: number;
  minCover: number;
}

const PARAM_FIELDS: ParamField[] = [
  { key: "m", label: "m (total samples)", min: 45, max: 54, hint: "45 ~ 54" },
  { key: "n", label: "n (selected samples)", min: 7, max: 25, hint: "7 ~ 25" },
  { key: "k", label: "k (group size)", min: 4, max: 7, hint: "4 ~ 7, default 6" },
  { key: "j", label: "j (cover subset size)", min: 3, max: 7, hint: "s ≤ j ≤ k" },
  { key: "s", label: "s (min intersection)", min: 3, max: 7, hint: "3 ≤ s ≤ j" },
  { key: "minCover", label: "minCover (min coverage)", min: 1, max: 20, hint: "usually 1" },
];

type SampleMode = "random" | "manual" | "pool";

interface Props {
  dbDir: string;
}

export default function Generate({ dbDir }: Props) {
  const [params, setParams] = useState<Params>({
    m: 45, n: 8, k: 6, j: 6, s: 5, minCover: 1,
  });
  const [sampleMode, setSampleMode] = useState<SampleMode>("random");
  const [manualInput, setManualInput] = useState("");
  const [selectedPool, setSelectedPool] = useState<string>("");
  const [pools, setPools] = useState<PoolFileInfo[]>([]);
  const [runCount, setRunCount] = useState(1);
  const [autoSave, setAutoSave] = useState(false);

  // Advanced solver settings
  const [timeLimit, setTimeLimit] = useState(120);
  const [threads, setThreads] = useState(0);           // 0 = auto
  const [noIncumbentCache, setNoIncumbentCache] = useState(false);
  const [seed, setSeed] = useState(0);                 // 0 = random

  const [loading, setLoading] = useState(false);
  const [result, setResult] = useState<GenerateResult | null>(null);
  const [error, setError] = useState<string>("");

  // Derived: algorithm that will be / was used
  const algoInfo = useMemo(
    () => getAlgoInfo(params.n, params.k, params.minCover),
    [params.n, params.k, params.minCover],
  );

  // Load pool file list
  useEffect(() => {
    listPools(dbDir)
      .then(setPools)
      .catch(() => setPools([]));
  }, [dbDir]);

  const setParam = (key: keyof Params, val: string) => {
    const num = parseInt(val, 10);
    if (!isNaN(num)) setParams((p) => ({ ...p, [key]: num }));
  };

  const validate = (): string => {
    const { m, n, k, j, s } = params;
    if (m < 45 || m > 54) return "m must be between 45 and 54";
    if (n < 7 || n > 25) return "n must be between 7 and 25";
    if (k < 4 || k > 7) return "k must be between 4 and 7";
    if (j < s || j > k) return "must satisfy s ≤ j ≤ k";
    if (s < 3) return "s must be ≥ 3";
    if (sampleMode === "manual") {
      const nums = manualInput.trim().split(/[\s,]+/).map(Number);
      if (nums.length !== n) return `Manual input requires ${n} samples, got ${nums.length}`;
      if (nums.some((v) => isNaN(v) || v < 1 || v > m)) return `Sample numbers must be between 1 and ${m}`;
      const uniq = new Set(nums);
      if (uniq.size !== nums.length) return "Duplicate numbers in sample input";
    }
    return "";
  };

  const handleGenerate = useCallback(async () => {
    const validErr = validate();
    if (validErr) { setError(validErr); return; }
    setError("");
    setLoading(true);
    setResult(null);

    try {
      let samples: number[] | undefined;
      if (sampleMode === "manual") {
        samples = manualInput.trim().split(/[\s,]+/).map(Number);
      }

      const res = await runGenerate({
        ...params,
        samples,
        save: autoSave,
        run: runCount,
        dbdir: dbDir,
        poolFile: sampleMode === "pool" ? selectedPool : undefined,
        timeLimit: timeLimit > 0 ? timeLimit : 120,
        threads: threads > 0 ? threads : undefined,
        noIncumbentCache: noIncumbentCache || undefined,
        seed: seed > 0 ? seed : undefined,
      });
      setResult(res);
      if (!res.success) setError(res.error || "Generation failed");
    } catch (e: unknown) {
      setError(String(e));
    } finally {
      setLoading(false);
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [params, sampleMode, manualInput, selectedPool, autoSave, runCount, dbDir,
      timeLimit, threads, noIncumbentCache, seed]);

  const sampleSet = result ? new Set(result.samples) : new Set<number>();

  return (
    <div className="flex h-full overflow-hidden">
      {/* ── Left: Parameter Panel ── */}
      <aside className="flex w-72 flex-col border-r">
        <div className="flex h-14 items-center border-b px-4">
          <h1 className="text-base font-semibold">Parameters</h1>
        </div>
        <ScrollArea className="flex-1 px-4 py-4">
          <div className="space-y-4">
            {PARAM_FIELDS.map(({ key, label, min, max, hint }) => (
              <div key={key} className="space-y-1">
                <Label htmlFor={key} className="flex items-center gap-1">
                  {label}
                  <span className="text-xs text-muted-foreground">({hint})</span>
                </Label>
                <Input
                  id={key}
                  type="number"
                  min={min}
                  max={max}
                  value={params[key]}
                  onChange={(e) => setParam(key, e.target.value)}
                />
              </div>
            ))}

            <Separator />

            {/* Sample Selection Mode */}
            <div className="space-y-2">
              <Label>Sample Selection</Label>
              <div className="space-y-1.5">
                {(["random", "manual", "pool"] as SampleMode[]).map((mode) => (
                  <label
                    key={mode}
                    className="flex cursor-pointer items-center gap-2 text-sm"
                  >
                    <input
                      type="radio"
                      name="sampleMode"
                      value={mode}
                      checked={sampleMode === mode}
                      onChange={() => setSampleMode(mode)}
                      className="accent-primary"
                    />
                    {mode === "random" && "Random"}
                    {mode === "manual" && "Manual Input"}
                    {mode === "pool" && "From Pool"}
                  </label>
                ))}
              </div>

              {sampleMode === "manual" && (
                <div className="mt-2 space-y-1">
                  <Label htmlFor="manual" className="text-xs text-muted-foreground">
                    Enter {params.n} numbers (space or comma separated)
                  </Label>
                  <Input
                    id="manual"
                    placeholder="e.g. 1 2 3 4 5 6 7 8"
                    value={manualInput}
                    onChange={(e) => setManualInput(e.target.value)}
                  />
                </div>
              )}

              {sampleMode === "pool" && (
                <div className="mt-2 space-y-1">
                  <Label htmlFor="pool-select" className="text-xs text-muted-foreground">
                    Select pool file
                  </Label>
                  {pools.length === 0 ? (
                    <p className="text-xs text-muted-foreground">
                      No pool files found. Go to "Pool Manager" to import one.
                    </p>
                  ) : (
                    <select
                      id="pool-select"
                      value={selectedPool}
                      onChange={(e) => setSelectedPool(e.target.value)}
                      className="w-full rounded-md border border-input bg-background px-3 py-2 text-sm focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring"
                    >
                      <option value="">-- Select --</option>
                      {pools.map((p) => (
                        <option key={p.path} value={p.path}>
                          {p.name} ({p.numbers.length} numbers)
                        </option>
                      ))}
                    </select>
                  )}
                </div>
              )}
            </div>

            <Separator />

            {/* Save Options */}
            <div className="space-y-2">
              <label className="flex cursor-pointer items-center gap-2 text-sm">
                <input
                  type="checkbox"
                  checked={autoSave}
                  onChange={(e) => setAutoSave(e.target.checked)}
                  className="accent-primary"
                />
                Auto-save to DB
              </label>
              {autoSave && (
                <div className="space-y-1">
                  <Label htmlFor="runCount" className="text-xs text-muted-foreground">
                    Run index
                  </Label>
                  <Input
                    id="runCount"
                    type="number"
                    min={1}
                    value={runCount}
                    onChange={(e) => setRunCount(parseInt(e.target.value, 10) || 1)}
                  />
                </div>
              )}
            </div>

            <Separator />

            {/* Advanced Solver Settings */}
            <div className="space-y-3">
              <Label className="text-xs font-semibold uppercase tracking-wide text-muted-foreground">
                Advanced Settings
              </Label>

              <div className="space-y-1">
                <Label htmlFor="timeLimit" className="flex items-center gap-1 text-sm">
                  Time limit
                  <span className="text-xs text-muted-foreground">(seconds)</span>
                </Label>
                <Input
                  id="timeLimit"
                  type="number"
                  min={10}
                  max={3600}
                  value={timeLimit}
                  onChange={(e) => setTimeLimit(parseInt(e.target.value, 10) || 120)}
                />
              </div>

              <div className="space-y-1">
                <Label htmlFor="threads" className="flex items-center gap-1 text-sm">
                  Threads
                  <span className="text-xs text-muted-foreground">(0 = auto)</span>
                </Label>
                <Input
                  id="threads"
                  type="number"
                  min={0}
                  max={64}
                  value={threads}
                  onChange={(e) => setThreads(parseInt(e.target.value, 10) || 0)}
                />
              </div>

              <div className="space-y-1">
                <Label htmlFor="seed" className="flex items-center gap-1 text-sm">
                  Random seed
                  <span className="text-xs text-muted-foreground">(0 = random)</span>
                </Label>
                <Input
                  id="seed"
                  type="number"
                  min={0}
                  value={seed}
                  onChange={(e) => setSeed(parseInt(e.target.value, 10) || 0)}
                />
              </div>

              <label className="flex cursor-pointer items-center gap-2 text-sm">
                <input
                  type="checkbox"
                  checked={noIncumbentCache}
                  onChange={(e) => setNoIncumbentCache(e.target.checked)}
                  className="accent-primary"
                />
                Disable incumbent cache
              </label>
            </div>
          </div>
        </ScrollArea>

        {/* Algorithm hint + Generate Button */}
        <div className="border-t p-4 space-y-3">
          {/* Algorithm indicator */}
          <div className="rounded-md border px-3 py-2 space-y-1">
            <div className="flex items-center gap-1.5">
              <span className="text-xs text-muted-foreground">Algorithm:</span>
              <span className={`rounded px-1.5 py-0.5 text-xs font-medium ${algoInfo.color}`}>
                {algoInfo.label}
              </span>
            </div>
            <p className="text-[11px] text-muted-foreground leading-relaxed">
              {algoInfo.description}
            </p>
          </div>

          {error && (
            <p className="flex items-center gap-1.5 rounded-md bg-destructive/10 px-3 py-2 text-xs text-destructive">
              <CloseCircleIcon className="h-4 w-4 shrink-0" />
              {error}
            </p>
          )}
          <Button
            className="w-full"
            onClick={handleGenerate}
            disabled={loading}
          >
            {loading ? (
              <><CogIcon className="h-4 w-4" /> Generating...</>
            ) : (
              <><PlayIcon className="h-4 w-4" /> Generate</>
            )}
          </Button>
        </div>
      </aside>

      {/* ── Right: Results Area ── */}
      <section className="flex flex-1 flex-col overflow-hidden">
        <div className="flex h-14 items-center justify-between border-b px-6">
          <h2 className="text-base font-semibold">Results</h2>
          {result?.success && (
            <div className="flex items-center gap-3">
              <Badge variant="secondary">
                {result.count} k-groups
              </Badge>
              <Badge
                variant={result.optimal ? "default" : "outline"}
                className="text-xs"
              >
                {result.optimal ? "Optimal" : result.feasible ? "Best feasible" : "Not verified"}
              </Badge>
              {result.dbFile && (
                <Badge variant="outline" className="gap-1 text-xs">
                  <DownloadIcon className="h-3.5 w-3.5" />
                  Saved
                </Badge>
              )}
              <Button
                variant="outline"
                size="sm"
                onClick={handleGenerate}
                disabled={loading}
              >
                <RegenerateIcon className="h-3.5 w-3.5" />
                Regenerate
              </Button>
            </div>
          )}
        </div>

        <ScrollArea className="flex-1">
          {!result && !loading && (
            <div className="flex h-full flex-col items-center justify-center gap-3 py-20 text-center">
              <Info className="h-10 w-10 text-muted-foreground/40" />
              <p className="text-sm text-muted-foreground">
                Configure parameters on the left, then click "Generate"
              </p>
            </div>
          )}

          {loading && (
            <div className="flex h-full flex-col items-center justify-center gap-3 py-20">
              <CogIcon className="h-10 w-10 text-primary" />
              <p className="text-sm text-muted-foreground">
                Running {algoInfo.name}...
              </p>
            </div>
          )}

          {result?.success && (
            <div className="p-6 space-y-6">
              {/* Selected Samples Card */}
              <Card>
                <CardHeader className="pb-3">
                  <CardTitle className="flex items-center gap-2 text-sm font-medium">
                    <ConfirmIcon className="h-5 w-5 text-green-500" />
                    {params.n} Selected Samples
                  </CardTitle>
                  <CardDescription>
                    m={params.m}, n={params.n}, k={params.k}, j={params.j}, s={params.s}, minCover={params.minCover}
                  </CardDescription>
                </CardHeader>
                <CardContent>
                  <div className="flex flex-wrap gap-1.5">
                    {result.samples.map((num) => (
                      <Badge key={num} className="font-mono">{num}</Badge>
                    ))}
                  </div>
                </CardContent>
              </Card>

              {/* Solver Report */}
              <Card>
                <CardHeader className="pb-3">
                  <CardTitle className="text-sm font-medium">
                    Solver Report
                  </CardTitle>
                  <CardDescription>
                    {result.algorithm || "Portfolio"} · {result.feasible ? "coverage verified" : "coverage not verified"}
                    {result.warmStarted ? " · warm-started from previous best" : ""}
                  </CardDescription>
                </CardHeader>
                <CardContent>
                  <div className="grid grid-cols-2 gap-3 text-sm md:grid-cols-4 xl:grid-cols-8">
                    <div>
                      <p className="text-xs text-muted-foreground">Status</p>
                      <p className="font-medium">{result.optimal ? "Optimal" : "Best feasible"}</p>
                    </div>
                    <div>
                      <p className="text-xs text-muted-foreground">Lower bound</p>
                      <p className="font-mono">{result.lowerBound}</p>
                    </div>
                    <div>
                      <p className="text-xs text-muted-foreground">Upper bound</p>
                      <p className="font-mono">{result.upperBound || result.count}</p>
                    </div>
                    <div>
                      <p className="text-xs text-muted-foreground">Cache</p>
                      <p className="font-mono">{result.cacheType || "none"}</p>
                    </div>
                    <div>
                      <p className="text-xs text-muted-foreground">Incumbent</p>
                      <p className="font-mono">{result.incumbentCount || "-"}</p>
                    </div>
                    <div>
                      <p className="text-xs text-muted-foreground">Gap</p>
                      <p className="font-mono">{((result.gap || 0) * 100).toFixed(1)}%</p>
                    </div>
                    <div>
                      <p className="text-xs text-muted-foreground">Time limit</p>
                      <p className="font-mono">{result.timeLimitSec || 120}s</p>
                    </div>
                    <div>
                      <p className="text-xs text-muted-foreground">Threads</p>
                      <p className="font-mono">{result.threads || "-"}</p>
                    </div>
                    <div>
                      <p className="text-xs text-muted-foreground">Nodes</p>
                      <p className="font-mono">{result.nodes ? result.nodes.toLocaleString() : "-"}</p>
                    </div>
                    <div>
                      <p className="text-xs text-muted-foreground">Nodes/s</p>
                      <p className="font-mono">{result.nodesPerSec ? Math.round(result.nodesPerSec).toLocaleString() : "-"}</p>
                    </div>
                    <div>
                      <p className="text-xs text-muted-foreground">Reduction</p>
                      <p className="font-mono">{result.reductionRatio ? `${(result.reductionRatio * 100).toFixed(1)}%` : "-"}</p>
                    </div>
                    <div>
                      <p className="text-xs text-muted-foreground">TT hits</p>
                      <p className="font-mono">{result.ttHits ? result.ttHits.toLocaleString() : "-"}</p>
                    </div>
                    <div>
                      <p className="text-xs text-muted-foreground">Proof time</p>
                      <p className="font-mono">{result.proofTimeSec ? `${result.proofTimeSec.toFixed(2)}s` : "-"}</p>
                    </div>
                  </div>
                </CardContent>
              </Card>

              {/* k-Group Results */}
              <Card>
                <CardHeader className="pb-3">
                  <CardTitle className="text-sm font-medium">
                    {result.optimal ? "Optimal" : "Best"} k-Groups ({result.count} groups)
                  </CardTitle>
                  <CardDescription>
                    Highlighted badges belong to the selected sample set
                  </CardDescription>
                </CardHeader>
                <CardContent className="p-0">
                  <GroupTable
                    groups={result.groups}
                    highlightSamples={sampleSet}
                  />
                </CardContent>
              </Card>

              {result.dbFile && (
                <p className="flex items-center justify-center gap-1 text-xs text-muted-foreground">
                  <ExternalLinkIcon className="h-3.5 w-3.5 shrink-0" />
                  Saved to: {result.dbFile}
                </p>
              )}
            </div>
          )}
        </ScrollArea>
      </section>
    </div>
  );
}
