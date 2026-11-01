// sqlite_io.cpp — SQLite C API wrapper
//
// libsqlite3 C API 直接, 不用 ORM 是更安全的。
//   sqlite3_open_v2 / sqlite3_exec / sqlite3_prepare_v2 / sqlite3_step / sqlite3_column_*
//
// fail-fast: 所有 IO 错返回 false, caller 处理。
//
// 2026-10-24 扩展 (算法能力补齐):
//   - select_for_a_algorithm 增补列 (content_type / learned_anchors / hash_id /
//     host), 并支持按 id 游标分页, 避免 340K 行一次性载入内存
//   - 新增 baseline / field_length_stats / learned_anchors 批量写接口
#include "sqlite_io.hpp"

#include <sqlite3.h>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

namespace algo_diff_cpp {

namespace {

// 打开 db, 返回 sqlite3* (RAII 关闭)
struct Db {
    sqlite3 *handle = nullptr;
    std::string path;

    bool open(const std::string& p, std::string& err) {
        path = p;
        int rc = sqlite3_open_v2(p.c_str(), &handle,
            SQLITE_OPEN_READWRITE, nullptr);
        if (rc != SQLITE_OK) {
            err = std::string("sqlite3_open_v2: ") +
                  (handle ? sqlite3_errmsg(handle) : "unknown");
            if (handle) { sqlite3_close(handle); handle = nullptr; }
            return false;
        }
        return true;
    }

