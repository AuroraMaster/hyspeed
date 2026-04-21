#!/bin/bash
#
# HySpeed v5.6 - ml-tcp Auto-Scaling Edition (UI Enhanced)
# Author: AuroraMaster
# GitHub: https://github.com/AuroraMaster/hyspeed
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/AuroraMaster/hyspeed/main/install.sh | sudo bash
#

set -e

# ================= 配置区域 =================
GITHUB_REPO="AuroraMaster/hyspeed"
GITHUB_BRANCH="main"
INSTALL_DIR="/opt/hyspeed"
MODULE_NAME="hyspeed"
VERSION="5.6"
CURRENT_TIME=$(date '+%Y-%m-%d %H:%M:%S')
CURRENT_USER=$(whoami)

# ================= 颜色定义 =================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
CYAN='\033[0;36m'
WHITE='\033[1;37m'
NC='\033[0m' # No Color

# ================= UI 核心算法 (安装脚本用) =================
BOX_WIDTH=70

# 计算视觉宽度 (忽略颜色代码, 中文/全角符号算2, 英文算1)
get_width() {
    local str="$1"
    # 移除 ANSI 颜色代码
    local clean_str=$(echo -e "$str" | sed -r "s/\x1B\[([0-9]{1,2}(;[0-9]{1,2})?)?[mGK]//g")

    local width=0
    local len=${#clean_str}

    for ((i=0; i<len; i++)); do
        local char="${clean_str:$i:1}"
        # 获取字符的 ASCII 值，简单的判断方法
        local ord=$(printf "%d" "'$char" 2>/dev/null || echo 128)
        if [ "$ord" -gt 127 ]; then
            ((width+=2))
        else
            ((width+=1))
        fi
    done
    echo $width
}

# 打印重复字符
repeat_char() {
    local char="$1"
    local count="$2"
    if [ "$count" -gt 0 ]; then
        printf "%0.s$char" $(seq 1 $count)
    fi
}

# 绘制盒子顶部
print_box_top() {
    local color="${1:-$CYAN}"
    echo -ne "${color}╔"
    repeat_char "═" $((BOX_WIDTH - 2))
    echo -e "╗${NC}"
}

# 绘制盒子分隔线
print_box_div() {
    local color="${1:-$CYAN}"
    echo -ne "${color}╟"
    repeat_char "─" $((BOX_WIDTH - 2))
    echo -e "╢${NC}"
}

# 绘制盒子底部
print_box_bottom() {
    local color="${1:-$CYAN}"
    echo -ne "${color}╚"
    repeat_char "═" $((BOX_WIDTH - 2))
    echo -e "╝${NC}"
}

# 绘制内容行 (支持 align=left|center)
print_box_row() {
    local content="$1"
    local align="${2:-left}"
    local color="${3:-$CYAN}"

    local content_width=$(get_width "$content")
    local total_padding=$((BOX_WIDTH - 2 - content_width))

    if [ $total_padding -lt 0 ]; then total_padding=0; fi

    echo -ne "${color}║${NC}"

    if [ "$align" == "center" ]; then
        local left_pad=$((total_padding / 2))
        local right_pad=$((total_padding - left_pad))
        repeat_char " " $left_pad
        echo -ne "$content"
        repeat_char " " $right_pad
    else
        echo -ne " $content"
        repeat_char " " $((total_padding - 1))
    fi

    echo -e "${color}║${NC}"
}

# ================= 基础日志函数 =================
log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_success() { echo -e "${GREEN}[✓]${NC} $1"; }

print_banner() {
    echo -e "${CYAN}"
    cat << "EOF"
╔══════════════════════════════════════════════════════════════════════╗
║                                                                      ║
║      _          _   ____                      _                      ║
║     | |    ___ | |_/ ___| _ __   ___  ___  __| |                     ║
║     | |   / _ \| __\___ \| '_ \ / _ \/ _ \/ _` |                     ║
║     | |__| (_) | |_ ___) | |_) |  __/  __/ (_| |                     ║
║     |_____\___/ \__|____/| .__/ \___|\___|\__,_|                     ║
║                          |_|                                         ║
║                                                                      ║
║                 ML-TCP Auto-Scaling Edition                          ║
║                       Version 5.6rc                                  ║
╚══════════════════════════════════════════════════════════════════════╝
EOF
    echo -e "${NC}"
}

# ================= 安装逻辑函数 =================

check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "This script must be run as root"
        echo -e "${YELLOW}Try: curl -fsSL <url> | sudo bash${NC}"
        exit 1
    fi
}

check_system() {
    log_info "Checking system compatibility..."

    # 检查 OS
    if [[ -f /etc/redhat-release ]]; then
        OS="centos"
        OS_VERSION=$(cat /etc/redhat-release | sed 's/.*release \([0-9]\).*/\1/')
    elif [[ -f /etc/debian_version ]]; then
        OS="debian"
        OS_VERSION=$(cat /etc/debian_version | cut -d. -f1)
        if grep -qi ubuntu /etc/os-release 2>/dev/null; then
            OS="ubuntu"
            OS_VERSION=$(grep VERSION_ID /etc/os-release | cut -d'"' -f2 | cut -d. -f1)
        fi
    else
        log_error "Unsupported operating system"
        exit 1
    fi

    # 检查内核版本
    KERNEL_VERSION=$(uname -r | cut -d. -f1-2)
    KERNEL_MAJOR=$(echo $KERNEL_VERSION | cut -d. -f1)
    KERNEL_MINOR=$(echo $KERNEL_VERSION | cut -d. -f2)

    if [[ $KERNEL_MAJOR -lt 4 ]] || ([[ $KERNEL_MAJOR -eq 4 ]] && [[ $KERNEL_MINOR -lt 9 ]]); then
        log_error "Kernel version must be >= 4.9 (current: $(uname -r))"
        exit 1
    fi

    # 检查架构
    ARCH=$(uname -m)
    if [[ "$ARCH" != "x86_64" ]] && [[ "$ARCH" != "aarch64" ]]; then
        log_warn "Architecture $ARCH may not be fully tested"
    fi

    log_success "System: $OS $OS_VERSION (kernel $(uname -r), $ARCH)"
}

