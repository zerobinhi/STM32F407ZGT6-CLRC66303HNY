/**
  ******************************************************************************
  * @file    rc663.h
  * @brief   CLRC66303HNY 寄存器驱动头文件（全双工 SPI1 模式）
  *          遵循 ISO/IEC 14443 Type A 协议，支持多级级联防冲撞与数据块读写
  *          平台：STM32F407ZGT6 + STM32Cube HAL
  *
  * 迭代记录:
  *   - v1.0: 基础寄存器定义与通信接口
  *   - v1.1: 完善 ISO14443-A 协议寄存器及射频载波配置
  *   - v1.2: 优化 FIFO 访问接口，规范状态返回枚举 RC663_Status_t
  *   - v1.3: 增加 RC663_ReadCardUID 级联处理及 RC663_ReadBlock 块读取接口
  *
  * 引脚定义（实物接线规范）：
  *   - SPI1_SCK  : PA5
  *   - SPI1_MISO : PA6
  *   - SPI1_MOSI : PA7
  *   - CS (NSS)  : PA4（极速推挽输出，默认高电平，低电平使能）
  *   - EN (PDOWN): PB5（推挽输出，低电平正常工作，高电平硬掉电复位）
  *   - IRQ       : PB4（外部中断引脚）
  *
  * 硬件电平配置（SPI 接口模式）：
  *   - IFSEL0 (Pin 26) : 必须接 GND (0V)
  *   - IFSEL1 (Pin 27) : 必须接 3.3V (PAD_VDD)
  ******************************************************************************
  */
#ifndef __RC663_H
#define __RC663_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * CLRC663 官方寄存器地址定义（对齐 NXP SW6693 与 Datasheet Table 50）
 * ============================================================================ */
#define RC663_REG_COMMAND        0x00U   /**< CommandReg：命令状态机控制 */
#define RC663_REG_HOSTCTRL       0x01U   /**< HostCtrlReg：主机总线与 SAM 接口配置 */
#define RC663_REG_FIFOCONTROL    0x02U   /**< FIFOControlReg：FIFO 属性控制与清空 (0xB0) */
#define RC663_REG_WATERLEVEL     0x03U   /**< WaterLevelReg：FIFO 水位警告阈值 */
#define RC663_REG_FIFOLENGTH     0x04U   /**< FIFOLengthReg：当前 FIFO 缓冲字节数 */
#define RC663_REG_FIFODATA       0x05U   /**< FIFODataReg：FIFO 数据读写端口 */
#define RC663_REG_IRQ0           0x06U   /**< IRQ0Reg：中断标志寄存器 0 (读状态/写0x7F清除) */
#define RC663_REG_IRQ1           0x07U   /**< IRQ1Reg：中断标志寄存器 1 */
#define RC663_REG_IRQ0EN         0x08U   /**< IRQ0EnReg：中断使能寄存器 0 */
#define RC663_REG_IRQ1EN         0x09U   /**< IRQ1EnReg：中断使能寄存器 1 */
#define RC663_REG_ERROR          0x0AU   /**< ErrorReg：硬件错误状态寄存器 */
#define RC663_REG_STATUS         0x0BU   /**< StatusReg：通信状态及加密标识 */
#define RC663_REG_RXBITCTRL      0x0CU   /**< RxBitCtrlReg：位碰撞处理与对齐 (0x80) */
#define RC663_REG_RXCOLL         0x0DU   /**< RxCollReg：碰撞位置指示 */
#define RC663_REG_DRIVERMODE     0x28U   /**< DrvModeReg：驱动模式控制（0x8E=开场，0x86=关场） */
#define RC663_REG_TXCRCCON       0x2CU   /**< TxCrcPresetReg：发送 CRC-16 预设与使能 (0x18关/0x19开) */
#define RC663_REG_RXCRCCON       0x2DU   /**< RxCrcPresetReg：接收 CRC-16 预设与使能 (0x18关/0x19开) */
#define RC663_REG_TXDATANUM      0x2EU   /**< TxDataNumReg：末字节发送位数控制 (0x0F=7位/0x08=8位) */
#define RC663_REG_TXMODWIDTH     0x2FU   /**< TxModWidthReg：调制宽度 */
#define RC663_REG_FRAMECON       0x33U   /**< FrameConReg：奇偶校验与帧格式配置 */
#define RC663_REG_VERSION        0x7FU   /**< VersionReg：芯片版本识别寄存器 (0x1A=CLRC663 plus) */

