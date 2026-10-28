# Xiaomi renoir（SM7350）Linux 主线移植报告

> 本报告对应仓库中的 `sm7350-renoir-port` 本地分支。除特别注明外，所有
> 构建与检查均在容器内完成；没有任何代码被推送到 remote。

## 1. 任务概述与结论

目标是在 Linux 主线 7.2.0 上加入 Qualcomm SM7350（Shima）平台和
Xiaomi Mi 11 Lite 5G（renoir）设备支持，并尽可能完成构建、DT schema
检查、QEMU 启动验证和移植记录。

本次实际完成的内容：

- 新增 SM7350 GCC 时钟控制器驱动和时钟 ID 绑定；
- 新增 SM7350 TLMM pinctrl 驱动和绑定；
- 为 RPMh clock、RPMh power domain、PDC、watchdog、SCM、CPUFREQ EPSS、
  UFS host 增加 SM7350 compatible；
- 新增 `sm7350.dtsi`、`sm7350-renoir.dts` 以及 DTB 构建入口；
- 新增 `renoir.config` 内核配置片段；
- GCC 交叉编译 `Image`、`dtbs`、`modules` 成功；
- `sm7350-renoir.dtb` 通过 `dtbs_check`，新增/修改 binding 通过
  `dt_binding_check`；
- QEMU `virt` 启动到用户态 shell 并执行命令、正常 poweroff；
- 所有改动以本地 commit 保存。

本次没有完成、也不能在容器内完成的内容：

- 没有任何真机验证。所有 UFS、USB、PMIC、TLMM、GCC、显示等硬件功能
  都只能标记为 `hardware-unverified`。
- 未移植 interconnect、DPU/DSI、GPU、摄像头、音频、WiFi/BT、触摸、
  NFC、充电、modem/ADSP/CDSP remoteproc、thermal zone、LLCC、SDHCI、PCIe。
- 未获得 mu-silicium UEFI 参考包，因此 UEFI 路线只做了文档层面的比较，
  未实际构建或验证。
- 没有移植 vendor 的 display/audio/camera techpack，也没有复制任何
  vendor firmware 或二进制。

因此，本次交付的准确定位是：**可编译、DT schema 干净、QEMU 可启动的
SM7350/renoir 主板级 bring-up 基线**，不是“已经能在真机上完整运行”的
移植。

## 2. 平台事实

- SM7350 是 Snapdragon 780G，平台代号 **Shima**，设备代号 **Renoir**。
- 对应机型：Xiaomi Mi 11 Lite 5G（小米 11 青春版 5G）。
- SM7350 **不是** Snapdragon 888；SM8350 才是 Snapdragon 888（Waipio），
  在本任务中只是参考平台。
- SM7325（Kodiak）与 SM7350 同代同家族，主线已有
  `sm7325.dtsi` / `kodiak.dtsi` / `sm7325-xiaomi-taoyao.dts`，本次大量
  参考了它们的主线写法。
- 本次基线是仓库当前的 Linux 7.2.0（`Makefile` 中 VERSION/PATCHLEVEL/
  SUBLEVEL = 7.2.0），主线原本没有 `sm7350`/`renoir` 支持。

## 3. 环境与工具链

- 容器：Ubuntu 26.04 LTS（resolute），x86_64，16 vCPU，30 GiB 内存。
- 交叉编译器：`aarch64-linux-gnu-gcc` 15.2.0（Ubuntu 15.2.0-16ubuntu1）。
- binutils：GNU ld 2.46。
- 设备树编译器：内核自带 `scripts/dtc`；系统 `dtc` 1.7.2。
- dtschema：2026.9；yamllint 随 dtschema venv 安装。
- QEMU：qemu-system-aarch64 10.2.1。
- 构建目录：`/var/tmp/renoir-build`、`/var/tmp/renoir-frag-build`。
- 日志与产物：`artifacts/`（已被 `.gitignore` 排除），以及
  `/var/tmp/renoir-work`。

构建命令（仓库根目录执行）：

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=/var/tmp/renoir-build defconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=/var/tmp/renoir-build \
     Image dtbs modules -j"$(nproc)"
```

使用配置片段时：

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=/var/tmp/renoir-frag-build defconfig
scripts/kconfig/merge_config.sh -m -O /var/tmp/renoir-frag-build \
    /var/tmp/renoir-frag-build/.config arch/arm64/configs/renoir.config
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=/var/tmp/renoir-frag-build \
    olddefconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=/var/tmp/renoir-frag-build \
    Image dtbs modules -j"$(nproc)"
```

## 4. 参考项目对比（实际挂载路径）

开始工作前枚举 `/data`，实际发现的项目如下。这些绝对路径只是本次容器内
的挂载事实，不写入任何仓库脚本；仓库自身一律使用相对路径。

