# STM32F407ZGT6 + CLRC66303HNY (CLRC663 plus) RFID/NFC 读卡工程

> **工程名称**：`STM32F407ZGT6-CLRC66303HNY`  
> **固件版本**：`v1.4.2 [Release: 2026-11-08]`  
> **MCU 平台**：STM32F407ZGT6 (ARM Cortex-M4 @ 168MHz)  
> **NFC 芯片**：NXP CLRC66303HNY (CLRC663 plus, Silicon Rev 03, VersionReg=0x1A)  
> **总线接口**：全双工 SPI1 (PA5-SCK, PA6-MISO, PA7-MOSI, PA4-CS)  
> **开发工具**：CMake + Ninja + GNU Arm Embedded Toolchain (arm-none-eabi-gcc 14.3.1)

---

## 一、版本迭代与修改历程 (Engineering Change Log)

经过多轮开发、示波器抓包与实物卡片兼容性测试，当前工程已完成以下核心修改与调优：

- **v1.0 (2026-10-18) - 基础硬件连通**
  - 完成 STM32F407ZGT6 与 CLRC663 HVQFN32 SPI1 接口配置。
  - 纠正 IFSEL 模式选择引脚（IFSEL1 接 3.3V，IFSEL0 接地锁定为 SPI 模式）。
  - 实现基于 PB5 的 PDOWN 硬件脉冲复位与微秒级精准 DWT 延时。

- **v1.1 (2026-10-26) - 协议栈载入与寻卡初探**
  - 配置 ISO14443-A 106 kbps 射频模拟寄存器参数（DrvModeReg 开场强 0x8E）。
  - 增加基础 REQA 轮询寻卡逻辑，初步调通 ATQA 应答。

- **v1.2 (2026-11-02) - 攻克 BCC 校验异常（关键修改）**
  - **现象定位**：原先采用 SPI FIFO 突发连续读时，频繁出现 `[CARD] BCC Error (calc 0x98 != recv 0x00)`。
  - **根因分析**：CLRC663 的 SPI 接口每次从 MISO 读取数据时，MOSI 线上必须严格带出目标寄存器地址。连续读发送的 0x00 被芯片解析为 `CommandReg` 地址，导致除第一个字节外后续全部读出 0x00。
  - **修改方案**：全面废弃突发连续读取，改用单字节安全传输，每读一个字节均带独立 CS 拉低、地址发送与 CS 释放时延（5μs），彻底消除数据移位与 BCC 校验错误。

- **v1.3 (2026-11-05) - 多级级联防冲撞支持**
  - 完善 Cascade Level 1 (0x93)、Level 2 (0x95) 及 Level 3 (0x97) 统一防冲撞与选卡循环。
  - 自动识别 CT 级联标志（0x88），支持 4 字节（MIFARE Classic）、7 字节（NTAG / Ultralight）及 10 字节 UID。

- **v1.4 (2026-11-08) - 进出场状态机与数据块读取**
  - 引入非阻塞卡片进出场检测机制（`cardPresent` / `lastUid`），入场只触发一次详细信息打印，驻留期间静默，离开时立刻提示，彻底解决串口刷屏。
  - 调整卡片事务生命周期：在选卡完成后保持卡片处于 ACTIVE 状态，支持调用 `RC663_ReadBlock()` 自动读取 Block 4（Page 4..7）用户数据区 16 字节，读完后再执行 `HLTA` 挂起卡片。
  - 提高 PA4 (CS) 与 PB5 (EN) 的 GPIO 翻转速度等级为 `GPIO_SPEED_FREQ_VERY_HIGH`。

---

## 二、硬件引脚接线对照表

