### hyspeed adaptive ml-tcp

<div align=center>
    <img src="https://github.com/AuroraMaster/hyspeed/blob/main/logo.png" width="400" height="400" />
</div>


### supported kernel

* kernel_version:
    - "6.18.2" # LTS
    - "6.12.8"
    - "6.11.9"
    - "5.15.99"

### project profile

* `main`: hyspeed 是一个 FAST delay-based 拥塞控制变体，叠加历史学习、RTT 噪声过滤、高延迟补偿、brave 抗抖动和自适应可靠度门控。
* 当前版本融合了 BBRv3 风格的 inflight 上下界护栏，以及类似 Polar 码“可靠信道释放、不可靠信道冻结”的可靠度调度思想：可靠时允许更强 probe/push，不可靠时冻结进攻增益并回到硬护栏。


### control model

<div align=center>
    <img src="img/hyspeed_adaptive_model.png" width="1024" />
</div>

核心控制链路：

```text
raw RTT / delivery rate / loss / inflight
    -> RTT short/long EWMA + jitter filter
    -> reliability score
    -> lightweight online optimizer
    -> FAST target cwnd
    -> adaptive attack gate
    -> BBRv3-style inflight_hi / inflight_lo guard
    -> pacing_rate
```

数学形式：

```text
FAST_target = cwnd * base_rtt / filtered_rtt + alpha
reliability = f(queue_growth, rtt_trend, rtt_dev, loss_ewma, bw_fallback)
score       = bw_gain - queue_weight * queue_growth - loss_weight * loss_ewma
cwnd_next   = clamp(FAST_target * attack_gate, min_cwnd, inflight_guard)
pacing_rate = cwnd_next * MSS / filtered_rtt * reliability_gain
```


* auto install


```bash
curl -fsSL https://raw.githubusercontent.com/AuroraMaster/hyspeed/main/install.sh | sudo bash
#   or
wget -qO- https://raw.githubusercontent.com/AuroraMaster/hyspeed/main/install.sh | sudo bash
```


* manual compile and load

```bash

# 下载代码/编译

git clone https://github.com/AuroraMaster/hyspeed.git

cd hyspeed && make

# 加载模块
sudo insmod hyspeed.ko

# 设置为当前拥塞控制算法
sudo sysctl -w net.ipv4.tcp_congestion_control=hyspeed
sudo sysctl -w net.ipv4.tcp_no_metrics_save=1

# 查看是否生效
sysctl net.ipv4.tcp_congestion_control

# 查看日志
dmesg -w

```


* helper （hyspeed_beta 越小越激进；共享 VPS 建议保持 safe mode，不要长期使用过低 beta）

```bash

[cce ~]$ hyspeed status
╔════════════════════════════════════════════════════════════════════╗
║                   HySpeed v5.6 Status (ML-TCP)                    ║
╟────────────────────────────────────────────────────────────────────╢
║ Module Status                                               Loaded ║
║ Reference Count                                                  1 ║
║ Active Connections                                              00 ║
║ Active Algorithm                                          hyspeed ║
╟────────────────────────────────────────────────────────────────────╢
║                         Current Parameters                         ║
╟────────────────────────────────────────────────────────────────────╢
║ Global Rate Limit                          125.00 MB/s (1.00 Gbps) ║
║ Min CWND                                                16 packets ║
║ Max CWND                                             15000 packets ║
║ Fairness (Beta)                                                60% ║
║ Turbo Mode                                                Disabled ║
║ Safe Mode                                                  Enabled ║
║ FAST Alpha                                              20 packets ║
║ FAST Gamma                                                     50% ║
║ SS Exit Threshold                                              25% ║
║ High-Delay Mode                                            Enabled ║
║ HD Threshold                                              180000us ║
║ HD Reference RTT                                           80000us ║
║ HD Gamma Boost                                                 20% ║
║ HD Alpha Boost                                          10 packets ║
║ Brave Mode                                                 Enabled ║
║ Brave RTT Tolerance                                            25% ║
║ Brave Hold Time                                              400ms ║
║ Brave Floor                                                    85% ║
║ Brave Push                                                      8% ║
║ Adaptive Mode                                              Enabled ║
║ Attack Push                                                   10% ║
║ Attack Max                                                    25% ║
║ Probe Gain                                                   115% ║
║ Loss Guard                                                     5% ║
║ Queue Guard                                                   70% ║
║ Reliability Floor                                             35% ║
║ Inflight Headroom                                             15% ║
║ Online Optimizer                                          Enabled ║
║ Optimizer Interval                                         2 RTTs ║
║ Optimizer Step                                                1% ║
║ Optimizer Max Attack                                         30% ║
║ Optimizer Max Probe                                         130% ║
╚════════════════════════════════════════════════════════════════════╝
[cce ~]$ hyspeed help
╔════════════════════════════════════════════════════════════════════╗
║                      HySpeed v5.6 Management                      ║
╟────────────────────────────────────────────────────────────────────╢
║ start                                               Start HySpeed ║
║ stop                                                 Stop HySpeed ║
║ restart                                           Restart HySpeed ║
║ status                                                Check Status ║
║ preset [name]                                         Apply Preset ║
║ set [k] [v]                                          Set Parameter ║
║ monitor                                                  Live Logs ║
║ uninstall                                        Remove Completely ║
╟────────────────────────────────────────────────────────────────────╢
║ Presets: conservative, balanced, aggressive                        ║
╚════════════════════════════════════════════════════════════════════╝

```