| 实际路径 | 内容 | 本次使用方式 |
|---|---|---|
| `/data/sm8350` | Xiaomi 5.4.197 厂商内核（`MiCode/Xiaomi_Kernel_OpenSource`），含 `arch/arm64/boot/dts/vendor/qcom/shima*.dtsi`、`renoir-sm7350*.dts*`、`xiaomi-sm7350-common.dtsi` 以及 `gcc-shima.c`、`pinctrl-shima.c` 等 | **主要硬件事实来源**：读取 DTS 地址、GPIO、PMIC、UFS/USB、reserved-memory；读取 shima GCC/pinctrl 驱动表并翻译到主线框架 |
| `/data/renoir_kernel` | Xiaomi 5.4.61 内核源码，含 `arch/arm64/configs/vendor/renoir_*.config` | 只读参考。实际检查发现它**没有** renoir/Shima 的设备树，只有配置片段和 AMD Renoir GPU 驱动同名文件，因此没有作为 DTS 来源 |
| `/data/mipad-6sp` | Xiaomi Pad 6S Pro（SM8550/sheng）Linux 移植项目，`code002-2/Xiaomi-pad-6s-pro-Linux` | 只读方法参考：`sheng-kernel_build.sh` 的 `Image.gz-dtb` 拼接、`mkbootimg` 参数、`modules_install`、`ukify` 生成 `bootaa64.efi`，以及 Ubuntu 26 rootfs/debootstrap 流程。**未复制其 SM8550 DTS/驱动/配置，未执行其构建或刷机脚本** |
| `/data/sm7350` | 本次工作仓库，Linux 7.2.0，branch `sm7350-renoir-port` | 唯一写入的位置 |
| `/data/sm_7350` | 空目录，不是 git 仓库 | 未使用；本任务实际工作区是 `/data/sm7350` |
| `/data/flash/xiaomi11` | 未挂载 | 不存在，未使用 |
| mu-silicium | 未挂载 | 无法阅读 `renoirPkg` 或 `renoir.dts`；UEFI 路线只能基于通用知识记录 |

参考项目只读规则执行情况：

- 没有在 `/data/sm8350`、`/data/renoir_kernel`、`/data/mipad-6sp` 内
  创建、修改、删除文件；
- 没有执行 mipad 项目的 `sheng-kernel_build.sh`、rootfs 脚本、`mkbootimg`
  或任何刷机/分区命令；
- 没有复制 vendor firmware、rootfs、boot.img、密钥或二进制到仓库；
- 保留并注明了 vendor 源码原有版权与 SPDX 头。

## 5. 移植策略与差距矩阵

总原则：mainline-first，优先复用现有 QCOM 框架和 SM7325/SM8350/SC7280
的同类 IP 支持，不整棵搬 5.4 厂商树。

### 5.1 复用关系

- **GCC**：寄存器基址、时钟位和大部分父子关系与 SC7280/SM8350 家族一致，
  但时钟 ID、部分 branch/GDSC 与厂商表不同；因此新增独立的
  `gcc-sm7350.c`，并从厂商 `gcc-shima.c` 翻译时钟表，而不是简单加
  compatible。
- **TLMM**：SM7350 的 pin/function/group 表和 SC7280 不同（205 GPIO，
  基址 `0xf000000`，大小 `0x1000000`），因此新增独立
  `pinctrl-sm7350.c`。
- **UFS**：主线 `qcom,ufshc` 通用驱动 + `qcom,sm8350-qmp-ufs-phy`
  与厂商 `qcom,ufs-phy-qmp-v4-lahaina` 对应，作为可复用路径。
- **USB**：DWC3 使用 `qcom,sm8350-dwc3`/`qcom,dwc3` 包装器和
  `qcom,sm8350-usb-hs-phy`；QMP combo PHY 节点保留但默认 disabled。
- **PMIC**：PM8350/PM8350B/PM8350C/PMR735B/PMK8350 的 dtsi 和 RPMh
  regulator/SPMI 驱动主线已有，直接复用。
- **RPMh clock/PD**：厂商 shima RPMh clock 列表与 SC7280 相同，直接复用
  SC7280 描述；RPMhPD 资源集合与 SC7280 一致，新增 SM7350 描述表。
- **SCM/CPUFREQ EPSS/PDC/watchdog/GENI**：复用主线通用驱动，新增
  SoC compatible 或 fallback。

### 5.2 差距矩阵