/* ============================================================================
 * IRQ0 标志位定义（Datasheet Table 64 & SW6693）
 * ============================================================================ */
#define RC663_IRQ0_SET           0x80U   /**< 标志位写使能极性 */
#define RC663_IRQ0_CLR_ALL       0x7FU   /**< 清除全部 IRQ0 中断标志 */
#define RC663_IRQ0_HIALERT       0x40U   /**< FIFO 高水位 */
#define RC663_IRQ0_LOALERT       0x20U   /**< FIFO 低水位 (发送完毕后变空触发) */
#define RC663_IRQ0_IDLE          0x10U   /**< 命令自动终结并返回空闲 */
#define RC663_IRQ0_TX            0x08U   /**< 数据发送完毕 */
#define RC663_IRQ0_RX            0x04U   /**< 接收器检测到帧结束并接收完成 */
#define RC663_IRQ0_ERR           0x02U   /**< 发生硬件错误 */
#define RC663_IRQ0_SOF           0x01U   /**< 接收到 SOF 帧头 */

/* SW6693 中标准的 Transceive 完成特征字：
 * LoAlert(0x20) | Idle(0x10) | Tx(0x08) | Rx(0x04) = 0x3C */
#define RC663_IRQ0_XFER_COMPLETE 0x3CU

/* ============================================================================
 * ErrorReg 硬件错误标志（Datasheet Table 72）
 * ============================================================================ */
#define RC663_ERR_EE             0x80U   /**< EEPROM 校验或操作失败 */
#define RC663_ERR_FIFOWR         0x40U   /**< FIFO 写错误 */
#define RC663_ERR_FIFOOVL        0x20U   /**< FIFO 溢出错误 */
#define RC663_ERR_MINFRAME       0x10U   /**< 帧长度小于最小规定 */
#define RC663_ERR_NODATA         0x08U   /**< 无数据或接收超时 */
#define RC663_ERR_COLLDET        0x04U   /**< 碰撞检测到 */
#define RC663_ERR_PROT           0x02U   /**< 协议违规 */
#define RC663_ERR_INTEG          0x01U   /**< 数据完整性错误（CRC或奇偶校验错误） */

/* ============================================================================
 * CLRC663 常用命令字（Datasheet Section 8.10.3 & Table 52）
 * ============================================================================ */
#define RC663_CMD_IDLE           0x00U   /**< 取消当前命令，返回 Idle 空闲模式 */
#define RC663_CMD_LPCD           0x01U   /**< 低功耗卡片检测 */
#define RC663_CMD_LOADKEY        0x02U   /**< 加载密钥 */
#define RC663_CMD_MFAUTHENT      0x03U   /**< MIFARE 密钥认证 */
#define RC663_CMD_RECEIVE        0x05U   /**< 启动接收回路 */
#define RC663_CMD_TRANSMIT       0x06U   /**< 发送 FIFO 中数据 */
#define RC663_CMD_TRANSCEIVE     0x07U   /**< 发射 FIFO 数据并自动启动接收回路等待应答 */
#define RC663_CMD_LOADPROTOCOL   0x0DU   /**< 从内部 EEPROM 加载指定协议预设配置 (Type A=0x00,0x00) */
#define RC663_CMD_SOFTRESET      0x1FU   /**< 软复位命令（Datasheet 8.10.3.17 规范为 0x1F） */

/* ============================================================================
 * CRC 配置与末字节发送参数
 * ============================================================================ */
#define RC663_CRC_OFF_A          0x18U   /**< CRC-16 预设 0x6363，关闭 CRC 发送与校验 */
#define RC663_CRC_ON_A           0x19U   /**< CRC-16 预设 0x6363，开启 CRC 发送与硬件自动剥离校验 */
#define RC663_TXDATANUM_DATAEN   0x08U   /**< DataEn：末字节数据使能位 */

