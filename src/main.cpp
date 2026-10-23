// KrgnMiner - GPU miner for Kerrigan (KRGN), Equihash(192,7) with the "kerrigan"
// personalization, Zcash-style stratum (rplant).  No developer fee.  Apache-2.0.
//
//   krgnminer.exe                     mine with krgnminer.conf
//   krgnminer.exe --test              CPU checks + GPU solver against Kerrigan's own solutions
//   krgnminer.exe --bench [seconds]   Sol/s benchmark on the selected GPUs
//
// Options (also key=value lines in krgnminer.conf, without "--"):
//   -o / --pool URL (repeatable, failover order)   -u / --user ADDRESS.RIG   -p / --pass X
//   --gpus 0,1|all   --tpb N (threads per block, collision kernels)   --stats N (seconds)
//   --log FILE   --args-file FILE   --cafile FILE
//
// Stratum (rplant / NOMP equihash):
//   -> mining.subscribe [agent]                  <- [session, extranonce1]
//   -> mining.authorize [user, pass]             <- true
//   <- mining.set_target [target_hex_be]
//   <- mining.notify [job, version, prevhash, merkleroot, reserved, time, bits, clean, "192_7", "kerrigan"]
//   -> mining.submit [user, job, time, nonce2, "fd9001" + solution(400 bytes)]
// Header fields are sent in serialized byte order; nonce (32 bytes) = extranonce1 || nonce2.
#include <stdio.h>
#include <stdlib.h>
#include <cstdlib>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "eq_host.h"
#include "eq_vectors.h"
#include "json.h"
#include "tls_conn.h"
#include "../gpu/eq_gpu.h"

#define KRGNMINER_VERSION "0.1.0"

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
static std::mutex g_log_mtx;
static FILE* g_logf = nullptr;

static void logf_(const char* fmt, ...)
{
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
    std::lock_guard<std::mutex> lk(g_log_mtx);
    printf("[%s] %s\n", ts, msg);
    fflush(stdout);
    if (g_logf) { fprintf(g_logf, "[%s] %s\n", ts, msg); fflush(g_logf); }
}
#define LOG(...) logf_(__VA_ARGS__)

// ---------------------------------------------------------------------------
// hex / target helpers
// ---------------------------------------------------------------------------
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static std::string strip0x(const std::string& s)
{
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return s.substr(2);
    return s;
}
static bool hex_to_bytes(const std::string& h0, uint8_t* out, size_t n)
{
    std::string h = strip0x(h0);
    if (h.size() != 2 * n) return false;
    for (size_t i = 0; i < n; i++) {
        int a = hexval(h[2 * i]), b = hexval(h[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return true;
}
static std::string to_hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s(2 * n, '0');
    for (size_t i = 0; i < n; i++) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
    return s;
}
// value of a big-endian 256-bit number
static double be_value(const uint8_t t[32])
{
    double v = 0;
    for (int i = 0; i < 32; i++) v = v * 256.0 + t[i];
    return v;
}
// pool difficulty (NOMP equihash): diff 1 = target 0x0007ffff << 224
static double target_diff(const uint8_t t[32])
{
    double v = be_value(t);
    return v > 0 ? (double)0x07ffff * pow(2.0, 224) / v : 0;
}
static void diff_to_target(double diff, uint8_t out[32])
{
    memset(out, 0, 32);
    if (diff <= 0) { memset(out, 0xff, 32); return; }
    double v = (double)0x07ffff * pow(2.0, 224) / diff;
    for (int i = 31; i >= 0; i--) {
        double q = floor(v / 256.0);
        out[i] = (uint8_t)(v - q * 256.0);
        v = q;
    }
    if (v >= 1) memset(out, 0xff, 32);
}
// SHA256d output (little-endian number) <= big-endian target
static bool hash_le_target(const uint8_t h[32], const uint8_t t[32])
{
    for (int i = 0; i < 32; i++) {
        uint8_t a = h[31 - i], b = t[i];
        if (a != b) return a < b;
    }
    return true;
}
// nBits given as 4 little-endian bytes (header order) -> big-endian target
static bool nbits_le_target(const uint8_t b[4], uint8_t out[32])
{
    uint32_t nb = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    int exp = (int)(nb >> 24);
    uint32_t man = nb & 0x7fffff;
    memset(out, 0, 32);
    if (exp < 3 || exp > 32 || !man) return false;
    int pos = 32 - exp;
    out[pos] = (uint8_t)(man >> 16);
    out[pos + 1] = (uint8_t)(man >> 8);
    out[pos + 2] = (uint8_t)man;
    return true;
}

// ---------------------------------------------------------------------------
// configuration
// ---------------------------------------------------------------------------
struct Config {
    std::vector<std::string> pools;
    std::string user, pass = "x";
    std::vector<int> gpus;          // empty = all
    int tpb = 256;
    std::string cafile;
    int stats = 30;
    std::string logfile;
    bool test = false;
    bool bench = false;
    bool profile = false;
    int bench_seconds = 60;
    std::string exe_dir = ".";
};

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static bool apply_option(Config& c, const std::string& key, const std::string& val)
{
    std::string k = key;
    while (!k.empty() && k[0] == '-') k.erase(0, 1);
    if (k == "o" || k == "pool" || k == "url") c.pools.push_back(val);
    else if (k == "u" || k == "user") c.user = val;
    else if (k == "p" || k == "pass") c.pass = val;
    else if (k == "gpus" || k == "devices") {
        c.gpus.clear();
        if (val != "all" && !val.empty()) {
            size_t p = 0;
            while (p <= val.size()) {
                size_t q = val.find(',', p);
                std::string one = trim(val.substr(p, q == std::string::npos ? std::string::npos : q - p));
                if (!one.empty()) c.gpus.push_back(atoi(one.c_str()));
                if (q == std::string::npos) break;
                p = q + 1;
            }
        }
    }
    else if (k == "tpb") c.tpb = atoi(val.c_str());
    else if (k == "cafile") c.cafile = val;
    else if (k == "stats") c.stats = atoi(val.c_str());
    else if (k == "log") c.logfile = val;
    else return false;
    return true;
}

static bool load_config_file(Config& c, const std::string& path)
{
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string l = trim(line);
        if (l.empty() || l[0] == '#' || l[0] == ';') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        if (!apply_option(c, k, v)) fprintf(stderr, "config: unknown key '%s'\n", k.c_str());
    }
    fclose(f);
    return true;
}

static std::string exe_directory(const char* argv0)
{
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    std::string p = n ? std::string(buf, n) : std::string(argv0);
#else
    std::string p = argv0;
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) p.assign(buf, (size_t)n);
#endif
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? "." : p.substr(0, s);
}

