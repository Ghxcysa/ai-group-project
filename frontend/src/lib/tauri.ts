import { invoke } from "@tauri-apps/api/core";

// ── Type Definitions ─────────────────────────────────────────────────────────

export interface GenerateParams {
  m: number;
  n: number;
  k: number;
  j: number;
  s: number;
  minCover: number;
  samples?: number[];   // if empty, random selection is used
  save: boolean;
  run: number;
  dbdir: string;
  poolFile?: string;
}

export interface GenerateResult {
  success: boolean;
  count: number;
  dbFile: string;
  samples: number[];
  groups: number[][];
  error?: string;
}

export interface DbFileInfo {
  path: string;
  name: string;
  /** Parameters parsed from filename, e.g. "45-8-6-6-5-1-4" */
  label: string;
}

export interface PoolFileInfo {
  path: string;
  name: string;
  numbers: number[];
}

// ── Command Wrappers ──────────────────────────────────────────────────────────

/** Invoke C++ sidecar to generate optimal k-groups */
export async function runGenerate(params: GenerateParams): Promise<GenerateResult> {
  return invoke<GenerateResult>("run_generate", { params });
}

/** List all DB files under dbDir */
export async function listDb(dbDir: string): Promise<DbFileInfo[]> {
  return invoke<DbFileInfo[]>("list_db", { dbDir });
}

/** Read k-group content from a DB file */
export async function readDb(path: string): Promise<number[][]> {
  return invoke<number[][]>("read_db", { path });
}

/** Delete an entire DB file */
export async function deleteDb(path: string): Promise<void> {
  return invoke<void>("delete_db", { path });
}

/** Delete a k-group at the given index (0-based) from a DB file */
export async function deleteGroup(path: string, index: number): Promise<void> {
  return invoke<void>("delete_group", { path, index });
}

/** List all pool files under dbDir */
export async function listPools(dbDir: string): Promise<PoolFileInfo[]> {
  return invoke<PoolFileInfo[]>("list_pools", { dbDir });
}

/** Read contents of a pool file */
export async function readPool(path: string): Promise<number[]> {
  return invoke<number[]>("read_pool", { path });
}

/** Open system file picker, select a pool file and import it into dbDir */
export async function importPool(dbDir: string): Promise<PoolFileInfo | null> {
  return invoke<PoolFileInfo | null>("import_pool", { dbDir });
}
