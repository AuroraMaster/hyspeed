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
root@racknerd-6bf1e7b:~# lotspeed
╔════════════════════════════════════════════════════════════════════╗
║                      LotSpeed v5.6 Management                      ║
╟────────────────────────────────────────────────────────────────────╢
║ start                                               Start LotSpeed ║
║ stop                                                 Stop LotSpeed ║
║ restart                                           Restart LotSpeed ║
║ status                                                Check Status ║
║ preset [name]                                         Apply Config ║
║ set [k] [v]                                          Set Parameter ║
║ monitor                                                  Live Logs ║
║ uninstall                                        Remove Completely ║
╟────────────────────────────────────────────────────────────────────╢
║ Presets: conservative, balanced                                    ║
╚════════════════════════════════════════════════════════════════════╝
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


PAC (Proactive ACK Control) for TCP Incast Congestion
==========================================

* https://github.com/uk0/TCP-Incast/tree/zeta-tcp 



-----------------------------------


[![Star History Chart](https://api.star-history.com/svg?repos=uk0/lotspeed&type=Date)](https://star-history.com/#uk0/lotspeed&Date)

