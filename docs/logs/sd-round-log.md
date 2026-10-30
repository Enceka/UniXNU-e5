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

## Round 7 (kernel 269, v7) — PMIC writes land, card powered, CMD8 CRC

Report: newest dumps/uefi-report-*.txt (after -210905); full job log in the
session spill. Both probes:

- Probe 1: `pmic prot 8000 sdcore pd 0001 vol 00b4 sdio pd 0001 vol 00b4`
  → writes OK (no E5SERRSKIP) → `pmic after sdcore pd 0000 vol 00b4 sdio
  pd 0000 vol 00b4` = both LDOs ENABLED at 3.0V. CMD0 st 00000001 clk 1107.
  CMD8 st 00018001 r7 00000000, fail line err 0001 = COMMAND CRC ERROR
  (rounds 1-6 were err 000c timeout) — the bus is alive, something drives CMD.
- Probe 2 (IOKit rematch): `inherit hc1 00 hc2 3000 clk 0000 power 00
  reset 08 ps 01f00000` — PSTATE 0x01f00000 = CMD+DAT line levels HIGH =
  card powered & idling (eMMC host: 0x01F000F0). Rail state persisted
  (`pmic prot 8000 sdcore pd 0000 vol 00b4`), hotplug/rmldo persisted.
  CMD0 ok; CMD8 same CRC error.
- Diagnostic quirk: our `pstate %04x` printed r16(0x24) = low half only,
  always 0 (line bits are 20-24); the inherit `ps %08x` r32 print showed
  the truth. Fixed in v8: all pstate prints are r32 %08x.

Decode: card has power + pull-ups and sees our clock, but the CMD8 echo
CRCs corrupt. Probe 1 fired CMD8 right after the 20ms LDO ramp while lines
were still low (probe 2, seconds later, shows them high → ramp > 20ms);
we sent a single CMD0 (Linux mmc_go_idle sends CMD0, 1ms, CMD0) and had no
CMD8 retry.

## v8 (commit 486fef6, kernel 270) — timing + retries

1. after vqmmc enable: poll r32(0x24) until (& 0x01F00000) != 0, up to
   1000 x 1ms; log `lines ready pstate %08x tries %u`; continue on timeout.
2. pmic-after / hotplug / fail-line pstate prints → r32 %08x (checker
   regex `pstate (\w+)` still matches).
3. after setClock(400000): IODelay(10ms), double CMD0 (CMD0, 2ms, CMD0),
   log the second (grammar unchanged).
4. CMD8 retry loop ≤10 x 5ms on failure; intermediate failures log
   `CMD8 try %u st %08x r7 %08x err %08x`; final outcome keeps the
   existing `CMD8 st … r7 …` / `CMD8 fail …` lines. No checker change.

## Round 8 (kernel 270, v8) — timing fixed, CMD8 CRC deterministic

Report: newest dumps/uefi-report-*.txt. Both probes:

- Probe 2 (full): `pmic prot 8000 sdcore pd 0000 vol 00b4 sdio pd 0000
  vol 00b4` (rails on from probe 1), `lines ready pstate 01f00000 tries 0`
  — lines were already high, so the >20ms ramp theory is dead. Hotplug
  persisted, pstate 01f00000 everywhere. CMD0 st 00000001 clk 1107.
- CMD8: `try 1..9` + final, ALL st 00018001 r7 00000000 err 0001 =
  command CRC error, identical to the bit. 10/10 deterministic.
- eMMC regression green; no panic; clean boot to shell.

Decode: a CRC error (not timeout) means the card DID drive CMD in the
response window; the host clocked in 48 bits and the CRC7 mismatched
every time. With power, pull-ups, 382kHz clock, double CMD0 and 50ms of
settle all verified, the remaining suspect is response sampling phase:
0x208 CMD_DLY_INV/POSRD_DLY_INV (we set both for <=400kHz like the
vendor) or the actual SDCLK being other than we compute (mux parent).

## v9 (commit b8b0018, kernel 271) — discriminator + matrix

On CMD8 retry exhaustion:
1. CMD55 (R1, CRC-checked): if it also CRCs → response path globally
   corrupt (phase/clock), not CMD8-specific. Logged `CMD55 st/r1/err`.