static bool file_exists(const std::string& p)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// shared mining state
// ---------------------------------------------------------------------------
struct PoolJob {
    uint64_t seq = 0;
    std::string job_id, ntime_hex;
    uint8_t header[108];             // version .. bits (serialized order)
    uint8_t target[32];              // share target, big-endian
    uint8_t net_target[32];          // from nbits (zero if unknown)
    bool clean = false;
    uint8_t xn[32];
    size_t xn_len = 0;
};

struct Share {
    uint64_t seq;
    std::string job_id, ntime_hex, nonce2_hex, sol_hex, hash_hex;
    int gpu;
    bool block;
};

static std::atomic<bool> g_running(true);
static std::atomic<int> g_gpu_failed(0);
static std::mutex g_mtx;
static std::shared_ptr<const PoolJob> g_job;
static bool g_paused = true;
static uint64_t g_job_seq = 0;
static uint64_t g_clean_seq = 0;          // seq of the last job with clean = true
static std::deque<Share> g_shares;

struct GpuStat {
    std::atomic<uint64_t> runs{ 0 };
    std::atomic<uint64_t> sols{ 0 };        // valid solutions (CPU-checked)
    std::atomic<uint64_t> dupes{ 0 };       // candidates with repeated indices (normal)
    std::atomic<uint64_t> bad{ 0 };         // candidates that failed the CPU check (GPU error)
    std::atomic<uint64_t> shares{ 0 };
    std::atomic<int> run_ms{ 0 };
    std::atomic<bool> alive{ false };
    std::atomic<long long> beat_ms{ 0 };
    std::string name;
    int dev = -1;
};
static std::vector<std::unique_ptr<GpuStat>> g_stats;

static long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

