import { useState, useCallback } from "react";
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

  const [loading, setLoading] = useState(false);
  const [result, setResult] = useState<GenerateResult | null>(null);
  const [error, setError] = useState<string>("");

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
      });
      setResult(res);
      if (!res.success) setError(res.error || "Generation failed");
    } catch (e: unknown) {
      setError(String(e));
    } finally {
      setLoading(false);
    }
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [params, sampleMode, manualInput, selectedPool, autoSave, runCount, dbDir]);

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
          </div>
        </ScrollArea>

        {/* Generate Button */}
        <div className="border-t p-4">
          {error && (
            <p className="mb-2 flex items-center gap-1.5 rounded-md bg-destructive/10 px-3 py-2 text-xs text-destructive">
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
              <p className="text-sm text-muted-foreground">Running greedy algorithm...</p>
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

              {/* k-Group Results */}
              <Card>
                <CardHeader className="pb-3">
                  <CardTitle className="text-sm font-medium">
                    Optimal k-Groups ({result.count} groups)
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
