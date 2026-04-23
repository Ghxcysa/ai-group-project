import { useState, useEffect } from "react";
import { Database, Layers, Settings2 } from "lucide-react";
import ArrowRightIcon from "@/assets/icons/line-md--arrow-right-square.svg?react";
import { cn } from "@/lib/utils";
import { Separator } from "@/components/ui/separator";
import { ScrollArea } from "@/components/ui/scroll-area";
import Generate from "@/pages/Generate";
import DBBrowser from "@/pages/DBBrowser";
import PoolManager from "@/pages/PoolManager";
import { appDataDir, join } from "@tauri-apps/api/path";

type Page = "generate" | "db" | "pool";

interface NavItem {
  id: Page;
  label: string;
  icon: React.ReactNode;
  desc: string;
}

const navItems: NavItem[] = [
  {
    id: "generate",
    label: "Configure & Generate",
    icon: <Settings2 className="h-4 w-4" />,
    desc: "Set parameters and generate optimal k-groups",
  },
  {
    id: "db",
    label: "Results",
    icon: <Database className="h-4 w-4" />,
    desc: "View and manage saved DB files",
  },
  {
    id: "pool",
    label: "Pool Manager",
    icon: <Layers className="h-4 w-4" />,
    desc: "Import and manage sample number pools",
  },
];

export default function App() {
  const [page, setPage] = useState<Page>("generate");
  const [dbDir, setDbDir] = useState<string>("");

  useEffect(() => {
    appDataDir()
      .then((dir) => join(dir, "db"))
      .then(setDbDir)
      .catch(() => setDbDir("./db"));
  }, []);

  return (
    <div className="flex h-screen bg-background text-foreground">
      {/* ── Left Sidebar ── */}
      <aside className="flex w-56 flex-col border-r bg-muted/30">
        {/* Logo / Title */}
        <div className="flex h-14 items-center gap-2 px-4">
          <div className="flex h-8 w-8 items-center justify-center rounded-md bg-primary text-primary-foreground text-xs font-bold">
            OS
          </div>
          <span className="text-sm font-semibold leading-tight">
            Optimal Sample<br />Selection System
          </span>
        </div>

        <Separator />

        {/* Navigation Items */}
        <ScrollArea className="flex-1">
          <nav className="py-2">
            {navItems.map((item) => (
              <button
                key={item.id}
                onClick={() => setPage(item.id)}
                className={cn(
                  "flex w-full items-start gap-3 rounded-md px-3 py-2.5 mx-1 text-left transition-colors",
                  "hover:bg-accent hover:text-accent-foreground",
                  page === item.id
                    ? "bg-accent text-accent-foreground font-medium"
                    : "text-muted-foreground"
                )}
                style={{ width: "calc(100% - 8px)" }}
              >
                <span className="mt-0.5 shrink-0">{item.icon}</span>
                <div className="flex-1">
                  <p className="text-sm leading-tight">{item.label}</p>
                  <p className="mt-0.5 text-xs text-muted-foreground leading-tight">
                    {item.desc}
                  </p>
                </div>
                {page === item.id && (
                  <ArrowRightIcon className="h-4 w-4 shrink-0 text-primary" />
                )}
              </button>
            ))}
          </nav>
        </ScrollArea>

        {/* Bottom Version Info */}
        <div className="border-t px-4 py-3">
          <p className="text-xs text-muted-foreground">v0.1.0</p>
        </div>
      </aside>

      {/* ── Right Content Area ── */}
      <main className="flex flex-1 flex-col overflow-hidden">
        {page === "generate" && <Generate dbDir={dbDir} />}
        {page === "db" && <DBBrowser dbDir={dbDir} />}
        {page === "pool" && <PoolManager dbDir={dbDir} />}
      </main>
    </div>
  );
}