install_dependencies() {
    log_info "Installing dependencies..."

    if [[ "$OS" == "centos" ]]; then
        yum install -y gcc make kernel-devel-$(uname -r) kernel-headers-$(uname -r) wget curl bc 2>/dev/null || {
            log_warn "Some packages may be missing, trying alternative..."
            yum install -y gcc make kernel-devel kernel-headers wget curl bc
        }
        # 尝试安装 clang 作为备用编译器（如果 gcc 版本过旧）
        local gcc_ver=$(gcc -dumpversion 2>/dev/null | cut -d. -f1)
        if [[ -n "$gcc_ver" ]] && [[ "$gcc_ver" -lt 8 ]]; then
            log_info "GCC version is old, installing clang as backup..."
            yum install -y clang 2>/dev/null || log_warn "Could not install clang"
        fi
    elif [[ "$OS" == "debian" ]] || [[ "$OS" == "ubuntu" ]]; then
        apt-get update >/dev/null 2>&1
        apt-get install -y gcc make linux-headers-$(uname -r) wget curl bc kmod 2>/dev/null || {
            log_warn "Some packages may be missing, trying alternative..."
            apt-get install -y gcc make linux-headers-generic wget curl bc kmod
        }
        if grep -qi clang /proc/version 2>/dev/null; then
            log_info "Kernel was built with Clang, installing LLVM toolchain helpers..."
            apt-get install -y clang lld llvm 2>/dev/null || log_warn "Could not install generic LLVM packages"
        fi
        # 尝试安装 clang 作为备用编译器（如果 gcc 版本过旧）
        local gcc_ver=$(gcc -dumpversion 2>/dev/null | cut -d. -f1)
        if [[ -n "$gcc_ver" ]] && [[ "$gcc_ver" -lt 8 ]]; then
            log_info "GCC version is old, installing clang as backup..."
            apt-get install -y clang 2>/dev/null || log_warn "Could not install clang"
        fi
    fi

    log_success "Dependencies installed"
}

find_versioned_tool() {
    local base="$1"
    local major="$2"

    if [[ -n "$major" ]] && command -v "${base}-${major}" &>/dev/null; then
        command -v "${base}-${major}"
        return 0
    fi

    command -v "$base" 2>/dev/null || true
}

kernel_clang_major() {
    local version_line=""
    version_line=$(cat /proc/version 2>/dev/null || true)
    echo "$version_line" | grep -qi clang || return 1
    echo "$version_line" | grep -oE 'clang version [0-9]+' | awk '{print $3}' | head -1
}

llvm_make_args() {
    local clang_major="$1"
    local clang_bin ld_bin ar_bin nm_bin objcopy_bin objdump_bin strip_bin

    clang_bin=$(find_versioned_tool clang "$clang_major")
    [[ -n "$clang_bin" ]] || return 1

    ld_bin=$(find_versioned_tool ld.lld "$clang_major")
    ar_bin=$(find_versioned_tool llvm-ar "$clang_major")
    nm_bin=$(find_versioned_tool llvm-nm "$clang_major")
    objcopy_bin=$(find_versioned_tool llvm-objcopy "$clang_major")
    objdump_bin=$(find_versioned_tool llvm-objdump "$clang_major")
    strip_bin=$(find_versioned_tool llvm-strip "$clang_major")

    printf 'LLVM=1 CC=%q' "$clang_bin"
    [[ -n "$ld_bin" ]] && printf ' LD=%q' "$ld_bin"
    [[ -n "$ar_bin" ]] && printf ' AR=%q' "$ar_bin"
    [[ -n "$nm_bin" ]] && printf ' NM=%q' "$nm_bin"
    [[ -n "$objcopy_bin" ]] && printf ' OBJCOPY=%q' "$objcopy_bin"
    [[ -n "$objdump_bin" ]] && printf ' OBJDUMP=%q' "$objdump_bin"
    [[ -n "$strip_bin" ]] && printf ' STRIP=%q' "$strip_bin"
}

run_make_capture() {
    local make_cmd="$1"
    local rc

    set +e
    compile_output=$(eval "$make_cmd" 2>&1)
    rc=$?
    set -e
    return $rc
}

download_source() {
    log_info "Downloading HySpeed v$VERSION source code..."

    # 创建安装目录
    mkdir -p $INSTALL_DIR
    cd $INSTALL_DIR

    # 下载源代码
    curl -fsSL "https://raw.githubusercontent.com/$GITHUB_REPO/$GITHUB_BRANCH/hyspeed.c" -o hyspeed.c || {
        log_error "Failed to download hyspeed.c"
        exit 1
    }

    # 下载 Makefile
    curl -fsSL "https://raw.githubusercontent.com/$GITHUB_REPO/$GITHUB_BRANCH/Makefile" -o Makefile || {
        log_error "Failed to download Makefile"
        exit 1
    }

    log_success "Source code downloaded"
}