    ~Db() {
        if (handle) sqlite3_close(handle);
    }
};

// 检查 db 里某个列是否存在
bool column_exists(sqlite3 *db, const char* table, const char* col) {
    std::string sql = "PRAGMA table_info(" + std::string(table) + ");";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* name = sqlite3_column_text(stmt, 1);
        if (name && std::strcmp(reinterpret_cast<const char*>(name), col) == 0) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

bool table_exists(sqlite3 *db, const char* table) {
    std::string sql = "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
    bool exists = (sqlite3_step(stmt) == SQLITE_ROW &&
                   sqlite3_column_int(stmt, 0) > 0);
    sqlite3_finalize(stmt);
    return exists;
}

// 事务守卫: BEGIN ... COMMIT / ROLLBACK
class Tx {
public:
    explicit Tx(sqlite3* h) : h_(h), active_(false) {
        char* err = nullptr;
        if (sqlite3_exec(h_, "BEGIN TRANSACTION;", nullptr, nullptr, &err) != SQLITE_OK) {
            if (err) sqlite3_free(err);
            return;
        }
        active_ = true;
    }
    ~Tx() {
        if (active_) {
            sqlite3_exec(h_, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }
    bool active() const { return active_; }
    bool commit(std::string& err) {
        if (!active_) { err = "no active transaction"; return false; }
        char* e = nullptr;
        if (sqlite3_exec(h_, "COMMIT;", nullptr, nullptr, &e) != SQLITE_OK) {
            err = std::string("COMMIT: ") + (e ? e : "?");
            if (e) sqlite3_free(e);
            return false;
        }
        active_ = false;
        return true;
    }

private:
    sqlite3* h_;
    bool active_;
};

}  // namespace

// 可选表的存在性 (由 verify_schema 填充)
bool g_has_length_stats = false;
bool g_has_baseline = false;
bool g_has_learned_anchors = false;
// 2026-10-25: recon_business_config 存在时, 只处理 enabled=1 的业务。
// daily_monitor 已按该开关跳过被关闭业务的扫描, 但 algo_diff_cpp 若不过滤,
// 被关闭业务的历史行仍会被判定为变更并进入报告/通知。
bool g_has_biz_config = false;
// 2026-10-25: 三包样本列 (curr_body_1 / curr_body_2) 是否存在。
// 不存在时 select_for_a_algorithm 不取这两列, 走 2 包路径。
bool g_has_3pkg_cols = false;
// PATCH-FINAL-URL-2026-10-28: final_url 列存在与否 (STEP 1 redirect 槽位)
bool g_has_final_url_col = false;

// ============================================================
// verify_schema
// ============================================================
bool verify_schema(const std::string& db_path, std::string& error_msg) {
    Db db;
    if (!db.open(db_path, error_msg)) return false;

    // 2026-10-24: 三张"学习型"表是可选的 —— 它们由 algo_diff 自己写入,
    // 但库可能是从没有跑过 algo_diff 的旧库迁移过来的。
    // 缺表时降级为"该能力不生效", 而不是让整个 pipeline 失败。
    g_has_length_stats = table_exists(db.handle, "field_length_stats");
    g_has_baseline     = table_exists(db.handle, "web_subdomain_baseline");
    g_has_learned_anchors = column_exists(db.handle, "web_hashes", "learned_anchors");
    g_has_biz_config   = table_exists(db.handle, "recon_business_config");
    g_has_3pkg_cols    = column_exists(db.handle, "web_subdomains", "curr_body_1") &&
                         column_exists(db.handle, "web_subdomains", "curr_body_2");
    g_has_final_url_col = column_exists(db.handle, "web_subdomains", "final_url") &&
                          column_exists(db.handle, "web_subdomains_pre", "final_url");

    auto require = [&](const char* table, const char* col) -> bool {
        if (!table_exists(db.handle, table)) {
            error_msg = std::string("table missing: ") + table;
            return false;
        }
        if (!column_exists(db.handle, table, col)) {
            error_msg = std::string("column missing: ") + table + "." + col;
            return false;
        }
        return true;
    };

    bool ok =
        require("web_subdomains", "id") &&
        require("web_subdomains", "curr_body") &&
        require("web_subdomains", "prev_body") &&
        require("web_subdomains", "change_type") &&
        require("web_subdomains", "is_active") &&
        require("web_subdomains_pre", "id") &&
        require("web_subdomains_pre", "is_active") &&
        require("web_subdomains_pre", "raw_json");

    if (!ok) {
        error_msg = std::string("schema verify failed: ") + error_msg;
        return false;
    }
    return true;
}

// ============================================================
// select_for_fast_transition
// ============================================================
bool select_for_fast_transition(const std::string& db_path,
                                std::vector<Row>& out,
                                std::string& error_msg) {
    Db db;
    if (!db.open(db_path, error_msg)) return false;

    // 2026-10-25: 只处理 enabled=1 的业务 (recon_business_config 存在时)。
    if (!g_has_biz_config) {
        g_has_biz_config = table_exists(db.handle, "recon_business_config");
    }
    if (!g_has_3pkg_cols) {
        g_has_3pkg_cols = column_exists(db.handle, "web_subdomains", "curr_body_1") &&
                           column_exists(db.handle, "web_subdomains", "curr_body_2");
    }
    if (!g_has_final_url_col) {
        g_has_final_url_col = column_exists(db.handle, "web_subdomains", "final_url") &&
                              column_exists(db.handle, "web_subdomains_pre", "final_url");
    }
    std::string sql =
        "SELECT cur.id, cur.change_type, cur.is_active, "
        "       pre.is_active IS NOT NULL, "
        "       COALESCE(pre.is_active, 0), "
        "       cur.curr_body "
        "FROM web_subdomains cur "
        "LEFT JOIN web_subdomains_pre pre ON pre.id = cur.id ";
    if (g_has_biz_config) {
        // 2026-10-25 修: 原来是 INNER JOIN, 会静默丢掉"没有 config 行"或
        // "config 行 enabled=0"的业务的全部行 —— 那些行走不到 T3 的状态机,
        // change_type 停在旧值 (实测 smoke 出现 ct=8 失活残留), 而日志里
        // 看不到任何解释。
        // 改成 LEFT JOIN + 显式条件: 只过滤"明确 enabled=0"的业务;
        // 没有 config 行 (= 未明确停用) 的业务保留。
        sql +=
            "LEFT JOIN web_hashes wh_biz ON wh_biz.id = cur.hash_id "
            "LEFT JOIN recon_business_config rbc ON rbc.business_id = wh_biz.business_id ";
        sql += "WHERE cur.change_type = 0 "
               "AND (rbc.enabled = 1 OR rbc.business_id IS NULL)";
    } else {
        sql += "WHERE cur.change_type = 0;";
    }

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db.handle, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error_msg = std::string("prepare select_for_fast: ") + sqlite3_errmsg(db.handle);
        return false;
    }

    out.clear();
    while (true) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            error_msg = std::string("step select_for_fast: ") + sqlite3_errmsg(db.handle);
            sqlite3_finalize(stmt);
            return false;
        }
        Row r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.ct = static_cast<int8_t>(sqlite3_column_int(stmt, 1));
        r.curr_is_active = sqlite3_column_int(stmt, 2) != 0;
        r.pre_is_active_exists = sqlite3_column_int(stmt, 3) != 0;
        r.pre_is_active = sqlite3_column_int(stmt, 4) != 0;

        const unsigned char* blob = static_cast<const unsigned char*>(
            sqlite3_column_blob(stmt, 5));
        int blob_sz = sqlite3_column_bytes(stmt, 5);
        if (blob && blob_sz > 0) {
            r.curr_body.assign(blob, blob + blob_sz);
        }
        out.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return true;
}

// ============================================================
// select_for_a_algorithm
// ============================================================
bool select_for_a_algorithm(const std::string& db_path,
                            std::vector<Row>& out,
                            std::string& error_msg,
                            int64_t after_id,
                            int64_t batch_size) {
    Db db;
    if (!db.open(db_path, error_msg)) return false;

    // verify_schema 可能没被调用过 (单元测试直接调本函数), 这里幂等探测一次
    if (!g_has_learned_anchors && !g_has_length_stats) {
        g_has_learned_anchors = column_exists(db.handle, "web_hashes", "learned_anchors");
        g_has_length_stats = table_exists(db.handle, "field_length_stats");
        g_has_baseline = table_exists(db.handle, "web_subdomain_baseline");
    }
    if (!g_has_biz_config) {
        g_has_biz_config = table_exists(db.handle, "recon_business_config");
    }

    // 2026-10-24: 增补列
    //   col 21: cur.subdomain + ':' + cur.port  -> host (field_length_stats 主键)
    //   col 22: wh.learned_anchors (跨天累积的锚点, JSON)
    //   col 23: wh.id (learned_anchors 宿主行)
    //   col 24: COALESCE(cur.content_type, pre.content_type, '')
    // 分页: after_id > 0 时只取 id > after_id 的行, ORDER BY id, LIMIT batch。
    //
    // 三张"学习型"表/列是可选的 (旧库可能没有), 此时对应的 SQL 片段
    // 退化成常量, 让该能力降级为"不生效"而不是查询失败。
    std::string sql;
    sql +=
        "SELECT cur.id, cur.change_type, pre.is_active IS NOT NULL, "
        "       COALESCE(pre.is_active, 0), "
        "       cur.curr_body, pre.raw_json, pre.curr_body, "
        "       wh_pre.response_hash, wh.response_hash, ";
    // baseline: 存在时取两列, 不存在时退化成常量 0 / 0.0
    if (g_has_baseline) {
        sql += "       b.baseline_ratio IS NOT NULL, COALESCE(b.baseline_ratio, 0.0), ";
    } else {
        sql += "       0, 0.0, ";
    }
    sql +=
        "       COALESCE(pre.status_code, 0), COALESCE(pre.content_length, 0), "
        "       COALESCE(pre.title, ''), COALESCE(pre.technologies, ''), "
        "       COALESCE(pre.content_type, ''), "
        "       COALESCE(cur.status_code, 0), COALESCE(cur.content_length, 0), "
        "       COALESCE(cur.title, ''), COALESCE(cur.technologies, ''), "
        "       COALESCE(cur.content_type, ''), "
        "       cur.subdomain || ':' || cur.port, ";
    // learned_anchors: 列不存在时退化成常量 '[]'
    if (g_has_learned_anchors) {
        sql += "       COALESCE(wh.learned_anchors, '[]'), ";
    } else {
        sql += "       '[]', ";
    }
    sql +=
        "       COALESCE(wh.id, 0), "
        "       COALESCE(cur.content_type, pre.content_type, ''), ";
    // 三包样本列是后加的 (2026-10-25 迁移), 老库没有 -> 不取这两列,
    // T4 走 2 包路径。硬编码会让 prepare 直接失败。
    if (g_has_3pkg_cols) {
        sql += "cur.curr_body_1, cur.curr_body_2 ";
    } else {
        sql += "NULL, NULL ";
    }
    // PATCH-FINAL-URL-2026-10-28: final_url (httpx 跟随重定向后的最终落点)。
    // 填进 STEP 1 快字段里那个一直没被用过的 redirect 槽位。
    // 不能用 url 列代替: url 的形态逐日会翻转 (域名 <-> IP), 会把形态抖动
    // 误判成变更。final_url 形态稳定; 2xx 响应里没有该字段 -> 空串,
    // 两边都空即相等 -> 自动跳过 (200 与 302 不需要分流)。
    // 列不存在时退化成常量 '' (老库), 槽位恒空 -> 行为与改动前一致。
    // 放在 SELECT 末尾: 不影响前面各列的 col 序号。
    if (g_has_final_url_col) {
        sql += ", COALESCE(pre.final_url, ''), COALESCE(cur.final_url, '') ";
    } else {
        sql += ", '', '' ";
    }
    sql +=
        "FROM web_subdomains cur "
        "LEFT JOIN web_subdomains_pre pre ON pre.id = cur.id "
        "LEFT JOIN web_hashes wh ON wh.id = cur.hash_id "
        "LEFT JOIN web_hashes wh_pre ON wh_pre.id = pre.hash_id ";
    if (g_has_baseline) {
        sql += "LEFT JOIN web_subdomain_baseline b ON b.subdomain_id = cur.id ";
    }
    // 2026-10-25: 只处理 enabled=1 的业务 (recon_business_config 存在时)。
    // 与 select_for_fast_transition 保持一致: 被关闭业务不参与变更判定,
    // 其历史行不再被写入 change_type, 也就不会进报告 / 通知 / 面板。
    if (g_has_biz_config) {
        // 2026-10-25 修: 同 select_for_fast_transition —— INNER JOIN 会静默
        // 丢掉没有 config 行 / 明确 enabled=0 的业务的行。
        sql +=
            "LEFT JOIN recon_business_config rbc ON rbc.business_id = wh.business_id ";
    }
    sql += "WHERE cur.change_type = 10";
    if (g_has_biz_config) {
        sql += " AND (rbc.enabled = 1 OR rbc.business_id IS NULL)";
    }
    if (after_id > 0) {
        sql += " AND cur.id > " + std::to_string(after_id);
    }
    sql += " ORDER BY cur.id";
    if (batch_size > 0) {
        sql += " LIMIT " + std::to_string(batch_size);
    }
    sql += ";";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db.handle, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error_msg = std::string("prepare select_for_a: ") + sqlite3_errmsg(db.handle);
        return false;
    }

