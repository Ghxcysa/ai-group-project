use std::fs;
use std::io::{self, BufRead, Write};
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use tauri::AppHandle;
use tauri_plugin_shell::ShellExt;

// ── 公共数据结构 ─────────────────────────────────────────────────────────────

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GenerateParams {
    pub m: i32,
    pub n: i32,
    pub k: i32,
    pub j: i32,
    pub s: i32,
    #[serde(rename = "minCover")]
    pub min_cover: i32,
    pub samples: Option<Vec<i32>>,
    pub save: bool,
    pub run: i32,
    pub dbdir: String,
    #[serde(rename = "poolFile")]
    pub pool_file: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GenerateResult {
    pub success: bool,
    pub count: usize,
    #[serde(rename = "dbFile")]
    pub db_file: String,
    pub samples: Vec<i32>,
    pub groups: Vec<Vec<i32>>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DbFileInfo {
    pub path: String,
    pub name: String,
    pub label: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PoolFileInfo {
    pub path: String,
    pub name: String,
    pub numbers: Vec<i32>,
}

// ── 辅助函数 ─────────────────────────────────────────────────────────────────

/// 解析一行逗号分隔的整数（# 开头的行视为注释，直接返回空）
fn parse_line(line: &str) -> Vec<i32> {
    let trimmed = line.trim();
    if trimmed.is_empty() || trimmed.starts_with('#') {
        return vec![];
    }
    trimmed
        .split(',')
        .filter_map(|t| t.trim().parse::<i32>().ok())
        .collect()
}

/// 读取 DB 文件（每行一个 k-组，逗号分隔）
fn read_db_file(path: &Path) -> io::Result<Vec<Vec<i32>>> {
    let file = fs::File::open(path)?;
    let reader = io::BufReader::new(file);
    let mut groups = Vec::new();
    for line in reader.lines() {
        let l = line?;
        let nums = parse_line(&l);
        if !nums.is_empty() {
            groups.push(nums);
        }
    }
    Ok(groups)
}

/// 将 k-组列表写回 DB 文件
fn write_db_file(path: &Path, groups: &[Vec<i32>]) -> io::Result<()> {
    let mut file = fs::File::create(path)?;
    for group in groups {
        let line = group
            .iter()
            .map(|n| n.to_string())
            .collect::<Vec<_>>()
            .join(",");
        writeln!(file, "{}", line)?;
    }
    Ok(())
}

/// 判断文件名是否为 DB 文件（不以 pool_ 开头，以 .txt 结尾）
fn is_db_file(name: &str) -> bool {
    name.ends_with(".txt") && !name.starts_with("pool_")
}

/// 从 stdout 中提取 JSON 行（找最后一个以 { 开头的行）
fn extract_json(output: &str) -> &str {
    output
        .lines()
        .rev()
        .find(|l| l.trim_start().starts_with('{'))
        .unwrap_or(output)
        .trim()
}

// ── Tauri Commands ───────────────────────────────────────────────────────────

/// 调用 C++ sidecar 执行生成任务
#[tauri::command]
pub async fn run_generate(
    app: AppHandle,
    params: GenerateParams,
) -> Result<GenerateResult, String> {
    // 确保 dbdir 存在
    fs::create_dir_all(&params.dbdir).map_err(|e| e.to_string())?;

    // 构建参数列表
    let mut args = vec![
        "--cli".to_string(),
        format!("--m={}", params.m),
        format!("--n={}", params.n),
        format!("--k={}", params.k),
        format!("--j={}", params.j),
        format!("--s={}", params.s),
        format!("--minCover={}", params.min_cover),
        format!("--run={}", params.run),
        format!("--dbdir={}", params.dbdir),
    ];

    if let Some(samples) = &params.samples {
        let s = samples
            .iter()
            .map(|n| n.to_string())
            .collect::<Vec<_>>()
            .join(",");
        args.push(format!("--samples={}", s));
    }

    if params.save {
        args.push("--save".to_string());
    }

    if let Some(pool) = &params.pool_file {
        if !pool.is_empty() {
            args.push(format!("--poolFile={}", pool));
        }
    }

    // 调用 sidecar（名称与 tauri.conf.json 中的 externalBin 对应）
    let output = app
        .shell()
        .sidecar("optimal_sample")
        .map_err(|e| e.to_string())?
        .args(&args)
        .output()
        .await
        .map_err(|e| e.to_string())?;

    let stdout = String::from_utf8_lossy(&output.stdout).to_string();
    let json_str = extract_json(&stdout);

    let result: GenerateResult =
        serde_json::from_str(json_str).map_err(|e| {
            format!(
                "JSON parse error: {}\nRaw output:\n{}",
                e,
                stdout.chars().take(500).collect::<String>()
            )
        })?;

    Ok(result)
}

/// 列出 dbDir 下所有 DB 文件（排除 pool_*.txt）
#[tauri::command]
pub fn list_db(db_dir: String) -> Result<Vec<DbFileInfo>, String> {
    let dir = Path::new(&db_dir);
    if !dir.exists() {
        return Ok(vec![]);
    }

    let mut files: Vec<DbFileInfo> = fs::read_dir(dir)
        .map_err(|e| e.to_string())?
        .filter_map(|e| e.ok())
        .filter(|e| {
            if !e.path().is_file() { return false; }
            let name = e.file_name().to_string_lossy().to_string();
            is_db_file(&name)
        })
        .map(|e| {
            let path = e.path().to_string_lossy().to_string();
            let name = e.file_name().to_string_lossy().to_string();
            let label = name.strip_suffix(".txt").unwrap_or(&name).to_string();
            DbFileInfo { path, name, label }
        })
        .collect();

    files.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(files)
}

/// 读取 DB 文件内容
#[tauri::command]
pub fn read_db(path: String) -> Result<Vec<Vec<i32>>, String> {
    read_db_file(Path::new(&path)).map_err(|e| e.to_string())
}

/// 删除整个 DB 文件
#[tauri::command]
pub fn delete_db(path: String) -> Result<(), String> {
    fs::remove_file(&path).map_err(|e| e.to_string())
}

/// 从 DB 文件中删除指定索引（0-based）的 k-组
#[tauri::command]
pub fn delete_group(path: String, index: usize) -> Result<(), String> {
    let p = Path::new(&path);
    let mut groups = read_db_file(p).map_err(|e| e.to_string())?;
    if index >= groups.len() {
        return Err(format!(
            "Index {} out of range (len={})",
            index,
            groups.len()
        ));
    }
    groups.remove(index);
    write_db_file(p, &groups).map_err(|e| e.to_string())
}

/// 列出 dbDir 下所有 pool 文件（pool_*.txt）
#[tauri::command]
pub fn list_pools(db_dir: String) -> Result<Vec<PoolFileInfo>, String> {
    let dir = Path::new(&db_dir);
    if !dir.exists() {
        return Ok(vec![]);
    }

    let mut pools: Vec<PoolFileInfo> = fs::read_dir(dir)
        .map_err(|e| e.to_string())?
        .filter_map(|e| e.ok())
        .filter(|e| {
            let name = e.file_name().to_string_lossy().to_string();
            e.path().is_file() && name.starts_with("pool_") && name.ends_with(".txt")
        })
        .filter_map(|e| {
            let path = e.path().to_string_lossy().to_string();
            let name = e.file_name().to_string_lossy().to_string();
            let numbers = read_pool_file(&e.path()).ok()?;
            Some(PoolFileInfo { path, name, numbers })
        })
        .collect();

    pools.sort_by(|a, b| a.name.cmp(&b.name));
    Ok(pools)
}

/// 读取单个 pool 文件
fn read_pool_file(path: &Path) -> io::Result<Vec<i32>> {
    let file = fs::File::open(path)?;
    let reader = io::BufReader::new(file);
    let mut numbers = Vec::new();
    for line in reader.lines() {
        let l = line?;
        numbers.extend(parse_line(&l));
    }
    Ok(numbers)
}

/// 读取 pool 文件内容（Command 版本）
#[tauri::command]
pub fn read_pool(path: String) -> Result<Vec<i32>, String> {
    read_pool_file(Path::new(&path)).map_err(|e| e.to_string())
}

/// 打开系统文件选择对话框，将选中的 pool 文件复制到 dbDir 并返回信息
#[tauri::command]
pub async fn import_pool(
    app: AppHandle,
    db_dir: String,
) -> Result<Option<PoolFileInfo>, String> {
    use tauri_plugin_dialog::DialogExt;

    let file_path = app
        .dialog()
        .file()
        .add_filter("文本文件", &["txt"])
        .blocking_pick_file();

    let src_path = match file_path {
        Some(p) => p.into_path().map_err(|e| e.to_string())?,
        None => return Ok(None),
    };

    let file_name = src_path
        .file_name()
        .ok_or("Invalid file name")?
        .to_string_lossy()
        .to_string();

    // 如果文件名不以 pool_ 开头，自动添加前缀
    let dest_name = if file_name.starts_with("pool_") {
        file_name.clone()
    } else {
        format!("pool_{}", file_name)
    };

    fs::create_dir_all(&db_dir).map_err(|e| e.to_string())?;

    let dest_path = Path::new(&db_dir).join(&dest_name);
    fs::copy(&src_path, &dest_path).map_err(|e| e.to_string())?;

    let numbers = read_pool_file(&dest_path).map_err(|e| e.to_string())?;
    let path_str = dest_path.to_string_lossy().to_string();

    Ok(Some(PoolFileInfo {
        path: path_str,
        name: dest_name,
        numbers,
    }))
}

