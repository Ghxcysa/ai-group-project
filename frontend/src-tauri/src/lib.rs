mod commands;

use commands::*;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_shell::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_fs::init())
        .invoke_handler(tauri::generate_handler![
            run_generate,
            list_db,
            read_db,
            delete_db,
            delete_group,
            list_pools,
            read_pool,
            import_pool,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