| SoC 块 | 厂商是否有 | 主线原状态 | 本次处理 | 状态 |
|---|---|---|---|---|
| CPU/PSCI/GIC/timer | 有 | 通用支持 | 新建 DTS，去掉未验证的 PSCI idle states | build-verified |
| RPMh RSC | 有 | 通用支持 | 新建节点 + SM7350 PD/clock 表 | build-verified |
| GCC | 有 shima 驱动 | 无 SM7350 | 新驱动 + binding | build-verified |
| Interconnect | 有 shima ICC | 无 SM7350 | **未移植**，DTS 中未描述 ICC provider | blocked |
| TLMM | 有 shima 驱动 | 无 SM7350 | 新驱动 + binding | build-verified |
| SPMI/PMIC/regulator | 有 | PM8350 家族已支持 | 复用，板级只建 UFS/USB/key 所需 rail | build-verified |
| UFS + PHY | 有 | 通用 UFS + SM8350 QMP PHY | 复用，新增 sm7350-ufshc compatible | hardware-unverified |
| USB DWC3 + HS PHY | 有 | 通用 DWC3/HS PHY | 复用 SM8350/7nm 兼容串 | hardware-unverified |
| GENI UART/I2C/SPI | 有 | 通用 GENI | 新建 debug UART 与若干 I2C/SPI 节点 | build-verified |
| Display DPU/DSI/panel | 有 techpack | 无 Shima 专用支持 | 仅 `simple-framebuffer` | blocked |
| GPU/Adreno | 有 | 无 Shima 数据 | 未描述 | blocked |
| Camera/audio/modem | 有 techpack | 无 | 未移植 | blocked |
| WiFi/BT/NFC/touch/charger | 有厂商驱动 | 无板级支持 | 未描述 | blocked |
| Thermal/TSENS/LLCC | 有 | 无 Shima 数据 | 未描述 | blocked |
| SDHCI/PCIe | 有 | 通用支持 | 未描述（该机型未确认使用） | 未启用 |

## 6. 设备树移植明细

新增文件：

- `arch/arm64/boot/dts/qcom/sm7350.dtsi`
- `arch/arm64/boot/dts/qcom/sm7350-renoir.dts`
- `arch/arm64/boot/dts/qcom/Makefile` 增加 `sm7350-renoir.dtb`
- `Documentation/devicetree/bindings/arm/qcom.yaml` 增加
  `xiaomi,renoir` + `qcom,sm7350`

| 块 | 来源 | 主线实现 | 状态/备注 |
|---|---|---|---|
| CPU 拓扑 | 厂商 `shima.dtsi`：4×A55 + 3×A78 + 1×X1，容量值 1024/1946/1985 | `qcom,kryo` + `qcom,freq-domain` + 主线 cache 节点 | 未提供 OPP 表；cpufreq 可从 EPSS LUT 动态生成 |
| PSCI | 厂商 `arm,psci-1.0` | 主线 PSCI | 未移植厂商 `qcom,pm-cpu-level` 深度 idle；无 `arm,idle-state` |
| GIC | 厂商 `intc@17a00000` | `arm,gic-v3` | 地址与 GICR 大小按厂商 |
| PDC | 厂商 `qcom,shima-pdc` | `qcom,sm7350-pdc`, `qcom,pdc` + `qcom,pdc-ranges` | 使用厂商 ranges；新增 binding enum |
| arch timer / memtimer | 厂商 | `arm,armv8-timer` / `arm,armv7-timer-mem` | 中断号按厂商 |
| watchdog | 厂商 `qcom,msm-watchdog@17c10000` | `qcom,apss-wdt-sm7350`, `qcom,kpss-wdt` | 仅 build 验证；真机可能由 hyp/ABL 占用 |
| RPMh RSC | 厂商 `rsc@18200000` | `qcom,rpmh-rsc` | tcs-config 按厂商（CONTROL_TCS 0） |
| RPMhPD | 厂商 `rpmh-regulator-*` + 资源 | `qcom,sm7350-rpmhpd` | CX/CX_AO/EBI/GFX/MX/MX_AO/LMX/LCX/MSS |
| RPMh clock | 厂商 `qcom,shima-rpmh-clk` | `qcom,sm7350-rpmh-clk` | 复用 SC7280 clock 列表 |
| GCC | 厂商 `clock-controller@100000` | `qcom,gcc-sm7350` | 新驱动；UFS/USB GDSC 已建模 |
| TLMM | 厂商 `pinctrl@f000000` | `qcom,sm7350-tlmm` | 205 GPIO，uart5 引脚状态 |
| SPMI | 厂商 `spmi@c440000` | `qcom,spmi-pmic-arb` | 五个 PMIC dtsi 全部包含 |
| UFS host/PHY | 厂商 `ufshc@1d84000`、`ufsphy_mem@1d87000` | `qcom,sm7350-ufshc`,`qcom,ufshc`; `qcom,sm8350-qmp-ufs-phy` | 板级接 L7B/L9B/S10B/L10C/L6B；硬件未验证 |
| USB | 厂商 `ssusb@a600000` + HS PHY | `qcom,sm8350-dwc3`,`qcom,dwc3`; `qcom,sm8350-usb-hs-phy`,`qcom,usb-snps-hs-7nm-phy` | 仅 HS；QMP combo 节点 disabled |
| GENI | 厂商 `shima-qupv3.dtsi` | `qcom,geni-se-qup` + `qcom,geni-debug-uart` | 仅 uart5 默认；i2c1/spi2/i2c9/i2c13 节点 disabled |
| 显示 | 厂商 techpack | `simple-framebuffer` on `splash@e1000000` | 仅利用 bootloader splash；无 DPU/DSI |
| 按键 | 厂商 `pm8350_gpios` gpio6 | `gpio-keys` + `pm8350_gpios` pinctrl | build-verified |
| 保留内存 | 厂商 `shima.dtsi` | `reserved-memory` 子集 | 去掉重叠的 wlan 区和 shared-dma pools，避免 overlap |

