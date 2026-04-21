/*
 * hyspeed_zeta_v5_6.c
 * "公路超跑" Zeta-TCP (FAST Edition)
 * FAST (Jin et al., INFOCOM 2004) delay-based congestion control
 * Loss response aligned with RFC3517 fast recovery; generic behavior friendly to RFC3135 PEP paths.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/moduleparam.h>
#include <linux/jiffies.h>
#include <linux/time.h>
#include <linux/string.h>
#include <linux/math64.h>
#include <net/tcp.h>
#include <linux/tcp.h>
#include <linux/hashtable.h>
#include <linux/rculist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define SAFETY_CHECK(ptr, ret) do { \
    if (unlikely(!(ptr))) { \
        return ret; \
    } \
} while (0)

#define SAFE_DIV64(n, d) ((d) ? div64_u64((n), (d)) : 0)

// 检测 cong_control API 版本
// - 4.13 之前: void (*)(struct sock *, u32, int, const struct rate_sample *)
// - 4.13 到 6.10: void (*)(struct sock *, const struct rate_sample *)
// - 6.11+: void (*)(struct sock *, u32, int, const struct rate_sample *)
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 0)
#define HYSPEED_OLD_CONG_CONTROL_API
#endif

#ifdef HYSPEED_NEW_CONG_CONTROL_API
// 6.11+ 使用带 ack/flag 参数的新 API (由 Makefile 定义)
#define HYSPEED_611_CONG_CONTROL_API
#endif

#define HYSPEED_BETA_SCALE 1024
#define HYSPEED_PROBE_RTT_INTERVAL_MS 10000
#define HYSPEED_PROBE_RTT_DURATION_MS 500
#define HYSPEED_MAX_U32 ((u32)~0U)
#define FAST_GAMMA_SCALE 100

// --- 模块参数（仅保留 FAST 所需） ---
static unsigned long hyspeed_rate = 125000000;      // 全局速率上限 (bytes/sec)
static unsigned int hyspeed_min_cwnd = 16;          // 最小拥塞窗口 (packets)
static unsigned int hyspeed_max_cwnd = 15000;       // 最大拥塞窗口 (packets)
static unsigned int hyspeed_beta = 616;             // 丢包时缩放 (cwnd * beta / 1024)
static bool hyspeed_turbo = false;                  // 关闭缩减（仅供实验）
static bool hyspeed_safe_mode = true;               // true 时遵守 beta 缩减
static unsigned int hyspeed_fast_alpha = 20;        // 目标队列长度（包）
static unsigned int hyspeed_fast_gamma = 50;        // 平滑系数（百分比）
static unsigned int hyspeed_fast_ss_exit = 25;      // RTT 膨胀阈值（百分比）触发退出 SS
// 高延迟补偿/激进模式
static bool hyspeed_hd_enable = true;
static unsigned int hyspeed_hd_thresh_us = 180000;  // 超过此 RTT 视为高延迟（默认 180ms）
static unsigned int hyspeed_hd_ref_us = 80000;      // 参考 RTT，用于 Hybla 式倍率（默认 80ms）
static unsigned int hyspeed_hd_gamma_boost = 20;    // 高延迟时额外 gamma 提升（百分比）
static unsigned int hyspeed_hd_alpha_boost = 10;    // 高延迟时额外队列目标（包）
// 轻量历史缓存
static bool hyspeed_hist_enable = true;
static unsigned int hyspeed_hist_ttl_sec = 1200;    // TTL: 20 分钟
static unsigned int hyspeed_hist_min_samples = 6;   // 需要至少 6 个样本
static unsigned int hyspeed_hist_max_entries = 8192;// 最大条目数
// 勇敢模型：抗抖动、维持高发包
static bool hyspeed_brave_enable = true;
static unsigned int hyspeed_brave_rtt_pct = 25;     // RTT 突增容忍度（百分比）
static unsigned int hyspeed_brave_hold_ms = 400;    // 突增后冻结窗口/速率时间
static unsigned int hyspeed_brave_floor_pct = 85;   // 冻结时窗口不低于此比例
static unsigned int hyspeed_brave_push_pct = 8;     // 正常时目标窗口额外提升
// RTT 噪声过滤：降低 ACK 压缩/无线抖动/隧道排队尖峰对 FAST 控制的影响
static bool hyspeed_rtt_filter_enable = true;
static unsigned int hyspeed_rtt_noise_pct = 15;     // RTT 偏差超过 base RTT 的百分比时视为噪声较高
static unsigned int hyspeed_rtt_trend_pct = 8;      // 短期 RTT 高于长期 RTT 的百分比，视为真实排队趋势

// 参数校验
static int param_set_min_cwnd(const char *val, const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    if (!ret && hyspeed_min_cwnd < 2)
        hyspeed_min_cwnd = 2;
    return ret;
}

static int param_set_max_cwnd(const char *val, const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    if (!ret && hyspeed_max_cwnd < hyspeed_min_cwnd)
        hyspeed_max_cwnd = hyspeed_min_cwnd;
    return ret;
}

static int param_set_beta(const char *val, const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    if (!ret) {
        if (hyspeed_beta > HYSPEED_BETA_SCALE)
            hyspeed_beta = HYSPEED_BETA_SCALE;
        if (hyspeed_beta < 128)
            hyspeed_beta = 128; // 不要过于激进
    }
    return ret;
}

static int param_set_alpha(const char *val, const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    if (!ret) {
        unsigned int *p = (unsigned int *)kp->arg;
        if (*p < 1) *p = 1;
        if (*p > 10000) *p = 10000;
    }
    return ret;
}

static int param_set_percent_100(const char *val, const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    if (!ret) {
        unsigned int *p = (unsigned int *)kp->arg;
        if (*p < 1) *p = 1;
        if (*p > 100) *p = 100;
    }
    return ret;
}

static int param_set_usec(const char *val, const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    if (!ret) {
        unsigned int *p = (unsigned int *)kp->arg;
        if (*p < 1000) *p = 1000;
        if (*p > 2000000) *p = 2000000; // 2s 上限，防止极端数值
    }
    return ret;
}

static int param_set_msec(const char *val, const struct kernel_param *kp)
{
    int ret = param_set_uint(val, kp);
    if (!ret) {
        unsigned int *p = (unsigned int *)kp->arg;
        if (*p < 1) *p = 1;
        if (*p > 600000) *p = 600000; // 10 分钟上限
    }
    return ret;
}

static const struct kernel_param_ops param_ops_rate = { .set = param_set_ulong, .get = param_get_ulong, };
static const struct kernel_param_ops param_ops_min_cwnd = { .set = param_set_min_cwnd, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_max_cwnd = { .set = param_set_max_cwnd, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_beta = { .set = param_set_beta, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_alpha = { .set = param_set_alpha, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_percent_100 = { .set = param_set_percent_100, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_usec = { .set = param_set_usec, .get = param_get_uint, };
static const struct kernel_param_ops param_ops_msec = { .set = param_set_msec, .get = param_get_uint, };

module_param_cb(hyspeed_rate, &param_ops_rate, &hyspeed_rate, 0644);
module_param_cb(hyspeed_min_cwnd, &param_ops_min_cwnd, &hyspeed_min_cwnd, 0644);
module_param_cb(hyspeed_max_cwnd, &param_ops_max_cwnd, &hyspeed_max_cwnd, 0644);
module_param_cb(hyspeed_beta, &param_ops_beta, &hyspeed_beta, 0644);
module_param(hyspeed_turbo, bool, 0644);
module_param(hyspeed_safe_mode, bool, 0644);
module_param_cb(hyspeed_fast_alpha, &param_ops_alpha, &hyspeed_fast_alpha, 0644);
module_param_cb(hyspeed_fast_gamma, &param_ops_percent_100, &hyspeed_fast_gamma, 0644);
module_param_cb(hyspeed_fast_ss_exit, &param_ops_percent_100, &hyspeed_fast_ss_exit, 0644);
module_param(hyspeed_hd_enable, bool, 0644);
module_param_cb(hyspeed_hd_thresh_us, &param_ops_usec, &hyspeed_hd_thresh_us, 0644);
module_param_cb(hyspeed_hd_ref_us, &param_ops_usec, &hyspeed_hd_ref_us, 0644);
module_param_cb(hyspeed_hd_gamma_boost, &param_ops_percent_100, &hyspeed_hd_gamma_boost, 0644);
module_param_cb(hyspeed_hd_alpha_boost, &param_ops_alpha, &hyspeed_hd_alpha_boost, 0644);
module_param(hyspeed_hist_enable, bool, 0644);
module_param_cb(hyspeed_hist_ttl_sec, &param_ops_rate, &hyspeed_hist_ttl_sec, 0644);
module_param_cb(hyspeed_hist_min_samples, &param_ops_min_cwnd, &hyspeed_hist_min_samples, 0644);
module_param_cb(hyspeed_hist_max_entries, &param_ops_max_cwnd, &hyspeed_hist_max_entries, 0644);
module_param(hyspeed_brave_enable, bool, 0644);
module_param_cb(hyspeed_brave_rtt_pct, &param_ops_percent_100, &hyspeed_brave_rtt_pct, 0644);
module_param_cb(hyspeed_brave_hold_ms, &param_ops_msec, &hyspeed_brave_hold_ms, 0644);
module_param_cb(hyspeed_brave_floor_pct, &param_ops_percent_100, &hyspeed_brave_floor_pct, 0644);
module_param_cb(hyspeed_brave_push_pct, &param_ops_percent_100, &hyspeed_brave_push_pct, 0644);
module_param(hyspeed_rtt_filter_enable, bool, 0644);
module_param_cb(hyspeed_rtt_noise_pct, &param_ops_percent_100, &hyspeed_rtt_noise_pct, 0644);
module_param_cb(hyspeed_rtt_trend_pct, &param_ops_percent_100, &hyspeed_rtt_trend_pct, 0644);

// --- 状态机 ---
enum hyspeed_state {
    FAST_STARTUP = 0,
    FAST_CA,
    PROBE_RTT,
};

struct hyspeed {
    u64 pacing_rate;      // 上次计算的 pacing 速率
    u32 rtt_min;          // 基准 RTT（usec）
    u32 rtt_short;        // 短周期 RTT EWMA（usec）
    u32 rtt_long;         // 长周期 RTT EWMA（usec）
    u32 rtt_dev;          // RTT 偏差 EWMA（usec）
    u32 last_state_ts;    // 状态切换时间戳（jiffies32）
    u32 probe_rtt_ts;     // 上次探测 RTT（jiffies32）
    u32 brave_hold_cwnd;  // 勇敢模式冻结时的窗口基线
    u32 brave_freeze_until; // 勇敢模式冻结截至时间（jiffies32）
    enum hyspeed_state state;
    bool ss_mode;
};

// 历史样本
struct ls_hist_entry {
    struct hlist_node node;
    struct rcu_head rcu;
    u32 daddr;
    u64 bw_bytes_sec;
    u32 rtt_min_us;
    u32 rtt_median_us;
    u32 loss_ewma;
    u32 sample_cnt;
    u64 last_update_jif;
};

#define LS_HIST_BITS 12
static DEFINE_HASHTABLE(ls_hist_table, LS_HIST_BITS);
static DEFINE_SPINLOCK(ls_hist_lock);
static struct kmem_cache *ls_hist_cache;
static atomic_t ls_hist_count = ATOMIC_INIT(0);

static void enter_state(struct sock *sk, enum hyspeed_state new_state)
{
    struct hyspeed *ca = inet_csk_ca(sk);
    SAFETY_CHECK(ca, );
    if (ca->state != new_state) {
        ca->state = new_state;
        ca->last_state_ts = tcp_jiffies32;
    }
}

static void hyspeed_update_rtt(struct sock *sk, u32 rtt_us)
{
    struct hyspeed *ca = inet_csk_ca(sk);
    SAFETY_CHECK(ca, );
    if (!rtt_us)
        return;
    if (!ca->rtt_min || rtt_us < ca->rtt_min)
        ca->rtt_min = rtt_us;
}

static u32 hyspeed_abs_diff_u32(u32 a, u32 b)
{
    return a > b ? a - b : b - a;
}

static u32 hyspeed_max3_u32(u32 a, u32 b, u32 c)
{
    return max(max(a, b), c);
}

/*
 * FAST should react to persistent queue growth, not isolated RTT spikes.
 * Keep a short EWMA, a long EWMA, and an EWMA deviation:
 *   - short RTT drives the control equation
 *   - long RTT identifies real upward trends
 *   - deviation identifies noisy samples from ACK compression/jitter
 * If a raw RTT sample is a high-noise spike without a matching trend, cap the
 * control RTT so cwnd/pacing do not collapse or trigger brave freeze.
 */