#define KM_STUCK_MS 20000
static void deadline_watchdog(int seconds, const char* what)
{
    std::thread([seconds, what]() {
        for (int i = 0; i < seconds * 10; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        LOG("%s TIMEOUT after %d s: a GPU is stuck (unstable clocks?), exiting with code 5", what, seconds);
        fflush(nullptr);
        std::_Exit(5);
    }).detach();
}

static std::atomic<uint64_t> g_accepted(0), g_rejected(0), g_blocks(0);
static void on_signal(int) { g_running = false; }

// midstate words for kg_solve
static void midstate_words(const uint8_t header[EQ_HEADER_BYTES], uint64_t w[10])
{
    EqMidstate ms = eq_midstate(header);
    for (int i = 0; i < 8; i++) w[i] = ms.h[i];
    w[8] = ms.tail[0];
    w[9] = ms.tail[1];
}

// Candidates of one run -> valid ordered solutions (indices); counts duplicates / errors.
static void filter_candidates(const uint8_t header[EQ_HEADER_BYTES], const uint32_t* sols, int nc,
                              std::vector<std::vector<uint32_t>>& out, int& dupes, int& bad, std::string* why_bad)
{
    std::set<std::vector<uint32_t>> seen;
    for (int c = 0; c < nc; c++) {
        std::vector<uint32_t> idx(sols + (size_t)c * EQ_SOL_INDICES, sols + (size_t)(c + 1) * EQ_SOL_INDICES);
        if (!eq_distinct(idx.data())) { dupes++; continue; }
        eq_canonical(idx.data());
        if (seen.count(idx)) { dupes++; continue; }
        std::string why;
        if (!eq_validate(header, idx.data(), &why)) {
            bad++;
            if (why_bad) *why_bad = why;
            continue;
        }
        seen.insert(idx);
        out.push_back(idx);
    }
}

// ---------------------------------------------------------------------------
// GPU worker
// ---------------------------------------------------------------------------
static void gpu_worker(const Config& cfg, int idx, int dev)
{
    GpuStat& st = *g_stats[idx];
    void* g = kg_create(dev, cfg.tpb);
    if (!g) {
        LOG("GPU %d: init failed: %s", dev, kg_last_error(nullptr));
        g_gpu_failed = dev + 1;
        g_running = false;
        return;
    }
    st.name = kg_name(g);
    st.beat_ms = now_ms();
    st.alive = true;
    LOG("GPU %d: %s, sm_%d, %d SMs, %.0f MB (solver uses %.0f MB), %d threads/block, %s", dev, st.name.c_str(), kg_compute_capability(g),
        kg_sm_count(g), kg_memory(g) / 1048576.0, kg_memory_used(g) / 1048576.0, cfg.tpb, kg_layout());

    std::mt19937_64 rng((uint64_t)Clock::now().time_since_epoch().count() ^ ((uint64_t)dev << 40) ^ ((uint64_t)idx << 52));
    const uint32_t session = (uint32_t)rng();
    uint64_t counter = 0;
    std::vector<uint32_t> sols((size_t)KG_MAX_CANDS * EQ_SOL_INDICES);

    while (g_running) {
        st.beat_ms = now_ms();
        std::shared_ptr<const PoolJob> job;
        bool paused;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            job = g_job;
            paused = g_paused;
        }
        if (paused || !job) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }

        // nonce = extranonce1 || nonce2; nonce2 = gpu index, session (4 bytes), counter (8 bytes LE), zeros
        uint8_t header[EQ_HEADER_BYTES];
        memcpy(header, job->header, 108);
        uint8_t* nonce = header + 108;
        memset(nonce, 0, 32);
        memcpy(nonce, job->xn, job->xn_len);
        uint8_t tmp[13];
        tmp[0] = (uint8_t)idx;
        for (int i = 0; i < 4; i++) tmp[1 + i] = (uint8_t)(session >> (8 * i));
        for (int i = 0; i < 8; i++) tmp[5 + i] = (uint8_t)(counter >> (8 * i));
        const size_t n2 = 32 - job->xn_len;
        if (n2 >= 13) memcpy(nonce + job->xn_len, tmp, 13);
        else {   // short nonce2: gpu index + counter only
            nonce[job->xn_len] = (uint8_t)idx;
            for (size_t i = 1; i < n2; i++) nonce[job->xn_len + i] = (uint8_t)(counter >> (8 * (i - 1)));
        }
        counter++;
        uint64_t mw[10];
        midstate_words(header, mw);

        int nc = 0;
        double ms = 0;
        if (kg_solve(g, mw, sols.data(), &nc, nullptr, &ms) != 0) {
            LOG("GPU %d: CUDA error: %s", dev, kg_last_error(g));
            g_gpu_failed = dev + 1;
            g_running = false;
            break;
        }
        st.runs++;
        st.run_ms = (int)ms;
        st.beat_ms = now_ms();
        std::vector<std::vector<uint32_t>> good;
        int dupes = 0, bad = 0;
        std::string why;
        filter_candidates(header, sols.data(), nc, good, dupes, bad, &why);
        st.dupes += dupes;
        if (bad) {
            st.bad += bad;
            LOG("GPU %d: %d WRONG solution(s) rejected by the CPU check (%s): unstable clocks?", dev, bad, why.c_str());
        }
        st.sols += good.size();
        if (good.empty()) continue;
        bool stale;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            stale = g_clean_seq > job->seq;
        }
        if (stale) continue;
        for (auto& s : good) {
            uint8_t sol[EQ_SOL_BYTES], h[32];
            eq_compress(s.data(), sol);
            eq_block_hash(header, sol, h);
            if (!hash_le_target(h, job->target)) continue;
            Share sh;
            sh.seq = job->seq;
            sh.job_id = job->job_id;
            sh.ntime_hex = job->ntime_hex;
            sh.nonce2_hex = to_hex(nonce + job->xn_len, n2);
            sh.sol_hex = "fd9001" + to_hex(sol, EQ_SOL_BYTES);
            uint8_t hb[32];
            for (int i = 0; i < 32; i++) hb[i] = h[31 - i];
            sh.hash_hex = to_hex(hb, 32);
            sh.gpu = dev;
            sh.block = hash_le_target(h, job->net_target);
            st.shares++;
            if (sh.block) LOG("GPU %d: BLOCK candidate (hash %s)", dev, sh.hash_hex.c_str());
            std::lock_guard<std::mutex> lk(g_mtx);
            g_shares.push_back(sh);
        }
    }
    st.alive = false;
    kg_destroy(g);
}

// ---------------------------------------------------------------------------
// pool client (Zcash / NOMP equihash stratum)
// ---------------------------------------------------------------------------
struct Pending { int gpu; std::string job_id, hash; bool block; };

static const json::Value* arr_at(const json::Value* a, size_t i)
{
    if (!a || !a->is_arr() || a->arr.size() <= i) return nullptr;
    return &a->arr[i];
}
static std::string arr_str(const json::Value* a, size_t i)
{
    const json::Value* v = arr_at(a, i);
    return (v && (v->is_str() || v->is_num())) ? v->s : std::string();
}
static bool arr_bool(const json::Value* a, size_t i)
{
    const json::Value* v = arr_at(a, i);
    return v && v->is_bool() && v->b;
}

static bool parse_notify(const json::Value* p, const uint8_t* xn, size_t xn_len, const uint8_t target[32], PoolJob& j, std::string& why)
{
    j.job_id = arr_str(p, 0);
    if (j.job_id.empty()) { why = "missing job id"; return false; }
    struct F { size_t i, off, n; const char* name; } f[] = {
        { 1, 0, 4, "version" }, { 2, 4, 32, "prevhash" }, { 3, 36, 32, "merkleroot" },
        { 4, 68, 32, "reserved" }, { 5, 100, 4, "time" }, { 6, 104, 4, "bits" } };
    for (auto& x : f)
        if (!hex_to_bytes(arr_str(p, x.i), j.header + x.off, x.n)) { why = std::string("bad ") + x.name; return false; }
    std::string algo = arr_str(p, 8);
    if (!algo.empty() && algo != "192_7") { why = "not an Equihash 192,7 job (" + algo + ")"; return false; }
    uint32_t ver = (uint32_t)j.header[0] | ((uint32_t)j.header[1] << 8);
    if ((ver & 0x0f00) != 0x0600) { why = "block version is not Equihash 192,7"; return false; }
    j.ntime_hex = strip0x(arr_str(p, 5));
    j.clean = arr_bool(p, 7);
    memcpy(j.target, target, 32);
    if (!nbits_le_target(j.header + 104, j.net_target)) memset(j.net_target, 0, 32);
    memset(j.xn, 0, sizeof j.xn);
    memcpy(j.xn, xn, xn_len);
    j.xn_len = xn_len;
    return true;
}