detect_compiler() {
    # 检测可用的编译器
    local gcc_version=""
    local clang_version=""
    local preferred_cc=""

    if command -v gcc &>/dev/null; then
        gcc_version=$(gcc -dumpversion 2>/dev/null | cut -d. -f1)
        log_info "Detected GCC version: $gcc_version"
    fi

    if command -v clang &>/dev/null; then
        clang_version=$(clang --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1)
        log_info "Detected Clang version: $clang_version"
    fi

    # 优先使用 gcc，但如果版本过旧（<8）且有 clang，使用 clang
    if [[ -n "$gcc_version" ]]; then
        if [[ "$gcc_version" -ge 8 ]]; then
            preferred_cc="gcc"
        elif [[ -n "$clang_version" ]]; then
            log_warn "GCC version $gcc_version is old, using Clang instead"
            preferred_cc="clang"
        else
            preferred_cc="gcc"
        fi
    elif [[ -n "$clang_version" ]]; then
        preferred_cc="clang"
    else
        log_error "No C compiler found. Please install gcc or clang."
        exit 1
    fi

    echo "$preferred_cc"
}

compile_module() {
    log_info "Compiling HySpeed v$VERSION kernel module..."

    cd $INSTALL_DIR
    make clean >/dev/null 2>&1

    local compiler=$(detect_compiler)
    local compile_success=0
    local compile_output=""
    local clang_major=""
    local llvm_args=""

    clang_major=$(kernel_clang_major || true)
    if [[ -n "$clang_major" ]]; then
        llvm_args=$(llvm_make_args "$clang_major" || true)
        if [[ -n "$llvm_args" ]]; then
            log_info "Kernel was built with Clang $clang_major; trying LLVM build first..."
            if run_make_capture "make $llvm_args" && [[ -f hyspeed.ko ]]; then
                compile_success=1
            else
                log_warn "LLVM build did not succeed, falling back to compiler probing..."
                make clean >/dev/null 2>&1
            fi
        fi
    fi

    # 第一次尝试：使用检测到的编译器
    if [[ $compile_success -eq 0 ]]; then
        log_info "Trying compilation with $compiler..."
        if [[ "$compiler" == "clang" ]]; then
            if run_make_capture "make CC=clang" && [[ -f hyspeed.ko ]]; then
                compile_success=1
            fi
        else
            if run_make_capture "make" && [[ -f hyspeed.ko ]]; then
                compile_success=1
            fi
        fi
    fi

    # 如果 gcc 编译失败，检查是否是不支持的选项错误，尝试 clang
    if [[ $compile_success -eq 0 ]]; then
        if echo "$compile_output" | grep -qE "unrecognized command-line option|unknown argument"; then
            log_warn "Compiler option error detected, trying with clang..."
            if [[ -n "$llvm_args" ]]; then
                make clean >/dev/null 2>&1
                if run_make_capture "make $llvm_args" && [[ -f hyspeed.ko ]]; then
                    compile_success=1
                    log_success "Compilation succeeded with LLVM toolchain"
                fi
            elif command -v clang &>/dev/null; then
                make clean >/dev/null 2>&1
                if run_make_capture "make CC=clang" && [[ -f hyspeed.ko ]]; then
                    compile_success=1
                    log_success "Compilation succeeded with clang"
                fi
            fi
        fi
    fi

    # 如果仍然失败，显示错误信息
    if [[ $compile_success -eq 0 ]]; then
        log_error "Compilation failed. Error output:"
        echo "$compile_output" | tail -30
        echo ""
        log_error "Possible solutions:"
        echo "  1. Update your gcc to version 8 or higher"
        echo "  2. Install clang/lld/llvm matching the kernel compiler"
        echo "  3. Check if kernel headers are properly installed"
        exit 1
    fi

    if [[ ! -f hyspeed.ko ]]; then
        log_error "Module compilation failed - hyspeed.ko not found"
        exit 1
    fi

    log_success "Module compiled successfully"
}

load_module() {
    log_info "Loading HySpeed v$VERSION module..."

    # 卸载旧模块（如果存在）
    rmmod hyspeed 2>/dev/null || true

    install -D -m 0644 "$INSTALL_DIR/hyspeed.ko" "/lib/modules/$(uname -r)/extra/hyspeed.ko"
    depmod -a

    # 加载新模块
    modprobe hyspeed || insmod "$INSTALL_DIR/hyspeed.ko" || {
        log_error "Failed to load module"
        dmesg | tail -10
        exit 1
    }

    # 设置为默认拥塞控制算法
    sysctl -w net.ipv4.tcp_congestion_control=hyspeed >/dev/null 2>&1

    # 持久化设置
    cat > /etc/sysctl.d/99-hyspeed.conf <<'EOF'
net.ipv4.tcp_congestion_control = hyspeed
net.ipv4.tcp_no_metrics_save = 1
EOF
    sysctl -p /etc/sysctl.d/99-hyspeed.conf >/dev/null 2>&1 || true

    # 设置开机自动加载
    echo "hyspeed" > /etc/modules-load.d/hyspeed.conf

    log_success "Module loaded and set as default"
}