static u32 hyspeed_filter_rtt(struct sock *sk, u32 rtt_us, u32 base_rtt, bool *noise_spike)
{
    struct hyspeed *ca = inet_csk_ca(sk);
    u32 old_short;
    u32 delta;
    u32 noise_floor;
    u32 trend_floor;
    u32 spike_floor;
    bool noisy;
    bool trend_up;
    bool spike_sample;

    SAFETY_CHECK(ca, rtt_us);
    if (noise_spike)
        *noise_spike = false;

    if (!hyspeed_rtt_filter_enable || !rtt_us)
        return rtt_us;

    if (!ca->rtt_short) {
        ca->rtt_short = rtt_us;
        ca->rtt_long = rtt_us;
        ca->rtt_dev = 0;
        return rtt_us;
    }

    old_short = ca->rtt_short;
    delta = hyspeed_abs_diff_u32(rtt_us, old_short);
    ca->rtt_short = (old_short * 7 + rtt_us) >> 3;         // 1/8 EWMA
    ca->rtt_long = (ca->rtt_long * 31 + rtt_us) >> 5;      // 1/32 EWMA
    ca->rtt_dev = (ca->rtt_dev * 7 + delta) >> 3;

    if (!base_rtt)
        base_rtt = ca->rtt_min ? ca->rtt_min : ca->rtt_short;

    noise_floor = max_t(u32, (base_rtt * hyspeed_rtt_noise_pct) / 100, 1000);
    trend_floor = max_t(u32, (base_rtt * hyspeed_rtt_trend_pct) / 100, ca->rtt_dev);
    spike_floor = hyspeed_max3_u32(ca->rtt_dev * 3,
                                    (base_rtt * hyspeed_fast_ss_exit) / 100,
                                    1000);

    noisy = ca->rtt_dev > noise_floor;
    trend_up = ca->rtt_short > ca->rtt_long + trend_floor;
    spike_sample = rtt_us > old_short + spike_floor;

    if (spike_sample && noisy && !trend_up) {
        if (noise_spike)
            *noise_spike = true;
        return min_t(u32, ca->rtt_short + spike_floor, rtt_us);
    }

    return ca->rtt_short;
}

