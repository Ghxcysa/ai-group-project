import { useState, useEffect, useCallback } from "react";
import { Layers, ChevronRight } from "lucide-react";
import CloudDownloadIcon from "@/assets/icons/line-md--cloud-alt-download-filled-loop.svg?react";
import LoadingIcon from "@/assets/icons/eos-icons--loading.svg?react";
import RefreshIcon from "@/assets/icons/line-md--rotate-270.svg?react";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { Card, CardContent, CardHeader, CardTitle, CardDescription } from "@/components/ui/card";
import { ScrollArea } from "@/components/ui/scroll-area";
import { listPools, importPool, type PoolFileInfo } from "@/lib/tauri";
import { cn } from "@/lib/utils";

interface Props {
  dbDir: string;
}

export default function PoolManager({ dbDir }: Props) {
  const [pools, setPools] = useState<PoolFileInfo[]>([]);
  const [loading, setLoading] = useState(false);
  const [importing, setImporting] = useState(false);
  const [selected, setSelected] = useState<PoolFileInfo | null>(null);

  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      const list = await listPools(dbDir);
      setPools(list);
      if (selected && !list.find((p) => p.path === selected.path)) {
        setSelected(null);
      }
    } catch (e) {
      console.error(e);
    } finally {
      setLoading(false);
    }
  }, [dbDir, selected]);

  useEffect(() => {
    refresh();
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [dbDir]);

  const handleImport = async () => {
    setImporting(true);
    try {
      const pool = await importPool(dbDir);
      if (pool) {
        await refresh();
        setSelected(pool);
      }
    } catch (e) {
      alert("Import failed: " + String(e));
    } finally {
      setImporting(false);
    }
  };

  return (
    <div className="flex h-full overflow-hidden">
      {/* ── Left: Pool List ── */}
      <aside className="flex w-64 flex-col border-r">
        <div className="flex h-14 items-center justify-between border-b px-4">
          <h1 className="text-base font-semibold">Number Pools</h1>
          <Button
            variant="ghost"
            size="icon"
            onClick={refresh}
            disabled={loading}
            title="Refresh"
          >
            <RefreshIcon className={cn("h-4 w-4", loading && "animate-spin")} />
          </Button>
        </div>

        <ScrollArea className="flex-1">
          {loading && pools.length === 0 ? (
            <div className="flex items-center justify-center py-10">
              <LoadingIcon className="h-5 w-5 text-muted-foreground" />
            </div>
          ) : pools.length === 0 ? (
            <p className="py-8 text-center text-xs text-muted-foreground">
              No pool files found
            </p>
          ) : (
            <nav className="py-2">
              {pools.map((pool) => (
                <button
                  key={pool.path}
                  onClick={() => setSelected(pool)}
                  className={cn(
                    "flex w-full items-center gap-2 px-3 py-2.5 text-left text-sm transition-colors",
                    "hover:bg-accent hover:text-accent-foreground",
                    selected?.path === pool.path
                      ? "bg-accent font-medium"
                      : "text-muted-foreground"
                  )}
                >
                  <Layers className="h-4 w-4 shrink-0" />
                  <div className="flex-1 overflow-hidden">
                    <p className="truncate text-xs font-medium">{pool.name}</p>
                    <p className="text-xs text-muted-foreground">
                      {pool.numbers.length} numbers
                    </p>
                  </div>
                  {selected?.path === pool.path && (
                    <ChevronRight className="h-3.5 w-3.5 shrink-0" />
                  )}
                </button>
              ))}
            </nav>
          )}
        </ScrollArea>

        {/* Import Button */}
        <div className="border-t p-3">
          <Button
            className="w-full"
            size="sm"
            variant="outline"
            onClick={handleImport}
            disabled={importing}
          >
            {importing ? (
              <><LoadingIcon className="h-4 w-4 mr-2" /> Importing...</>
            ) : (
              <><CloudDownloadIcon className="h-4 w-4 mr-2" /> Import Pool File</>
            )}
          </Button>
        </div>
      </aside>

      {/* ── Right: Preview ── */}
      <section className="flex flex-1 flex-col overflow-hidden">
        {!selected ? (
          <div className="flex h-full flex-col items-center justify-center gap-3 text-center">
            <Layers className="h-10 w-10 text-muted-foreground/30" />
            <p className="text-sm text-muted-foreground">Select a pool on the left to view its contents</p>
            <p className="text-xs text-muted-foreground max-w-xs">
              Pool files (pool_*.txt) are stored in the db/ directory, with comma-separated integers and # comment lines
            </p>
            <Button
              variant="outline"
              size="sm"
              onClick={handleImport}
              disabled={importing}
            >
              <CloudDownloadIcon className="h-4 w-4 mr-2" />
              Import Pool File
            </Button>
          </div>
        ) : (
          <>
            <div className="flex h-14 items-center justify-between border-b px-6">
              <div className="flex items-center gap-3">
                <h2 className="text-sm font-semibold">{selected.name}</h2>
                <Badge variant="secondary">{selected.numbers.length} numbers</Badge>
              </div>
            </div>

            <ScrollArea className="flex-1 p-6">
              <div className="space-y-4">
                {/* File Info */}
                <Card>
                  <CardHeader className="pb-2">
                    <CardTitle className="text-sm font-medium">File Info</CardTitle>
                    <CardDescription className="font-mono text-xs">
                      {selected.path}
                    </CardDescription>
                  </CardHeader>
                  <CardContent className="text-sm space-y-1">
                    <p>Count: <span className="font-semibold">{selected.numbers.length}</span></p>
                    <p>
                      Range:{" "}
                      <span className="font-semibold">
                        {Math.min(...selected.numbers)} ~ {Math.max(...selected.numbers)}
                      </span>
                    </p>
                  </CardContent>
                </Card>

                {/* Number Preview */}
                <Card>
                  <CardHeader className="pb-2">
                    <CardTitle className="text-sm font-medium">Numbers</CardTitle>
                  </CardHeader>
                  <CardContent>
                    <div className="flex flex-wrap gap-1.5">
                      {selected.numbers.map((n, i) => (
                        <Badge
                          key={i}
                          variant="outline"
                          className="font-mono text-xs"
                        >
                          {n}
                        </Badge>
                      ))}
                    </div>
                  </CardContent>
                </Card>

                <p className="text-xs text-muted-foreground text-center">
                  Select "From Pool" in the Configure page to use this file
                </p>
              </div>
            </ScrollArea>
          </>
        )}
      </section>
    </div>
  );
}
