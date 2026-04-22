import { useState, useEffect, useCallback } from "react";
import { ChevronRight } from "lucide-react";
import LoadingIcon from "@/assets/icons/eos-icons--loading.svg?react";
import RefreshIcon from "@/assets/icons/line-md--rotate-270.svg?react";
import FolderIcon from "@/assets/icons/line-md--folder-arrow-down.svg?react";
import CancelIcon from "@/assets/icons/line-md--cancel.svg?react";
import { Button } from "@/components/ui/button";
import { Badge } from "@/components/ui/badge";
import { ScrollArea } from "@/components/ui/scroll-area";
import { Separator } from "@/components/ui/separator";
import GroupTable from "@/components/GroupTable";
import {
  listDb,
  readDb,
  deleteDb,
  deleteGroup,
  type DbFileInfo,
} from "@/lib/tauri";
import { cn } from "@/lib/utils";

interface Props {
  dbDir: string;
}

export default function DBBrowser({ dbDir }: Props) {
  const [files, setFiles] = useState<DbFileInfo[]>([]);
  const [loadingList, setLoadingList] = useState(false);
  const [selectedFile, setSelectedFile] = useState<DbFileInfo | null>(null);
  const [groups, setGroups] = useState<number[][]>([]);
  const [loadingGroups, setLoadingGroups] = useState(false);
  const [deletingFile, setDeletingFile] = useState(false);

  const refreshList = useCallback(async () => {
    setLoadingList(true);
    try {
      const list = await listDb(dbDir);
      setFiles(list);
      // If currently selected file is no longer in list, clear selection
      if (selectedFile && !list.find((f) => f.path === selectedFile.path)) {
        setSelectedFile(null);
        setGroups([]);
      }
    } catch (e) {
      console.error(e);
    } finally {
      setLoadingList(false);
    }
  }, [dbDir, selectedFile]);

  useEffect(() => {
    refreshList();
  // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [dbDir]);

  const selectFile = async (file: DbFileInfo) => {
    setSelectedFile(file);
    setLoadingGroups(true);
    try {
      const data = await readDb(file.path);
      setGroups(data);
    } catch (e) {
      console.error(e);
      setGroups([]);
    } finally {
      setLoadingGroups(false);
    }
  };

  const handleDeleteFile = async () => {
    if (!selectedFile) return;
    if (!window.confirm(`Delete ${selectedFile.name}? This action cannot be undone.`)) return;
    setDeletingFile(true);
    try {
      await deleteDb(selectedFile.path);
      setSelectedFile(null);
      setGroups([]);
      await refreshList();
    } catch (e) {
      alert("Delete failed: " + String(e));
    } finally {
      setDeletingFile(false);
    }
  };

  const handleDeleteGroup = async (index: number) => {
    if (!selectedFile) return;
    try {
      await deleteGroup(selectedFile.path, index);
      // Reload file content
      const data = await readDb(selectedFile.path);
      setGroups(data);
    } catch (e) {
      alert("Failed to delete k-group: " + String(e));
    }
  };

  return (
    <div className="flex h-full overflow-hidden">
      {/* ── Left: File List ── */}
      <aside className="flex w-64 flex-col border-r">
        <div className="flex h-14 items-center justify-between border-b px-4">
          <h1 className="text-base font-semibold">DB Files</h1>
          <Button
            variant="ghost"
            size="icon"
            onClick={refreshList}
            disabled={loadingList}
            title="Refresh"
          >
            <RefreshIcon className={cn("h-4 w-4", loadingList && "animate-spin")} />
          </Button>
        </div>

        <ScrollArea className="flex-1">
          {loadingList && files.length === 0 ? (
            <div className="flex items-center justify-center py-10">
              <LoadingIcon className="h-5 w-5 text-muted-foreground" />
            </div>
          ) : files.length === 0 ? (
            <p className="py-8 text-center text-xs text-muted-foreground">
              No DB files found
            </p>
          ) : (
            <nav className="py-2">
              {files.map((file) => (
                <button
                  key={file.path}
                  onClick={() => selectFile(file)}
                  className={cn(
                    "flex w-full items-center gap-2 px-3 py-2.5 text-left text-sm transition-colors",
                    "hover:bg-accent hover:text-accent-foreground",
                    selectedFile?.path === file.path
                      ? "bg-accent font-medium"
                      : "text-muted-foreground"
                  )}
                >
                  <FolderIcon className="h-4 w-4 shrink-0" />
                  <span className="flex-1 truncate font-mono text-xs">
                    {file.label}
                  </span>
                  {selectedFile?.path === file.path && (
                    <ChevronRight className="h-3.5 w-3.5 shrink-0" />
                  )}
                </button>
              ))}
            </nav>
          )}
        </ScrollArea>

        <div className="border-t p-3">
          <p className="text-xs text-muted-foreground text-center">
            {files.length} file{files.length !== 1 ? "s" : ""}
          </p>
        </div>
      </aside>

      {/* ── Right: Content Area ── */}
      <section className="flex flex-1 flex-col overflow-hidden">
        {!selectedFile ? (
          <div className="flex h-full flex-col items-center justify-center gap-2 text-center">
            <FolderIcon className="h-10 w-10 text-muted-foreground/30" />
            <p className="text-sm text-muted-foreground">Select a file on the left to view its contents</p>
          </div>
        ) : (
          <>
            {/* File Title Bar */}
            <div className="flex h-14 items-center justify-between border-b px-6">
              <div className="flex items-center gap-3">
                <h2 className="text-sm font-semibold font-mono">
                  {selectedFile.name}
                </h2>
                {!loadingGroups && (
                  <Badge variant="secondary">{groups.length} k-groups</Badge>
                )}
              </div>
              <Button
                variant="destructive"
                size="sm"
                onClick={handleDeleteFile}
                disabled={deletingFile || loadingGroups}
              >
                {deletingFile ? (
                  <LoadingIcon className="h-3.5 w-3.5" />
                ) : (
                  <CancelIcon className="h-3.5 w-3.5" />
                )}
                Delete File
              </Button>
            </div>

            {/* k-Group Content */}
            <ScrollArea className="flex-1">
              {loadingGroups ? (
                <div className="flex items-center justify-center py-16">
                  <LoadingIcon className="h-6 w-6 text-muted-foreground" />
                </div>
              ) : (
                <div className="p-6">
                  <GroupTable groups={groups} onDelete={handleDeleteGroup} />

                  {groups.length > 0 && (
                    <>
                      <Separator className="my-4" />
                      <p className="text-xs text-muted-foreground text-center">
                        Click "Delete" at the end of a row to remove that k-group
                      </p>
                    </>
                  )}
                </div>
              )}
            </ScrollArea>
          </>
        )}
      </section>
    </div>
  );
}