// --- 初始化与释放 ---
static void hyspeed_init(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct hyspeed *ca = inet_csk_ca(sk);
    struct ls_hist_entry *hist = NULL;
    u32 daddr = sk->sk_daddr;

    memset(ca, 0, sizeof(*ca));
    ca->state = FAST_STARTUP;
    ca->ss_mode = true;
    ca->last_state_ts = tcp_jiffies32;
    ca->probe_rtt_ts = tcp_jiffies32;
    tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
    cmpxchg(&sk->sk_pacing_status, SK_PACING_NONE, SK_PACING_NEEDED);
#endif

    // 历史初始化：如果命中历史，预填充基准 RTT 与起始 cwnd
    if (hyspeed_hist_enable && daddr) {
        rcu_read_lock();
        hash_for_each_possible_rcu(ls_hist_table, hist, node, daddr) {
            if (hist->daddr == daddr) {
                u64 age_ms = jiffies_to_msecs(get_jiffies_64() - hist->last_update_jif);
                if (age_ms < (u64)hyspeed_hist_ttl_sec * 1000 &&
                    hist->sample_cnt >= hyspeed_hist_min_samples &&
                    hist->rtt_min_us > 0) {
                    u32 init_cwnd;
                    ca->rtt_min = hist->rtt_min_us;
                    // 计算初始 cwnd，避免在 clamp_t 内使用复杂三元表达式
                    if (hist->rtt_min_us && tp->mss_cache) {
                        u64 bw_cwnd = div64_u64(hist->bw_bytes_sec * hist->rtt_min_us,
                                                (u64)tp->mss_cache * 1000000ULL);
                        init_cwnd = (u32)min_t(u64, bw_cwnd, UINT_MAX);
                    } else {
                        init_cwnd = hyspeed_min_cwnd;
                    }
                    tp->snd_cwnd = clamp_t(u32, init_cwnd, hyspeed_min_cwnd, hyspeed_max_cwnd);
                    ca->ss_mode = false;
                    ca->state = FAST_CA;
                }
                break;
            }
        }
        rcu_read_unlock();
    }
}

