/*
 * lotspeed_zeta_v5_6.c
 * "公路超跑" Zeta-TCP (Auto-Scaling Edition)
 * Author: uk0
 *
 * Feature:
 * 1. Soft Start: New connections start at 'lotserver_start_rate' (default ~50Mbps) to protect low-bandwidth clients.
 * 2. Auto Scaling: Automatically climbs up if the pipe is healthy, finding the true bottleneck.
 * 3. Global Cap: Individual connection rate never exceeds 'lotserver_rate' (1Gbps).
 * 4. Smart Guard: Includes Loss Rate Cap & BDP Cap from v5.3/v5.4.
 */

#include <linux/module.h>
#include <linux/version.h>
#include <net/tcp.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/rtc.h>
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/rculist.h>
#include <linux/compiler.h>
#include <linux/workqueue.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>

// --- 安全性宏定义 ---
#define SAFETY_CHECK(ptr, ret) do { \
    if (unlikely(!(ptr))) { \
        return ret; \
    } \
} while(0)

#define SAFE_DIV(n, d) ((d) ? (n)/(d) : 0)
#define SAFE_DIV64(n, d) ((d) ? div64_u64(n, d) : 0)

// --- 基础宏定义 ---
#define CURRENT_TIMESTAMP ({ \
    static char __ts[32]; \
    struct timespec64 ts; \
    struct tm tm; \
    ktime_get_real_ts64(&ts); \
    time64_to_tm(ts.tv_sec, 0, &tm); \
    snprintf(__ts, sizeof(__ts), "%04ld-%02d-%02d %02d:%02d:%02d", \
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, \
            tm.tm_hour, tm.tm_min, tm.tm_sec); \
    __ts; \
})

// --- Zeta-TCP 核心配置 ---
#define HISTORY_BITS 10
#define HISTORY_MAX_ENTRIES 16384
#define HISTORY_TTL_SEC 3600
#define LOSS_DIFFERENTIATION_K 125
#define RTT_JITTER_TOLERANCE_US 2000

#define ZETA_ALPHA 85
#define ZETA_PROBE_GAIN 110
#define ZETA_MIN_SAMPLES 10

// --- 保护阈值 (Smart Guard) ---
#define LOSS_RATE_CAP_PERCENT 15     // 丢包率熔断阈值 (15%)
#define BDP_CAP_SCALE 3              // BDP 上限倍数 (v5.4 fix: 3x BDP)
#define PACKET_HISTORY_WIN 128       // 滑动窗口大小

// --- 历史/平滑参数 ---
#define HIST_LOW_LOSS_PERCENT 3
#define HIST_HIGH_LOSS_PERCENT 10
#define LOTSPEED_RATE_UP_FAST 107    // +7%
#define LOTSPEED_RATE_UP 103         // +3%
#define LOTSPEED_RATE_DOWN 94        // -6%
#define LOTSPEED_RATE_DOWN_HARD 90   // -10%
#define LOTSPEED_SMOOTH_NUM 7        // EWMA numerator (7/10)
#define LOTSPEED_SMOOTH_DEN 10
#define LOTSPEED_DEF_PROBE_BOOST 112         // +12% burst probe
#define LOTSPEED_DEF_PROBE_BOOST_HIGHRTT 120 // +20% for high RTT paths
#define LOTSPEED_DEF_HIGH_RTT_US 150000      // >=150ms 视为洲际路径
#define LOTSPEED_DEF_UP_MAX 120              // 动态上调上限 +20%
#define LOTSPEED_DEF_DOWN_MIN 88             // 动态下调下限 -12%
#define LOTSPEED_DEF_RTT_PRESSURE_LOW 120
#define LOTSPEED_DEF_RTT_PRESSURE_HIGH 180
#define LOTSPEED_DEF_LOSS_NOISE_PCT 3
#define LOTSPEED_DEF_NOISE_WINDOW_HYST 2

// --- BBR/LotSpeed 参数 ---
#define LOTSPEED_BETA_SCALE 1024
#define LOTSPEED_PROBE_RTT_INTERVAL_MS 10000
#define LOTSPEED_PROBE_RTT_DURATION_MS 500
#define LOTSPEED_STARTUP_GROWTH_TARGET 1280
#define LOTSPEED_STARTUP_EXIT_ROUNDS 2
#define LOTSPEED_MAX_U32 ((u32)~0U)
#define LOTSPEED_MAX_U64 ((u64)~0ULL)

// API 兼容性
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
#define LOTSPEED_NEW_CONG_CONTROL_API 1
#else
#define LOTSPEED_OLD_CONG_CONTROL_API 1
#endif

// --- 模块参数 ---
// 1. 全局物理上限 (1Gbps = 125MB/s)
static unsigned long lotserver_rate = 125000000;
// 2. [新增] 软启动速率 (80Mbps = 10.00MB/s) - 保护小水管
static unsigned long lotserver_start_rate = 10000000;

static unsigned int lotserver_gain = 20;
static unsigned int lotserver_min_cwnd = 16;
static unsigned int lotserver_max_cwnd = 15000; // 稍微放宽，因为有BDP Cap保护
static unsigned int lotserver_beta = 616; // 默认 0.6 * 1024 = 614.4
static bool lotserver_adaptive = true;
static bool lotserver_turbo = false;
static bool lotserver_verbose = false;
static bool lotserver_safe_mode = true;
static bool lotserver_autotune = true;
static unsigned int lotserver_tune_interval_ms = 1500;
static unsigned int lotserver_high_rtt_us = 130000; // 130ms 起视为洲际/高RTT
static unsigned int lotserver_probe_boost = 130;    // +30%
static unsigned int lotserver_probe_boost_highrtt = 150; // +50%
static unsigned int lotserver_up_max = 130;         // +30%
static unsigned int lotserver_down_min = 92;        // -8% 基线，下调还会随健康度动态抬升
static unsigned int lotserver_rtt_pressure_low = 110;
static unsigned int lotserver_rtt_pressure_high = 170;
static unsigned int lotserver_loss_noise_pct = 4;
static unsigned int lotserver_noise_window_hyst = 3;
static bool lotserver_ml_enabled = true;
static int lotserver_ml_w_health = 2;
static int lotserver_ml_w_rtt = 1;
static int lotserver_ml_w_loss = 3;
static int lotserver_ml_bias = 0;

// --- 参数回调 ---
static int param_set_rate(const char *val, const struct kernel_param *kp) { return param_set_ulong(val, kp); }
static int param_set_gain(const char *val, const struct kernel_param *kp) { return param_set_uint(val, kp); }
static int param_set_min_cwnd(const char *val, const struct kernel_param *kp) {
    int ret = param_set_uint(val, kp);
    if (!ret && lotserver_min_cwnd < 4) lotserver_min_cwnd = 4;
    return ret;
}
static int param_set_max_cwnd(const char *val, const struct kernel_param *kp) {
    int ret = param_set_uint(val, kp);
    if (!ret && lotserver_max_cwnd > 100000) lotserver_max_cwnd = 100000;
    return ret;
}
static int param_set_adaptive(const char *val, const struct kernel_param *kp) { return param_set_bool(val, kp); }
static int param_set_turbo(const char *val, const struct kernel_param *kp) { return param_set_bool(val, kp); }
static int param_set_beta(const char *val, const struct kernel_param *kp) {
    int ret = param_set_uint(val, kp);
    if (!ret) {
        if (lotserver_beta > LOTSPEED_BETA_SCALE) lotserver_beta = LOTSPEED_BETA_SCALE;
        if (lotserver_beta < 512) lotserver_beta = 512;
    }
    return ret;
}
static int param_set_percent(const char *val, const struct kernel_param *kp) {
    int ret = param_set_uint(val, kp);
    if (!ret) {
        unsigned int *p = (unsigned int *)kp->arg;
        if (*p > 300) *p = 300;
        if (*p == 0) *p = 1;
    }
    return ret;
}
static int param_set_msec(const char *val, const struct kernel_param *kp) {
    int ret = param_set_uint(val, kp);
    if (!ret) {
        unsigned int *p = (unsigned int *)kp->arg;
        if (*p < 10) *p = 10;
        if (*p > 600000) *p = 600000;
    }
    return ret;
}
static int param_set_weight(const char *val, const struct kernel_param *kp) {
    int ret = param_set_int(val, kp);
    if (!ret) {
        int *p = (int *)kp->arg;
        if (*p > 64) *p = 64;
        if (*p < -64) *p = -64;
    }
    return ret;
}