2. Matrix 3 cfgs x 3 tries: noinv (invert bits off @400k), clk200
   (invert on @200k), noinv-clk200. Win logs `CMD8 win <cfg>` and init
   continues under the winning config (ACMD41 → … unchanged).
3. All fail → `CMD55b` repeat + standard `CMD8 fail` line, return false.
Standard grammar lines (`CMD8 st … r7 …`, `CMD8 fail …`) preserved; no
checker change needed.

## Round 9 (kernel 271, v9) — all matrix configs negative; Linux rebind bisect

Both probes identical: CMD8 tries 1-9 st 00018001 r7 00000000 err 0001;
**CMD55 st 00018001 r1 00000000 err 0001** — every response command
CRCs with the response register suppressed → response-path corruption
is global, not CMD8-specific. Matrix noinv / clk200 / noinv-clk200: all
identical → DLL_DLY_OFFSET inversion (0x208 bits 13,21) and clock rate
(382k vs 200k) both irrelevant. CMD55b after matrix: same.

Android-side bisect (decisive): `echo 22210000.sdio >
.../sdhci_sprd_r11/unbind; bind` under Linux re-initializes the card
warm (regulators off→on, 400k 1-bit legacy init, re-tune) with **zero**
errors — straight back to SDR104, same RCA 5048, same tuning result.
So the card, pads, pinmux (DT has none for this node — pins are
MUX UNCLAIMED under Android), pull-ups and warm-power-cycle behavior
are all fine; the gap is purely our register sequence vs sdhci core.

