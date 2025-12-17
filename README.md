### lotspeed ml-tcp

<div align=center>
    <img src="https://github.com/uk0/lotspeed/blob/ml-tcp/logo.png" width="400" height="400" />
</div>



### branch explanation

* `ml-tcp`: lotspeed ml-tcp 基于学习历史记录的模式进行加速，并且洲际场景抖动不会降速避让。


* auto install


```bash
curl -fsSL https://raw.githubusercontent.com/uk0/lotspeed/ml-tcp/install.sh | sudo bash
#   or
wget -qO- https://raw.githubusercontent.com/uk0/lotspeed/ml-tcp/install.sh | sudo bash
```


* manual compile and load

```bash

# 下载代码/编译

git clone https://github.com/uk0/lotspeed.git 

cd lotspeed && make

# 加载模块
sudo insmod lotspeed.ko

# 设置为当前拥塞控制算法
sudo sysctl -w net.ipv4.tcp_congestion_control=lotspeed
sudo sysctl -w net.ipv4.tcp_no_metrics_save=1

# 查看是否生效
sysctl net.ipv4.tcp_congestion_control

# 查看日志
dmesg -w

```


* helper （lotserver_beta越小强的越凶，建议大雨620否则会导致CPU飙高）

```bash

#推荐参数值
默认参数调整为更适合 130–200ms 洲际链路的激进配置（文件：lotspeed.c）：

  - 高 RTT 判定 130ms (lotserver_high_rtt_us=130000)。
  - 突刺/上调：probe_boost=130、probe_boost_highrtt=150、up_max=130。
  - 下调底线更稳：down_min=92（健康高时仍会动态抬升）。
  - RTT 压力阈值：rtt_pressure_low=110、rtt_pressure_high=170。
  - 噪声丢包识别：loss_noise_pct=4、noise_window_hyst=3。
  - 自调开启，周期 1500ms：lotserver_autotune=1、lotserver_tune_interval_ms=1500。
  - 轻量 ML 默认开启，权重 health=2、rtt=1、loss=3、bias=0。

  当前默认加载即使用上述值；仍可在运行时调整：

  p=/sys/module/lotspeed/parameters
  # 可按需覆盖
  echo 120000 > $p/lotserver_high_rtt_us        # 如需更保守
  echo 140     > $p/lotserver_up_max             # 如需更激进上调

```


### test youtube


<div align=center>
    <img src="https://github.com/uk0/lotspeed/blob/ml-tcp/zeta-tcp.png" width="1024" height="768" />
</div>


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


### todo

✅ 基于“时延+丢包”混合驱动的拥塞控制
✅ 学习型状态机
✅ 洲际场景适配



### speedtest 测试结果

* 用之前

![b058ec2ebdb2a095d396cea05dccf499.png](img/b058ec2ebdb2a095d396cea05dccf499.png)

* 用之后

![f7525becdae16659ddfd54d99efe0f66.png](img/f7525becdae16659ddfd54d99efe0f66.png)


PAC (Proactive ACK Control) for TCP Incast Congestion
==========================================

* https://github.com/uk0/TCP-Incast/tree/zeta-tcp 



-----------------------------------


[![Star History Chart](https://api.star-history.com/svg?repos=uk0/lotspeed&type=Date)](https://star-history.com/#uk0/lotspeed&Date)