| STM32F407ZGT6 引脚 | CLRC663 模块引脚 | 功能说明 | 关键配置与注意点 |
| :--- | :--- | :--- | :--- |
| **PA5** | **SCK / CLK** | SPI1 时钟线 | 复用推挽，最高速 (5.25 MHz) |
| **PA6** | **MISO / SDO**| SPI1 主入从出 | 模块数据返回线 |
| **PA7** | **MOSI / SDA**| SPI1 主出从入 | 模块命令接收线 |
| **PA4** | **NSS / CS**  | SPI1 软件片选 | 推挽输出，默认高电平，极速翻转 |
| **PB5** | **EN / PDOWN**| 模块掉电/复位 | **高电平硬件掉电，低电平正常上电工作** |
| **PB4** | **IRQ**       | 模块中断信号 | 浮空输入/上拉，下降沿中断 |
| **3.3V** | **VCC / PAD_VDD** | 3.3V 主电源 | 必须保证充足电流（天线发射峰值约 100~150mA） |
| **GND** | **GND**       | 逻辑与模拟地 | 必须与 MCU 严密共地 |
| **GND** | **IFSEL0 (Pin 26)**| 接口模式配置 0 | **必须可靠接地 (0V)！** |
| **3.3V**| **IFSEL1 (Pin 27)**| 接口模式配置 1 | **必须可靠接 3.3V (PAD_VDD)！进入 SPI 模式** |

---

## 三、编译与固件产物

### 1. 固件文件路径
工程支持 CMake + Ninja 快速编译，生成的固件位于 `build/Debug/` 目录下：
- **可执行文件**：`build/Debug/STM32F407ZGT6-CLRC66303HNY.elf`
- **烧录固件 (HEX)**：[`build/Debug/STM32F407ZGT6-CLRC66303HNY.hex`](file:///c:/Users/Administrator/Desktop/帮我评估一下这两份代码-Antigravity/STM32F407ZGT6-CLRC66303HNY/build/Debug/STM32F407ZGT6-CLRC66303HNY.hex)
- **烧录固件 (BIN)**：[`build/Debug/STM32F407ZGT6-CLRC66303HNY.bin`](file:///c:/Users/Administrator/Desktop/帮我评估一下这两份代码-Antigravity/STM32F407ZGT6-CLRC66303HNY/build/Debug/STM32F407ZGT6-CLRC66303HNY.bin)

### 2. 本地重新编译命令
```powershell
cmake --build "c:\Users\Administrator\Desktop\帮我评估一下这两份代码-Antigravity\STM32F407ZGT6-CLRC66303HNY\build\Debug"
```

---

## 四、串口调试输出效果（USART1 115200 8N1）

### 1. 系统上电与自检阶段
```text
=======================================================
  CLRC66303HNY SPI Reader (ISO/IEC 14443-A)
  Platform: STM32F407ZGT6 | SPI1 (PA5-SCK, PA6-MISO, PA7-MOSI, PA4-CS)
  Hardware: IFSEL1=3.3V, IFSEL0=GND (SPI Mode) | PDOWN=PB5
  Firmware: Build v1.4.2 [Release: 2026-11-08]
=======================================================

[RC663] Hardware reset via PDOWN (Pin 21, EN=PB5)...
[RC663] Hardware reset and SoftReset (0x1F) completed.
[RC663] Read VersionReg(0x7F) = 0x1A
[RC663] Detected CLRC663 plus (Silicon Rev 03, matches CLRC66303HNY)!
[RC663] Register R/W verify PASS!
[RC663] ISO14443-A (106 kbps) loaded, RF Transmitter is ON.
[APP] Reader Ready! Please place an ISO14443-A card near the antenna...
```

### 2. 7 字节卡测试（NTAG 213/215/216 或 手机 NFC）
```text
-------------------------------------------------------
[CARD] Card Detected! (卡片已进入)
       UID(7):  04 5A 32 89 A1 43 80 
       ATQA:    0x0044
       SAK:     0x00 (NTAG213/215/216 / MIFARE Ultralight)
[READ] Reading Block 0x04 (Page 4) User Memory...
[DATA] Block 0x04 (16 Bytes):
       01 02 03 04  05 06 07 08  09 0A 0B 0C  0D 0E 0F 10 
[APP] Card transaction complete. Placed into HALT state.
```
*卡片贴近天线时保持静止，串口无多余刷屏。*

### 3. 卡片离开天线
```text
<<< [CARD] Card Removed (卡片已离开)
```

### 4. 4 字节卡测试（MIFARE Classic 1K）
```text
-------------------------------------------------------
[CARD] Card Detected! (卡片已进入)
       UID(4):  A3 B2 C1 D0 
       ATQA:    0x0004
       SAK:     0x08 (MIFARE Classic 1K)
[APP] Card transaction complete. Placed into HALT state.
```