Vendor source facts (sdhci-sprd.c): probe order is clocks →
`sdhci_sprd_init_config` (0x28c |= DLL_BAK|DLL_VAL bits 0,1) →
sdhci_setup_host (sdhci core does SDHCI_RESET_ALL in sdhci_init) →
power up (vmmc on, 200us, vqmmc on via signal_voltage_on_off) →
voltage_switch (vqmmc set + `sdhci_reset(CMD|DAT)` — vendor comment:
"Otherwise the controller will not work normally. We had ever
encoutered CMD2 timeout before") → 400k clock → CMD0. We did
everything except RESET_ALL and the 0x28c debounce bits; the CMD|DAT
reset after vqmmc was already present (resetLines).

## v10 (commit 97f5091, kernel 272) — RESET_ALL + DLL backup mode

At the head of the cold path (before clock config): write 0x2f = 0x09
(RESET_ALL bit0 self-clearing | Unisoc card-hw-reset bit3), poll bit0
clear, then `0x28c |= 3`, log `host reset %02x->%02x deb %08x`, fail
`host reset stuck` if bit0 never clears. Rationale: the controller
carries SDR104-era sampler state across the warm reboot; RESET_ALL is
the only thing that restores the command/response sampler to defaults.
Checker (e5-xnu commit pending): requires the new line (after bit0
clear, deb bits 0,1 set) + new fail marker; also split clean() into
cmdClean (command statuses: error bits clear + CC, no TC requirement —
fixes a latent bug that would have failed a real successful probe) and
clean (data transfers: CC+TC).

Live-DT extras recorded: sprd,phy-delay-legacy = <0 0 0 0> (our
0x204=0 write matches DT); highspeed 7f 7f c0 c0, sdr50 7f 7f 4d 4d,
sdr104 7f a3 b2 b2. pinctrl: 642e0000.pinctrl, SD0 pins 121-126
(CMD/D_0/D_1/CLK/D_2/D_3), SD1 pins 65-70, plus *_MISC sets — all
MUX UNCLAIMED/GPIO UNCLAIMED under Android.

## Round 11 (kernel 273 + FD rebuild) — DXE abort, host apb gated at UEFI

v11 (commit 8451500): cold path stops rewriting the sdio0 clock chain;
observes mux 0x280 / 2x-div 0x284 / 1x 0x288 and derives sourceHz
(E5Sd::sdio0SourceHz). Live Android regmap proved the vendor clk table
is offset -0xC from silicon: real chain is mux@0x280=4 (rpll 390M),
2x@0x284=0 (÷1), 1x@0x288=1 → 195 MHz 1x; the LK-era 0x280/84/88
addresses were right all along, so rounds 1-10 really ran ext-26m
13 MHz and the chain was never the bug. Linux re-bind keeps the rpll
chain untouched.

First FD rebuild since baseline-flash59 added an E5-SD residue dump to
E5BringupDxe (host 0x22210000 caps/dll/clk, clock chain, aon gates).
Board result: **Synchronous Exception at DXE, FAR 0x22210040** — the
SD host apb is GATED OFF at UEFI time (Android shutdown clears
sdio0_eb; nothing in LK→UEFI re-enables it) and a read of gated apb
aborts with no DXE recovery → boot hang → slot B exhausted → Android.
No NativeSd run. Lesson: never read potentially-gated MMIO in DXE
without enabling the gate first. Side discovery: at XNU the host
emerges from the apb gate as fresh-reset (inherit hc1 00 clk 0000),
i.e. our XNU starting state differs from Linux's proven warm state by
a full apb-gate cycle.

## v12 (commit 6d4feba, kernel 274 + FD gate fix)

Two pieces: (1) E5BringupDxe writes the SC set window ap-ahb
0x20001000 bit0 (sdio0_eb, set-only, non-destructive) + spins on the
status readback before any 0x22210000 access, so the residue dump
survives; (2) NativeSd cold path pulses the vendor card hw_reset —
0x2f bit3 LOW 20us → HIGH, settle 500us (sdhci_sprd_hw_reset; Linux
mmc core calls hw_reset during every mmc_power_up). v10 wrote 0x09
(bit3 asserted forever, never pulsed). Checker (e5-xnu): cold mode
requires `card rst hh->hh pstate xxxxxxxx` with bit3 released.

## Round 12 / 12b (kernel 274) — DXE abort removed; v12 card-reset pulse negative

Round 12: the E5-SD DXE dump's own ap-ahb gate write (0x20001000) hit an
UNMAPPED page (BootShim MMIO map is partial) -> translation fault -> hang ->
slot exhausted. Removed the E5-SD block from E5BringupDxe (rounds 11-12
documented in the comment); FD rebuilt clean.

Round 12b (same kernel 274, clean FD): v12 vendor hw_reset pulse executed
(`card rst 08->08 pstate 01f00000`) — no effect: CMD8/CMD55/matrix all
identical (st 00018001, response register 0, err 0001). First v11 chain
observation landed: at XNU the chain is mux 1 (ext-26m) d2x 0 d1x 1 ->
13 MHz source, divider 0907 — clock source again exonerated. CINS (PSTATE
bit16) still 0 with lines high.

## v13 — card-detect polarity matrix (the DT decode was wrong)

`cd-gpios` raw bytes `00 00 01 8e | 00 00 00 23 | 00 00 00 00`:
phandle 0x18e = /soc/aon/gpio@2000c0 (eic-sync, sprd,qogirn6lite-eic-sync),
gpio NUMBER 0x23 = 35 (gpio-339 "cd", reads hi = card present, active-high),
flags 0x00. Earlier "flags 0x23 active-low" was a misdecode: 0x23 is the pin
number. So `detect_gpio_polar = flags = 0` -> FALSE -> the vendor
fast_hotplug_enable() takes the ELSE branch and SETS the aon 0x520 bit21
detect polarity. Our hotplugValue() CLEARS it — inverted on this board.

With the controller's detect inverted, the card (pin high) reads ABSENT:
CINS=0 every powered round. The Linux-proven state has CINS=1, and the
fast-hotplug machinery plausibly gates the DAT/CMD response path while it
believes no card is inserted — exactly our signature (CC on every command,
responses never sampled: CRC error + zero response register).

v13 sweeps three aon 0x520 states after CMD0, CMD8 in each: `vendor`
(polar clear — current, wrong), `pinv` (polar SET — Linux-proven), `none`
(fast-hotplug off). First state whose CMD8 echoes r7 0x1aa wins and init
continues under it. Kernel 275, CI run 41080389067.

## Round 13 (kernel 275) — SUCCESS: the card is read natively

The polarity theory held. With aon 0x520 bit21 SET (`cdpol pinv` state)
CMD8 echoed immediately: `cdpol pinv hp 00300001 cins 0 pstate 01f00000
cmd8 st 00000001 r7 000001aa`. The `cdpol none` probe that followed also
echoed — the effect of the corrected polarity is sticky in the controller
(and PSTATE CINS itself never moved; the gate was the fast-hotplug detect,
not the CINS status bit). The matrix adopted `pinv` and the full legacy
init completed for the first time:

    ACMD41 st 00000001 ocr c0ff8000 tries 35
    CMD3 st 00000001 r6 50480500 rca 5048        (same RCA as Android)
    CSD 400e0032 5b590000 735f7f80 0a400000 sectors 30244864
    selected st 00000001 r1 00000900 rca 5048
    reference lba 0/1/2048/2050 st 00000003 crc f9e85b82/b2aa7578/b2aa7578/c1117912
    reference 4/4 repeat 16/16 mode legacy-4bit-13MHz
    probe Success sectors 30244864 mode legacy-4bit-13MHz
    publishing read-only SD card 30244864 sectors

XNU probed the published media (three rw-1-block reads at LBA 0 from the
IOMedia stack) and the eMMC regression stayed fully green. Report
archived: e5-xnu dumps/uefi-report-233919.txt.

## v14 — final form (commit 104bf6e, kernel 276)

hotplugValue() now encodes the vendor else-branch directly:
`(cur | 0x00210001u) & ~0x000001feu` (polarity bit21 SET). The cdpol
matrix is removed; the standard `CMD8 st %08x r7 %08x` retry-loop grammar
returns. Unit tests cases 12-14 updated (hotplugValue(0) == 0x00210001).
Checker (e5-xnu) v14: the hotplug line must show bit21 set. Round 14 =
the clean-driver confirmation boot.

## Round 14 (kernel 276) — clean v14 confirms: polarity was the whole fix

No matrix, no diagnostics: the corrected hotplug value lands directly
(`hotplug 00000000->00210001`), and the card initializes on the first pass —
ACMD41 ocr c0ff8000 (35 tries), CMD3 rca 5048, CSD/sectors match, all four
reference CRCs green, 16 repeats clean, `probe Success sectors 30244864
mode legacy-4bit-13MHz`, media published read-only. IOMedia probed it
(3× rw). eMMC regression green. Report dumps/uefi-report-235704.txt.

Checker FAILed on a grammar artifact only: the `CMD8 st … r7 …` line lived
inside the failure branch, so a first-try success never printed it
(63775cc prints it on the clean path too; ace8448 removes a duplicated
brace my first edit left). Kernel 277 carries the fix; round 15 is the
final acceptance record.

## Round 15 (kernel 277) — FINAL: checker PASS

First-try CMD8 echo prints the standard grammar line
(`CMD8 st 00000001 r7 000001aa`), full init green, and
tools/e5-sd-check.py returns:

  PASS: SD 30244864 sectors via legacy-4bit-13MHz, reference 4/4 CRC
  match, repeat 16/16; eMMC v8 + E5EMMC mount + shell intact (EXITH 5/5)

Report dumps/uefi-report-001448.txt. SD bring-up objective achieved:
identity, capacity, and known-sector reads verified natively at low-speed
legacy mode, published read-only; eMMC regression green throughout.
Follow-ups for later rounds: throughput (SDR50/104 + DLL tuning, vendor
tuning procedure already captured in /tmp/sdhci-sprd-vendor.c), ext4
image block mapping, OS/SystemOS hookup.

## v15 — throughput: default-speed chain + multi-block experiment (branch stage2/sd-throughput)

Reads verified end-to-end at v14 (kernel 277, checker PASS on
dumps/uefi-report-001448.txt). v15 raises the ceiling in two guarded steps:

1. Clock: after the legacy verify passes (chain residue mux 1 ext-26m,
   1x -> 13MHz source, N=0 bypass -> 6.5MHz effective), switch the sdio0
   functional chain to Android's own runtime parent: mux 4 (rpll_390m),
   2x div 0, 1x enable -> 195MHz source, then default speed 24.375MHz
   (divider N=2, no bypass). The recheck runs under softVerify — a
   failure restores the ext-26m chain, re-verifies, and keeps the legacy
   mode; if even that fails the probe fails loud.
2. Multi-block: readSectors(lba, n<=16) does CMD18 (0x123a, TM 0x33 =
   multi|count|DMA|read) with the v4 32-bit block count and a manual
   CMD12 (0x0c1a) stop. Timed loops over 128 sequential sectors
   (LBA 16..143, inside the ext4 partition): single-block baseline, then
   8x16-sector CMD18 with first-sector CRC comparison per chunk.
   multiOk gates 16-sector chunking in doAsyncReadWrite (>=4 blocks
   remaining); otherwise the production path is byte-identical to v14.

Log grammar added: `chain rpll mux %u d2x %u d1x %u src %uHz`,
`tp single %u sectors in %llu us`, `tp multi %u sectors in %llu us
match %c`, plus fail lines `mread fail`, `mread stop fail`,
`mread mismatch`, `default-speed recheck failed, falling back`.
Mode string `default-4bit-24MHz` on success; `legacy-4bit-13MHz` on
fallback. Checker (e5-xnu d8488db) accepts both, requires the rpll line
(mux 4, src 195000000) and both tp lines in default mode; synthetic
validation 4/4. Kernel 278 = CI run 41089616546.

## Round 16 (kernel 278) — default speed + multi-block: 6x measured, checker PASS

Report dumps/uefi-report-003713.txt (e5-xnu side). Boot green, eMMC
regression green (E5EMMC mount-complete, INIT-START-v9, 5xEXITH|0,
SH|READY, READ-BLOCK).

- Legacy cold init green as always (CMD8 r7 1aa first try, ACMD41
  c0ff8000 tries 35, CMD3 rca 5048, CSD match, 4/4 reference CRCs,
  repeat 16/16 at legacy).
- **rpll chain switch engaged**: `chain rpll mux 4 d2x 0 d1x 1 src
  195000000Hz` — rpll was NOT gated at XNU (the fallback never ran).
- Default speed (24.375MHz, divider N=2 from the 195MHz source) verify:
  all 4 reference CRCs + 16 repeats green, `reference 4/4 repeat 16/16
  mode default-4bit-24MHz`.
- Timed loops: `tp single 128 sectors in 36207 us` (283us/sector,
  ~1.77 MiB/s — per-CMD17 overhead dominated) vs **`tp multi 128
  sectors in 5995 us match y`** (46.8us/sector, ~10.7 MiB/s) — 6.0x,
  all 8x16-sector CMD18 reads byte-identical to the single-block pass.
  multiOk=true, so doAsyncReadWrite now serves >=4-block requests in
  16-sector chunks.
- `probe Success sectors 30244864 mode default-4bit-24MHz`, publishing
  read-only SD card.
- Checker verdict: `PASS: SD 30244864 sectors via default-4bit-24MHz,
  reference 4/4 CRC match, repeat 16/16; eMMC v8 + E5EMMC mount + shell
  intact (EXITH 5/5)` (tools/e5-sd-check.py @ e5-xnu d8488db).

Throughput ladder so far: 6.5MHz single-block (v14) -> 24.375MHz
single-block (1.77 MiB/s) -> 24.375MHz 16-sector CMD18 (10.7 MiB/s).
Next rungs when needed: SDR50/SDR104 + DLL tuning (vendor
enable_phy_dll + tuning procedure), larger DMA than one page.

## Round 17 prep — ext4 read-side walk (branch stage2/sd-ext4)

Offline recon against the card over adb (dumps archived in docs/logs/sd-ext4/
+ sd-ext4-reference.json): the partition at LBA 2048 is an ext4 volume named
`e5-openwrt` (last_mounted `/newroot`) laid out like an OpenWrt rootfs —
4096B blocks, 32768 bpg, 8192 ipg, 116 groups. GDT at fs-block 1; gd0 places
the group-0 inode table at fs-block 145. Root inode 2 (itable+256) is a
4096-byte extent-mapped directory whose single extent points at fs-block
4241; that block holds 22 dirents (`.` `..` lost+found bin dev etc init lib
lib64 mnt opt overlay proc rom root sbin sys tmp usr var www run), CRC32
70633ae6. Image quirk: the superblock's first_ino/inode_size fields are
corrupt (0/11) — the real inode stride is 256 (proven from f30a placements),
so the walk passes the stride explicitly and never trusts those fields.

v16 (commit 23ceb15): pure parsers ext4ParseExtents()/ext4CountDirents() in
SdProtocol.h (unit-tested against the golden bytes, cases 27-31), driver walk
after the speed-mode verify via readSectors (CMD18 multi), soft-guarded.
Checker (e5-xnu d2a363e) requires the four ext4 evidence lines with the
golden values. CI kernel 279 dispatched (run 41143425911).
