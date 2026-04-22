// 在 Windows 系统上禁用控制台窗口（release 模式）
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    optimal_sample_lib::run();
}
