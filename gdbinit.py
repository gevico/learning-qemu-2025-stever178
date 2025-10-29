import os
import gdb
import subprocess
import time

current_arch = gdb.execute("show architecture", to_string=True)

def extract_target():
    history_file = os.path.expanduser('~/.zsh_history')
    
    # 使用 strings 提取所有可读文本
    text = subprocess.run(
        ['strings', history_file], 
        capture_output=True, 
        text=True
    )
    lines = text.stdout.splitlines()
    
    result = ""
    for line in reversed(lines):
        if 'make -C' in line and 'gdbstub-' in line:
            result = line.split('gdbstub-', 1)[1].split()[0]
            break
    return result


if "riscv" in current_arch.lower():
    gdb.execute("printf \"=== RISC-V session set ===\\n\"")
    
    # 1. 架构和环境设置
    gdb.execute("set architecture riscv:rv64")

    target = extract_target()
    # print(f"Extracted target: {target}")
    
    # 2. 符号和源码设置
    if target and target != "":
        file_path = f"build/tests/gevico/tcg/riscv64-softmmu/test-{target}"
        if os.path.exists(file_path):
            print(f"Loading symbols: {file_path}")
            gdb.execute(f"file {file_path}")
        else:
            print(f"Warning: Symbol file not found: {file_path}")
    gdb.execute("directory tests/gevico/tcg/riscv64/")

    # 3. 远程协议配置
    gdb.execute("set remote kill-packet off")
    gdb.execute("set remotetimeout 15")  # 稍长的超时时间
    gdb.execute("set verbose off")       # 连接时减少输出噪音

    # 4. 连接远程目标
    gdb.execute("set mi-async on") # 在等待目标响应时继续接受用户命令
    # gdb.execute("set non-stop on") # 当某个线程遇到断点时，其他线程继续运行

    time.sleep(0.1)  # 确保GDB完全就绪
    gdb.execute("target remote :1234")
    gdb.execute("set verbose on")        # 恢复详细输出

    # 5. 连接后设置断点和显示
    gdb.execute("b *0x80000000")
    gdb.execute("display $pc")
    
    # 6. others
    if_code = """set $debug_enable = 0
    if $debug_enable
        printf "pre: mtvec = 0x%lx\\n", $mtvec
        set $mtvec=0x1000
        printf "now: mtvec = 0x%lx\\n", $mtvec
    end
    """
    gdb.execute(if_code)
    
    gdb.execute("printf \"=== RISC-V session ready ===\\n\"")

else:
    gdb.execute("printf \"=== x86 session ready ===\\n\"")