static void pool_worker(const Config& cfg)
{
    TlsConn conn;
    size_t pool_idx = 0;
    int backoff = 2;
    while (g_running) {
        std::string url = cfg.pools[pool_idx % cfg.pools.size()];
        LOG("pool: connecting to %s", url.c_str());
        if (!conn.connect(url, cfg.cafile, 10000)) {
            LOG("pool: %s", conn.error().c_str());
            pool_idx++;
            for (int i = 0; i < backoff * 10 && g_running; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            backoff = backoff * 2 > 60 ? 60 : backoff * 2;
            continue;
        }
        LOG("pool: connected to %s", conn.host().c_str());
        int next_id = 3;
        std::map<int, Pending> pending;
        bool subscribed = false, authorized = false;
        uint8_t xn[32] = { 0 };
        size_t xn_len = 0;
        uint8_t target[32];
        diff_to_target(1.0, target);
        json::Value last_notify;
        bool have_notify = false;
        int raw_lines = 0;
        auto t_conn = Clock::now();
        auto t_rx = Clock::now();
        conn.send_line(std::string("{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[") +
                       json::quote(std::string("krgnminer/") + KRGNMINER_VERSION) + "]}");
        bool lost = false;

        auto take_job = [&](const json::Value* params) {
            if (!subscribed) return;
            auto j = std::make_shared<PoolJob>();
            std::string why;
            if (!parse_notify(params, xn, xn_len, target, *j, why)) {
                LOG("pool: ignoring job: %s: %.300s", why.c_str(), json::dump(*params).c_str());
                return;
            }
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                j->seq = ++g_job_seq;
                if (j->clean) g_clean_seq = j->seq;
                g_job = j;
                g_paused = false;
            }
            LOG("pool: new job %s%s (share difficulty %.4g, network %.4g)", j->job_id.c_str(), j->clean ? " (new block)" : "",
                target_diff(j->target), target_diff(j->net_target));
        };

        while (g_running && !lost) {
            std::vector<std::string> lines;
            if (!conn.poll_lines(lines, 100)) { LOG("pool: %s", conn.error().c_str()); lost = true; break; }
            if (!lines.empty()) t_rx = Clock::now();
            for (auto& line : lines) {
                if (raw_lines < 6) { raw_lines++; LOG("pool <- %.400s", line.c_str()); }
                json::Value v;
                if (!json::parse(line, v) || !v.is_obj()) { LOG("pool: unparsable line: %.200s", line.c_str()); continue; }
                const json::Value* method = v.get("method");
                const json::Value* params = v.get("params");
                if (method && method->is_str()) {
                    const std::string& m = method->s;
                    if (m == "mining.notify") {
                        last_notify = params ? *params : json::Value();
                        have_notify = true;
                        take_job(params);
                    } else if (m == "mining.set_target") {
                        std::string t = arr_str(params, 0);
                        uint8_t be[32];
                        if (hex_to_bytes(t, be, 32)) {
                            memcpy(target, be, 32);
                            LOG("pool: share target %s (difficulty %.4g)", strip0x(t).c_str(), target_diff(target));
                            if (have_notify) take_job(&last_notify);   // applies to the current job
                        } else LOG("pool: bad set_target: %.200s", line.c_str());
                    } else if (m == "mining.set_difficulty") {
                        double d = atof(arr_str(params, 0).c_str());
                        diff_to_target(d, target);
                        LOG("pool: share difficulty %.4g", d);
                        if (have_notify) take_job(&last_notify);
                    } else if (m == "mining.set_extranonce") {
                        std::string x = strip0x(arr_str(params, 0));
                        if (!x.empty() && x.size() <= 48 && x.size() % 2 == 0) {
                            xn_len = x.size() / 2;
                            memset(xn, 0, sizeof xn);
                            hex_to_bytes(x, xn, xn_len);
                            LOG("pool: new extranonce1 %s", x.c_str());
                            if (have_notify) take_job(&last_notify);
                        }
                    } else if (m == "client.reconnect") {
                        LOG("pool: reconnect requested");
                        lost = true;
                    } else if (m == "client.show_message") {
                        LOG("pool message: %s", arr_str(params, 0).c_str());
                    } else {
                        LOG("pool: unhandled method %s: %.300s", m.c_str(), line.c_str());
                    }
                    continue;
                }
                long long id = v.num("id", -1);
                const json::Value* result = v.get("result");
                const json::Value* error = v.get("error");
                bool has_err = error && !error->is_null();
                if (id == 1) {
                    std::string x = strip0x(arr_str(result, 1));
                    if (has_err || x.size() > 48 || x.size() % 2) {
                        LOG("pool: subscribe refused: %s", line.c_str());
                        lost = true;
                        break;
                    }
                    xn_len = x.size() / 2;
                    memset(xn, 0, sizeof xn);
                    if (xn_len) hex_to_bytes(x, xn, xn_len);
                    subscribed = true;
                    LOG("pool: subscribed, extranonce1 '%s' (%d nonce bytes left)", x.c_str(), 32 - (int)xn_len);
                    conn.send_line(std::string("{\"id\":2,\"method\":\"mining.authorize\",\"params\":[") + json::quote(cfg.user) + "," +
                                   json::quote(cfg.pass) + "]}");
                    continue;
                }
                if (id == 2) {
                    bool ok = !has_err && result && ((result->is_bool() && result->b) || (!result->is_null() && !result->is_bool()));
                    if (!ok) {
                        LOG("pool: authorization refused: %s", line.c_str());
                        lost = true;
                        backoff = 60;
                        break;
                    }
                    authorized = true;
                    backoff = 2;
                    LOG("pool: authorized as %s", cfg.user.c_str());
                    if (have_notify) take_job(&last_notify);
                    continue;
                }
                auto it = pending.find((int)id);
                if (it == pending.end()) { LOG("pool: unexpected reply: %.300s", line.c_str()); continue; }
                Pending pd = it->second;
                pending.erase(it);
                bool acc = !has_err && result && !result->is_null() && !(result->is_bool() && !result->b);
                if (acc) {
                    g_accepted++;
                    if (pd.block) g_blocks++;
                    LOG("share accepted (GPU %d)%s %s  [%llu/%llu]", pd.gpu, pd.block ? " BLOCK" : "", pd.hash.substr(0, 16).c_str(),
                        (unsigned long long)g_accepted.load(), (unsigned long long)(g_accepted.load() + g_rejected.load()));
                } else {
                    g_rejected++;
                    LOG("share REJECTED (GPU %d) job %s: %s  [%llu/%llu]", pd.gpu, pd.job_id.c_str(),
                        has_err ? json::dump(*error).c_str() : line.c_str(),
                        (unsigned long long)g_accepted.load(), (unsigned long long)(g_accepted.load() + g_rejected.load()));
                }
            }
            if (lost) break;
            if (authorized) {
                std::deque<Share> q;
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    q.swap(g_shares);
                }
                for (auto& s : q) {
                    int id = next_id++;
                    pending[id] = Pending{ s.gpu, s.job_id, s.hash_hex, s.block };
                    std::string msg = "{\"id\":" + std::to_string(id) + ",\"method\":\"mining.submit\",\"params\":[" + json::quote(cfg.user) + "," +
                                      json::quote(s.job_id) + "," + json::quote(s.ntime_hex) + "," + json::quote(s.nonce2_hex) + "," +
                                      json::quote(s.sol_hex) + "]}";
                    if (g_accepted.load() + g_rejected.load() < 2) LOG("pool -> %.300s...", msg.c_str());
                    if (!conn.send_line(msg)) { LOG("pool: send failed: %s", conn.error().c_str()); lost = true; break; }
                }
            } else if (std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - t_conn).count() > 60) {
                LOG("pool: no authorization within 60 s");
                lost = true;
            }
            if (lost) break;
            if (Clock::now() - t_rx > std::chrono::seconds(300)) { LOG("pool: nothing received for 300 s, reconnecting"); lost = true; }
        }
        conn.close();
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_paused = true;
            g_job.reset();
            g_shares.clear();
        }
        if (!g_running) break;
        pool_idx++;
        LOG("pool: disconnected, retrying in %d s", backoff);
        for (int i = 0; i < backoff * 10 && g_running; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        backoff = backoff * 2 > 60 ? 60 : backoff * 2;
    }
}