static void hyspeed_release(struct sock *sk)
{
    struct hyspeed *ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    u32 daddr = sk->sk_daddr;

    if (!hyspeed_hist_enable || !ca || !daddr)
        return;

    if (tp->srtt_us == 0 || tp->mss_cache == 0)
        return;

    if (ca->rtt_min == 0)
        ca->rtt_min = tp->srtt_us >> 3;

    // 汇总样本
    {
        struct ls_hist_entry *entry = NULL, *oldest = NULL;
        u64 oldest_jif = ULLONG_MAX;
        int bkt;
        u64 bw_bytes_sec = 0;
        u32 loss = 0;

        if (tp->snd_cwnd > 0 && ca->rtt_min > 0) {
            u64 bytes_in_flight = (u64)tp->snd_cwnd * tp->mss_cache;
            bw_bytes_sec = SAFE_DIV64(bytes_in_flight * USEC_PER_SEC, ca->rtt_min);
        }

        spin_lock_bh(&ls_hist_lock);
        hash_for_each_possible(ls_hist_table, entry, node, daddr) {
            if (entry->daddr == daddr) {
                // 更新现有
                entry->bw_bytes_sec = entry->bw_bytes_sec ? (entry->bw_bytes_sec * 7 + bw_bytes_sec * 3) / 10 : bw_bytes_sec;
                entry->rtt_min_us = entry->rtt_min_us && ca->rtt_min ? min(entry->rtt_min_us, ca->rtt_min) : ca->rtt_min;
                entry->rtt_median_us = entry->rtt_median_us ? (entry->rtt_median_us * 7 + (tp->srtt_us >> 3)) / 8 : (tp->srtt_us >> 3);
                entry->loss_ewma = loss;
                entry->sample_cnt++;
                entry->last_update_jif = get_jiffies_64();
                goto out_unlock;
            }
        }

        // 容量控制
        if (atomic_read(&ls_hist_count) >= hyspeed_hist_max_entries) {
            struct ls_hist_entry *tmp;
            hash_for_each(ls_hist_table, bkt, tmp, node) {
                if (tmp->last_update_jif < oldest_jif) {
                    oldest_jif = tmp->last_update_jif;
                    oldest = tmp;
                }
            }
            if (oldest) {
                hash_del(&oldest->node);
                kmem_cache_free(ls_hist_cache, oldest);
                atomic_dec(&ls_hist_count);
            }
        }

        entry = kmem_cache_alloc(ls_hist_cache, GFP_ATOMIC);
        if (entry) {
            entry->daddr = daddr;
            entry->bw_bytes_sec = bw_bytes_sec;
            entry->rtt_min_us = ca->rtt_min;
            entry->rtt_median_us = tp->srtt_us >> 3;
            entry->loss_ewma = loss;
            entry->sample_cnt = 1;
            entry->last_update_jif = get_jiffies_64();
            hash_add(ls_hist_table, &entry->node, daddr);
            atomic_inc(&ls_hist_count);
        }
out_unlock:
        spin_unlock_bh(&ls_hist_lock);
    }
}

