# Optimal Sample Selection System — Frontend (Tauri + React)

A desktop application built with Tauri 2 + React 18 + TypeScript + Vite + Tailwind CSS + shadcn/ui.

## Directory Structure

```
frontend/
├── src/                    # React frontend code
│   ├── pages/
│   │   ├── Generate.tsx    # Page 1: Parameter configuration & generation
│   │   ├── DBBrowser.tsx   # Page 2: Results management
│   │   └── PoolManager.tsx # Page 3: Number pool management
│   ├── components/
│   │   ├── GroupTable.tsx  # k-group results table
│   │   └── ui/             # shadcn/ui base components
│   ├── lib/
│   │   ├── tauri.ts        # Tauri invoke wrappers
│   │   └── utils.ts        # Utility functions
│   ├── App.tsx             # Sidebar navigation layout
│   └── main.tsx
├── src-tauri/              # Rust/Tauri backend
│   ├── src/
│   │   ├── commands.rs     # Tauri commands implementation
│   │   ├── lib.rs          # Plugin registration entry point
│   │   └── main.rs         # Program entry point
│   ├── binaries/           # ← Place compiled C++ sidecar here
│   ├── Cargo.toml
│   └── tauri.conf.json
└── scripts/
    ├── build-sidecar-macos.sh    # macOS C++ compilation script
    ├── build-sidecar-windows.bat # Windows C++ compilation script
    └── dev-setup.sh              # One-command dev environment setup
```

## Quick Start

### 1. Compile the C++ sidecar

**macOS:**
```bash
./scripts/build-sidecar-macos.sh
```
Generates `optimal_sample-aarch64-apple-darwin` (Apple Silicon) or `optimal_sample-x86_64-apple-darwin` (Intel) in `src-tauri/binaries/`.

**Windows:**
```bat
scripts\build-sidecar-windows.bat
```

### 2. Install dependencies & start dev server

```bash
npm install
npm run tauri dev
```

### 3. Build for release

```bash
npm run tauri build
```
Output is in `src-tauri/target/release/bundle/`.

---

## Dependencies

| Dependency | Purpose |
|------------|---------|
| Tauri 2 | Cross-platform desktop container with Rust backend |
| React 18 | UI framework |
| Vite 6 | Build tool |
| Tailwind CSS v3 | Styling |
| shadcn/ui (Radix UI) | Unstyled + Tailwind component library |
| lucide-react | Icons |
| serde_json | Rust JSON serialization |
| tauri-plugin-shell | Rust sidecar invocation |
| tauri-plugin-dialog | Native file picker dialog |
| tauri-plugin-fs | Filesystem access |

## Customizing the Theme

Edit the CSS variables in `tailwind.config.js`, or modify the `:root` block in `src/index.css`:

```css
:root {
  --primary: 221.2 83.2% 53.3%;  /* Primary color (blue) */
}
```

## Tauri Command Reference

| React call | Rust command | Function |
|------------|--------------|----------|
| `runGenerate(params)` | `run_generate` | Invoke C++ sidecar to generate k-groups |
| `listDb(dir)` | `list_db` | List DB files |
| `readDb(path)` | `read_db` | Read DB file contents |
| `deleteDb(path)` | `delete_db` | Delete a DB file |
| `deleteGroup(path, idx)` | `delete_group` | Delete a single k-group |
| `listPools(dir)` | `list_pools` | List pool files |
| `readPool(path)` | `read_pool` | Read pool file |
| `importPool(dir)` | `import_pool` | Import a pool file |