# ================= 创建管理脚本 (嵌入 UI 算法) =================
create_management_script() {
    log_info "Creating management script..."

    # 使用 'SCRIPT_EOF' 避免变量在此时展开，而是在生成的脚本运行时展开
    cat > /usr/local/bin/hyspeed << 'SCRIPT_EOF'
#!/bin/bash
# HySpeed Management Script (Auto-Aligned UI)

ACTION=$1
INSTALL_DIR="/opt/hyspeed"
VERSION="5.6"

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
WHITE='\033[1;37m'
BOLD='\033[1m'
NC='\033[0m'

# ================= UI 算法 (嵌入) =================
BOX_WIDTH=70

# 计算视觉宽度
get_width() {
    local str="$1"
    local clean_str=$(echo -e "$str" | sed -r "s/\x1B\[([0-9]{1,2}(;[0-9]{1,2})?)?[mGK]//g")
    local width=0
    local len=${#clean_str}
    for ((i=0; i<len; i++)); do
        local char="${clean_str:$i:1}"
        local ord=$(printf "%d" "'$char" 2>/dev/null || echo 128)
        if [ "$ord" -gt 127 ]; then ((width+=2)); else ((width+=1)); fi
    done
    echo $width
}

repeat_char() {
    if [ "$2" -gt 0 ]; then printf "%0.s$1" $(seq 1 $2); fi
}

print_box_top() {
    local color="${1:-$CYAN}"
    echo -ne "${color}╔"
    repeat_char "═" $((BOX_WIDTH - 2))
    echo -e "╗${NC}"
}

print_box_div() {
    local color="${1:-$CYAN}"
    echo -ne "${color}╟"
    repeat_char "─" $((BOX_WIDTH - 2))
    echo -e "╢${NC}"
}

print_box_bottom() {
    local color="${1:-$CYAN}"
    echo -ne "${color}╚"
    repeat_char "═" $((BOX_WIDTH - 2))
    echo -e "╝${NC}"
}

print_box_row() {
    local content="$1"
    local align="${2:-left}"
    local color="${3:-$CYAN}"

    local content_width=$(get_width "$content")
    local total_padding=$((BOX_WIDTH - 2 - content_width))
    [ $total_padding -lt 0 ] && total_padding=0

    echo -ne "${color}║${NC}"
    if [ "$align" == "center" ]; then
        local left_pad=$((total_padding / 2))
        local right_pad=$((total_padding - left_pad))
        repeat_char " " $left_pad
        echo -ne "$content"
        repeat_char " " $right_pad
    else
        echo -ne " $content"
        repeat_char " " $((total_padding - 1))
    fi
    echo -e "${color}║${NC}"
}

# 打印键值对行 (Left Key ...... Right Value)
print_kv_row() {
    local key="$1"
    local val="$2"
    local color="${3:-$CYAN}"

    local key_width=$(get_width "$key")
    local val_width=$(get_width "$val")

    # 左右各1空格Padding + 中间
    local available=$((BOX_WIDTH - 4))
    local padding=$((available - key_width - val_width))

    [ $padding -lt 1 ] && padding=1

    echo -ne "${color}║${NC} $key"
    repeat_char " " $padding
    echo -e "$val ${color}║${NC}"
}

# ================= 业务逻辑 =================

format_bytes() {
    local bytes=$1
    if [[ $bytes -ge 1000000000 ]]; then
        echo "$(echo "scale=2; $bytes/1000000000" | bc) GB/s"
    elif [[ $bytes -ge 1000000 ]]; then
        echo "$(echo "scale=2; $bytes/1000000" | bc) MB/s"
    elif [[ $bytes -ge 1000 ]]; then
        echo "$(echo "scale=2; $bytes/1000" | bc) KB/s"
    else
        echo "$bytes B/s"
    fi
}

format_bps() {
    local bytes=$1
    local bits=$((bytes * 8))
    if [[ $bits -ge 1000000000 ]]; then
        echo "$(echo "scale=2; $bits/1000000000" | bc) Gbps"
    elif [[ $bits -ge 1000000 ]]; then
        echo "$(echo "scale=2; $bits/1000000" | bc) Mbps"
    elif [[ $bits -ge 1000 ]]; then
        echo "$(echo "scale=2; $bits/1000" | bc) Kbps"
    else
        echo "$bits bps"
    fi
}

get_default_congestion_control() {
    AVAILABLE=$(sysctl net.ipv4.tcp_available_congestion_control | awk -F= '{print $2}')
    if echo "$AVAILABLE" | grep -q "cubic"; then echo "cubic";
    elif echo "$AVAILABLE" | grep -q "reno"; then echo "reno";
    elif echo "$AVAILABLE" | grep -q "bbr"; then echo "bbr";
    else echo "$AVAILABLE" | awk '{print $1}'; fi
}

show_status() {
    print_box_top
    print_box_row "HySpeed v$VERSION Status (ML-TCP)" "center"
    print_box_div

    # 检查模块状态
    if lsmod | grep -q hyspeed; then
        print_kv_row "Module Status" "${GREEN}Loaded${NC}"

        REF_COUNT=$(lsmod | grep hyspeed | awk '{print $3}')
        print_kv_row "Reference Count" "${CYAN}$REF_COUNT${NC}"

        # 修复：确保 ACTIVE_CONNS 只是一个数字，没有换行
        ACTIVE_CONNS=$(ss -tin 2>/dev/null | grep -c hyspeed || echo "0")
        # 去除可能的换行和空格
        ACTIVE_CONNS=$(echo $ACTIVE_CONNS | tr -d '\n' | tr -d ' ')
        print_kv_row "Active Connections" "${CYAN}$ACTIVE_CONNS${NC}"
    else
        print_kv_row "Module Status" "${RED}○ Not Loaded${NC}"
        print_box_bottom
        return
    fi

    # 检查当前算法
    CURRENT=$(sysctl -n net.ipv4.tcp_congestion_control)
    if [[ "$CURRENT" == "hyspeed" ]]; then
        print_kv_row "Active Algorithm" "${GREEN}hyspeed${NC}"
    else
        print_kv_row "Active Algorithm" "${YELLOW}$CURRENT${NC}"
    fi

    print_box_div
    print_box_row "Current Parameters" "center"
    print_box_div

    if [[ -d /sys/module/hyspeed/parameters ]]; then
        for param in hyspeed_rate hyspeed_min_cwnd hyspeed_max_cwnd hyspeed_beta \
                     hyspeed_turbo hyspeed_safe_mode hyspeed_fast_alpha hyspeed_fast_gamma \
                     hyspeed_fast_ss_exit hyspeed_hd_enable hyspeed_hd_thresh_us \
                     hyspeed_hd_ref_us hyspeed_hd_gamma_boost hyspeed_hd_alpha_boost \
                     hyspeed_brave_enable hyspeed_brave_rtt_pct hyspeed_brave_hold_ms \
                     hyspeed_brave_floor_pct hyspeed_brave_push_pct \
                     hyspeed_rtt_filter_enable hyspeed_rtt_noise_pct hyspeed_rtt_trend_pct; do

            param_file="/sys/module/hyspeed/parameters/$param"
            if [[ -f "$param_file" ]]; then
                value=$(cat $param_file 2>/dev/null)
                case $param in
                    hyspeed_rate)
                        formatted=$(format_bytes $value)
                        bps=$(format_bps $value)
                        print_kv_row "Global Rate Limit" "$formatted ($bps)"
                        ;;
                    hyspeed_beta)
                        beta_val=$((value * 100 / 1024))
                        print_kv_row "Fairness (Beta)" "${beta_val}%"
                        ;;
                    hyspeed_min_cwnd)
                        print_kv_row "Min CWND" "$value packets"
                        ;;
                    hyspeed_max_cwnd)
                        print_kv_row "Max CWND" "$value packets"
                        ;;
                    hyspeed_turbo)
                        if [[ "$value" == "Y" ]] || [[ "$value" == "1" ]]; then
                            print_kv_row "Turbo Mode" "${YELLOW}Enabled ⚡${NC}"
                        else
                            print_kv_row "Turbo Mode" "Disabled"
                        fi
                        ;;
                    hyspeed_safe_mode)
                        if [[ "$value" == "Y" ]] || [[ "$value" == "1" ]]; then
                            print_kv_row "Safe Mode" "${GREEN}Enabled${NC}"
                        else
                            print_kv_row "Safe Mode" "Disabled"
                        fi
                        ;;
                    hyspeed_fast_alpha)
                        print_kv_row "FAST Alpha" "$value packets"
                        ;;
                    hyspeed_fast_gamma)
                        print_kv_row "FAST Gamma" "${value}%"
                        ;;
                    hyspeed_fast_ss_exit)
                        print_kv_row "SS Exit Threshold" "${value}%"
                        ;;
                    hyspeed_hd_enable)
                        if [[ "$value" == "Y" ]] || [[ "$value" == "1" ]]; then
                            print_kv_row "High-Delay Mode" "${GREEN}Enabled${NC}"
                        else
                            print_kv_row "High-Delay Mode" "Disabled"
                        fi
                        ;;
                    hyspeed_hd_thresh_us)
                        print_kv_row "HD Threshold" "${value}us"
                        ;;
                    hyspeed_hd_ref_us)
                        print_kv_row "HD Reference RTT" "${value}us"
                        ;;
                    hyspeed_hd_gamma_boost)
                        print_kv_row "HD Gamma Boost" "${value}%"
                        ;;
                    hyspeed_hd_alpha_boost)
                        print_kv_row "HD Alpha Boost" "$value packets"
                        ;;
                    hyspeed_brave_enable)
                        if [[ "$value" == "Y" ]] || [[ "$value" == "1" ]]; then
                            print_kv_row "Brave Mode" "${GREEN}Enabled${NC}"
                        else
                            print_kv_row "Brave Mode" "Disabled"
                        fi
                        ;;
                    hyspeed_brave_rtt_pct)
                        print_kv_row "Brave RTT Tolerance" "${value}%"
                        ;;
                    hyspeed_brave_hold_ms)
                        print_kv_row "Brave Hold Time" "${value}ms"
                        ;;
                    hyspeed_brave_floor_pct)
                        print_kv_row "Brave Floor" "${value}%"
                        ;;
                    hyspeed_brave_push_pct)
                        print_kv_row "Brave Push" "${value}%"
                        ;;
                    hyspeed_rtt_filter_enable)
                        if [[ "$value" == "Y" ]] || [[ "$value" == "1" ]]; then
                            print_kv_row "RTT Filter" "${GREEN}Enabled${NC}"
                        else
                            print_kv_row "RTT Filter" "Disabled"
                        fi
                        ;;
                    hyspeed_rtt_noise_pct)
                        print_kv_row "RTT Noise Threshold" "${value}%"
                        ;;
                    hyspeed_rtt_trend_pct)
                        print_kv_row "RTT Trend Threshold" "${value}%"
                        ;;
                esac
            fi
        done
    fi
    print_box_bottom
}