// --- 核心 FAST 控制 ---
static void hyspeed_adapt_and_control(struct sock *sk, const struct rate_sample *rs, int flag)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct hyspeed *ca = inet_csk_ca(sk);
    u32 rtt_us = tp->srtt_us >> 3;
    u32 control_rtt_us;
    u32 base_rtt;
    u32 cwnd = tp->snd_cwnd;
    u32 mss = tp->mss_cache ? : 1460;
    u64 gamma;
    u32 pipe;
    bool high_delay_path;
    bool hist_hint = false;
    u32 hist_alpha = 0;
    bool brave_active = false;
    bool rtt_noise_spike = false;
    u32 now_jif = tcp_jiffies32;

    SAFETY_CHECK(tp && ca, );
    (void)flag;

    // 历史 RTT 预热
    if (hyspeed_hist_enable && ca->rtt_min == 0 && sk->sk_daddr) {
        struct ls_hist_entry *hist;
        rcu_read_lock();
        hash_for_each_possible_rcu(ls_hist_table, hist, node, sk->sk_daddr) {
            if (hist->daddr == sk->sk_daddr) {
                u64 age_ms = jiffies_to_msecs(get_jiffies_64() - hist->last_update_jif);
                if (age_ms < (u64)hyspeed_hist_ttl_sec * 1000 &&
                    hist->sample_cnt >= hyspeed_hist_min_samples &&
                    hist->rtt_min_us > 0) {
                    ca->rtt_min = hist->rtt_min_us;
                    hist_alpha = hist->loss_ewma < 3 ? hyspeed_fast_alpha + 5 : hyspeed_fast_alpha;
                    hist_hint = true;
                }
                break;
            }
        }
        rcu_read_unlock();
    }

    hyspeed_update_rtt(sk, rtt_us);
    if (!rtt_us)
        rtt_us = ca->rtt_min ? ca->rtt_min : 1000;
    base_rtt = ca->rtt_min ? ca->rtt_min : rtt_us;
    control_rtt_us = hyspeed_filter_rtt(sk, rtt_us, base_rtt, &rtt_noise_spike);
    if (!control_rtt_us)
        control_rtt_us = rtt_us;
    high_delay_path = hyspeed_hd_enable && base_rtt >= hyspeed_hd_thresh_us;
    brave_active = hyspeed_brave_enable && time_before((unsigned long)now_jif, (unsigned long)ca->brave_freeze_until);

    // 定期进入 PROBE_RTT 刷新基准 RTT
    if (ca->state != PROBE_RTT &&
        time_after32(tcp_jiffies32, ca->probe_rtt_ts + msecs_to_jiffies(HYSPEED_PROBE_RTT_INTERVAL_MS))) {
        enter_state(sk, PROBE_RTT);
    }

    if (ca->state == PROBE_RTT) {
        tp->snd_cwnd = hyspeed_min_cwnd;
        if (time_after32(tcp_jiffies32, ca->last_state_ts + msecs_to_jiffies(HYSPEED_PROBE_RTT_DURATION_MS))) {
            ca->probe_rtt_ts = tcp_jiffies32;
            enter_state(sk, ca->ss_mode ? FAST_STARTUP : FAST_CA);
        }
        goto out_pacing;
    }

    // 勇敢模式：检测突发 RTT 抖动，冻结窗口/速率，防止瞬时下滑
    if (hyspeed_brave_enable && base_rtt > 0 && !rtt_noise_spike) {
        u32 rtt_thresh = base_rtt + (base_rtt * hyspeed_brave_rtt_pct) / 100;
        if (control_rtt_us > rtt_thresh && tp->snd_cwnd > hyspeed_min_cwnd) {
            ca->brave_hold_cwnd = tp->snd_cwnd;
            ca->brave_freeze_until = now_jif + msecs_to_jiffies(hyspeed_brave_hold_ms);
            brave_active = true;
        }
    }

    if (ca->ss_mode) {
        if (rs && rs->acked_sacked > 0) {
            if (tp->snd_cwnd < HYSPEED_MAX_U32 - rs->acked_sacked)
                cwnd = tp->snd_cwnd + rs->acked_sacked;
        } else if (tp->snd_cwnd < HYSPEED_MAX_U32 - 1) {
            cwnd = tp->snd_cwnd + 1;
        }

        if (base_rtt && !rtt_noise_spike &&
            control_rtt_us > base_rtt + (base_rtt * hyspeed_fast_ss_exit) / 100) {
            ca->ss_mode = false;
            enter_state(sk, FAST_CA);
        }

        tp->snd_cwnd = clamp(cwnd, hyspeed_min_cwnd, hyspeed_max_cwnd);
        goto out_pacing;
    }

    // FAST 拥塞避免：cwnd = (1-gamma)*cwnd + gamma*(base_rtt/cur_rtt*cwnd + alpha)
    gamma = min_t(u64, hyspeed_fast_gamma, FAST_GAMMA_SCALE);
    if (base_rtt > 0 && control_rtt_us > 0) {
        u64 cwnd_target = ((u64)tp->snd_cwnd * base_rtt) / control_rtt_us;
        cwnd_target += hist_hint ? hist_alpha : hyspeed_fast_alpha;

        if (high_delay_path) {
            // Hybla 风格 RTT 补偿：按 RTT 比例放大，避免高 RTT 吞吐吃亏
            u64 ref = hyspeed_hd_ref_us ? hyspeed_hd_ref_us : base_rtt;
            u64 rho = ref ? SAFE_DIV64((u64)base_rtt * 100, ref) : 100;
            if (rho < 100) rho = 100;   // 不低于 1x
            if (rho > 400) rho = 400;   // 上限 4x 避免失控
            cwnd_target = min_t(u64, SAFE_DIV64(cwnd_target * rho, 100) + hyspeed_hd_alpha_boost, (u64)HYSPEED_MAX_U32);

            gamma = min_t(u64, gamma + hyspeed_hd_gamma_boost, FAST_GAMMA_SCALE);
        }

        cwnd = (u32)SAFE_DIV64((u64)tp->snd_cwnd * (FAST_GAMMA_SCALE - gamma) +
                               cwnd_target * gamma, FAST_GAMMA_SCALE);

        // 正常路径额外 push（勇敢模型），仅在非抖动时
        if (hyspeed_brave_enable && !brave_active &&
            !rtt_noise_spike &&
            control_rtt_us <= base_rtt + (base_rtt * hyspeed_fast_ss_exit) / 100) {
            u64 pushed = (u64)cwnd * (100 + hyspeed_brave_push_pct) / 100;
            if (pushed > HYSPEED_MAX_U32) pushed = HYSPEED_MAX_U32;
            cwnd = (u32)pushed;
        }
    }

    cwnd = clamp_t(u32, cwnd, hyspeed_min_cwnd, hyspeed_max_cwnd);
    pipe = tcp_packets_in_flight(tp);
    if (pipe > 0 && cwnd < pipe + 1)
        cwnd = pipe + 1; // RFC3517 风格：确保cwnd不小于在途数据量，避免停顿

    if (brave_active && ca->brave_hold_cwnd) {
        u32 floor = (ca->brave_hold_cwnd * hyspeed_brave_floor_pct) / 100;
        if (floor < hyspeed_min_cwnd) floor = hyspeed_min_cwnd;
        if (cwnd < floor) cwnd = floor;
    }

    tp->snd_cwnd = cwnd;