未在 DTS 中保留的厂商属性包括：`qcom,msm-id`、`qcom,board-id`、
Android `firmware/android`、`qcom,ion`、`qcom,ipa-*`、`qcom,sde-*`、
`qcom,msm-bus-*`、`qcom,iommu-*`、`qcom,pm-qos-*` 等下游私有属性。

## 7. 驱动移植明细

| 驱动/文件 | 类型 | 说明 | 状态 |
|---|---|---|---|
| `drivers/clk/qcom/gcc-sm7350.c` | 新增 | 从厂商 `gcc-shima.c` 翻译；去掉下游 vdd-level，改用主线 parent_hws/safe_src_index 语义；新增 UFS/USB GDSC | build-verified |
| `include/dt-bindings/clock/qcom,gcc-sm7350.h` | 新增 | 厂商时钟 ID，保留 SPDX/版权 | build-verified |
| `drivers/pinctrl/qcom/pinctrl-sm7350.c` | 新增 | 从厂商 `pinctrl-shima.c` 翻译到 `struct pingroup`/`struct pinfunction`；去掉下游 qup/wake 扩展 | build-verified |
| `drivers/clk/qcom/clk-rpmh.c` | 修改 | 新 compatible 指向 SC7280 RPMh 描述 | build-verified |
| `drivers/pmdomain/qcom/rpmhpd.c` | 修改 | 新增 SM7350 PD 表和 compatible | build-verified |
| `drivers/clk/qcom/Kconfig` / `Makefile` | 修改 | `SM_GCC_7350` | build-verified |
| `drivers/pinctrl/qcom/Kconfig.msm` / `Makefile` | 修改 | `PINCTRL_SM7350` | build-verified |
| `Documentation/devicetree/bindings/clock/qcom,gcc-sm7350.yaml` | 新增 | GCC binding，8 个外部时钟 | dt_binding_check 通过 |
| `Documentation/devicetree/bindings/pinctrl/qcom,sm7350-tlmm.yaml` | 新增 | 205 GPIO、183 个 function | dt_binding_check 通过 |

复用的既有主线驱动/绑定：

- `qcom,scm`、`qcom,rpmh-rsc`、`qcom,spmi-pmic-arb`
- `qcom,pm8350-rpmh-regulators`、`qcom,pm8350c-rpmh-regulators`
- `qcom,geni-se-qup`、`qcom,geni-debug-uart`、`qcom,geni-i2c`、`qcom,geni-spi`
- `qcom,sm8350-dwc3`、`qcom,sm8350-usb-hs-phy`
- `qcom,sm7350-ufshc`（驱动通过 `qcom,ufshc` fallback 绑定）
- `qcom,sm8350-qmp-ufs-phy`
- `qcom,cpufreq-epss`、`qcom,kpss-wdt`、`qcom,pdc`

## 8. 内核配置与模块

选择 **配置片段** 方案：`arch/arm64/configs/renoir.config`，而不是完整
`renoir_defconfig`。原因是当前主线 defconfig 会随版本频繁变化，片段更
容易审阅和合并；同时保留 defconfig 中 QEMU/virtio/PL011 等选项，便于
QEMU 验证。

片段内容：