apply_preset() {
    PRESET=$2

    # 模拟设置参数 (实际写入 sysfs)
    set_val() {
        echo $2 > /sys/module/hyspeed/parameters/$1 2>/dev/null
    }

    print_box_top
    print_box_row "Applying Preset: $PRESET" "center"
    print_box_div

    case $PRESET in
        conservative)
            set_val hyspeed_rate 125000000
            set_val hyspeed_min_cwnd 16
            set_val hyspeed_max_cwnd 15000
            set_val hyspeed_beta 717
            set_val hyspeed_turbo 0
            set_val hyspeed_safe_mode 1
            set_val hyspeed_fast_alpha 15
            set_val hyspeed_fast_gamma 40
            set_val hyspeed_fast_ss_exit 20
            set_val hyspeed_hd_enable 1
            set_val hyspeed_brave_enable 1
            set_val hyspeed_brave_rtt_pct 20
            set_val hyspeed_rtt_filter_enable 1
            set_val hyspeed_rtt_noise_pct 12
            set_val hyspeed_rtt_trend_pct 6
            print_box_row "Applied: Conservative (1Gbps, Safe)" "left"
            ;;
        balanced)
            set_val hyspeed_rate 256000000
            set_val hyspeed_min_cwnd 16
            set_val hyspeed_max_cwnd 15000
            set_val hyspeed_beta 616
            set_val hyspeed_turbo 0
            set_val hyspeed_safe_mode 1
            set_val hyspeed_fast_alpha 20
            set_val hyspeed_fast_gamma 50
            set_val hyspeed_fast_ss_exit 25
            set_val hyspeed_hd_enable 1
            set_val hyspeed_brave_enable 1
            set_val hyspeed_brave_rtt_pct 25
            set_val hyspeed_rtt_filter_enable 1
            set_val hyspeed_rtt_noise_pct 15
            set_val hyspeed_rtt_trend_pct 8
            print_box_row "Applied: Balanced (2.5Gbps, FAST)" "left"
            ;;
        aggressive)
            set_val hyspeed_rate 500000000
            set_val hyspeed_min_cwnd 16
            set_val hyspeed_max_cwnd 20000
            set_val hyspeed_beta 512
            set_val hyspeed_turbo 0
            set_val hyspeed_safe_mode 1
            set_val hyspeed_fast_alpha 30
            set_val hyspeed_fast_gamma 60
            set_val hyspeed_fast_ss_exit 30
            set_val hyspeed_hd_enable 1
            set_val hyspeed_hd_gamma_boost 30
            set_val hyspeed_brave_enable 1
            set_val hyspeed_brave_push_pct 12
            set_val hyspeed_rtt_filter_enable 1
            set_val hyspeed_rtt_noise_pct 20
            set_val hyspeed_rtt_trend_pct 10
            print_box_row "Applied: Aggressive (4Gbps, High Push)" "left"
            ;;
        *)
            print_box_row "Unknown preset: $PRESET" "left" "${RED}"
            print_box_div
            print_box_row "Available: conservative, balanced, aggressive" "left"
            print_box_bottom
            exit 1
            ;;
    esac
    print_box_bottom
}