    out.clear();
    while (true) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            error_msg = std::string("step select_for_a: ") + sqlite3_errmsg(db.handle);
            sqlite3_finalize(stmt);
            return false;
        }
        Row r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.ct = static_cast<int8_t>(sqlite3_column_int(stmt, 1));
        r.curr_is_active = true;
        r.pre_is_active_exists = sqlite3_column_int(stmt, 2) != 0;
        r.pre_is_active = sqlite3_column_int(stmt, 3) != 0;

        // col 4: cur.curr_body (BLOB)
        const unsigned char* blob = static_cast<const unsigned char*>(
            sqlite3_column_blob(stmt, 4));
        int blob_sz = sqlite3_column_bytes(stmt, 4);
        if (blob && blob_sz > 0) {
            r.curr_body.assign(blob, blob + blob_sz);
        }

        // col 5: pre.raw_json (text)
        const unsigned char* raw = sqlite3_column_text(stmt, 5);
        int raw_sz = sqlite3_column_bytes(stmt, 5);
        if (raw && raw_sz > 0) {
            r.raw_json.assign(reinterpret_cast<const char*>(raw), raw_sz);
        }

        // col 6: pre.curr_body (BLOB)
        const unsigned char* pre_blob = static_cast<const unsigned char*>(
            sqlite3_column_blob(stmt, 6));
        int pre_blob_sz = sqlite3_column_bytes(stmt, 6);
        if (pre_blob && pre_blob_sz > 0) {
            r.pre_curr_body.assign(pre_blob, pre_blob + pre_blob_sz);
        }