static const struct kernel_param_ops param_ops_rate = { .set = param_set_rate, .get = param_get_ulong, };
static const struct kernel_param_ops param_ops_gain = { .set = param_set_gain, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_min_cwnd = { .set = param_set_min_cwnd, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_max_cwnd = { .set = param_set_max_cwnd, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_adaptive = { .set = param_set_adaptive, .get = param_get_bool, };
static const struct kernel_param_ops param_ops_turbo = { .set = param_set_turbo, .get = param_get_bool, };
static const struct kernel_param_ops param_ops_beta = { .set = param_set_beta, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_percent = { .set = param_set_percent, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_msec = { .set = param_set_msec, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_weight = { .set = param_set_weight, .get = param_get_int, };

module_param_cb(lotserver_rate, &param_ops_rate, &lotserver_rate, 0644);
module_param_cb(lotserver_start_rate, &param_ops_rate, &lotserver_start_rate, 0644); // 新增
module_param_cb(lotserver_gain, &param_ops_gain, &lotserver_gain, 0644);
module_param_cb(lotserver_min_cwnd, &param_ops_min_cwnd, &lotserver_min_cwnd, 0644);
module_param_cb(lotserver_max_cwnd, &param_ops_max_cwnd, &lotserver_max_cwnd, 0644);
module_param_cb(lotserver_adaptive, &param_ops_adaptive, &lotserver_adaptive, 0644);
module_param_cb(lotserver_turbo, &param_ops_turbo, &lotserver_turbo, 0644);
module_param_cb(lotserver_beta, &param_ops_beta, &lotserver_beta, 0644);
module_param(lotserver_verbose, bool, 0644);
module_param(lotserver_safe_mode, bool, 0644);
module_param_cb(lotserver_high_rtt_us, &param_ops_msec, &lotserver_high_rtt_us, 0644);
module_param_cb(lotserver_probe_boost, &param_ops_percent, &lotserver_probe_boost, 0644);
module_param_cb(lotserver_probe_boost_highrtt, &param_ops_percent, &lotserver_probe_boost_highrtt, 0644);
module_param_cb(lotserver_up_max, &param_ops_percent, &lotserver_up_max, 0644);
module_param_cb(lotserver_down_min, &param_ops_percent, &lotserver_down_min, 0644);
module_param_cb(lotserver_rtt_pressure_low, &param_ops_percent, &lotserver_rtt_pressure_low, 0644);
module_param_cb(lotserver_rtt_pressure_high, &param_ops_percent, &lotserver_rtt_pressure_high, 0644);
module_param_cb(lotserver_loss_noise_pct, &param_ops_percent, &lotserver_loss_noise_pct, 0644);
module_param_cb(lotserver_noise_window_hyst, &param_ops_percent, &lotserver_noise_window_hyst, 0644);
module_param_cb(lotserver_autotune, &param_ops_bool, &lotserver_autotune, 0644);
module_param_cb(lotserver_tune_interval_ms, &param_ops_msec, &lotserver_tune_interval_ms, 0644);
module_param_cb(lotserver_ml_enabled, &param_ops_bool, &lotserver_ml_enabled, 0644);
module_param_cb(lotserver_ml_w_health, &param_ops_weight, &lotserver_ml_w_health, 0644);
module_param_cb(lotserver_ml_w_rtt, &param_ops_weight, &lotserver_ml_w_rtt, 0644);
module_param_cb(lotserver_ml_w_loss, &param_ops_weight, &lotserver_ml_w_loss, 0644);
module_param_cb(lotserver_ml_bias, &param_ops_weight, &lotserver_ml_bias, 0644);

// --- 全局统计 ---
static atomic_t active_connections = ATOMIC_INIT(0);
static atomic64_t total_bytes_sent = ATOMIC64_INIT(0);
static atomic_t total_losses = ATOMIC_INIT(0);
static atomic_t history_entries_count = ATOMIC_INIT(0);
static struct kmem_cache *zeta_history_cache;
static struct dentry *lotspeed_debugfs_dir;

struct lotspeed_tune_stats {
    u32 health_ewma;
    u32 rtt_pressure_ewma;
    u32 loss_ewma;
    spinlock_t lock;
};

static struct lotspeed_tune_stats ls_tune_stats;
static struct delayed_work ls_tuner_work;

// --- ZETA 学习引擎结构 ---
struct zeta_history_entry {
    struct hlist_node node;
    struct rcu_head rcu;
    u32 daddr;
    u64 cached_bw;
    u32 cached_min_rtt;
    u32 cached_median_rtt;
    u32 loss_ewma;
    u64 last_update;
    u32 sample_count;
    u32 loss_count;
};

static DEFINE_HASHTABLE(zeta_history_map, HISTORY_BITS);
static DEFINE_SPINLOCK(zeta_history_lock);

// --- 核心状态机 ---
enum lotspeed_state {
    STARTUP,
    PROBING,
    CRUISING,
    AVOIDING,
    PROBE_RTT
};

static const char* state_to_str(enum lotspeed_state state) {
    switch (state) {
        case STARTUP: return "STARTUP";
        case PROBING: return "PROBING";
        case CRUISING: return "CRUISING";
        case AVOIDING: return "AVOIDING";
        case PROBE_RTT: return "PROBE_RTT";
        default: return "UNKNOWN";
    }
}

// --- 私有数据结构 (内存优化版) ---
struct lotspeed {
    // 1. 8字节对齐字段 (u64)
    u64 target_rate;
    u64 actual_rate;
    u64 last_bw;

    // 2. 4字节对齐字段 (u32, enum)
    u32 cwnd_gain;
    u32 last_state_ts;
    u32 probe_rtt_ts;
    u32 last_cruise_ts;

    u32 rtt_min;
    u32 rtt_median;
    u32 rtt_cnt;
    u32 loss_count;

    u32 bw_stalled_rounds;
    u32 probe_cnt;
    u32 rtt_variance;
    u32 last_loss_rtt;
    u32 sample_count;
    u32 loss_ewma;

    // 丢包率统计
    u32 packet_window_count;
    u32 loss_window_count;

    enum lotspeed_state state;

    // 3. 1字节对齐字段 (bool)
    bool ss_mode;
    bool history_hit;
};

// --- 安全的历史引擎函数 ---
static void __maybe_unused free_history_entry_rcu(struct rcu_head *head)
{
    struct zeta_history_entry *entry = container_of(head, struct zeta_history_entry, rcu);
    if (zeta_history_cache)
        kmem_cache_free(zeta_history_cache, entry);
    atomic_dec(&history_entries_count);
}

static struct zeta_history_entry *find_history_safe(u32 daddr)
{
    struct zeta_history_entry *entry;
    hash_for_each_possible_rcu(zeta_history_map, entry, node, daddr) {
        if (entry && entry->daddr == daddr) {
            return entry;
        }
    }
    return NULL;
}

static void update_history_safe(u32 daddr, u64 bw, u32 rtt, u32 loss_rate)
{
    struct zeta_history_entry *entry, *oldest = NULL;
    u64 oldest_time = ULLONG_MAX;
    bool found = false;
    int bkt;

    if (!bw || !rtt) return;
    if (loss_rate > 100) loss_rate = 100;

    spin_lock_bh(&zeta_history_lock);

    entry = find_history_safe(daddr);
    if (entry) {
        entry->cached_bw = (entry->cached_bw * 7 + bw * 3) / 10;
        if (rtt < entry->cached_min_rtt) entry->cached_min_rtt = rtt;
        entry->cached_median_rtt = (entry->cached_median_rtt * 9 + rtt) / 10;
        entry->loss_ewma = (entry->loss_ewma * LOTSPEED_SMOOTH_NUM + loss_rate * (LOTSPEED_SMOOTH_DEN - LOTSPEED_SMOOTH_NUM)) / LOTSPEED_SMOOTH_DEN;
        entry->loss_count += loss_rate;
        entry->sample_count++;
        entry->last_update = get_jiffies_64();
        found = true;
    }

    if (!found) {
        if (atomic_read(&history_entries_count) >= HISTORY_MAX_ENTRIES) {
            struct zeta_history_entry *tmp;
            hash_for_each_rcu(zeta_history_map, bkt, tmp, node) {
                if (tmp && tmp->last_update < oldest_time) {
                    oldest_time = tmp->last_update;
                    oldest = tmp;
                }
            }
            if (oldest) {
                hash_del_rcu(&oldest->node);
                call_rcu(&oldest->rcu, free_history_entry_rcu);
            }
        }

        entry = kmem_cache_alloc(zeta_history_cache, GFP_ATOMIC);
        if (entry) {
            entry->daddr = daddr;
            entry->cached_bw = bw;
            entry->cached_min_rtt = rtt;
            entry->cached_median_rtt = rtt;
            entry->loss_ewma = loss_rate;
            entry->loss_count = loss_rate;
            entry->sample_count = 1;
            entry->last_update = get_jiffies_64();
            hash_add_rcu(zeta_history_map, &entry->node, daddr);
            atomic_inc(&history_entries_count);
        }
    }
    spin_unlock_bh(&zeta_history_lock);
}

// --- 状态切换 ---
static void enter_state(struct sock *sk, enum lotspeed_state new_state)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    SAFETY_CHECK(ca, );

    if (ca->state != new_state) {
        if (lotserver_verbose) {
            pr_info("lotspeed: [uk0] %s -> %s\n", state_to_str(ca->state), state_to_str(new_state));
        }
        ca->state = new_state;
        ca->last_state_ts = tcp_jiffies32;
        if (new_state == CRUISING) ca->last_cruise_ts = tcp_jiffies32;
    }
}

// --- 初始化 ---
static void lotspeed_init(struct sock *sk)
{
    struct tcp_sock *tp;
    struct lotspeed *ca;
    struct zeta_history_entry *history;
    u32 daddr;

    SAFETY_CHECK(sk, );
    tp = tcp_sk(sk);
    ca = inet_csk_ca(sk);
    daddr = sk->sk_daddr;

    memset(ca, 0, sizeof(struct lotspeed));

    ca->state = STARTUP;
    ca->last_state_ts = tcp_jiffies32;
    ca->probe_rtt_ts = tcp_jiffies32;

    // --- v5.6: 软启动 (Soft Start) ---
    // 默认更激进：初始目标速率拉高到 start_rate 的 3 倍，但不超过全局上限
    ca->target_rate = lotserver_start_rate * 3;
    if (ca->target_rate > lotserver_rate) ca->target_rate = lotserver_rate;

    ca->cwnd_gain = lotserver_gain;
    ca->ss_mode = true;
    ca->history_hit = false;
    ca->loss_ewma = 0;
    ca->last_bw = 0;
    ca->bw_stalled_rounds = 0;

    ca->packet_window_count = 0;
    ca->loss_window_count = 0;

    tp->snd_ssthresh = lotserver_turbo ? TCP_INFINITE_SSTHRESH : max(tp->snd_cwnd * 2, 10U);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
#endif

    atomic_inc(&active_connections);

    // Zeta Learning: 如果有历史记录，则根据历史记录设置起始速度
    rcu_read_lock();
    history = find_history_safe(daddr);
    if (history && history->sample_count >= ZETA_MIN_SAMPLES) {
        u64 age_ms = jiffies_to_msecs(get_jiffies_64() - history->last_update);
        u32 loss_hint = history->loss_ewma;
        if (!loss_hint && history->sample_count > 0) {
            loss_hint = SAFE_DIV(history->loss_count, history->sample_count);
        }
        if (age_ms < HISTORY_TTL_SEC * 1000ULL && history->cached_bw > 0) {
            // 使用历史带宽，但也受限于 global rate
            u64 learned_rate;

            if (loss_hint <= HIST_LOW_LOSS_PERCENT) {
                learned_rate = (history->cached_bw * LOTSPEED_RATE_UP_FAST) / 100;
            } else if (loss_hint <= HIST_HIGH_LOSS_PERCENT) {
                learned_rate = (history->cached_bw * 95) / 100;
            } else {
                learned_rate = (history->cached_bw * ZETA_ALPHA) / 100;
            }

            // 如果历史速度 > start_rate，则提升，但不超过 global rate
            if (learned_rate > ca->target_rate) {
                ca->target_rate = learned_rate;
            }
            if (ca->target_rate > lotserver_rate) {
                ca->target_rate = lotserver_rate;
            }

            ca->rtt_min = history->cached_min_rtt;
            ca->rtt_median = history->cached_median_rtt;
            ca->loss_ewma = loss_hint;
            ca->history_hit = true;

            if (tp->mss_cache > 0 && ca->rtt_min > 0) {
                u64 bdp = ca->target_rate * (u64)ca->rtt_min;
                u32 init_cwnd = SAFE_DIV64(bdp, (u64)tp->mss_cache * 1000000ULL);
                if (loss_hint <= HIST_LOW_LOSS_PERCENT) init_cwnd = init_cwnd * 12 / 10;
                init_cwnd = clamp(init_cwnd, 10U, lotserver_max_cwnd);
                tp->snd_cwnd = init_cwnd;
                tp->snd_ssthresh = max(init_cwnd, 10U);
                ca->state = PROBING;
                ca->ss_mode = false;
                if (lotserver_verbose) {
                    pr_info("lotspeed: [Zeta] HIT %pI4! Rate=%llu Mbps\n", &daddr, ca->target_rate * 8 / 1000000);
                }
            }
        }
    }
    rcu_read_unlock();
}

// --- 释放连接 ---
static void lotspeed_release(struct sock *sk)
{
    struct lotspeed *ca = inet_csk_ca(sk);

    if (!ca) {
        atomic_dec(&active_connections);
        return;
    }

    atomic_dec(&active_connections);
    if (ca->loss_count > 0) atomic_add(ca->loss_count, &total_losses);

    if (ca->sample_count >= ZETA_MIN_SAMPLES &&
        ca->actual_rate > 0 &&
        ca->rtt_min > 0 &&
        sk->sk_daddr != 0) {
        u32 loss_rate = 0;
        if (ca->packet_window_count > 0) {
            loss_rate = min(100U, SAFE_DIV(ca->loss_window_count * 100, ca->packet_window_count));
        }
        update_history_safe(sk->sk_daddr, ca->actual_rate, ca->rtt_min, loss_rate);
    }
}

static int lotspeed_debugfs_show(struct seq_file *m, void *v)
{
    unsigned long flags;
    u32 health, rtt_p, loss;

    spin_lock_irqsave(&ls_tune_stats.lock, flags);
    health = ls_tune_stats.health_ewma;
    rtt_p = ls_tune_stats.rtt_pressure_ewma;
    loss = ls_tune_stats.loss_ewma;
    spin_unlock_irqrestore(&ls_tune_stats.lock, flags);

    seq_printf(m, "health_ewma=%u\n", health);
    seq_printf(m, "rtt_pressure_ewma=%u\n", rtt_p);
    seq_printf(m, "loss_ewma=%u\n", loss);
    seq_printf(m, "up_max=%u\n", lotserver_up_max);
    seq_printf(m, "down_min=%u\n", lotserver_down_min);
    seq_printf(m, "probe_boost=%u\n", lotserver_probe_boost);
    seq_printf(m, "probe_boost_highrtt=%u\n", lotserver_probe_boost_highrtt);
    seq_printf(m, "high_rtt_us=%u\n", lotserver_high_rtt_us);
    seq_printf(m, "rtt_pressure_low=%u\n", lotserver_rtt_pressure_low);
    seq_printf(m, "rtt_pressure_high=%u\n", lotserver_rtt_pressure_high);
    seq_printf(m, "loss_noise_pct=%u\n", lotserver_loss_noise_pct);
    seq_printf(m, "noise_window_hyst=%u\n", lotserver_noise_window_hyst);
    seq_printf(m, "ml_enabled=%d\n", lotserver_ml_enabled);
    seq_printf(m, "ml_w_health=%d ml_w_rtt=%d ml_w_loss=%d ml_bias=%d\n",
               lotserver_ml_w_health, lotserver_ml_w_rtt, lotserver_ml_w_loss, lotserver_ml_bias);
    return 0;
}

static int lotspeed_debugfs_open(struct inode *inode, struct file *file)
{
    return single_open(file, lotspeed_debugfs_show, inode->i_private);
}

static const struct file_operations lotspeed_debugfs_fops = {
    .owner = THIS_MODULE,
    .open = lotspeed_debugfs_open,
    .read = seq_read,
    .llseek = seq_lseek,
    .release = single_release,
};

static void lotspeed_update_rtt(struct sock *sk, u32 rtt_us)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    SAFETY_CHECK(ca, );
    if (!rtt_us) return;

    if (ca->state == PROBE_RTT || !ca->rtt_min || rtt_us < ca->rtt_min) {
        ca->rtt_min = rtt_us;
    }

    if (ca->rtt_median == 0) ca->rtt_median = rtt_us;
    else ca->rtt_median = (ca->rtt_median * 9 + rtt_us) / 10;

    if (ca->rtt_min > 0) {
        u32 diff = (rtt_us > ca->rtt_min) ? (rtt_us - ca->rtt_min) : 0;
        ca->rtt_variance = (ca->rtt_variance * 3 + diff) / 4;
    }

    ca->rtt_cnt++;
    ca->sample_count++;
}

// --- 核心算法 ---
static void lotspeed_adapt_and_control(struct sock *sk, const struct rate_sample *rs, int flag)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);
    u64 bw = 0;
    u32 rtt_us = tp->srtt_us >> 3;
    u32 cwnd, target_cwnd;
    u32 mss = tp->mss_cache ? : 1460;
    bool congestion_detected = false;
    bool hist_recent = false;
    u64 hist_bw = 0;
    u32 hist_rtt = 0;
    u32 hist_loss = 0;
    bool high_rtt_path = false;
    u32 rtt_pressure = 100;
    u32 health_score = 100;
    u32 loss_rate_inst = 0;
    bool loss_prob_noise = false;
    static u8 noise_streak;
    u32 up_limit, down_floor, noise_thresh;
    s32 ml_score = 0;
    s32 ml_gain = 0;

    lotspeed_update_rtt(sk, rtt_us);
    if (!rtt_us) rtt_us = 1000;

    if (rs && rs->delivered > 0 && rs->interval_us > 0) {
        bw = (u64)rs->delivered * USEC_PER_SEC;
        bw = SAFE_DIV64(bw, (u64)rs->interval_us);
        ca->actual_rate = bw;

        ca->packet_window_count += (rs->delivered + rs->losses);
        ca->loss_window_count += rs->losses;

        if (ca->packet_window_count > PACKET_HISTORY_WIN) {
            ca->packet_window_count >>= 1;
            ca->loss_window_count >>= 1;
        }
    }

    if (ca->packet_window_count > 0) {
        loss_rate_inst = min(100U, SAFE_DIV(ca->loss_window_count * 100, ca->packet_window_count));
        if (ca->loss_ewma == 0) ca->loss_ewma = loss_rate_inst;
        else ca->loss_ewma = (ca->loss_ewma * LOTSPEED_SMOOTH_NUM + loss_rate_inst * (LOTSPEED_SMOOTH_DEN - LOTSPEED_SMOOTH_NUM)) / LOTSPEED_SMOOTH_DEN;
    }

    {
        struct zeta_history_entry *hist;
        rcu_read_lock();
        hist = find_history_safe(sk->sk_daddr);
        if (hist && hist->sample_count >= ZETA_MIN_SAMPLES) {
            u64 age_ms = jiffies_to_msecs(get_jiffies_64() - hist->last_update);
            if (age_ms < HISTORY_TTL_SEC * 1000ULL) {
                hist_recent = true;
                hist_bw = hist->cached_bw;
                hist_rtt = hist->cached_median_rtt ? hist->cached_median_rtt : hist->cached_min_rtt;
                hist_loss = hist->loss_ewma;
                if (!hist_loss && hist->sample_count > 0) hist_loss = SAFE_DIV(hist->loss_count, hist->sample_count);
            }
        }
        rcu_read_unlock();
    }

    if (bw > 0) {
        if (!ca->last_bw) ca->last_bw = bw;
        else ca->last_bw = (ca->last_bw * LOTSPEED_SMOOTH_NUM + bw * (LOTSPEED_SMOOTH_DEN - LOTSPEED_SMOOTH_NUM)) / LOTSPEED_SMOOTH_DEN;
    }

    if (ca->rtt_min && ca->rtt_min >= lotserver_high_rtt_us) high_rtt_path = true;
    else if (hist_recent && hist_rtt && hist_rtt >= lotserver_high_rtt_us) high_rtt_path = true;

    if (ca->rtt_min > 0) rtt_pressure = max_t(u32, 100, SAFE_DIV(rtt_us * 100, ca->rtt_min));
    if (rtt_pressure < 100) rtt_pressure = 100;

    health_score = 120;
    if (ca->loss_ewma) {
        u32 loss_penalty = min(80U, ca->loss_ewma * 2);
        if (health_score > loss_penalty) health_score -= loss_penalty;
        else health_score = 40;
    }
    if (rtt_pressure > 100) {
        u32 rtt_penalty = min(60U, (rtt_pressure - 100) / 2);
        if (health_score > rtt_penalty) health_score -= rtt_penalty;
        else health_score = 40;
    }
    health_score = clamp_t(u32, health_score, 40, 140);

    // 更新全局调参统计（抽样，降低锁竞争）
    if ((ca->sample_count & 0x3F) == 0) {
        unsigned long flags;
        spin_lock_irqsave(&ls_tune_stats.lock, flags);
        ls_tune_stats.health_ewma = (ls_tune_stats.health_ewma * LOTSPEED_SMOOTH_NUM + health_score * (LOTSPEED_SMOOTH_DEN - LOTSPEED_SMOOTH_NUM)) / LOTSPEED_SMOOTH_DEN;
        ls_tune_stats.rtt_pressure_ewma = (ls_tune_stats.rtt_pressure_ewma * LOTSPEED_SMOOTH_NUM + rtt_pressure * (LOTSPEED_SMOOTH_DEN - LOTSPEED_SMOOTH_NUM)) / LOTSPEED_SMOOTH_DEN;
        if (loss_rate_inst > 0 || ls_tune_stats.loss_ewma > 0)
            ls_tune_stats.loss_ewma = (ls_tune_stats.loss_ewma * LOTSPEED_SMOOTH_NUM + loss_rate_inst * (LOTSPEED_SMOOTH_DEN - LOTSPEED_SMOOTH_NUM)) / LOTSPEED_SMOOTH_DEN;
        spin_unlock_irqrestore(&ls_tune_stats.lock, flags);
    }

    // 动态调参：根据健康度/RTT压力调节上/下调界限与噪声阈值
    up_limit = lotserver_up_max;
    if (health_score > 110 && rtt_pressure <= lotserver_rtt_pressure_low)
        up_limit = min_t(u32, lotserver_up_max + 6, 130);
    else if (health_score < 90)
        up_limit = max_t(u32, lotserver_up_max - 5, 105);

    down_floor = lotserver_down_min;
    if (health_score > 110) down_floor = min_t(u32, 98, down_floor + 6);
    if (health_score < 90) down_floor = max_t(u32, down_floor - 4, 80);

    noise_thresh = lotserver_loss_noise_pct;
    if (health_score > 115) noise_thresh = max_t(u32, 1, lotserver_loss_noise_pct - 1);
    if (health_score < 90) noise_thresh = min_t(u32, lotserver_loss_noise_pct + 2, 10);

    if (lotserver_ml_enabled) {
        u32 loss_feature = ca->loss_ewma ? ca->loss_ewma : loss_rate_inst;
        ml_score = lotserver_ml_bias;
        ml_score += lotserver_ml_w_health * (s32)health_score;
        ml_score -= lotserver_ml_w_rtt * (s32)rtt_pressure;
        ml_score -= lotserver_ml_w_loss * (s32)loss_feature;
        ml_gain = clamp_t(s32, ml_score / 200, -20, 20);
        if (ml_gain > 0) {
            up_limit = min_t(u32, up_limit + ml_gain, 150);
            if (down_floor > 70) down_floor = max_t(u32, 70, down_floor - min(5, ml_gain));
        } else if (ml_gain < 0) {
            u32 mg = (u32)(-ml_gain);
            if (up_limit > 80) up_limit = max_t(u32, 80, up_limit > mg ? up_limit - mg : 80);
            down_floor = min_t(u32, 98, down_floor + min(5, mg));
        }
    }

    if (!lotserver_turbo || lotserver_safe_mode) {
        if (flag & CA_ACK_ECE) congestion_detected = true;
        if (ca->rtt_min > 0) {
            u32 threshold = ca->rtt_min + (ca->rtt_min >> 2) + ca->rtt_variance + RTT_JITTER_TOLERANCE_US;
            if (rtt_us > threshold) congestion_detected = true;
        }
    }

    // 概率性噪声丢包识别：低损+低RTT压力视为随机噪声，加入滞后计数
    if (loss_rate_inst > 0 && loss_rate_inst <= noise_thresh &&
        rtt_pressure <= lotserver_rtt_pressure_low && !(flag & CA_ACK_ECE)) {
        if (noise_streak < 250) noise_streak++;
    } else if (noise_streak > 0) {
        noise_streak--;
    }
    if (noise_streak >= lotserver_noise_window_hyst) loss_prob_noise = true;
    if (congestion_detected && loss_prob_noise) congestion_detected = false;

    if (congestion_detected && !(flag & CA_ACK_ECE) && hist_recent && hist_loss <= HIST_LOW_LOSS_PERCENT && bw > 0) {
        u32 ref_rtt = hist_rtt ? hist_rtt : (ca->rtt_median ? ca->rtt_median : ca->rtt_min);
        if (ref_rtt > 0) {
            u32 guard = ref_rtt + (ref_rtt >> 1) + RTT_JITTER_TOLERANCE_US;
            if (rtt_us <= guard && bw >= ca->target_rate * 8 / 10) {
                congestion_detected = false;
            }
        }
    }

    // --- v5.6+: 自适应攀升逻辑 (平滑/激进混合 + 洲际加速) ---
    if (bw > 0) {
        u64 bw_ref = ca->last_bw ? ca->last_bw : bw;
        bool healthy = (ca->loss_ewma <= HIST_HIGH_LOSS_PERCENT);
        bool very_healthy = (ca->loss_ewma <= HIST_LOW_LOSS_PERCENT);
        bool bandwidth_gap = (ca->target_rate < lotserver_rate * 95 / 100);

        // 健康通道：连续命中目标带宽后小步快爬
        if (healthy && bw_ref >= ca->target_rate * 85 / 100 && ca->state != AVOIDING) {
            ca->bw_stalled_rounds++;
            if (ca->bw_stalled_rounds >= (high_rtt_path ? 1U : 2U)) {
                u64 up_factor = very_healthy ? LOTSPEED_RATE_UP_FAST : LOTSPEED_RATE_UP;
                if (high_rtt_path) up_factor += 3;
                if (health_score > 100) up_factor += min_t(u64, 10, (health_score - 100) / 5);
                if (rtt_pressure <= lotserver_rtt_pressure_low) up_factor += 2;
                if (up_factor > up_limit) up_factor = up_limit;
                u64 next_rate = ca->target_rate * up_factor / 100;
                if (bw_ref > ca->target_rate) {
                    u64 bias = (bw_ref - ca->target_rate) / 3;
                    next_rate += bias;
                }
                if (next_rate > lotserver_rate) next_rate = lotserver_rate;
                ca->target_rate = next_rate;
                ca->bw_stalled_rounds = 0;
            }
            // 额外探测：健康且距离峰值还有空间时做一次冲高
            if (bandwidth_gap && bw_ref >= ca->target_rate * 70 / 100) {
                u64 burst = ca->target_rate * (high_rtt_path ? lotserver_probe_boost_highrtt : lotserver_probe_boost) / 100;
                u64 bw_bias = bw_ref * (high_rtt_path ? 110 : 105) / 100;
                if (bw_bias > burst) burst = bw_bias;
                if (burst > lotserver_rate) burst = lotserver_rate;
                ca->target_rate = max_t(u64, ca->target_rate, burst);
            }
        } else {
            ca->bw_stalled_rounds = 0;
            if ((bw_ref < ca->target_rate * 75 / 100 && !(high_rtt_path && very_healthy)) ||
                ca->loss_ewma >= HIST_HIGH_LOSS_PERCENT || congestion_detected) {
                u64 down_factor = (ca->loss_ewma > LOSS_RATE_CAP_PERCENT) ? LOTSPEED_RATE_DOWN_HARD : LOTSPEED_RATE_DOWN;
                if (high_rtt_path && health_score >= 100 && down_factor < 98) down_factor += 3;
                if (rtt_pressure > lotserver_rtt_pressure_high && ca->loss_ewma > HIST_HIGH_LOSS_PERCENT) {
                    if (down_factor > lotserver_down_min) down_factor -= 2;
                }
                if (loss_prob_noise && down_factor < 100) down_factor = min_t(u64, 100, down_factor + 6);
                if (health_score > 110 && loss_rate_inst <= lotserver_loss_noise_pct + 1 && down_factor < 97)
                    down_factor = 97;
                if (down_factor < down_floor) down_factor = down_floor;
                u64 next_rate = ca->target_rate * down_factor / 100;
                // 倾向实测带宽，避免过度收缩
                next_rate = (next_rate * 3 + bw_ref * 2) / 5;
                if (next_rate < lotserver_start_rate / 2) next_rate = lotserver_start_rate / 2;
                ca->target_rate = next_rate;
            }
        }
    }
    // ------------------------------------------

    if (hist_recent && ca->state != AVOIDING && hist_bw > 0) {
        u64 hist_rate = hist_bw;
        if (hist_loss > HIST_HIGH_LOSS_PERCENT) hist_rate = (hist_rate * 9) / 10;
        if (hist_rate > ca->target_rate) {
            u64 boosted = hist_rate * (high_rtt_path ? lotserver_probe_boost_highrtt : LOTSPEED_RATE_UP_FAST) / 100;
            if (boosted < hist_rate) boosted = hist_rate;
            ca->target_rate = max_t(u64, ca->target_rate, boosted);
        }
        if (ca->target_rate > lotserver_rate) ca->target_rate = lotserver_rate;
    }

    if (ca->target_rate < lotserver_start_rate / 2)
        ca->target_rate = lotserver_start_rate / 2;

    {
        u32 probe_rtt_interval = LOTSPEED_PROBE_RTT_INTERVAL_MS;
        if (high_rtt_path) probe_rtt_interval = probe_rtt_interval * 3 / 2;
        if (ca->state != PROBE_RTT && ca->rtt_min > 0 &&
            time_after32(tcp_jiffies32, ca->probe_rtt_ts + msecs_to_jiffies(probe_rtt_interval))) {
            enter_state(sk, PROBE_RTT);
        }
    }

    // 洲际路径下，轻微抖动不立刻进入避免模式
    if (high_rtt_path && !congestion_detected && bw > 0 && ca->loss_ewma <= HIST_HIGH_LOSS_PERCENT &&
        bw >= ca->target_rate * 7 / 10) {
        ca->bw_stalled_rounds = 0;
    }

    switch (ca->state) {
        case STARTUP:
            if (congestion_detected) enter_state(sk, AVOIDING);
            else if (bw > 0) {
                // 这里的逻辑改为依赖 target_rate 的攀升
                if (ca->target_rate > lotserver_start_rate * 2) {
                    ca->ss_mode = false;
                    enter_state(sk, PROBING);
                }
            }
            break;
        case PROBING:
            if (congestion_detected) enter_state(sk, AVOIDING);
            else if (bw > 0 && bw > ca->target_rate * 9 / 10) enter_state(sk, CRUISING);
            ca->probe_cnt++;
            if (ca->probe_cnt >= 100) ca->probe_cnt = 0;
            break;
        case CRUISING:
            if (congestion_detected) enter_state(sk, AVOIDING);
            else if (time_after32(tcp_jiffies32, ca->last_cruise_ts + msecs_to_jiffies(200))) enter_state(sk, PROBING);
            break;
        case AVOIDING:
            if (!congestion_detected) enter_state(sk, PROBING);
            break;
        case PROBE_RTT:
            if (time_after32(tcp_jiffies32, ca->last_state_ts + msecs_to_jiffies(LOTSPEED_PROBE_RTT_DURATION_MS))) {
                ca->probe_rtt_ts = tcp_jiffies32;
                enter_state(sk, STARTUP);
            }
            break;
    }

    // v5.6: 速率调整逻辑已经前置到 Auto-Scaling 部分，这里只需微调 gain
    switch (ca->state) {
        case STARTUP: ca->cwnd_gain = min(high_rtt_path ? lotserver_gain * 14 / 10 : lotserver_gain * 12 / 10, 36U); break;
        case PROBING: ca->cwnd_gain = high_rtt_path ? min(lotserver_gain * 12 / 10, 32U) : lotserver_gain; break;
        case CRUISING: ca->cwnd_gain = high_rtt_path ? min(lotserver_gain * 11 / 10, 30U) : lotserver_gain; break;
        case AVOIDING: ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 8 / 10, 10); break;
        case PROBE_RTT: break;
    }

    if (ca->state != PROBE_RTT) {
        u32 gain = ca->cwnd_gain;
        if (health_score > 100) {
            u32 boost = min(20U, (health_score - 100) / 2);
            gain = min_t(u32, gain * (100 + boost) / 100, 48U);
        } else {
            u32 cut = min(15U, (100 - health_score) / 2);
            gain = max_t(u32, gain * (100 - cut) / 100, 8U);
        }
        ca->cwnd_gain = gain;
    }

    // BDP 计算与窗口限制
    target_cwnd = 0;
    if (mss > 0 && rtt_us > 0 && ca->target_rate < LOTSPEED_MAX_U64 / rtt_us) {
        u64 bdp = ca->target_rate * (u64)rtt_us;
        target_cwnd = (u32)SAFE_DIV64(bdp, (u64)mss * 1000000ULL);
        if (ca->cwnd_gain > 0 && target_cwnd < LOTSPEED_MAX_U32 / ca->cwnd_gain) {
            target_cwnd = (target_cwnd * ca->cwnd_gain) / 10;
        }
    }

    // --- BDP Cap (Smart Guard) ---
    if (target_cwnd > 0) {
        u64 bdp_bytes = ca->target_rate * (u64)rtt_us;
        u32 bdp_pkts = (u32)SAFE_DIV64(bdp_bytes, (u64)mss * 1000000ULL);
        u32 bdp_cap_scale = high_rtt_path ? (BDP_CAP_SCALE + 1) : BDP_CAP_SCALE;
        u32 max_safe_cwnd = bdp_pkts * bdp_cap_scale;
        if (max_safe_cwnd < 16) max_safe_cwnd = 16;
        if (target_cwnd > max_safe_cwnd) target_cwnd = max_safe_cwnd;
    }
    // ---------------------------

    if (hist_recent && hist_bw > 0 && hist_rtt > 0 && target_cwnd > 0 && mss > 0) {
        u64 hist_bdp = hist_bw * (u64)hist_rtt;
        u32 hist_cwnd = (u32)SAFE_DIV64(hist_bdp, (u64)mss * 1000000ULL);
        if (ca->cwnd_gain > 0 && hist_cwnd < LOTSPEED_MAX_U32 / ca->cwnd_gain) {
            hist_cwnd = (hist_cwnd * ca->cwnd_gain) / 10;
        }
        if (hist_cwnd > 0) {
            u64 hist_bdp_bytes = hist_bw * (u64)hist_rtt;
            u32 hist_pkts = (u32)SAFE_DIV64(hist_bdp_bytes, (u64)mss * 1000000ULL);
            u32 hist_cap = hist_pkts * BDP_CAP_SCALE;
            if (hist_cap < 16) hist_cap = 16;
            if (hist_cwnd > hist_cap) hist_cwnd = hist_cap;
        }
        if (hist_cwnd > 0 && hist_cwnd > target_cwnd)
            target_cwnd = (target_cwnd * LOTSPEED_SMOOTH_NUM + hist_cwnd * (LOTSPEED_SMOOTH_DEN - LOTSPEED_SMOOTH_NUM)) / LOTSPEED_SMOOTH_DEN;
    }

    if (ca->state == PROBE_RTT) {
        cwnd = lotserver_min_cwnd;
    } else if (ca->ss_mode && tp->snd_cwnd < tp->snd_ssthresh) {
        if (tp->snd_cwnd < LOTSPEED_MAX_U32 / 2) cwnd = tp->snd_cwnd * 2;
        else cwnd = tp->snd_cwnd + 1;
        if (target_cwnd > 0 && cwnd >= target_cwnd) {
            ca->ss_mode = false;
            cwnd = target_cwnd;
        }
    } else {
        if (ca->state == STARTUP && rs && rs->acked_sacked > 0) {
            cwnd = (tp->snd_cwnd < LOTSPEED_MAX_U32 - rs->acked_sacked) ? tp->snd_cwnd + rs->acked_sacked : tp->snd_cwnd;
        } else {
            cwnd = target_cwnd;
        }
        if (ca->probe_cnt > 0 && ca->probe_cnt % 100 == 0 && cwnd < LOTSPEED_MAX_U32 * 10 / 11) {
            cwnd = cwnd * 11 / 10;
        }
    }

    if (!ca->ss_mode && ca->state != PROBE_RTT && target_cwnd > 0 && tp->snd_cwnd > 0 && cwnd > 0) {
        cwnd = (tp->snd_cwnd * LOTSPEED_SMOOTH_NUM + cwnd * (LOTSPEED_SMOOTH_DEN - LOTSPEED_SMOOTH_NUM)) / LOTSPEED_SMOOTH_DEN;
    }

    tp->snd_cwnd = clamp(cwnd, lotserver_min_cwnd, lotserver_max_cwnd);
    tp->snd_cwnd = min_t(u32, tp->snd_cwnd, tp->snd_cwnd_clamp);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
    {
        u64 desired_pacing;

        if (ca->state == AVOIDING) {
            desired_pacing = ca->target_rate;
        } else {
            // 1.2x Pacing
            if (ca->target_rate < LOTSPEED_MAX_U64 * 5 / 6) desired_pacing = (ca->target_rate * 6) / 5;
            else desired_pacing = ca->target_rate;
        }

        if (sk->sk_pacing_rate > 0) {
            desired_pacing = (sk->sk_pacing_rate * LOTSPEED_SMOOTH_NUM + desired_pacing * (LOTSPEED_SMOOTH_DEN - LOTSPEED_SMOOTH_NUM)) / LOTSPEED_SMOOTH_DEN;
        }

        sk->sk_pacing_rate = desired_pacing;
    }
#endif
}

#ifdef LOTSPEED_NEW_CONG_CONTROL_API
static void lotspeed_cong_control(struct sock *sk, u32 ack, int flag, const struct rate_sample *rs) {
    lotspeed_adapt_and_control(sk, rs, flag);
}
#else
static void lotspeed_cong_control(struct sock *sk, const struct rate_sample *rs) {
    lotspeed_adapt_and_control(sk, rs, 0);
}
#endif

// --- SSTHRESH ---
static u32 lotspeed_ssthresh(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);
    u32 rtt_us = tp->srtt_us >> 3;
    u32 tolerance, new_ssthresh;
    bool high_loss_detected = false;
    bool high_rtt = false;

    if (lotserver_turbo && !lotserver_safe_mode) return TCP_INFINITE_SSTHRESH;

    if (!rtt_us) rtt_us = ca->rtt_median ? ca->rtt_median : 20000;
    u32 base_rtt = ca->rtt_min ? ca->rtt_min : rtt_us;

    if ((ca->rtt_min && ca->rtt_min >= lotserver_high_rtt_us) ||
        (ca->rtt_median && ca->rtt_median >= lotserver_high_rtt_us)) {
        high_rtt = true;
    }

    // --- 丢包率熔断 ---
    if (ca->packet_window_count > 20) {
        u32 loss_rate = SAFE_DIV(ca->loss_window_count * 100, ca->packet_window_count);
        if (loss_rate > LOSS_RATE_CAP_PERCENT) {
            high_loss_detected = true;
        }
    }

    tolerance = (base_rtt * LOSS_DIFFERENTIATION_K) / 100;
    tolerance += RTT_JITTER_TOLERANCE_US;
    if (ca->rtt_variance > 0) tolerance += ca->rtt_variance / 2;

    if (rtt_us <= tolerance && !high_loss_detected) {
        // Loss Immunity
        ca->last_loss_rtt = rtt_us;
        if ((high_rtt && ca->loss_ewma <= HIST_HIGH_LOSS_PERCENT) ||
            (ca->loss_ewma && ca->loss_ewma <= HIST_LOW_LOSS_PERCENT)) {
            new_ssthresh = tp->snd_cwnd;
        } else if (lotserver_safe_mode) {
            new_ssthresh = (tp->snd_cwnd * 95) / 100;
        } else {
            new_ssthresh = tp->snd_cwnd;
        }
    } else {
        // Congestion
        ca->loss_count++;
        ca->last_loss_rtt = rtt_us;

        if (high_loss_detected) {
            ca->cwnd_gain = 10;
            new_ssthresh = max(tp->snd_cwnd >> 1, 10U);
        } else {
            if (high_rtt && ca->loss_ewma <= HIST_HIGH_LOSS_PERCENT) {
                ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 9 / 10, 12);
                new_ssthresh = (tp->snd_cwnd * 90) / 100;
            } else {
                ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 8 / 10, 10);
                new_ssthresh = (tp->snd_cwnd * lotserver_beta) / LOTSPEED_BETA_SCALE;
            }
        }
    }

    return max_t(u32, new_ssthresh, lotserver_min_cwnd);
}