// ---------------------------------------------------------------------------
// tests and benchmark
// ---------------------------------------------------------------------------
static std::vector<int> selected_devices(const Config& cfg)
{
    int n = kg_device_count();
    std::vector<int> d;
    if (cfg.gpus.empty()) { for (int i = 0; i < n; i++) d.push_back(i); }
    else { for (int g : cfg.gpus) if (g >= 0 && g < n) d.push_back(g); }
    return d;
}

static bool vector_header(const EqVector& v, uint8_t h[EQ_HEADER_BYTES]) { return hex_to_bytes(v.header_hex, h, EQ_HEADER_BYTES); }

static int run_tests(const Config& cfg)
{
    int fails = 0;
    // 1) CPU: BLAKE2b known answers (libsodium), both code paths
    {
        int bad = 0;
        for (auto& k : eq_blake_kat) {
            uint8_t h[EQ_HEADER_BYTES], out[48], expect[48];
            hex_to_bytes(eq_vectors[k.vector].header_hex, h, EQ_HEADER_BYTES);
            hex_to_bytes(k.digest_hex, expect, 48);
            eq_ref_hash(h, k.g, out);
            uint64_t w[6];
            EqMidstate ms = eq_midstate(h);
            eq_hash_index(ms, k.g, w);
            uint8_t o2[48];
            for (int i = 0; i < 6; i++)
                for (int b = 0; b < 8; b++) o2[8 * i + b] = (uint8_t)(w[i] >> (8 * b));
            if (memcmp(out, expect, 48) || memcmp(o2, expect, 48)) bad++;
        }
        LOG("TEST CPU BLAKE2b-384 \"kerrigan\": %d known answers: %s", (int)(sizeof eq_blake_kat / sizeof eq_blake_kat[0]), bad ? "FAILED" : "OK");
        fails += bad;
    }
    // 2) CPU: Kerrigan's solutions are valid, corrupted ones are not, encoding round trip
    {
        int nsol = 0, bad = 0;
        for (auto& v : eq_vectors) {
            uint8_t h[EQ_HEADER_BYTES];
            vector_header(v, h);
            for (int s = 0; s < v.nsols; s++) {
                uint8_t sol[EQ_SOL_BYTES], s2[EQ_SOL_BYTES];
                hex_to_bytes(v.sols[s], sol, EQ_SOL_BYTES);
                uint32_t idx[EQ_SOL_INDICES];
                eq_expand(sol, idx);
                eq_compress(idx, s2);
                std::string why;
                if (!eq_validate(h, idx, &why) || memcmp(sol, s2, EQ_SOL_BYTES)) { bad++; LOG("TEST CPU: reference solution rejected: %s", why.c_str()); }
                uint32_t c1[EQ_SOL_INDICES];
                memcpy(c1, idx, sizeof c1);
                c1[17] ^= 1u << (s % 25);
                if (eq_validate(h, c1, nullptr)) { bad++; LOG("TEST CPU: corrupted solution ACCEPTED"); }
                memcpy(c1, idx, sizeof c1);
                std::swap(c1[0], c1[1]);
                if (eq_validate(h, c1, nullptr)) { bad++; LOG("TEST CPU: badly ordered solution ACCEPTED"); }
                nsol++;
            }
        }
        LOG("TEST CPU validation: %d reference solutions of %d headers (Kerrigan's solver) + corrupted variants: %s", nsol,
            (int)(sizeof eq_vectors / sizeof eq_vectors[0]), bad ? "FAILED" : "OK");
        fails += bad;
    }
    if (fails) { LOG("TEST RESULT: FAILED"); return 1; }

    // 3) GPU: every reference header, compared with Kerrigan's solutions and the
    //    exact string counts of every level (CPU emulation in the cloud)
    std::vector<int> devs = selected_devices(cfg);
    if (devs.empty()) { LOG("TEST: no CUDA device"); LOG("TEST RESULT: FAILED"); return 1; }
    int total_fail = 0;
    for (int dev : devs) {
        void* g = kg_create(dev, cfg.tpb);
        if (!g) { LOG("TEST GPU %d: create failed: %s", dev, kg_last_error(nullptr)); total_fail++; continue; }
        LOG("TEST GPU %d: %s, solver memory %.0f MB, %d threads/block", dev, kg_name(g), kg_memory_used(g) / 1048576.0, cfg.tpb);
        int fail = 0, found_ref = 0, total_ref = 0, extra = 0;
        double tms = 0;
        std::vector<uint32_t> sols((size_t)KG_MAX_CANDS * EQ_SOL_INDICES);
        for (size_t vi = 0; vi < sizeof eq_vectors / sizeof eq_vectors[0]; vi++) {
            const EqVector& v = eq_vectors[vi];
            uint8_t h[EQ_HEADER_BYTES];
            vector_header(v, h);
            uint64_t mw[10];
            midstate_words(h, mw);
            int nc = 0;
            double ms = 0;
            uint32_t lc[EQ_K];
            if (kg_solve(g, mw, sols.data(), &nc, lc, &ms) != 0) { LOG("TEST GPU %d: %s", dev, kg_last_error(g)); fail++; break; }
            tms += ms;
            std::vector<std::vector<uint32_t>> good;
            int dupes = 0, bad = 0;
            std::string why;
            filter_candidates(h, sols.data(), nc, good, dupes, bad, &why);
            std::set<std::string> ref, got;
            for (int s = 0; s < v.nsols; s++) ref.insert(v.sols[s]);
            for (auto& s : good) { uint8_t b[EQ_SOL_BYTES]; eq_compress(s.data(), b); got.insert(to_hex(b, EQ_SOL_BYTES)); }
            int hit = 0;
            for (auto& r : ref) if (got.count(r)) hit++;
            int ext = (int)got.size() - hit;
            bool counts_ok = true;
            std::string cs;
            for (int L = 0; L < EQ_K; L++) {
                if (lc[L] != v.level_counts[L]) counts_ok = false;
                cs += (L ? " " : "") + std::to_string(lc[L]);
            }
            bool ok = bad == 0 && counts_ok && hit == v.nsols && ext == 0;
            if (!ok) fail++;
            found_ref += hit;
            total_ref += v.nsols;
            extra += ext;
            LOG("TEST GPU %d vector %zu: %d/%d reference solutions, %d other, %d with repeated indices, %d WRONG; level counts %s %s; %.0f ms: %s", dev,
                vi + 1, hit, v.nsols, ext, dupes, bad, cs.c_str(), counts_ok ? "(exact)" : "(DIFFERENT from the CPU emulation)", ms, ok ? "OK" : "FAILED");
        }
        // a few more runs for the timing
        if (!fail) {
            std::mt19937_64 rng(7);
            uint8_t h[EQ_HEADER_BYTES];
            vector_header(eq_vectors[0], h);
            int runs = 10, nsol = 0, bad = 0;
            double t = 0;
            for (int r = 0; r < runs; r++) {
                for (int i = 108; i < EQ_HEADER_BYTES; i++) h[i] = (uint8_t)rng();
                uint64_t mw[10];
                midstate_words(h, mw);
                int nc = 0;
                double ms = 0;
                if (kg_solve(g, mw, sols.data(), &nc, nullptr, &ms) != 0) { LOG("TEST GPU %d: %s", dev, kg_last_error(g)); fail++; break; }
                t += ms;
                std::vector<std::vector<uint32_t>> good;
                int dupes = 0;
                filter_candidates(h, sols.data(), nc, good, dupes, bad, nullptr);
                nsol += (int)good.size();
            }
            if (bad) fail++;
            LOG("TEST GPU %d: %d random nonces: %d valid solutions, %d wrong, %.1f ms per run (~%.1f Sol/s)", dev, runs, nsol, bad, t / runs,
                t > 0 ? nsol * 1000.0 / t : 0.0);
        }
        LOG("TEST GPU %d %s: %s (%d/%d reference solutions found)", dev, kg_name(g), fail ? "FAILED" : "OK", found_ref, total_ref);
        total_fail += fail;
        kg_destroy(g);
    }
    LOG("TEST RESULT: %s", total_fail ? "FAILED" : "ALL OK");
    return total_fail ? 1 : 0;
}