        // col 7: pre.web_hashes.response_hash (text, mmh3 int string)
        const unsigned char* wh_pre = sqlite3_column_text(stmt, 7);
        if (wh_pre) {
            r.pre_web_hash_exists = true;
            r.pre_web_hash.assign(reinterpret_cast<const char*>(wh_pre));
        } else {
            r.pre_web_hash_exists = false;
        }

        // col 8: cur.web_hashes.response_hash (text, mmh3 int string)
        const unsigned char* wh = sqlite3_column_text(stmt, 8);
        if (wh) {
            r.cur_web_hash_exists = true;
            r.cur_web_hash.assign(reinterpret_cast<const char*>(wh));
        } else {
            r.cur_web_hash_exists = false;
        }

        // col 9-10: baseline_ratio exists + value
        r.baseline_exists = sqlite3_column_int(stmt, 9) != 0;
        r.baseline_ratio = sqlite3_column_double(stmt, 10);

        // col 11-15: pre 5 快速字段
        r.pre_status_code = sqlite3_column_int(stmt, 11);
        r.pre_content_length = sqlite3_column_int64(stmt, 12);
        const unsigned char* title = sqlite3_column_text(stmt, 13);
        if (title) r.pre_title.assign(reinterpret_cast<const char*>(title));
        const unsigned char* tech = sqlite3_column_text(stmt, 14);
        if (tech) r.pre_technologies.assign(reinterpret_cast<const char*>(tech));
        const unsigned char* ctype = sqlite3_column_text(stmt, 15);
        if (ctype) r.pre_content_type.assign(reinterpret_cast<const char*>(ctype));

        // col 16-20: cur 端 5 快速字段 (T4 Step 1/2 用)
        r.cur_status_code = sqlite3_column_int(stmt, 16);
        r.cur_content_length = sqlite3_column_int64(stmt, 17);
        const unsigned char* cur_title = sqlite3_column_text(stmt, 18);
        if (cur_title) r.cur_title.assign(reinterpret_cast<const char*>(cur_title));
        const unsigned char* cur_tech = sqlite3_column_text(stmt, 19);
        if (cur_tech) r.cur_technologies.assign(reinterpret_cast<const char*>(cur_tech));
        const unsigned char* cur_ctype = sqlite3_column_text(stmt, 20);
        if (cur_ctype) r.cur_content_type.assign(reinterpret_cast<const char*>(cur_ctype));