- `CONFIG_ARCH_QCOM=y`
- `CONFIG_SM_GCC_7350=y`
- `CONFIG_PINCTRL_SM7350=y`
- `CONFIG_QCOM_CLK_RPMH=y`
- `CONFIG_QCOM_RPMHPD=y`
- `CONFIG_QCOM_GDSC=y`
- `CONFIG_QCOM_SMD_RPM=y`
- `CONFIG_QCOM_SMEM=y`
- `CONFIG_QCOM_SCM=y`
- `CONFIG_QCOM_PDC=y`
- `CONFIG_ARM_QCOM_CPUFREQ_HW=y`
- `CONFIG_SCSI_UFSHCD=y`
- `CONFIG_SCSI_UFSHCD_PLATFORM=y`
- `CONFIG_PHY_QCOM_QMP=y`
- `CONFIG_PHY_QCOM_QMP_UFS=y`
- `CONFIG_USB_DWC3=y`
- `CONFIG_USB_DWC3_QCOM=y`
- `CONFIG_PHY_QCOM_USB_SNPS_FEMTO_V2=y`
- `CONFIG_REGULATOR_QCOM_RPMH=y`
- `CONFIG_QCOM_WDT=y`

注意：`PHY_QCOM_QMP_COMBO` 依赖 `TYPEC`/`DRM` 的 tristate，默认仍为
module；本次 USB 只启用 HS PHY，不依赖该模块。若后续要在 initramfs
阶段使用 USB3/DP，需要同时把 `TYPEC`/`DRM` 调成 built-in 或把模块放入
initramfs。

模块安装命令（`<rootfs>` 替换为目标 rootfs 挂载点）：

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- O=/var/tmp/renoir-build     INSTALL_MOD_PATH=<rootfs> modules_install
```

不要把 `.config`、`Image`、`.dtb`、模块或 initramfs 提交进仓库；本仓库
只提交 `renoir.config` 片段。

## 9. 构建与测试结果

### 9.1 基线构建

- defconfig 未修改代码构建：成功（`artifacts/baseline-build.log`）。
- 工具链、配置生成、`Image dtbs modules` 全部通过。

### 9.2 移植后构建

- `Image`、`dtbs`、`modules` 全量构建成功（`artifacts/build.log`）。
- 产物：`Image` 约 41 MiB；`sm7350-renoir.dtb` 约 23 KiB。
- 新增 `gcc-sm7350.o`、`pinctrl-sm7350.o` 均编译进内核。
- 使用 `renoir.config` 片段的全量 `Image dtbs modules` 构建同样成功
  （`artifacts/fragment-build.log`）。

### 9.3 DT schema 检查

```sh
PATH=/opt/dtschema-venv/bin:$PATH make ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- O=/var/tmp/renoir-build \
  CHECK_DTBS=y qcom/sm7350-renoir.dtb
```

结果：无输出、exit 0（clean）。日志 `artifacts/dtbs-check.log`。

`dt_binding_check` 针对新增/修改 binding 运行，exit 0。日志
`artifacts/dt-binding-check.log`。

### 9.4 checkpatch

- 单独文件检查：`gcc-sm7350.c` 为 0 errors / 0 warnings / 0 checks；
  `pinctrl-sm7350.c` 仅剩上游同类 pinctrl 驱动也会出现的宏参数
  `id`/`offset` 复用提示，无 error/warning。
- `W=1` 单独构建新增驱动无 warning，日志 `artifacts/w1-build.log`。
- 整个补丁序列（`git format-patch --stdout master..HEAD`）：0 errors、
  23 warnings。其余 warning 主要是把多个 patch 一次性交给
  `checkpatch.pl` 时的已知误报（“Duplicate signature”、“DT binding
  docs and includes should be a separate patch”），以及厂商 GPL-2.0-only
  绑定头文件被要求双许可证的提示。原 vendor 头文件未改许可证。

### 9.5 QEMU arm64 启动

使用主线 defconfig + 本次改动构建的 `Image`，QEMU `virt` 机型，
静态 arm64 busybox initramfs：

```sh
qemu-system-aarch64 -M virt -cpu cortex-a72 -smp 4 -m 2048 \
  -kernel /var/tmp/renoir-build/arch/arm64/boot/Image \
  -initrd /var/tmp/renoir-work/initramfs.cpio.gz \
  -append "console=ttyAMA0 rdinit=/init panic=-1" \
  -nographic -no-reboot