static void lotspeed_set_state_hook(struct sock *sk, u8 new_state)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    bool high_rtt = (ca && ((ca->rtt_min && ca->rtt_min >= lotserver_high_rtt_us) ||
                    (ca->rtt_median && ca->rtt_median >= lotserver_high_rtt_us)));
    bool clean_path = (ca && ca->loss_ewma && ca->loss_ewma <= HIST_LOW_LOSS_PERCENT);
    switch (new_state) {
        case TCP_CA_Loss:
            if (!lotserver_turbo || lotserver_safe_mode) {
                ca->loss_count++;
                enter_state(sk, AVOIDING);
            }
            break;
        case TCP_CA_Recovery:
            if (!lotserver_turbo || lotserver_safe_mode) {
                if (high_rtt && clean_path) {
                    ca->cwnd_gain = max_t(u32, ca->cwnd_gain, min(lotserver_gain * 13 / 10, 40U));
                } else if (ca->loss_ewma && ca->loss_ewma <= HIST_LOW_LOSS_PERCENT) {
                    ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 95 / 100, 15);
                } else {
                    ca->cwnd_gain = max_t(u32, ca->cwnd_gain * 9 / 10, 15);
                }
            }
            break;
        case TCP_CA_Open:
            ca->ss_mode = false;
            break;
    }
}