set_param() {
    PARAM=$2
    VALUE=$3
    if [[ -z "$PARAM" ]] || [[ -z "$VALUE" ]]; then
        print_box_top
        print_box_row "Parameter Set Error" "center" "${RED}"
        print_box_div
        print_box_row "Usage: hyspeed set <parameter> <value>" "left"
        print_box_row "Example: hyspeed set hyspeed_rate 125000000" "left"
        print_box_row "Example: hyspeed set hyspeed_min_cwnd 16" "left"
        print_box_row "Example: hyspeed set hyspeed_max_cwnd 15000" "left"
        print_box_row "Example: hyspeed set hyspeed_beta 616" "left"
        print_box_row "Example: hyspeed set hyspeed_fast_alpha 20" "left"
        print_box_row "Example: hyspeed set hyspeed_fast_gamma 50" "left"
        print_box_row "Example: hyspeed set hyspeed_turbo 1/0" "left"
        print_box_row "Example: hyspeed set hyspeed_safe_mode 1/0" "left"
        print_box_row "Example: hyspeed set hyspeed_brave_enable 1/0" "left"
        print_box_bottom
        exit 1
    fi

    PARAM_FILE="/sys/module/hyspeed/parameters/$PARAM"
    if [[ -f "$PARAM_FILE" ]]; then
        echo $VALUE > $PARAM_FILE 2>/dev/null || {
             echo -e "${RED}Error setting value${NC}"; exit 1;
        }
        print_box_top "${GREEN}"
        print_box_row "Parameter Updated" "center" "${GREEN}"
        print_box_div "${GREEN}"
        print_kv_row "$PARAM" "$VALUE" "${GREEN}"
        print_box_bottom "${GREEN}"
    else
        echo -e "${RED}Parameter not found${NC}"
    fi
}