        // col 21: host
        const unsigned char* host = sqlite3_column_text(stmt, 21);
        if (host) r.host.assign(reinterpret_cast<const char*>(host));

        // col 22: learned_anchors
        const unsigned char* la = sqlite3_column_text(stmt, 22);
        if (la) r.learned_anchors_json.assign(reinterpret_cast<const char*>(la));

        // col 23: hash_id
        r.hash_id = sqlite3_column_int64(stmt, 23);

        // col 24: effective content_type
        const unsigned char* eff_ct = sqlite3_column_text(stmt, 24);
        if (eff_ct) r.cur_content_type.assign(reinterpret_cast<const char*>(eff_ct));

        // 三包样本 (2026-10-25): col 25 = curr_body_1, col 26 = curr_body_2
        {
            const void* b1 = sqlite3_column_blob(stmt, 25);
            int b1_sz = sqlite3_column_bytes(stmt, 25);
            if (b1 && b1_sz > 0) {
                r.curr_body_1.assign(static_cast<const unsigned char*>(b1),
                                     static_cast<const unsigned char*>(b1) + b1_sz);
            }
            const void* b2 = sqlite3_column_blob(stmt, 26);
            int b2_sz = sqlite3_column_bytes(stmt, 26);
            if (b2 && b2_sz > 0) {
                r.curr_body_2.assign(static_cast<const unsigned char*>(b2),
                                     static_cast<const unsigned char*>(b2) + b2_sz);
            }
        }

        // PATCH-FINAL-URL-2026-10-28: col 27 = pre.final_url, col 28 = cur.final_url
        {
            const unsigned char* pf = sqlite3_column_text(stmt, 27);
            if (pf) r.pre_final_url.assign(reinterpret_cast<const char*>(pf));
            const unsigned char* cf = sqlite3_column_text(stmt, 28);
            if (cf) r.cur_final_url.assign(reinterpret_cast<const char*>(cf));
        }

        out.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return true;
}

