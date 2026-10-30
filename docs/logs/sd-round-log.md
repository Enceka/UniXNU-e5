# NativeSd 上板轮记录（stage2/native-sd）

## 2026-10-30 kernel 263 / v1（uefi-report-191023.txt）
冷路径 legacy 3.0V 识别：CMD0 完成后 CMD8 `st 000c8101 r7 00000000` ——
无卡响应，超时错误。host 寄存器在上电时被复位（inherit 条件
`(clk&5)==5` 不成立），Android 的 SDR104 配置不可热继承。eMMC 回归全绿。

## 2026-10-30 kernel 264 / v2（uefi-report-193901.txt）
按 vendor `sdhci_sprd_fast_hotplug_enable` 补齐 rmldo(0xba0 bit0)、
0x520（protect bit20 + debounce-en bit0 + count 清零 + polar bit21 清零，
经 APB-prot 0xff0 窗口写）。写入全部落地（hotplug 0→0x00100001、
rmldo 0→1，二次探测保持），但卡仍无响应。决定性证据：PSTATE(0x24)
全 0 —— CMD/DAT 线电平位全低 + CINS=0（对照 eMMC 主机 0x01f000f0），
即卡与上拉都无电。结论：Android 关机时经 regulator 框架关闭
vddsdcore/vddsdio，LK/UEFI 从不打开；rmldo/hotplug 只是插拔检测管道。
同轮修正：pstate 诊断寄存器 0x20→0x24（0x20 是 Buffer Data Port）；
0x29 power 写读回 0、放弃（mainline 从不写）。eMMC 回归全绿。

## 2026-10-30 kernel 265 / v3（commit f30d63d，进行中）
驱动内直接走 ADI（`0x400000`，qogirn6pro/r5px：FIFO drain(0x30 bit10) +
从机窗 `0x420000+X` 写 + retire 等待(0x30 bit31 清)，读走 0x28 RD_CMD /
0x2c RD_DATA busy 轮询）。序列：`0x23d0=0x6e7f` 解锁电源域写 →
vmmc=LDO_VDDSDCORE VOL(0x21b0)=0xb4 (3.0V) + PD(0x21ac) 清 bit0 →
1ms → vqmmc=LDO_VDDSDIO VOL(0x21bc)=0xb4 + PD(0x21b8) 清 bit0 → 20ms，
然后原 hotplug 序列、legacy 3.0V 识别。新增日志行：
`pmic prot/sdcore pd vol/sdio pd vol`（前）、`pmic after ... pstate`（后）。
验收 `e5-xnu tools/e5-sd-check.py` 已扩展 LDO 状态检查（pd bit0 清、
vol==b4）。单元测试：ldoVolUv 3.0V=0xb4、3.3V=0xd2、1.2V=0、3.75V=0xff。

## 2026-10-30 kernel 265 / v3（uefi-report-200141.txt）——内核 panic
首个 ADI 读触发同步外部中止（ESR 0x96000010，DFSC 0x10 外部中止，
far = ADI 映射页 + 0x2c = REG_ADI_RD_DATA 读取；x8=ADI 页基址、
x9=0x42223d0 即 pmicRead(0x23d0) 的 RD_CMD 值），panic 于
pc 0xfffffff007985440。eMMC 回归因 panic 中断（非 eMMC 自身退化）。
Android 回退正常（adb ~330s 回来）。根因：**aon 门控 adi_eb**
（vendor ums9620-clk.c：状态 0x64900008 bit9，SC set 0x64901008，
clear 0x64902008）。ADI 的 DT 节点没有 clocks —— Linux 从不管理它；
正常开机 LK 保持它开启，但 Android 的 sprd watchdog 是 ADI 消费者，
其 shutdown 回调在 `adb reboot` 路径上把 ADI 时钟关掉（与 SD LDO 被
regulator shutdown 关闭同类）。既有 ap-ahb 0x20001000 bit0（sdio0_eb）
与 aon 0x6490113c bits2,3 写均为 SC set-only 语义，无副作用。

## v4（kernel 266）
冷路径在 PMIC 序列前使能 adi_eb：读 0x64900008 → set 0x64901008
bit9 → 回读校验，拒绝则干净退出（log "adi gate enable refused"，
不再碰死总线）。adi 映射从 0x40 拓宽到 0x1000（如需读 0xff0 prot
状态）。验收 checker 增加 adi gate 行（bit9 必须置位）。

## 2026-10-30 kernel 266 / v4（uefi-report-202502.txt）——同点位再次 panic，新根因链
v4 的 adi_eb 门控使能通过了回读校验（未走"refused"干净退出路径），随后首个
ADI 读仍在 pmicRead(0x23d0) 的 REG_ADI_RD_DATA load 上外部中止（pc
0xfffffff007985480，far=ADI 页+0x2c，x9=0x42223d0）→ **adi_eb 单独不足**。
panic 后 pstore dmesg-ramoops-0 只剩 panic 文本，NativeSd 探针行被环形覆盖，
无法看到 gate 行的具体数值（但逻辑上必然 g1 bit9=1）。

### 证据链（全部来自 Android 侧 + vendor 源码，无板端轮次）
- Android clk_summary（debugfs 挂载后可读）：**adi 有两个时钟**——`adi-eb`
  （aon gate 0x64900008 bit9，26M APB 门控）与 `adi`（**功能时钟 MUX**，
  ums9620_aonapb_clk[] 基址 0x64920000，reg 0x34 bits[2:0]；parents：
  0 rco-100m-4m / 1 ext-26m / 2 rco-100m-25m / 3 tgpll-38m4 / 4 tgpll-51m2；
  Android 当前选 2 = 25MHz）。