case "$ACTION" in
    start)
        modprobe hyspeed 2>/dev/null || insmod $INSTALL_DIR/hyspeed.ko
        sysctl -w net.ipv4.tcp_congestion_control=hyspeed >/dev/null
        print_box_top "${GREEN}"
        print_box_row "HySpeed Started" "center" "${GREEN}"
        print_box_bottom "${GREEN}"
        ;;
    stop)
        DEFAULT_ALGO=$(get_default_congestion_control)
        sysctl -w net.ipv4.tcp_congestion_control=$DEFAULT_ALGO >/dev/null 2>&1
        rmmod hyspeed 2>/dev/null
        print_box_top "${YELLOW}"
        print_box_row "HySpeed Stopped" "center" "${YELLOW}"
        print_kv_row "Current Algo" "$DEFAULT_ALGO" "${YELLOW}"
        print_box_bottom "${YELLOW}"
        ;;
    restart)
        $0 stop
        sleep 1
        $0 start
        ;;
    status)
        show_status
        ;;
    preset)
        apply_preset $@
        ;;
    set)
        set_param $@
        ;;
    log|logs)
        print_box_top
        print_box_row "Kernel Logs (Last 10)" "center"
        print_box_bottom
        dmesg | grep -i hyspeed | tail -10
        ;;
    monitor)
        echo -e "${CYAN}Monitoring logs (Ctrl+C to stop)...${NC}"
        dmesg -w | grep --color=always -i hyspeed
        ;;
    uninstall)
        print_box_top "${MAGENTA}"
        print_box_row "HySpeed v$VERSION Uninstaller" "center" "${MAGENTA}"
        print_box_div "${MAGENTA}"

        # 停止算法
        DEFAULT_ALGO=$(get_default_congestion_control)
        print_box_row "Switching to $DEFAULT_ALGO..." "left" "${MAGENTA}"
        sysctl -w net.ipv4.tcp_congestion_control=$DEFAULT_ALGO >/dev/null 2>&1

        # 尝试卸载模块
        if rmmod hyspeed 2>/dev/null; then
            print_kv_row "Module Unload" "${GREEN}Success${NC}" "${MAGENTA}"
        else
            print_kv_row "Module Unload" "${YELLOW}In Use${NC}" "${MAGENTA}"
            print_box_div "${MAGENTA}"
            print_box_row "${YELLOW}Module is still loaded in memory ${NC}" "center" "${MAGENTA}"
            print_box_row "${YELLOW}Active connections are preventing unload${NC}" "center" "${MAGENTA}"
            print_box_row "${RED}Clean everything after reboot${NC}" "center" "${MAGENTA}"
            print_box_div "${MAGENTA}"
        fi

        # 删除文件
        print_box_row "Removing files..." "left" "${MAGENTA}"
        rm -rf $INSTALL_DIR
        rm -f /etc/modules-load.d/hyspeed.conf
        rm -f /lib/modules/$(uname -r)/extra/hyspeed.ko
        depmod -a
        sed -i '/net.ipv4.tcp_congestion_control=hyspeed/d' /etc/sysctl.conf

        print_kv_row "Config Files" "${GREEN}Removed${NC}" "${MAGENTA}"
        print_kv_row "Startup Scripts" "${GREEN}Removed${NC}" "${MAGENTA}"

        # 最终检查 - 修复这里的对齐问题
        print_box_div "${MAGENTA}"
        if lsmod | grep -q hyspeed; then
            # 内嵌的重启提示框
            print_box_row "" "center" "${MAGENTA}"
            print_box_row "${RED} ${NC}" "center" "${MAGENTA}"
            print_box_row "${RED}REBOOT REQUIRED{NC}" "center" "${MAGENTA}"
            print_box_row "${RED}Module will be completely removed${NC}" "center" "${MAGENTA}"
            print_box_row "${RED}after system reboot.${NC}" "center" "${MAGENTA}"
            print_box_row "" "center" "${MAGENTA}"
        else
            print_box_row "${GREEN}✅ HySpeed Completely Uninstalled!${NC}" "center" "${MAGENTA}"
        fi
        print_box_bottom "${MAGENTA}"

        # 删除自己
        rm -f /usr/local/bin/hyspeed
        rm -f /etc/hyspeed/config.conf
        rm -f /etc/systemd/system/hyspeed-config.service
        systemctl daemon-reload 2>/dev/null || true
        ;;
    save)
        CONFIG_DIR="/etc/hyspeed"
        CONFIG_FILE="$CONFIG_DIR/config.conf"

        mkdir -p "$CONFIG_DIR"

        print_box_top "${GREEN}"
        print_box_row "Saving HySpeed Configuration" "center" "${GREEN}"
        print_box_div "${GREEN}"

        if [[ ! -d /sys/module/hyspeed/parameters ]]; then
            print_box_row "${RED}Error: Module not loaded${NC}" "center" "${GREEN}"
            print_box_bottom "${GREEN}"
            exit 1
        fi

        # 保存所有参数到配置文件
        echo "# HySpeed Configuration" > "$CONFIG_FILE"
        echo "# Generated: $(date '+%Y-%m-%d %H:%M:%S')" >> "$CONFIG_FILE"
        echo "" >> "$CONFIG_FILE"

        for param in hyspeed_rate hyspeed_min_cwnd hyspeed_max_cwnd hyspeed_beta \
                     hyspeed_turbo hyspeed_safe_mode hyspeed_fast_alpha hyspeed_fast_gamma \
                     hyspeed_fast_ss_exit hyspeed_hd_enable hyspeed_hd_thresh_us \
                     hyspeed_hd_ref_us hyspeed_hd_gamma_boost hyspeed_hd_alpha_boost \
                     hyspeed_brave_enable hyspeed_brave_rtt_pct hyspeed_brave_hold_ms \
                     hyspeed_brave_floor_pct hyspeed_brave_push_pct \
                     hyspeed_rtt_filter_enable hyspeed_rtt_noise_pct hyspeed_rtt_trend_pct; do
            param_file="/sys/module/hyspeed/parameters/$param"
            if [[ -f "$param_file" ]]; then
                value=$(cat "$param_file" 2>/dev/null)
                echo "$param=$value" >> "$CONFIG_FILE"
            fi
        done

        print_kv_row "Config File" "$CONFIG_FILE" "${GREEN}"

        # 创建 systemd 服务以在启动时恢复配置
        cat > /etc/systemd/system/hyspeed-config.service << 'SERVICE_EOF'