// ============================================================
// update_reverify_results
// ============================================================
bool update_reverify_results(const std::string& db_path,
                             const std::vector<UpdateRow>& rows,
                             std::string& error_msg) {
    if (rows.empty()) return true;
    Db dbs;
    if (!dbs.open(db_path, error_msg)) return false;

    // 跑事务
    char* errmsg = nullptr;
    if (sqlite3_exec(dbs.handle, "BEGIN TRANSACTION;", nullptr, nullptr, &errmsg) != SQLITE_OK) {
        error_msg = std::string("BEGIN: ") + (errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        return false;
    }

    const char* upd_sql =
        "UPDATE web_subdomains SET "
        "curr_body = ?, "
        "change_type = ?, "
        "last_comparison_ratio = NULL, "
        "last_comparison_kind = ?, "
        "last_comparison_at = ? "
        "WHERE id = ?";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(dbs.handle, upd_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error_msg = std::string("prepare update_reverify: ") + sqlite3_errmsg(dbs.handle);
        sqlite3_exec(dbs.handle, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    for (const auto& r : rows) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        // body bytes (UpdateRow.curr_body 通过 UpdateRow::body 间接加 50KB)
        // 注: UpdateRow 没有 body 字段, 仅 new_ct + kind + at。reverify
        // 写回 body 留给 pipeline.cpp 单独做 BLOB upload (SQLITE_LIMIT_LENGTH)。
        // 本函数只更新 ct / kind / last_comparison_at。
        sqlite3_bind_int(stmt, 1, static_cast<int>(r.new_ct));
        sqlite3_bind_text(stmt, 2, r.kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, r.at.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, r.id);

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            error_msg = std::string("step update_reverify: ") + sqlite3_errmsg(dbs.handle);
            sqlite3_finalize(stmt);
            sqlite3_exec(dbs.handle, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }
    sqlite3_finalize(stmt);

    if (sqlite3_exec(dbs.handle, "COMMIT;", nullptr, nullptr, &errmsg) != SQLITE_OK) {
        error_msg = std::string("COMMIT: ") + (errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

// ============================================================
// update_change_type_batch
// ============================================================
bool update_change_type_batch(const std::string& db_path,
                              const std::vector<UpdateRow>& updates,
                              std::string& error_msg) {
    if (updates.empty()) return true;
    Db dbs;
    if (!dbs.open(db_path, error_msg)) return false;

    char* errmsg = nullptr;
    if (sqlite3_exec(dbs.handle, "BEGIN TRANSACTION;", nullptr, nullptr, &errmsg) != SQLITE_OK) {
        error_msg = std::string("BEGIN: ") + (errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        return false;
    }

    const char* upd_sql =
        "UPDATE web_subdomains SET "
        "change_type = ?, "
        "last_comparison_ratio = ?, "
        "last_comparison_kind = ?, "
        "last_comparison_at = COALESCE(?, last_comparison_at) "
        "WHERE id = ?";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(dbs.handle, upd_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error_msg = std::string("prepare update_batch: ") + sqlite3_errmsg(dbs.handle);
        sqlite3_exec(dbs.handle, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    for (const auto& r : updates) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);

        sqlite3_bind_int(stmt, 1, static_cast<int>(r.new_ct));
        if (r.ratio == r.ratio) {  // non-NaN
            sqlite3_bind_double(stmt, 2, r.ratio);
        } else {
            sqlite3_bind_null(stmt, 2);
        }
        if (!r.kind.empty()) {
            sqlite3_bind_text(stmt, 3, r.kind.c_str(), -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, 3);
        }
        if (!r.at.empty()) {
            sqlite3_bind_text(stmt, 4, r.at.c_str(), -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(stmt, 4);
        }
        sqlite3_bind_int64(stmt, 5, r.id);

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            error_msg = std::string("step update_batch: ") + sqlite3_errmsg(dbs.handle);
            sqlite3_finalize(stmt);
            sqlite3_exec(dbs.handle, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }
    sqlite3_finalize(stmt);

    if (sqlite3_exec(dbs.handle, "COMMIT;", nullptr, nullptr, &errmsg) != SQLITE_OK) {
        error_msg = std::string("COMMIT: ") + (errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

// ============================================================
// upsert_baseline_batch
// ============================================================
// 与 Python compute.py 的 _write_back 一致:
//   INSERT ... ON CONFLICT(subdomain_id) DO UPDATE SET
//     baseline_ratio = ((baseline_ratio * sample_count) + excluded.baseline_ratio)
//                     / (sample_count + 1),
//     sample_count = sample_count + 1
// 即滑动平均。
bool upsert_baseline_batch(const std::string& db_path,
                            const std::vector<BaselineRow>& rows,
                            std::string& error_msg) {
    if (rows.empty()) return true;
    // 表不存在 -> 降级 (baseline 能力不生效, 但 pipeline 不失败)
    if (!g_has_baseline) return true;
    Db dbs;
    if (!dbs.open(db_path, error_msg)) return false;

    Tx tx(dbs.handle);
    if (!tx.active()) {
        error_msg = "upsert_baseline: BEGIN failed";
        return false;
    }

    const char* sql =
        "INSERT INTO web_subdomain_baseline "
        "  (subdomain_id, baseline_ratio, sample_count, created_at, updated_at) "
        "VALUES (?, ?, 1, datetime('now'), datetime('now')) "
        "ON CONFLICT(subdomain_id) DO UPDATE SET "
        "  baseline_ratio = ((baseline_ratio * sample_count) + excluded.baseline_ratio) "
        "                  / (sample_count + 1), "
        "  sample_count   = sample_count + 1, "
        "  updated_at     = excluded.updated_at";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(dbs.handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error_msg = std::string("prepare upsert_baseline: ") + sqlite3_errmsg(dbs.handle);
        return false;
    }
    for (const BaselineRow& r : rows) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, r.subdomain_id);
        sqlite3_bind_double(stmt, 2, r.ratio);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            error_msg = std::string("step upsert_baseline: ") + sqlite3_errmsg(dbs.handle);
            sqlite3_finalize(stmt);
            return false;
        }
    }
    sqlite3_finalize(stmt);

    if (!tx.commit(error_msg)) return false;
    return true;
}

// ============================================================
// upsert_length_stats_batch
// ============================================================
// 长度漂移时 hit_count 重置为 1 (与 Python compute.py 一致):
//   hit_count = CASE WHEN length != excluded.length THEN 1 ELSE hit_count + 1 END
bool upsert_length_stats_batch(const std::string& db_path,
                               const std::vector<LengthStatRow>& rows,
                               std::string& error_msg) {
    if (rows.empty()) return true;
    // 表不存在 -> 降级
    if (!g_has_length_stats) return true;
    Db dbs;
    if (!dbs.open(db_path, error_msg)) return false;

    Tx tx(dbs.handle);
    if (!tx.active()) {
        error_msg = "upsert_length_stats: BEGIN failed";
        return false;
    }

    const char* sql =
        "INSERT INTO field_length_stats "
        "  (host, path, length, hit_count, content_sample_hash, "
        "   first_seen_at, last_seen_at) "
        "VALUES (?, ?, ?, 1, ?, datetime('now'), datetime('now')) "
        "ON CONFLICT(host, path) DO UPDATE SET "
        "  length = excluded.length, "
        "  hit_count = CASE "
        "      WHEN length != excluded.length THEN 1 "
        "      ELSE hit_count + 1 END, "
        "  content_sample_hash = excluded.content_sample_hash, "
        "  last_seen_at = excluded.last_seen_at";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(dbs.handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error_msg = std::string("prepare upsert_length_stats: ") + sqlite3_errmsg(dbs.handle);
        return false;
    }
    for (const LengthStatRow& r : rows) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, r.host.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, r.path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, r.length);
        sqlite3_bind_int64(stmt, 4, r.content_sample_hash);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            error_msg = std::string("step upsert_length_stats: ") + sqlite3_errmsg(dbs.handle);
            sqlite3_finalize(stmt);
            return false;
        }
    }
    sqlite3_finalize(stmt);

    if (!tx.commit(error_msg)) return false;
    return true;
}

// ============================================================
// update_learned_anchors_batch
// ============================================================
// 跨天累积的锚点写回 web_hashes.learned_anchors。
bool update_learned_anchors_batch(const std::string& db_path,
                                  const std::vector<AnchorRow>& rows,
                                  std::string& error_msg) {
    if (rows.empty()) return true;
    // 列不存在 -> 降级
    if (!g_has_learned_anchors) return true;
    Db dbs;
    if (!dbs.open(db_path, error_msg)) return false;

    Tx tx(dbs.handle);
    if (!tx.active()) {
        error_msg = "update_learned_anchors: BEGIN failed";
        return false;
    }

    const char* sql =
        "UPDATE web_hashes SET learned_anchors = ? WHERE id = ?";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(dbs.handle, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error_msg = std::string("prepare update_learned_anchors: ") + sqlite3_errmsg(dbs.handle);
        return false;
    }
    for (const AnchorRow& r : rows) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, r.anchors_json.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, r.hash_id);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            error_msg = std::string("step update_learned_anchors: ") + sqlite3_errmsg(dbs.handle);
            sqlite3_finalize(stmt);
            return false;
        }
    }
    sqlite3_finalize(stmt);

    if (!tx.commit(error_msg)) return false;
    return true;
}

// ============================================================
// select_length_stats
// ============================================================
bool select_length_stats(const std::string& db_path,
                          const std::string& host,
                          std::vector<LengthStat>& out,
                          std::string& error_msg) {
    std::vector<std::string> hosts;
    if (!host.empty()) hosts.push_back(host);
    return select_length_stats_multi(db_path, hosts, out, error_msg);
}

// ============================================================
// select_length_stats_multi
// ============================================================
// 一次查询取多个 host 的长度统计, 避免逐 host 查询的 N+1。
// ============================================================
// 三包取样 (2026-10-25)
// ============================================================
bool select_three_pkg_targets(const std::string& db_path,
                               int business_id,
                               std::vector<ThreePkgTarget>& out,
                               std::string& error_msg) {
    out.clear();
    Db db;
    if (!db.open(db_path, error_msg)) return false;

    // curr_body_1 / curr_body_2 是后加的列, 老库可能没有 -> 降级为不取样。
    if (!column_exists(db.handle, "web_subdomains", "curr_body_1")) {
        return true;
    }

    // 取样 URL = scheme://subdomain:port/, 匹配键 = subdomain:port。
    // 键必须与 http_fetch.cpp 的 normalize_hostport() 规则一致 (那边用的是
    // httpx 的 input 字段, 而 input 就是我们传进去的 URL 原文)。
    // 取样 URL 用 **域名** (c.subdomain) 而不是 c.url 的 host:
    //   - c.url 的 host 有小部分行是解析后的 IP 形式, HTTPS 端口直连 IP
    //     会 SNI 不匹配 -> 返回错误页 -> 样本长度与原 body 不一致 ->
    //     三包判 applicable=false -> 静默失效。
    //   - 扫描阶段 (scanner.sh) 的 httpx 输入是 域名:端口, curr_body 就是
    //     这么采到的; 三包必须用同样的 URL, 否则内容不可比。
    // scheme 仍取自 c.url (http/https 才是对的); port 取自 c.port 列。
    std::string sql =
        "SELECT c.id, "
        "       CASE WHEN instr(c.url, '://') > 0 "
        "            THEN substr(c.url, 1, instr(c.url, '://') - 1) "
        "            ELSE 'http' END "
        "       || '://' || c.subdomain || ':' || c.port || '/', "
        "       c.subdomain || ':' || c.port "
        "FROM web_subdomains c "
        "WHERE c.change_type = 10 AND c.curr_body IS NOT NULL "
        "  AND c.subdomain IS NOT NULL AND c.subdomain <> ''";
    if (business_id > 0) {
        sql += " AND c.business_id = " + std::to_string(business_id);
    }
    sql += " ORDER BY c.id;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.handle, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error_msg = std::string("prepare select_three_pkg: ") + sqlite3_errmsg(db.handle);
        return false;
    }
    while (true) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) {
            error_msg = std::string("step select_three_pkg: ") + sqlite3_errmsg(db.handle);
            sqlite3_finalize(stmt);
            return false;
        }
        ThreePkgTarget t;
        t.id = sqlite3_column_int64(stmt, 0);
        const unsigned char* u = sqlite3_column_text(stmt, 1);
        if (u) t.url.assign(reinterpret_cast<const char*>(u));
        const unsigned char* hpt = sqlite3_column_text(stmt, 2);
        if (hpt) t.host_port.assign(reinterpret_cast<const char*>(hpt));
        out.push_back(std::move(t));
    }
    sqlite3_finalize(stmt);
    return true;
}

