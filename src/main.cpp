// main.cpp — CLI 入口
//
// 用法:
//   ./algo_diff_cpp <db_path> [--business-id N] [--batch N] [--dry-run]
//                         [--skip-reverify] [--run-id ID]
//                         [--reverify-helper-path P]
//                         [--httpx-path P]
//
// 退出码 (硬):
//   0 = OK
//   2 = 参数错误
//   3 = schema verify failed
//   4 = zlib / json inflate 失败 (fail-fast, 不容错)
//   5 = batch update 失败 (T3/T4 写 db 失败)
//   6 = reverify helper (Python algo_diff_reverify.py) 失败

#include "pipeline.hpp"

#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <cstring>
#include <string>

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
        "用法: %s <db_path> [--business-id N] [--batch N] [--dry-run]\n"
        "                   [--skip-reverify] [--run-id ID]\n"
        "                   [--reverify-helper-path P]\n"
        "                   [--httpx-path P]\n"
        "\n"
        "db_path:                  SQLite db 路径\n"
        "--business-id N:          只处理单业务 (默认 0 = 全表)\n"
        "--batch N:                UPDATE batch 尺寸 (默认 5000)\n"
        "--dry-run:                只跑不写\n"
        "--skip-reverify:          跳过 T1/T2 reverify (test only)\n"
        "--reverify-helper-path P: Python 阶段编排器路径 (默认 '../lib/algo_diff_reverify.py',\n"
        "                          相对 binary 位置; CWD 是 <module>/bin/ 时解析成\n"
        "                          <module>/lib/algo_diff_reverify.py)\n"
        "--httpx-path P:           httpx 二进制路径 (默认 'httpx', C++ 当前不直接调,\n"
        "                          留作占位; T1/T2 reverify 调 Python helper, helper 自己\n"
        "                          从 config/pdtm.conf 读 httpx_bin)\n"
        "--run-id ID:              run_id for logs (默认 timestamp)\n"
        "\n"
        "退出码:\n"
        "  0 = OK, 2 = args, 3 = schema, 4 = inflate, 5 = update, 6 = reverify helper\n",
        prog);
}

std::string make_default_run_id() {
    auto t = std::time(nullptr);
    auto tm = *std::gmtime(&t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return std::string(buf);
}

}  // namespace

int main(int argc, char** argv) {
    // --help / -h 在任何位置接受 (不要求 db_path)
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(argv[0]); return 0;
        }
    }
    if (argc < 2) {
        usage(argv[0]); return 2;
    }

    algo_diff_cpp::PipelineOptions opts;
    opts.db_path = argv[1];
    opts.run_id = make_default_run_id();

    // PATCH-3PKG-HTTPX-2026-10-27
    // ---- 命令行参数 (最高优先级; 空串视为"未指定") ----
    // 2026-10-27 三包 httpx 路径解析修正:
    //   原实现顺序是 "env/pdtm.conf 兜底 -> 命令行解析", 于是命令行传的
    //   空值 (--httpx-path "") 会把刚解析出来的 httpx_bin 覆盖回空 ->
    //   stage_3pkg 判定"未配置" -> 三包静默降级成 2 包, 日志只有一行
    //   "stage_3pkg: httpx path not configured, skip (2-pkg fallback)"。
    //   改成: 命令行先解析 (空串不覆盖), 再按 env -> pdtm.conf 逐级兜底。
    bool cli_httpx_path_set = false;
    bool cli_resolvers_path_set = false;
    bool cli_fetch_samples_set = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--business-id" && i + 1 < argc) {
            opts.business_id = std::atoi(argv[++i]);
        } else if (a == "--batch" && i + 1 < argc) {
            opts.batch_size = static_cast<size_t>(std::atoi(argv[++i]));
        } else if (a == "--dry-run") {
            opts.dry_run = true;
        } else if (a == "--resolvers-path" && i + 1 < argc) {
            std::string v = argv[++i];
            if (!v.empty()) {
                opts.resolvers_path = v;
                cli_resolvers_path_set = true;
            }
        } else if (a == "--fetch-samples" && i + 1 < argc) {
            opts.fetch_samples = std::atoi(argv[++i]);
            cli_fetch_samples_set = true;
        } else if (a == "--skip-reverify") {
            opts.skip_reverify = true;
        } else if (a == "--run-id" && i + 1 < argc) {
            opts.run_id = argv[++i];
        } else if (a == "--reverify-helper-path" && i + 1 < argc) {
            opts.reverify_helper_path = argv[++i];
        } else if (a == "--httpx-path" && i + 1 < argc) {
            std::string v = argv[++i];
            if (!v.empty()) {
                opts.httpx_path = v;
                cli_httpx_path_set = true;
            }
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            usage(argv[0]); return 2;
        }
    }

    // ---- 环境变量兜底 (命令行没给的项) ----
    // HTTPX_BIN / RESOLVERS_FILE / FETCH_SAMPLES
    // daily_monitor 已导出 HTTPX_BIN (给 T1/T2 的 Python helper 用), 3 包
    // 方案复用同一个 httpx。resolvers 与 httpx 一样从 pdtm.conf 读, 保证
    // 和扫描阶段用的是同一份配置。
    if (!cli_httpx_path_set) {
        const char* hb = std::getenv("HTTPX_BIN");
        if (hb && *hb) opts.httpx_path = hb;
    }
    if (!cli_resolvers_path_set) {
        const char* rf = std::getenv("RESOLVERS_FILE");
        if (rf && *rf) opts.resolvers_path = rf;
    }
    if (!cli_fetch_samples_set) {
        const char* fs = std::getenv("FETCH_SAMPLES");
        if (fs && *fs) opts.fetch_samples = std::atoi(fs);
    }

    // ---- config/pdtm.conf 兜底 (最后一道; 只填仍然为空的项) ----
    // httpx_bin / resolvers_file
    bool need_httpx = !cli_httpx_path_set &&
                      (opts.httpx_path.empty() || opts.httpx_path == "httpx");
    bool need_resolvers = !cli_resolvers_path_set && opts.resolvers_path.empty();
    if (need_httpx || need_resolvers) {
        const char* conf = std::getenv("PDTMCONF");
        if (conf && *conf) {
            std::ifstream cf(conf);
            std::string line;
            while (std::getline(cf, line)) {
                if (line.empty() || line[0] == '#') continue;
                size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string k = line.substr(0, eq);
                std::string v = line.substr(eq + 1);
                while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.pop_back();
                if (k == "httpx_bin" && need_httpx) {
                    opts.httpx_path = v;
                } else if (k == "resolvers_file" && need_resolvers) {
                    opts.resolvers_path = v;
                }
            }
        }
    }

    return algo_diff_cpp::run_pipeline(opts);
}