### adaptive parameters

| 参数 | 默认值 | 作用 |
| --- | ---: | --- |
| `hyspeed_adaptive_enable` | `true` | 开启可靠度门控与 inflight 护栏 |
| `hyspeed_attack_push_pct` | `10` | 可靠状态下对 FAST 目标窗口的额外提升 |
| `hyspeed_attack_max_pct` | `25` | 自适应进攻提升上限 |
| `hyspeed_probe_gain_pct` | `115` | 可靠状态下 pacing 探测增益上限 |
| `hyspeed_loss_guard_pct` | `5` | loss EWMA 超过该比例后冻结进攻 |
| `hyspeed_queue_guard_pct` | `70` | 持续 RTT 队列膨胀超过该比例后进入硬护栏 |
| `hyspeed_reliability_floor_pct` | `35` | 可靠度低于该值时冻结进攻增益 |
| `hyspeed_inflight_headroom_pct` | `15` | 拥塞后为 inflight_hi 预留的 headroom |
| `hyspeed_optimizer_enable` | `true` | 开启轻量在线优化器 |
| `hyspeed_opt_interval_rtts` | `2` | 每 N 个 RTT 评估一次收益/代价 |
| `hyspeed_opt_step_pct` | `1` | 单次调整 attack/probe 的百分比步长 |
| `hyspeed_opt_max_attack_pct` | `30` | 在线优化器允许的 attack 上限 |
| `hyspeed_opt_max_probe_pct` | `130` | 在线优化器允许的 probe pacing 上限 |
| `hyspeed_opt_queue_weight` | `2` | 队列膨胀代价权重 |
| `hyspeed_opt_loss_weight` | `12` | 丢包代价权重 |

临时调参示例：

```bash
sudo sh -c 'echo 15 > /sys/module/hyspeed/parameters/hyspeed_attack_push_pct'
sudo sh -c 'echo 125 > /sys/module/hyspeed/parameters/hyspeed_probe_gain_pct'
sudo sh -c 'echo 6 > /sys/module/hyspeed/parameters/hyspeed_loss_guard_pct'
sudo sh -c 'echo 1 > /sys/module/hyspeed/parameters/hyspeed_opt_step_pct'
```


### test iperf3 loss

```bash
# disable lro
ethtool -K eth0 lro off
# 丢包16%
sudo tc qdisc add dev ens3 root netem loss 16%
sudo tc qdisc add dev eth0 root netem loss 16%

#取消丢包
sudo tc qdisc del dev ens3 root netem
sudo tc qdisc del dev eth0 root netem

# test command
iperf3 -4 -s -p 35201
iperf3 -c green1 -p 35201 -R -t 30
```

建议额外记录：

```bash
ss -tin 'sport = :35201 or dport = :35201'
nstat -az | egrep 'Retrans|Timeout|TCPLoss|TCPFastRetrans'
```

公平性测试建议同时跑 `hyspeed` 和 `cubic`/`bbr`，按 Jain index 计算：

```text
fairness = (x1 + x2)^2 / (2 * (x1^2 + x2^2))
```


### todo

✅ 基于“时延+丢包”混合驱动的拥塞控制
✅ 学习型状态机
✅ 洲际场景适配
✅ RTT 噪声过滤与运营商整形误判抑制
✅ BBRv3 风格 inflight 上下界护栏
✅ 可靠度门控的自适应进攻/冻结模型
✅ 小状态、可解释、快速收敛的在线优化器



-----------------------------------


## Star History

[![Star History Chart](https://api.star-history.com/svg?repos=AuroraMaster/hyspeed&type=timeline&logscale&legend=top-left)](https://www.star-history.com/#AuroraMaster/hyspeed&type=timeline&logscale&legend=top-left)