out_pacing:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 13, 0)
    if (mss > 0 && control_rtt_us > 0) {
        u64 rate = (u64)tp->snd_cwnd * mss;
        rate = SAFE_DIV64(rate * USEC_PER_SEC, control_rtt_us);
        if (high_delay_path) {
            // 高延迟路径给予轻微 pacing 提升，改善管道填充速度
            rate = rate * (100 + hyspeed_hd_gamma_boost / 2) / 100;
        }
        if (brave_active && ca->pacing_rate > 0 && rate < ca->pacing_rate)
            rate = ca->pacing_rate; // 冻结期间保持原速
        if (rate > hyspeed_rate)
            rate = hyspeed_rate;
        ca->pacing_rate = rate;
        sk->sk_pacing_rate = rate;
    }
#endif
}

#ifdef HYSPEED_OLD_CONG_CONTROL_API
// Linux < 4.13: 旧 API 带 ack/flag 参数
static void hyspeed_cong_control(struct sock *sk, u32 ack, int flag, const struct rate_sample *rs)
{
    hyspeed_adapt_and_control(sk, rs, flag);
}
#elif defined(HYSPEED_611_CONG_CONTROL_API)
// Linux 6.11+: 新 API 又带回 ack/flag 参数
static void hyspeed_cong_control(struct sock *sk, u32 ack, int flag, const struct rate_sample *rs)
{
    hyspeed_adapt_and_control(sk, rs, flag);
}
#else
// Linux 4.13 - 6.10: 简化 API 无 ack/flag 参数
static void hyspeed_cong_control(struct sock *sk, const struct rate_sample *rs)
{
    hyspeed_adapt_and_control(sk, rs, 0);
}
#endif