static int run_bench(const Config& cfg)
{
    std::vector<int> devs = selected_devices(cfg);
    if (devs.empty()) { LOG("BENCH: no CUDA device"); return 1; }
    auto j = std::make_shared<PoolJob>();
    uint8_t h[EQ_HEADER_BYTES];
    hex_to_bytes(eq_vectors[0].header_hex, h, EQ_HEADER_BYTES);
    memcpy(j->header, h, 108);
    memset(j->target, 0, 32);                  // impossible share target: pure solving
    memset(j->net_target, 0, 32);
    j->job_id = "bench";
    j->ntime_hex = "00000000";
    j->seq = 1;
    j->xn_len = 0;
    { std::lock_guard<std::mutex> lk(g_mtx); g_job = j; g_paused = false; g_job_seq = 1; }
    for (size_t i = 0; i < devs.size(); i++) { g_stats.emplace_back(new GpuStat()); g_stats.back()->dev = devs[i]; }
    std::vector<std::thread> th;
    for (size_t i = 0; i < devs.size(); i++) th.emplace_back(gpu_worker, std::cref(cfg), (int)i, devs[i]);
    for (int i = 0; i < 600 && g_running; i++) {
        bool all = true;
        for (auto& s : g_stats) if (s->runs < 2) all = false;
        if (all) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::vector<uint64_t> r0, s0;
    for (auto& s : g_stats) { r0.push_back(s->runs); s0.push_back(s->sols); }
    auto t0 = Clock::now();
    for (int i = 0; i < cfg.bench_seconds && g_running; i++) std::this_thread::sleep_for(std::chrono::seconds(1));
    double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    double total = 0, total_runs = 0;
    for (size_t i = 0; i < g_stats.size(); i++) {
        double runs = (double)(g_stats[i]->runs - r0[i]) / secs, sols = (double)(g_stats[i]->sols - s0[i]) / secs;
        total += sols;
        total_runs += runs;
        LOG("BENCH GPU %d: %.2f Sol/s, %.2f runs/s (%d ms/run, %.2f Sol/run), %llu wrong %s", g_stats[i]->dev, sols, runs, g_stats[i]->run_ms.load(),
            runs > 0 ? sols / runs : 0.0, (unsigned long long)g_stats[i]->bad.load(), g_stats[i]->name.c_str());
    }
    LOG("BENCH TOTAL: %.2f Sol/s (%.2f runs/s) over %zu GPU(s), %d threads/block, %.0f s", total, total_runs, g_stats.size(), cfg.tpb, secs);
    g_running = false;
    for (auto& t : th) t.join();
    return 0;
}

// time of every stage of one run, average of 3 runs after 2 warm-up runs
static int run_profile(const Config& cfg)
{
    std::vector<int> devs = selected_devices(cfg);
    if (devs.empty()) { LOG("PROFILE: no CUDA device"); return 1; }
    static const char* names[9] = { "blake2b", "step1", "step2", "step3", "step4", "step5", "step6", "final", "backtrack" };
    for (int dev : devs) {
        void* g = kg_create(dev, cfg.tpb);
        if (!g) { LOG("PROFILE GPU %d: %s", dev, kg_last_error(nullptr)); return 1; }
        uint8_t h[EQ_HEADER_BYTES];
        hex_to_bytes(eq_vectors[0].header_hex, h, EQ_HEADER_BYTES);
        uint64_t mw[10];
        double acc[9] = { 0 };
        std::vector<uint32_t> sols((size_t)KG_MAX_CANDS * EQ_SOL_INDICES);
        for (int r = 0; r < 5; r++) {
            h[120] = (uint8_t)r;
            midstate_words(h, mw);
            double st[9];
            if (kg_profile(g, mw, st, 9) != 0) { LOG("PROFILE GPU %d: %s", dev, kg_last_error(g)); kg_destroy(g); return 1; }
            if (r >= 2) for (int i = 0; i < 9; i++) acc[i] += st[i] / 3;
        }
        std::string line;
        double tot = 0;
        for (int i = 0; i < 9; i++) { char b[48]; snprintf(b, sizeof b, " %s %.1f", names[i], acc[i]); line += b; tot += acc[i]; }
        LOG("PROFILE GPU %d (tpb %d, %s): total %.1f ms:%s", dev, cfg.tpb, kg_layout(), tot, line.c_str());
        kg_destroy(g);
    }
    return 0;
}

static std::vector<std::string> expand_args(int argc, char** argv)
{
    std::vector<std::string> out;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--args-file" && i + 1 < argc) {
            FILE* f = fopen(argv[++i], "r");
            if (!f) { fprintf(stderr, "cannot open args file %s\n", argv[i]); continue; }
            char tok[1024];
            while (fscanf(f, "%1023s", tok) == 1) out.push_back(tok);
            fclose(f);
            continue;
        }
        out.push_back(a);
    }
    return out;
}