static u32 lotspeed_undo_cwnd(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct lotspeed *ca = inet_csk_ca(sk);
    bool high_rtt = (ca && ((ca->rtt_min && ca->rtt_min >= lotserver_high_rtt_us) ||
                    (ca->rtt_median && ca->rtt_median >= lotserver_high_rtt_us)));
    bool clean_path = (ca && ca->loss_ewma && ca->loss_ewma <= HIST_LOW_LOSS_PERCENT);
    u32 restored;

    if (ca->loss_count > 0) ca->loss_count--;
    ca->ss_mode = false;

    restored = max(tp->snd_cwnd, tp->prior_cwnd);
    if (high_rtt && clean_path) {
        u64 boosted = (u64)restored * 110 / 100;
        if (boosted > lotserver_max_cwnd) boosted = lotserver_max_cwnd;
        restored = (u32)boosted;
    }

    return min(restored, tp->snd_cwnd_clamp);
}

static void lotspeed_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
    struct lotspeed *ca = inet_csk_ca(sk);
    switch (event) {
        case CA_EVENT_LOSS:
            ca->loss_count++;
            if (!lotserver_turbo || lotserver_safe_mode) {
                u32 step = (ca->loss_ewma && ca->loss_ewma <= HIST_LOW_LOSS_PERCENT) ? 2 : 5;
                ca->cwnd_gain = max_t(u32, ca->cwnd_gain - step, 10);
            }
            break;
        case CA_EVENT_TX_START:
        case CA_EVENT_CWND_RESTART:
            ca->ss_mode = true;
            ca->probe_cnt = 0;
            break;
        default: break;
    }
}