- vendor ums9620-clk.c 复位表（挂在 aon-gate desc，基址 0x64900000）：
  **RESET_AON_APB_ADI_SOFT_RST = reg 0x14 BIT(9)**（同寄存器 bit29 AP_WDG、
  bit30 APCPU_WDG）；drivers/clk/sprd/reset.c：SC 复位 assert 写
  base+reg+0x1000，deassert 写 base+reg+0x2000，状态在 base+reg。
  被按住复位的 IP 不响应读 → 外部中止，与 3/4 轮现象完全一致。
- aon-apb-prot DT：sprd,lock_but_write_int 存在 → 锁定只拦写，读放行
  （且我们在 v2 已实测 aon 0xff0 prot status=0），APB 保护排除。
- watchdog shutdown 只是 adi_eb 的引用计数归零线索，不是充分解释。

### v5（kernel 267）
冷路径顺序：adi_eb 门控（v4）→ **ADI 软复位脉冲**（assert 0x64901014
bit9 → 50µs → deassert 0x64902014 bit9，回读 0x14 bit9 必须 0）→
**adi 功能 mux 强制 index 2**（0x64920034 = (old&~7)|2，RCO 25M 常开源）
→ PMIC LDO 序列（v3/v4）。aon 映射 0x2000→0x3000 以覆盖 0x64902014
deassert 窗口。checker 增加 adi rst / adi mux 行校验与 stuck 失败标记。

## 2026-10-30 kernel 267 / v5（uefi-report-204701.txt）——第三次同点位 panic，但这次之后找到了真根因
v5 复位脉冲 + mux 强制全部通过校验（否则会走干净退出），首个 ADI 读仍在外部
中止（pc 0xfffffff0079854fc，far=ADI 页+0x2c，x9=0x4223d0 = 0x420000+0x23d0，
kernel build Fri Oct 30 12:38:41 UTC = 新 267）。gate/reset/mux/prot 四条理论
全部排除后，转向总线拓扑本身。

### 真根因：ADI 基址错了（五轮 panic 同一根因）
`/soc/aon` 是 simple-bus，**ranges = <0x0 0x64000000 0x1016000>** —— 子节点
单元地址是**窗口内偏移**，物理地址 = 0x64000000 + 偏移。Android platform
设备名证实：`64400000.spi`（ADI 控制器）、`641e0000.watchdog`、
`64580000.apb-bm`、`64a10000.hwspinlock`。而 v3 把 `spi@400000` 的单元地址
当成了物理地址，映射 0x400000/0x420000 —— 都是不可访问空洞：读外部中止
（DFSC 0x10），写静默完成。x9=0x4223d0 也与 v3 代码 `0x420000u+off` 完全
吻合。gate/mux/reset 操作的是 aon syscon（0x64900000，地址正确）所以全部
"通过"，却与 ADI 本体无关。
其余旁证：apb-bm@580000（sprd,apb-busmonitor）纯观测单元（记录 hang/match
事件并中断/panic，不 fence 总线）；clk_summary 里 `adi`/`adi-eb` enable 计数
为 0 而框架并未真关门 —— 地址正确时无需这些门控。

### v6（commit 738de42，kernel 268）
adi.open(0x64400000, 0x1000)；adiSlave.open(0x64600000, 0x8000)（slave
窗口 = 控制器+0x20000，vendor spi-sprd-adi.c channel_offset 0x20000）；
pmicRead 的 RD_CMD 写**物理从机地址** 0x64600000+off。gate/rst/mux 序列
保留（无害且 checker 已依赖）。其余不变。

## 2026-10-30 kernel 268 / v6（uefi-report-211xxx.txt）——地址修复生效：无 panic，PMIC 真实可读
v6 把 ADI 基址改为 0x64400000 后：**不再 panic**，XNU 正常到 shell，eMMC 回归全绿，
NativeSd 两次探针均干净走完冷路径。真实 PMIC 读数回来了：
`adi gate a7ffff8f->a7ffff8f`（bit9 本来就置位）、`adi rst 0->0`、`adi mux 2->2`
（本来就选 rco-25m）——v4/v5 的门控/复位/mux 全是"本来就好"的空转。
`pmic prot 8000 sdcore pd 0001 vol 00b4 sdio pd 0001 vol 00b4`：两路 LDO
VOL 已是 3.0V 码值，但 PD=1（掉电）→ 与第 2 轮"卡没电"结论一致。
**但 5 次 pmicWrite 全部异常**：日志里 5 条 `E5SERRSKIP`（本内核的延迟 SError
跳过处理器）与 5 次写一一对应，`pmic after` 显示 PD/VOL/pstate 全没变。
存储走 fire-and-forget → 写入未映射物理孔洞以延迟中止返回。

### v6 仍然错了一处算术：slave 窗口
channel_offset=0x20000，窗口 = 0x64400000+0x20000 = **0x64420000**（DT reg
跨度 0x64400000..0x64500000 内）。v6 误映射 0x64600000（相当于 +0x220000，
在 reg 范围外 → 孔洞 → 写 SError）。读没受影响的原因：RD_CMD 的数值被 ADI
控制器按 17 位掩码解码（0x646023d0 & 0x1FFFF = 0x23d0，恰好对）。

### v7（commit 97a0c67，kernel 269）
adiSlave.open(0x64420000, 0x8000)；pmicRead RD_CMD = 0x64420000+off。
预期：PMIC 写落地（unlock 0x23d0=0x6e7f → VOL 已 0xb4 → PD 清零），
随后 pstate 出现 CINS/CMD 电平，CMD8 应答 r7=0x1aa。