```

日志 `artifacts/qemu-boot.log` 显示：

- 内核正常启动，`Run /init as init process`；
- `QEMU_SHELL_READY`；
- 用户态 shell 执行 `uname -a` 得到
  `Linux (none) 7.2.0-... aarch64 GNU/Linux`；
- 执行 `cat /proc/version`、`ls /`；
- `poweroff -f` 后 `reboot: Power down` 正常退出。

**注意**：QEMU `virt` 不能模拟 SM7350/renoir 硬件；本次 QEMU 验证的是
“内核自身 + 通用 arm64 用户态启动链路”，**不是** renoir DTB 的功能验证。
没有把 `sm7350-renoir.dtb` 传给 QEMU `virt`。

### 9.6 验证矩阵

| 对象 | 等级 | 证据 |
|---|---|---|
| GCC/pinctrl 驱动编译 | build-verified | 完整内核构建 |
| `sm7350-renoir.dtb` 生成 | build-verified | DTC 成功 |
| DT schema / binding | build-verified | dtbs_check / dt_binding_check clean |
| arm64 内核启动到用户态 | qemu-verified | QEMU 日志 |
| UFS 读写/根文件系统 | hardware-unverified | 无真机 |
| USB host/peripheral | hardware-unverified | 无真机 |
| PMIC regulator/GPIO/RTC | hardware-unverified | 无真机 |
| TLMM GPIO/中断 | hardware-unverified | 无真机 |
| 显示/触摸/按键 | hardware-unverified | 只有 simple-framebuffer 和按键 DTS |
| WiFi/BT/NFC/音频/相机/modem | not implemented | 见已知限制 |

## 10. 遇到的问题与解决方案

1. **工作区路径与提示词不一致**
   - 现象：提示词写 `/data/sm7350`，另一处写 `/data/sm_7350`；
     `/data/sm_7350` 实际为空目录。
   - 处理：以实际 git 仓库 `/data/sm7350` 为工作区，分支
     `sm7350-renoir-port`；未改动空目录。

2. **容器初始没有 git/gcc/make 等工具**
   - 处理：`apt-get` 安装 build-essential、交叉 gcc、QEMU、dtc、
     dtschema venv 等。没有执行来源不明的安装脚本。

3. **5.4 厂商 GCC 驱动与 7.2 主线 API 不兼容**
   - 现象：`vdd-level.h` 不存在、`struct clk_rcg2.enable_safe_config`
     不存在、`CLK_DONT_HOLD_STATE` 不存在、
     `clk_branch2_hw_ctl_ops`/`clk_branch2_force_off_ops` 不存在、
     `qcom_cc_sync_state` 不存在、`qcom_cc_really_probe()` 参数类型变化。
   - 处理：删除下游 vdd_data/regulator 投票；使用主线
     `CLK_ALPHA_PLL_TYPE_LUCID` + 5LPE ops；把 hw_ctl/force_off 改为
     `clk_branch2_ops`；删除 sync_state；使用 `&pdev->dev`；补齐
     `platform_device.h`/`of.h`。

4. **5.4 厂商 pinctrl 驱动与主线结构体不兼容**
   - 现象：主线使用 `struct pingroup`/`struct pinfunction`，没有
     `.wake_reg`、`.wake_bit`、`.qup_regs`，也没有
     `msm_gpio_mpm_wake_set()`。
   - 处理：重写 PINGROUP/SDC/UFS 宏为 `PINCTRL_PINGROUP()`，删除
     wake/qup 扩展，删除额外 probe 回调；保留 pin/function/group 数据。

5. **`dtbs_check` 报 GCC 不匹配**
   - 根因：DTS 使用 `qcom,sm7350-gcc`，binding 使用
     `qcom,gcc-sm7350`，两者命名不一致。
   - 处理：统一为 `qcom,gcc-sm7350`（主线命名习惯）。

6. **`dtbs_check` 其余告警**
   - SCM：需要 SoC compatible + `qcom,scm` fallback；
   - RPMh RSC：binding 要求 `power-domains`，补充最小 PSCI
     `cluster_pd`；
   - CPUFREQ：需要 `qcom,sm7350-cpufreq-epss`, `qcom,cpufreq-epss`；
   - UFS PHY：主线 binding 不接受 `vdda-phy-max-microamp`/
     `vdda-pll-max-microamp`，从板级 DTS 删除；
   - USB HS PHY：femto-v2 通用 compatible 不在 binding 允许组合中，
     改用 `qcom,sm8350-usb-hs-phy`, `qcom,usb-snps-hs-7nm-phy`。

7. **修改 Kconfig 后 make 卡住**
   - 现象：新增 `SM_GCC_7350` 等符号后，旧 `.config` 触发交互式
     oldconfig，后台 make 无 stdin 时挂起。
   - 处理：先 `make olddefconfig` 再构建；QEMU/构建脚本不使用交互式
     `oldconfig`。

8. **busybox 交叉编译失败**
   - 现象：busybox 1.36.1 的 `networking/tc.c` 使用已被新内核头删除的
     `TCA_CBQ_*`。
   - 处理：关闭 `CONFIG_TC` applet，重新静态编译 arm64 busybox。

9. **QEMU 串口输入丢失**
   - 现象：一次性重定向命令文件时，命令在 shell 就绪前被消费。
   - 处理：使用 FIFO 保持 QEMU stdin，等待 `QEMU_SHELL_READY` 后再写入
     命令，成功得到 shell 输出和 poweroff 日志。

## 11. 已知限制、未完成项与固件依赖

### 11.1 明确未验证

- 所有 renoir 硬件功能均未在真机验证。
- `sm7350-renoir.dtb` 只通过 DTC/schema；没有在真机或 QEMU 上绑定
  SM7350 设备。
- UFS 根文件系统、USB、PMIC、TLMM、按键、simple-framebuffer 都可能
  在真机上需要额外修正。

### 11.2 未移植的块

- interconnect（GCC/UFS/GENI 未做带宽投票；可能导致性能下降或某些
  高带宽场景不稳定，需真机确认）；
- Display DPU/DSI/panel/backlight；
- GPU/Adreno + zap；
- Camera（V4L2/CSI）、audio（LPASS/WSA/WCD）、modem/ADSP/CDSP；
- WiFi/BT（QCA6490/CNSS）、NFC、触摸、充电/电量计、传感器；
- TSENS/thermal zones、LLCC、SDHCI、PCIe、ramoops。

### 11.3 固件与分区依赖

- 本次仓库不包含任何 firmware。
- 真机启动通常还依赖 bootloader/XBL、UFS 分区表、`dtbo`/AVB 策略以及
  厂商分区中的 ADSP/CDSP/modem/WLAN/GPU zap 固件；这些未在容器内验证。
- 若使用 Android boot.img 路线，需要设备已解锁并具备可写 boot 分区；
  本容器没有 fastboot/adb，也没有执行任何刷机操作。

### 11.4 其他

- `renoir.config` 片段不改变主线 defconfig 的 module 策略；UFS PHY
  已 built-in，COMBO PHY 仍为 module。
- CPU idle states 未移植，功耗会高于 Android/vendor 内核。
- 没有提供 `renoir_defconfig`，只提供片段，原因见第 8 节。

## 12. 引导路径对比与发行版启动指南

### 12.1 Android boot.img + dtbo

- 打包：`Image.gz` 与 `sm7350-renoir.dtb` 拼接为 `Image.gz-dtb`，
  用 `mkbootimg` 生成 boot.img；cmdline 中包含
  `console=ttyMSM0,115200n8`、`root=PARTLABEL=...` 或 initramfs 参数。
- 优点：离现有 Android bootloader/分区最近；mipad 项目验证过
  `mkbootimg --base/--kernel_offset/--tags_offset/--pagesize` 参数组合；
  不需要额外 UEFI 固件。
- 缺点：受 Android boot image 头、dtbo/AVB、分区布局限制；DTB 追加
  方式需要 bootloader 支持；多发行版切换不如 UEFI 灵活。
- 本仓库不执行刷机，只给出方法。

### 12.2 UEFI（mu-silicium renoirPkg）

- 思路：使用 Project Silicium 的 renoir UEFI 包启动通用 EFI 载荷
  （EFI stub、GRUB、systemd-boot），DTB 可由 UEFI 提供或由引导器追加。
- 优点：更接近 PC/发行版启动模型，内核/initramfs/DTB 更新方便；
  可以绕开 Android boot header 和 AVB。
- 缺点：需要先移植/刷入 UEFI；本次容器未挂载 mu-silicium，无法读取
  `renoirPkg` 或 `renoir.dts`，因此未验证。

### 12.3 EFI 镜像（ukify）

- 思路：`ukify build` 把 `Image`、initramfs、cmdline 打成一个
  `bootaa64.efi`，由 UEFI 直接启动；mipad 项目使用过该方式。
- 优点：单文件、发行版友好、可带 initramfs；适合长期维护。
- 缺点：依赖 UEFI；cmdline/DTB 需要正确配置；本次未实际构建
  `bootaa64.efi`。

### 12.4 推荐

- **真机 bring-up 阶段**：优先 Android boot.img + `Image.gz-dtb` +
  initramfs，因为小米设备已有可用 bootloader，且 mipad 项目的
  `mkbootimg` 参数可直接借鉴（只读参考，不执行其脚本）。示意命令：

  ```sh
  gzip -c arch/arm64/boot/Image > Image.gz
  cat Image.gz arch/arm64/boot/dts/qcom/sm7350-renoir.dtb > Image.gz-dtb
  mkbootimg --kernel Image.gz-dtb \
      --cmdline "console=ttyMSM0,115200n8 root=PARTLABEL=linux rw" \
      --base 0x00000000 --kernel_offset 0x00008000 \
      --tags_offset 0x01e00000 --pagesize 4096 -o boot_renoir.img
  ```

  `base/kernel_offset/tags_offset/pagesize` 必须按 renoir 的 bootloader
  实际要求核对；上例来自 mipad 项目的 SM8550 经验，**不是已验证的
  renoir 参数**。UFS root 也可以改用 initramfs + `root=UUID=...`。
- **长期发行版维护**：如果 mu-silicium renoir UEFI 可用，优先 UEFI +
  EFI stub/ukify；它对 rootfs、内核和 DTB 的更新更友好。
- 无论哪条路线，首次启动都建议使用 initramfs 提供 UFS 驱动/模块，
  再切换到 UFS root，避免 `root=` 与内置驱动不匹配。

## 13. 本地提交列表

功能提交（`sm7350-renoir-port` 分支）：

- `2dc099c5b` dt-bindings: clock: qcom: document SM7350 GCC
- `347c48ba6` clk: qcom: add SM7350 GCC support
- `dcd3e89e8` dt-bindings: pinctrl: qcom: document SM7350 TLMM
- `c7bf639b8` pinctrl: qcom: add SM7350 TLMM support
- `2495ccfe8` clk: qcom: rpmh: add SM7350 RPMh clock controller
- `ff413a63c` pmdomain: qcom: rpmhpd: add SM7350 power domains
- `fcf7c5b99` dt-bindings: qcom: add SM7350 platform compatibles
- `f573aaab8` arm64: dts: qcom: add SM7350 SoC and Renoir board
- `f8d2f1c89` arm64: configs: add SM7350 Renoir config fragment

本报告本身以 `docs: porting: add SM7350 Renoir report` 提交。
分支基于原 `master` 的 `2709dd5ae`，未 push、未修改 remote。

## 14. 上游化建议

- **GCC 与 pinctrl**：当前版本是“下游表 + 主线框架”的可工作移植，
  上游化前应按时钟块/引脚组拆分 patch，补齐每块的 datasheet/下游来源
  说明，确认时钟 ID 和 GDSC 与最新 binding 一致；大表提交需要
  `git format-patch` 分块。
- **RPMh clock**：直接复用 SC7280 描述是本次的工程折衷；上游审查可能
  要求独立 `clk_rpmh_sm7350` 表，即使资源名相同。
- **DTS**：应先补 interconnect、TSENS、LLCC、SDHCI、PCIe，再补显示/
  GPU/音频；当前 DTS 不应被当作完整 renoir 上游 DTS。
- **UFS/USB PHY 复用**：`sm8350-qmp-ufs-phy` 与
  `sm8350-usb-hs-phy` 是基于厂商 `qmp-v4-lahaina`/`snps-femto` 的
  合理推断，上游化前必须用真机确认 PHY 序列和电源。
- **可上游的独立部分**：GCC/pinctrl binding、SM7350 SoC DTS 骨架、
  RPMhPD 表、qcom.yaml 条目；板级 regulator/display/触摸等应等真机
  验证后再提交。
- **下游专用**：任何依赖 Android boot.img 布局、vendor firmware、
  techpack 的代码不应进入主线。

## 15. 参考链接与来源

- Linux 主线仓库：https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
- linux-arm-msm 邮件列表：https://lore.kernel.org/linux-arm-msm/
- postmarketOS：https://postmarketos.org/
- 厂商内核参考（本次实际挂载）：
  - Xiaomi Kernel OpenSource 5.4.197：https://github.com/MiCode/Xiaomi_Kernel_OpenSource.git
  - Xiaomi Pad 6S Pro 项目：https://github.com/code002-2/Xiaomi-pad-6s-pro-Linux.git
- 本地厂商 DTS/驱动来源（只读）：
  - `arch/arm64/boot/dts/vendor/qcom/shima.dtsi`
  - `arch/arm64/boot/dts/vendor/qcom/shima-pinctrl.dtsi`
  - `arch/arm64/boot/dts/vendor/qcom/shima-pm.dtsi`
  - `arch/arm64/boot/dts/vendor/qcom/renoir-sm7350.dtsi`
  - `arch/arm64/boot/dts/vendor/qcom/xiaomi-sm7350-common.dtsi`
  - `drivers/clk/qcom/gcc-shima.c`
  - `drivers/pinctrl/qcom/pinctrl-shima.c`
- 主线参考 DTS：
  - `arch/arm64/boot/dts/qcom/sm7325.dtsi`
  - `arch/arm64/boot/dts/qcom/kodiak.dtsi`
  - `arch/arm64/boot/dts/qcom/sm7325-xiaomi-taoyao.dts`
  - `arch/arm64/boot/dts/qcom/sm8350.dtsi`

## 16. 安全与工作区声明

- 未 push 到 origin/upstream 或任何 remote；
- 未修改任何 remote URL；
- 未在参考仓库内写入；
- 未执行 fastboot/adb/edl/heimdall/TWRP/parted/dd 等刷机或分区操作；
- 未访问宿主路径、未逃逸容器；
- `/data/sm8350`、`/data/renoir_kernel`、`/data/mipad-6sp` 保持只读；
- 工作区中唯一的未跟踪目录 `renoir/` 是原有的厂商参考副本，已加入
  `.git/info/exclude`，未删除、未提交。