bool update_three_pkg_bodies(const std::string& db_path,
                             const std::vector<ThreePkgBodyRow>& rows,
                             std::string& error_msg) {
    if (rows.empty()) return true;
    Db dbs;
    if (!dbs.open(db_path, error_msg)) return false;

    if (!column_exists(dbs.handle, "web_subdomains", "curr_body_1") ||
        !column_exists(dbs.handle, "web_subdomains", "curr_body_2")) {
        error_msg = "columns curr_body_1/curr_body_2 missing";
        return false;
    }

    const char* upd_sql =
        "UPDATE web_subdomains SET curr_body_1 = ?, curr_body_2 = ? WHERE id = ?";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(dbs.handle, upd_sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error_msg = std::string("prepare update_three_pkg: ") + sqlite3_errmsg(dbs.handle);
        return false;
    }

    Tx tx(dbs.handle);
    if (!tx.active()) {
        error_msg = "BEGIN transaction failed";
        sqlite3_finalize(stmt);
        return false;
    }
    for (const ThreePkgBodyRow& r : rows) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_blob(stmt, 1, r.body_1.data(),
                          static_cast<int>(r.body_1.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 2, r.body_2.data(),
                          static_cast<int>(r.body_2.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 3, r.id);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            error_msg = std::string("step update_three_pkg: ") + sqlite3_errmsg(dbs.handle);
            sqlite3_finalize(stmt);
            return false;   // Tx 析构自动 ROLLBACK
        }
    }
    if (!tx.commit(error_msg)) {
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);
    return true;
}