// --- SSTHRESH ---
static u32 hyspeed_ssthresh(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct hyspeed *ca = inet_csk_ca(sk);
    u32 new_ssthresh;

    if (hyspeed_turbo && !hyspeed_safe_mode)
        return TCP_INFINITE_SSTHRESH;

    if (ca)
        ca->ss_mode = false;

    // RFC3517: reduce cwnd to ssthresh on loss; FAST uses beta缩减
    new_ssthresh = (tp->snd_cwnd * hyspeed_beta) / HYSPEED_BETA_SCALE;
    return max_t(u32, new_ssthresh, hyspeed_min_cwnd);
}

static void hyspeed_set_state_hook(struct sock *sk, u8 new_state)
{
    struct hyspeed *ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    u32 pipe;

    if (!ca)
        return;

    switch (new_state) {
    case TCP_CA_Loss:
        ca->ss_mode = false;
        enter_state(sk, FAST_CA);
        pipe = tcp_packets_in_flight(tp);
        if (!hyspeed_turbo || hyspeed_safe_mode) {
            tp->snd_cwnd = max_t(u32, tp->snd_ssthresh, pipe + 1);
        }
        break;
    case TCP_CA_Recovery:
    case TCP_CA_Open:
        ca->ss_mode = false;
        enter_state(sk, FAST_CA);
        break;
    default:
        break;
    }
}