// --- 轻量自调节（基于全局健康度） ---
static void lotspeed_tuner_fn(struct work_struct *work)
{
    u32 health, rtt_p, loss;
    unsigned long flags;

    spin_lock_irqsave(&ls_tune_stats.lock, flags);
    health = ls_tune_stats.health_ewma ? ls_tune_stats.health_ewma : 100;
    rtt_p = ls_tune_stats.rtt_pressure_ewma ? ls_tune_stats.rtt_pressure_ewma : 100;
    loss = ls_tune_stats.loss_ewma;
    spin_unlock_irqrestore(&ls_tune_stats.lock, flags);

    if (lotserver_autotune) {
        // 激进路径：健康高且 RTT 压力低
        if (health > 115 && rtt_p <= lotserver_rtt_pressure_low && loss <= 5) {
            if (lotserver_up_max < 140) lotserver_up_max++;
            if (lotserver_probe_boost < 150) lotserver_probe_boost += 2;
            if (lotserver_probe_boost_highrtt < 180) lotserver_probe_boost_highrtt += 2;
            if (lotserver_down_min > 70) lotserver_down_min--;
        }

        // 退让路径：健康低或 RTT 压力高或丢包高
        if (health < 90 || rtt_p > lotserver_rtt_pressure_high || loss > 10) {
            if (lotserver_up_max > 100) lotserver_up_max--;
            if (lotserver_probe_boost > 80) lotserver_probe_boost = max_t(u32, 80, lotserver_probe_boost - 2);
            if (lotserver_probe_boost_highrtt > 100) lotserver_probe_boost_highrtt = max_t(u32, 100, lotserver_probe_boost_highrtt - 2);
            if (lotserver_down_min < 95) lotserver_down_min++;
            if (lotserver_loss_noise_pct > 1) lotserver_loss_noise_pct = max_t(u32, 1, lotserver_loss_noise_pct - 1);
        }

        // 轻量“ML”偏置：根据全局健康趋势微调 bias/权重
        if (lotserver_ml_enabled) {
            if (health > 120 && rtt_p < lotserver_rtt_pressure_low) {
                if (lotserver_ml_bias < 32) lotserver_ml_bias++;
            } else if (health < 85 || rtt_p > lotserver_rtt_pressure_high) {
                if (lotserver_ml_bias > -32) lotserver_ml_bias--;
            }
        }
    }

    if (lotserver_autotune && lotserver_tune_interval_ms > 0) {
        schedule_delayed_work(&ls_tuner_work, msecs_to_jiffies(lotserver_tune_interval_ms));
    }
}