bool select_length_stats_multi(const std::string& db_path,
                               const std::vector<std::string>& hosts,
                               std::vector<LengthStat>& out,
                               std::string& error_msg) {
    out.clear();
    if (hosts.empty()) return true;
    // 表不存在 -> 降级: mask 层无统计可用, 但不影响主流程
    if (!g_has_length_stats) return true;

    Db db;
    if (!db.open(db_path, error_msg)) return false;

    // 分批 IN 查询, 每批 400 个 (SQLite 默认 SQLITE_MAX_VARIABLE_NUMBER=999)
    const size_t CHUNK = 400;
    for (size_t i = 0; i < hosts.size(); i += CHUNK) {
        size_t end = std::min(hosts.size(), i + CHUNK);
        std::string sql =
            "SELECT host, path, length, hit_count, "
            "       COALESCE(content_sample_hash, 0) "
            "FROM field_length_stats WHERE host IN (";
        for (size_t k = i; k < end; ++k) {
            if (k > i) sql += ",";
            sql += "?";
        }
        sql += ");";

        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(db.handle, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            error_msg = std::string("prepare select_length_stats: ") + sqlite3_errmsg(db.handle);
            return false;
        }
        for (size_t k = i; k < end; ++k) {
            sqlite3_bind_text(stmt, static_cast<int>(k - i) + 1,
                              hosts[k].c_str(), -1, SQLITE_TRANSIENT);
        }
        while (true) {
            int rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                error_msg = std::string("step select_length_stats: ") + sqlite3_errmsg(db.handle);
                sqlite3_finalize(stmt);
                return false;
            }
            LengthStat st;
            const unsigned char* host = sqlite3_column_text(stmt, 0);
            if (host) st.host.assign(reinterpret_cast<const char*>(host));
            const unsigned char* path = sqlite3_column_text(stmt, 1);
            if (path) st.path.assign(reinterpret_cast<const char*>(path));
            st.length = sqlite3_column_int64(stmt, 2);
            st.hit_count = sqlite3_column_int64(stmt, 3);
            st.content_sample_hash = sqlite3_column_int64(stmt, 4);
            out.push_back(std::move(st));
        }
        sqlite3_finalize(stmt);
    }
    return true;
}

}  // namespace algo_diff_cpp