static u32 hyspeed_undo_cwnd(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct hyspeed *ca = inet_csk_ca(sk);
    u32 restored = max(tp->snd_cwnd, tp->prior_cwnd);
    if (ca)
        ca->ss_mode = false;
    return min(restored, tp->snd_cwnd_clamp);
}

static void hyspeed_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
    struct hyspeed *ca = inet_csk_ca(sk);
    struct tcp_sock *tp = tcp_sk(sk);
    if (!ca)
        return;

    switch (event) {
    case CA_EVENT_LOSS:
        ca->ss_mode = false;
        enter_state(sk, FAST_CA);
        if (!hyspeed_turbo || hyspeed_safe_mode) {
            u32 pipe = tcp_packets_in_flight(tp);
            tp->snd_cwnd = max_t(u32, tp->snd_ssthresh, pipe + 1);
        }
        break;
    case CA_EVENT_TX_START:
    case CA_EVENT_CWND_RESTART:
        ca->ss_mode = true;
        enter_state(sk, FAST_STARTUP);
        break;
    default:
        break;
    }
}

// --- 模块注册 ---
static struct tcp_congestion_ops hyspeed_ops __read_mostly = {
    .name         = "hyspeed",
    .owner        = THIS_MODULE,
    .init         = hyspeed_init,
    .release      = hyspeed_release,
    .cong_control = hyspeed_cong_control,
    .ssthresh     = hyspeed_ssthresh,
    .set_state    = hyspeed_set_state_hook,
    .undo_cwnd    = hyspeed_undo_cwnd,
    .cwnd_event   = hyspeed_cwnd_event,
    .flags        = TCP_CONG_NON_RESTRICTED,
};

static int __init hyspeed_module_init(void)
{
    BUILD_BUG_ON(sizeof(struct hyspeed) > ICSK_CA_PRIV_SIZE);

    if (hyspeed_hist_enable) {
        ls_hist_cache = kmem_cache_create("hyspeed_hist",
                                          sizeof(struct ls_hist_entry),
                                          0, SLAB_HWCACHE_ALIGN, NULL);
        if (!ls_hist_cache)
            return -ENOMEM;
        hash_init(ls_hist_table);
    }

    if (tcp_register_congestion_control(&hyspeed_ops))
        return -EINVAL;

    pr_info("hyspeed v5.6 (FAST) loaded. alpha=%u gamma=%u ss_exit=%u\n",
            hyspeed_fast_alpha, hyspeed_fast_gamma, hyspeed_fast_ss_exit);
    pr_info("Struct size: %u bytes (limit: %u)\n",
            (unsigned int)sizeof(struct hyspeed),
            (unsigned int)ICSK_CA_PRIV_SIZE);
    return 0;
}

static void __exit hyspeed_module_exit(void)
{
    struct ls_hist_entry *entry;
    struct hlist_node *tmp;
    int bkt;

    tcp_unregister_congestion_control(&hyspeed_ops);
    if (ls_hist_cache) {
        spin_lock_bh(&ls_hist_lock);
        hash_for_each_safe(ls_hist_table, bkt, tmp, entry, node) {
            hash_del(&entry->node);
            kmem_cache_free(ls_hist_cache, entry);
        }
        spin_unlock_bh(&ls_hist_lock);
        kmem_cache_destroy(ls_hist_cache);
        ls_hist_cache = NULL;
    }

    pr_info("hyspeed v5.6 (FAST) unloaded.\n");
}

module_init(hyspeed_module_init);
module_exit(hyspeed_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AuroraMaster");
MODULE_VERSION("5.6");
MODULE_AUTHOR("NeoJ <super@qwq.chat>");
MODULE_DESCRIPTION("HySpeed Zeta - FAST delay-based congestion control");
MODULE_ALIAS("tcp_hyspeed");