static struct tcp_congestion_ops lotspeed_ops __read_mostly = {
        .name           = "lotspeed",
        .owner          = THIS_MODULE,
        .init           = lotspeed_init,
        .release        = lotspeed_release,
        .cong_control   = lotspeed_cong_control,
        .ssthresh       = lotspeed_ssthresh,
        .set_state      = lotspeed_set_state_hook,
        .undo_cwnd      = lotspeed_undo_cwnd,
        .cwnd_event     = lotspeed_cwnd_event,
        .flags          = TCP_CONG_NON_RESTRICTED,
};

static int __init lotspeed_module_init(void)
{
    BUILD_BUG_ON(sizeof(struct lotspeed) > ICSK_CA_PRIV_SIZE);

    zeta_history_cache = kmem_cache_create("lotspeed_zeta_history",
                                           sizeof(struct zeta_history_entry),
                                           0, SLAB_HWCACHE_ALIGN, NULL);
    if (!zeta_history_cache)
        return -ENOMEM;
    spin_lock_init(&ls_tune_stats.lock);

    pr_info("lotspeed v5.6 (Auto-Scaling) loaded.\n");
    pr_info("Struct size: %u bytes (limit: %u)\n",
            (unsigned int)sizeof(struct lotspeed),
            (unsigned int)ICSK_CA_PRIV_SIZE);

    hash_init(zeta_history_map);
    if (tcp_register_congestion_control(&lotspeed_ops)) {
        kmem_cache_destroy(zeta_history_cache);
        return -EINVAL;
    }
    lotspeed_debugfs_dir = debugfs_create_dir("lotspeed", NULL);
    if (!IS_ERR_OR_NULL(lotspeed_debugfs_dir)) {
        debugfs_create_file("stats", 0444, lotspeed_debugfs_dir, NULL, &lotspeed_debugfs_fops);
    }
    INIT_DELAYED_WORK(&ls_tuner_work, lotspeed_tuner_fn);
    if (lotserver_autotune && lotserver_tune_interval_ms > 0)
        schedule_delayed_work(&ls_tuner_work, msecs_to_jiffies(lotserver_tune_interval_ms));
    return 0;
}