/* ============================================================================
 * ISO14443-A 标准协议命令与常量
 * ============================================================================ */
#define RC663_ISO14443A_REQA         0x26U   /**< REQA（7-bit 短帧寻卡，IDLE 卡响应） */
#define RC663_ISO14443A_WUPA         0x52U   /**< WUPA（7-bit 短帧唤醒寻卡，IDLE/HALT 卡均响应） */
#define RC663_ISO14443A_HLTA         0x50U   /**< HLTA（卡片休眠命令，带 CRC） */
#define RC663_ISO14443A_SEL_CL1      0x93U   /**< Cascade Level 1 防冲撞/选卡 */
#define RC663_ISO14443A_SEL_CL2      0x95U   /**< Cascade Level 2 防冲撞/选卡 */
#define RC663_ISO14443A_SEL_CL3      0x97U   /**< Cascade Level 3 防冲撞/选卡 */
#define RC663_ISO14443A_CASCADE_TAG  0x88U   /**< 级联标志 CT (Cascade Tag) */
#define RC663_ISO14443A_NVB_AC       0x20U   /**< 防冲撞阶段 NVB (2字节已知: 命令+NVB) */
#define RC663_ISO14443A_NVB_SEL      0x70U   /**< 选卡阶段 NVB (7字节已知: 命令+NVB+4B UID+BCC) */
#define RC663_ISO14443A_READ_BLOCK   0x30U   /**< 读存储块命令（16 字节） */

/* ============================================================================
 * 驱动状态返回枚举
 * ============================================================================ */
typedef enum
{
    RC663_OK = 0,              /**< 成功 */
    RC663_ERR_SPI = 1,         /**< SPI 传输失败 */
    RC663_ERR_NO_CARD = 2,     /**< 场中无卡应答 */
    RC663_ERR_TIMEOUT = 3,     /**< 等待超时 */
    RC663_ERR_COLLISION = 4,   /**< 检测到位冲突 */
    RC663_ERR_CRC = 5,         /**< CRC 或奇偶校验错误 */
    RC663_ERR_BCC = 6,         /**< BCC 异或和校验错误 */
    RC663_ERR_PROTOCOL = 7,    /**< 协议违规或未识别格式 */
    RC663_ERR_UNSUPPORTED = 8  /**< 超过支持层级的未知卡片 */
} RC663_Status_t;

/* ============================================================================
 * 驱动导出函数接口声明
 * ============================================================================ */

/* 延时工具 */
void RC663_DelayUs(uint32_t us);

/* 寄存器与 FIFO 基础读写接口（严格遵循 SW6693 原厂规范） */
void    RC663_WriteRegister(uint8_t addr, uint8_t val);
uint8_t RC663_ReadRegister(uint8_t addr);
void    RC663_WriteFIFO(const uint8_t *pData, uint16_t length);
void    RC663_ReadFIFO(uint8_t *pData, uint16_t length);

/* 芯片初始化与链路自检 */
void           RC663_Init(void);
RC663_Status_t RC663_SelfTest(void);
RC663_Status_t RC663_LoadProtocolA(void);
void           RC663_SetRfField(bool enable);

/* ISO14443-A 卡片通信、多层级防冲突与选卡 */
RC663_Status_t RC663_WakeupCard(uint8_t atqa[2]);
RC663_Status_t RC663_Anticollision(uint8_t cascadeLevel, uint8_t uidResp[5]);
RC663_Status_t RC663_Select(uint8_t cascadeLevel, const uint8_t uidResp[5], uint8_t *pSak);
RC663_Status_t RC663_ReadCardUID(uint8_t *uid, uint8_t *uidLen, uint8_t *sak, uint8_t atqa[2]);
void           RC663_HaltCard(void);

/* 卡片数据块读写能力（SW6693 Ex3_Write_Block 扩展） */
RC663_Status_t RC663_ReadBlock(uint8_t blockAddr, uint8_t blockData[16]);

#ifdef __cplusplus
}
#endif

#endif /* __RC663_H */