[Unit]
Description=HySpeed Configuration Restore
After=network.target
ConditionPathExists=/sys/module/hyspeed/parameters

[Service]
Type=oneshot
ExecStart=/usr/local/bin/hyspeed load
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
SERVICE_EOF

        systemctl daemon-reload
        systemctl enable hyspeed-config.service 2>/dev/null

        print_kv_row "Systemd Service" "Enabled" "${GREEN}"
        print_box_div "${GREEN}"
        print_box_row "Config will be restored on reboot" "center" "${GREEN}"
        print_box_bottom "${GREEN}"
        ;;
    load)
        CONFIG_FILE="/etc/hyspeed/config.conf"

        print_box_top "${CYAN}"
        print_box_row "Loading HySpeed Configuration" "center" "${CYAN}"
        print_box_div "${CYAN}"

        if [[ ! -f "$CONFIG_FILE" ]]; then
            print_box_row "${RED}Error: Config file not found${NC}" "center" "${CYAN}"
            print_box_row "Run 'hyspeed save' first" "center" "${CYAN}"
            print_box_bottom "${CYAN}"
            exit 1
        fi

        if [[ ! -d /sys/module/hyspeed/parameters ]]; then
            print_box_row "${RED}Error: Module not loaded${NC}" "center" "${CYAN}"
            print_box_bottom "${CYAN}"
            exit 1
        fi

        # 从配置文件加载参数
        load_count=0
        while IFS='=' read -r param value; do
            # 跳过注释和空行
            [[ "$param" =~ ^#.*$ ]] && continue
            [[ -z "$param" ]] && continue

            param_file="/sys/module/hyspeed/parameters/$param"
            if [[ -f "$param_file" ]]; then
                echo "$value" > "$param_file" 2>/dev/null && load_count=$((load_count + 1))
            fi
        done < "$CONFIG_FILE"

        print_kv_row "Parameters Loaded" "$load_count" "${CYAN}"
        print_box_bottom "${CYAN}"
        ;;
    *)
        print_box_top
        print_box_row "HySpeed v$VERSION Management" "center"
        print_box_div
        print_kv_row "start" "Start HySpeed"
        print_kv_row "stop" "Stop HySpeed"
        print_kv_row "restart" "Restart HySpeed"
        print_kv_row "status" "Check Status"
        print_kv_row "preset [name]" "Apply Preset"
        print_kv_row "set [k] [v]" "Set Parameter"
        print_kv_row "save" "Save Config (persist reboot)"
        print_kv_row "load" "Load Saved Config"
        print_kv_row "monitor" "Live Logs"
        print_kv_row "uninstall" "Remove Completely"
        print_box_div
        print_box_row "Presets: conservative, balanced, aggressive" "left"
        print_box_bottom
        exit 1
        ;;
esac
SCRIPT_EOF

    chmod +x /usr/local/bin/hyspeed
    log_success "Management script created at /usr/local/bin/hyspeed"
}

print_kv_row() {
    local key="$1"
    local val="$2"
    local color="${3:-$CYAN}"

    local key_width=$(get_width "$key")
    local val_width=$(get_width "$val")

    # 左右各1空格Padding + 中间
    local available=$((BOX_WIDTH - 4))
    local padding=$((available - key_width - val_width))

    [ $padding -lt 1 ] && padding=1

    echo -ne "${color}║${NC} $key"
    repeat_char " " $padding
    echo -e "$val ${color}║${NC}"
}

# ================= 结尾显示 =================
show_info() {
    echo ""
    print_box_top "${GREEN}"
    print_box_row "HySpeed v$VERSION Installation Complete!" "center" "${GREEN}"
    print_box_row "ML-TCP Auto-Scaling Edition" "center" "${GREEN}"
    print_box_bottom "${GREEN}"

    echo ""

    # 调用新生成的脚本显示状态
    /usr/local/bin/hyspeed status

    echo ""
    print_box_top "${YELLOW}"
    print_box_row "Recommended Settings" "center" "${YELLOW}"
    print_box_div "${YELLOW}"
    print_kv_row "VPS/Cloud (<=1Gbps)" "hyspeed preset conservative" "${YELLOW}"
    print_kv_row "VPS/Cloud (>1Gbps)" "hyspeed preset balanced" "${YELLOW}"
    print_box_bottom "${YELLOW}"
    echo ""
}

error_exit() {
    log_error "$1"
    echo -e "${RED}Installation failed.${NC}"
    exit 1
}

# ================= 主流程 =================
main() {
    clear
    print_banner

    echo -e "${CYAN}Starting installation at $CURRENT_TIME${NC}"
    echo ""

    check_root || error_exit "Root check failed"
    check_system || error_exit "System check failed"
    install_dependencies || error_exit "Dependency installation failed"
    download_source || error_exit "Source download failed"
    compile_module || error_exit "Module compilation failed"
    load_module || error_exit "Module loading failed"
    create_management_script || error_exit "Script creation failed"

    show_info

    echo "[$(date '+%Y-%m-%d %H:%M:%S')] HySpeed v$VERSION installed by $CURRENT_USER" >> /var/log/hyspeed_install.log
}

main