static void __exit lotspeed_module_exit(void)
{
    struct zeta_history_entry *entry;
    struct hlist_node *tmp;
    int bkt, retry=0;

    tcp_unregister_congestion_control(&lotspeed_ops);
    cancel_delayed_work_sync(&ls_tuner_work);
    while (atomic_read(&active_connections) > 0 && retry < 50) { msleep(100); retry++; }
    synchronize_rcu();

    spin_lock_bh(&zeta_history_lock);
    hash_for_each_safe(zeta_history_map, bkt, tmp, entry, node) {
        hash_del(&entry->node);
        if (zeta_history_cache)
            kmem_cache_free(zeta_history_cache, entry);
    }
    spin_unlock_bh(&zeta_history_lock);
    debugfs_remove_recursive(lotspeed_debugfs_dir);
    if (zeta_history_cache) kmem_cache_destroy(zeta_history_cache);
    pr_info("lotspeed v5.6 unloaded.\n");
}

module_init(lotspeed_module_init);
module_exit(lotspeed_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("uk0");
MODULE_VERSION("5.6");
MODULE_AUTHOR("NeoJ <super@qwq.chat>");
MODULE_DESCRIPTION("LotSpeed Zeta - Auto-Scaling Edition");
MODULE_ALIAS("tcp_lotspeed");