int main(int argc, char** argv)
{
    Config cfg;
    cfg.exe_dir = exe_directory(argv[0]);
    std::vector<std::string> args = expand_args(argc, argv);
    std::string conf = cfg.exe_dir + "/krgnminer.conf";
    if (!file_exists(conf)) conf = cfg.exe_dir + "/../krgnminer.conf";
    for (size_t i = 0; i < args.size(); i++)
        if ((args[i] == "--config" || args[i] == "-c") && i + 1 < args.size()) conf = args[++i];
    bool have_conf = load_config_file(cfg, conf);
    std::vector<std::string> cli_pools;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--config" || a == "-c") { i++; continue; }
        if (a == "--test") { cfg.test = true; continue; }
        if (a == "--profile") { cfg.profile = true; continue; }
        if (a == "--bench") {
            cfg.bench = true;
            if (i + 1 < args.size() && args[i + 1][0] != '-') cfg.bench_seconds = atoi(args[++i].c_str());
            continue;
        }
        if (a == "--help" || a == "-h") {
            printf("KrgnMiner %s - Kerrigan (KRGN) Equihash 192,7 GPU miner, no dev fee\n"
                   "  krgnminer [--config file] [--args-file file] [-o url] [-u address.rig] [-p x] [--gpus 0,1|all]\n"
                   "            [--tpb N] [--cafile f] [--stats s] [--log file] [--test] [--bench [s]] [--profile]\n",
                   KRGNMINER_VERSION);
            return 0;
        }
        if (i + 1 < args.size()) {
            if (a == "-o" || a == "--pool" || a == "--url") { cli_pools.push_back(args[++i]); continue; }
            if (apply_option(cfg, a, args[i + 1])) { i++; continue; }
        }
        fprintf(stderr, "unknown argument %s (see --help)\n", a.c_str());
        return 2;
    }
    if (!cli_pools.empty()) cfg.pools = cli_pools;
    if (cfg.cafile.empty()) {
        const char* cands[] = { "C:\\msys64\\mingw64\\etc\\ssl\\certs\\ca-bundle.crt", "/etc/ssl/certs/ca-certificates.crt" };
        std::string local = cfg.exe_dir + "/ca-bundle.crt";
        if (file_exists(local)) cfg.cafile = local;
        else for (auto c : cands) if (file_exists(c)) { cfg.cafile = c; break; }
    }
    if (!cfg.logfile.empty()) g_logf = fopen(cfg.logfile.c_str(), "a");
    if (cfg.stats < 5) cfg.stats = 5;
    if (cfg.tpb < 32 || cfg.tpb > 1024 || cfg.tpb % 32) cfg.tpb = 256;
    net_global_init();

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    LOG("KrgnMiner %s (Kerrigan / KRGN, Equihash 192,7 \"kerrigan\") - no dev fee%s", KRGNMINER_VERSION, have_conf ? "" : " - no config file");

    if (cfg.test) { deadline_watchdog(600, "TEST"); return run_tests(cfg); }
    if (cfg.profile) { deadline_watchdog(300, "PROFILE"); return run_profile(cfg); }
    if (cfg.bench) { deadline_watchdog(cfg.bench_seconds + 240, "BENCH"); return run_bench(cfg); }

    if (cfg.pools.empty() || cfg.user.empty()) { LOG("missing pool url or user (KRGN address.rig), see krgnminer.conf"); return 2; }
    std::vector<int> devs = selected_devices(cfg);
    if (devs.empty()) { LOG("no CUDA device selected"); return 1; }

    for (size_t i = 0; i < devs.size(); i++) { g_stats.emplace_back(new GpuStat()); g_stats.back()->dev = devs[i]; }
    std::vector<std::thread> th;
    for (size_t i = 0; i < devs.size(); i++) th.emplace_back(gpu_worker, std::cref(cfg), (int)i, devs[i]);
    std::thread net(pool_worker, std::cref(cfg));

    std::vector<uint64_t> last(g_stats.size(), 0);
    auto t_last = Clock::now();
    while (g_running) {
        for (int i = 0; i < cfg.stats * 10 && g_running; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (i % 10 != 9) continue;
            long long t = now_ms();
            for (size_t k = 0; k < g_stats.size(); k++) {
                if (!g_stats[k]->alive) continue;
                long long idle = t - g_stats[k]->beat_ms.load();
                if (idle > KM_STUCK_MS) {
                    LOG("GPU %d stuck for %lld s (run never returned, unstable clocks?): exiting with code 4 for an automatic restart",
                        g_stats[k]->dev, idle / 1000);
                    fflush(nullptr);
                    std::_Exit(4);
                }
            }
        }
        if (!g_running) break;
        double secs = std::chrono::duration<double>(Clock::now() - t_last).count();
        t_last = Clock::now();
        double total = 0;
        std::string per;
        for (size_t i = 0; i < g_stats.size(); i++) {
            uint64_t s = g_stats[i]->sols;
            double r = (double)(s - last[i]) / secs;
            last[i] = s;
            total += r;
            char b[64];
            snprintf(b, sizeof b, "%sGPU%d %.1f", i ? " | " : "", g_stats[i]->dev, r);
            per += b;
        }
        bool paused;
        { std::lock_guard<std::mutex> lk(g_mtx); paused = g_paused; }
        std::string bad;
        for (size_t i = 0; i < g_stats.size(); i++) {
            uint64_t b = g_stats[i]->bad;
            if (b) { char x[48]; snprintf(x, sizeof x, "%sGPU%d %llu", bad.empty() ? "" : ", ", g_stats[i]->dev, (unsigned long long)b); bad += x; }
        }
        LOG("%.1f Sol/s total (%s) | shares %llu/%llu | blocks %llu%s%s%s", total, per.c_str(), (unsigned long long)g_accepted.load(),
            (unsigned long long)(g_accepted.load() + g_rejected.load()), (unsigned long long)g_blocks.load(), paused ? " | waiting for work" : "",
            bad.empty() ? "" : " | GPU ERRORS: ", bad.c_str());
    }
    if (g_gpu_failed) {
        LOG("GPU %d failed (CUDA error): exiting with code 3 for an automatic restart", g_gpu_failed.load() - 1);
        fflush(nullptr);
        std::_Exit(3);
    }
    LOG("stopping...");
    for (auto& t : th) t.join();
    net.join();
    return 0;
}
