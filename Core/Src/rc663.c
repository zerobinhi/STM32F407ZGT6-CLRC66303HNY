/**
  ******************************************************************************
  * @file    rc663.c
  * @brief   CLRC66303HNY 寄存器驱动实现（全双工 SPI1 模式）
  *          底层采用单字节隔离 SPI 时序，支持全级联防冲撞与卡片事务控制
  *          平台：STM32F407ZGT6 + STM32Cube HAL
  *
  * 迭代记录 (Revision History):
  *   - v1.0 (2026-10-18): 实现 SPI 硬件收发、DWT 微秒延时及基础读写函数
  *   - v1.1 (2026-10-26): 实现 RC663_Init 硬件 PDOWN 复位及寄存器自检
  *   - v1.2 (2026-11-02): 解决 SPI burst read 造成的 BCC 校验失败，改用逐字节读取
  *   - v1.3 (2026-11-05): 实现多级级联 (Cascade Level 1/2/3) 寻卡与 SAK 校验
  *   - v1.4 (2026-11-08): 增加 RC663_ReadBlock (0x30 命令) 实现对扇区数据的安全读取
  ******************************************************************************
  */
#include "rc663.h"
#include "spi.h"
#include <stdio.h>
#include <string.h>

/* SPI 硬件句柄引用（来自 spi.c） */
extern SPI_HandleTypeDef hspi1;

/* 片选 CS 宏控制（PA4：软件推挽片选，低电平使能，高电平释放） */
#define RC663_CS_LOW()          HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_RESET)
#define RC663_CS_HIGH()         HAL_GPIO_WritePin(CS_GPIO_Port, CS_Pin, GPIO_PIN_SET)

/* EN 引脚电平控制（PB5：高电平硬件掉电，低电平正常上电工作） */
#define RC663_EN_POWER_DOWN()   HAL_GPIO_WritePin(EN_GPIO_Port, EN_Pin, GPIO_PIN_SET)
#define RC663_EN_POWER_UP()     HAL_GPIO_WritePin(EN_GPIO_Port, EN_Pin, GPIO_PIN_RESET)

#define RC663_SPI_TIMEOUT_MS    10U

/* ============================================================================
 * DWT 微秒精确硬件延时实现
 * ============================================================================ */
static void RC663_DwtInit(void)
{
    SystemCoreClockUpdate();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void RC663_DelayUs(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    uint32_t ticks = us * (SystemCoreClock / 1000000U);
    while ((DWT->CYCCNT - start) < ticks) { }
}

/* ============================================================================
 * 底层 SPI 寄存器与 FIFO 读写操作（严格遵循 SW6693 单字节 SPI 时序规范）
 * ============================================================================ */

/**
 * @brief  向 CLRC663 写入单个寄存器
 * @param  addr 寄存器地址 (0x00 ~ 0x7F)
 * @param  val  待写入的 8 位数据
 */
void RC663_WriteRegister(uint8_t addr, uint8_t val)
{
    uint8_t tx[2];
    tx[0] = (uint8_t)((addr << 1U) & 0xFEU); /* bit0 = 0 表示写操作 */
    tx[1] = val;

    RC663_CS_LOW();
    HAL_SPI_Transmit(&hspi1, tx, 2, RC663_SPI_TIMEOUT_MS);
    RC663_CS_HIGH();

    /* NXP 官方 SW6693 WAITSPI 延时，确保片选高电平脉宽满足 tNTSH 规范 */
    RC663_DelayUs(5);
}

/**
 * @brief  从 CLRC663 读取单个寄存器
 * @param  addr 寄存器地址 (0x00 ~ 0x7F)
 * @return 读出的 8 位寄存器数据
 */
uint8_t RC663_ReadRegister(uint8_t addr)
{
    uint8_t tx[2], rx[2] = {0};
    tx[0] = (uint8_t)((addr << 1U) | 0x01U); /* bit0 = 1 表示读操作 */
    tx[1] = 0x00U;                            /* Dummy 字节，提供读取时钟 */

    RC663_CS_LOW();
    HAL_SPI_TransmitReceive(&hspi1, tx, rx, 2, RC663_SPI_TIMEOUT_MS);
    RC663_CS_HIGH();

    RC663_DelayUs(5);
    return rx[1];
}

/**
 * @brief  向 FIFO 写入多字节数据（严格遵循 SW6693：按字节单次独立写，杜绝总线冲突）
 * @param  pData  待写入数据指针
 * @param  length 写入字节数
 */
void RC663_WriteFIFO(const uint8_t *pData, uint16_t length)
{
    if ((pData == NULL) || (length == 0)) return;

    for (uint16_t i = 0; i < length; i++)
    {
        RC663_WriteRegister(RC663_REG_FIFODATA, pData[i]);
    }
}

/**
 * @brief  从 FIFO 读取多字节数据（严格遵循 SW6693：按字节单次独立读，彻底根除突发读取地址覆盖 Bug）
 * @param  pData  存放读出数据的缓冲区
 * @param  length 待读取字节数
 */
void RC663_ReadFIFO(uint8_t *pData, uint16_t length)
{
    if ((pData == NULL) || (length == 0)) return;

    for (uint16_t i = 0; i < length; i++)
    {
        pData[i] = RC663_ReadRegister(RC663_REG_FIFODATA);
    }
}

/* ============================================================================
 * 等待卡片应答辅助函数（融合 NXP SW6693 核心判定与安全错误解析）
 * ============================================================================ */

/**
 * @brief  等待 Transceive 发射完毕并接收卡片应答
 * @param  timeoutMs 超时时间（毫秒）
 * @return RC663_Status_t 返回状态码
 */
static RC663_Status_t RC663_WaitForResponse(uint32_t timeoutMs)
{
    uint32_t expire = HAL_GetTick() + timeoutMs;

    while (HAL_GetTick() < expire)
    {
        uint8_t irq0 = RC663_ReadRegister(RC663_REG_IRQ0);

        /* 1. 精确匹配 NXP SW6693 特征字：0x3C (LoAlert | Idle | Tx | Rx) 或检测到帧结束 RxIRQ (0x04) */
        if (((irq0 & 0x3CU) == 0x3CU) || (irq0 & RC663_IRQ0_RX))
        {
            /* 进一步核查错误寄存器，避免将冲突或 CRC 错误帧误认为正常数据 */
            uint8_t err = RC663_ReadRegister(RC663_REG_ERROR);
            if (err & RC663_ERR_COLLDET) return RC663_ERR_COLLISION;
            if (err & RC663_ERR_INTEG)   return RC663_ERR_CRC;
            if (err & RC663_ERR_PROT)    return RC663_ERR_PROTOCOL;
            return RC663_OK;
        }

        /* 2. 错误中断标志 ErrIRQ (0x02) */
        if (irq0 & RC663_IRQ0_ERR)
        {
            uint8_t err = RC663_ReadRegister(RC663_REG_ERROR);
            if (err & RC663_ERR_COLLDET) return RC663_ERR_COLLISION;
            if (err & RC663_ERR_INTEG)   return RC663_ERR_CRC;
            if (err & RC663_ERR_NODATA)  return RC663_ERR_NO_CARD;
            return RC663_ERR_PROTOCOL;
        }

        /* 3. 命令自动回退 Idle (0x10) 且未置位 RxIRQ，说明等待超时场中无应答 */
        if (irq0 & RC663_IRQ0_IDLE)
        {
            if (irq0 & RC663_IRQ0_RX)
            {
                return RC663_OK;
            }
            return RC663_ERR_NO_CARD;
        }

        RC663_DelayUs(50);
    }

    return RC663_ERR_TIMEOUT;
}

/* ============================================================================
 * 芯片初始化与链路自检
 * ============================================================================ */

/**
 * @brief  初始化 CLRC663（硬件 PDOWN 脉冲 + 权威 SoftReset 0x1F）
 */
void RC663_Init(void)
{
    RC663_DwtInit();

    /* 初始释放片选 */
    RC663_CS_HIGH();

    printf("\r\n[RC663] Hardware reset via PDOWN (Pin 21, EN=PB5)...\r\n");
    /* 1. PDOWN 硬件脉冲：拉高掉电 -> 拉低上电 */
    RC663_EN_POWER_DOWN();
    HAL_Delay(10);
    RC663_EN_POWER_UP();
    HAL_Delay(25); /* 满足数据手册中 27.12MHz 晶振稳定起振及内部自检时间 */

    /* 2. 执行 SoftReset (0x1F) 命令 */
    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_IDLE);
    RC663_WriteRegister(RC663_REG_FIFOCONTROL, 0xB0U); /* 清空 FIFO 并复位读写指针 */
    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_SOFTRESET);
    HAL_Delay(20); /* 等待内部从 EEPROM 重载默认寄存器映射 */

    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_IDLE);
    RC663_WriteRegister(RC663_REG_FIFOCONTROL, 0xB0U);

    printf("[RC663] Hardware reset and SoftReset (0x1F) completed.\r\n");
}

/**
 * @brief  通信链路自检与芯片版本识别
 * @return RC663_OK = 通过；其它 = 失败
 */
RC663_Status_t RC663_SelfTest(void)
{
    /* 1. 读取权威版本寄存器 VersionReg (0x7F) */
    uint8_t version = RC663_ReadRegister(RC663_REG_VERSION);
    printf("[RC663] Read VersionReg(0x7F) = 0x%02X\r\n", version);

    if ((version == 0x00U) || (version == 0xFFU))
    {
        printf("[RC663] ERROR: SPI Communication Failed! (Read 0x%02X)\r\n", version);
        printf("        Hardware Troubleshooting Checklist:\r\n");
        printf("        1. IFSEL0 (Pin 26) MUST tie to GND (0V)\r\n");
        printf("        2. IFSEL1 (Pin 27) MUST tie to 3.3V (PAD_VDD) -> SPI Mode!\r\n");
        printf("        3. SPI Pins: PA5=SCK, PA6=MISO, PA7=MOSI, PA4=CS\r\n");
        printf("        4. PDOWN (Pin 21, PB5) must stay LOW (0V) for normal operation\r\n");
        printf("        5. Check 27.12 MHz crystal and VDD/VDDA/GND connections\r\n");
        return RC663_ERR_SPI;
    }

    if ((version == 0x1AU) || (version == 0x1BU))
    {
        printf("[RC663] Detected CLRC663 plus (Silicon Rev 03, matches CLRC66303HNY)!\r\n");
    }
    else if (version == 0x18U)
    {
        printf("[RC663] Detected standard CLRC663 (Rev 1.0/2.0).\r\n");
    }
    else
    {
        printf("[RC663] Warning: Unknown silicon version 0x%02X.\r\n", version);
    }

    /* 2. 寄存器读写回环自检（使用水位寄存器 WaterLevelReg 0x03） */
    uint8_t origin = RC663_ReadRegister(RC663_REG_WATERLEVEL);
    RC663_WriteRegister(RC663_REG_WATERLEVEL, 0x2AU);
    uint8_t back = RC663_ReadRegister(RC663_REG_WATERLEVEL);
    RC663_WriteRegister(RC663_REG_WATERLEVEL, origin); /* 恢复原值 */

    if (back == 0x2AU)
    {
        printf("[RC663] Register R/W verify PASS!\r\n");
        return RC663_OK;
    }

    printf("[RC663] Register R/W verify FAIL (wrote 0x2A, read back 0x%02X)!\r\n", back);
    return RC663_ERR_SPI;
}

/**
 * @brief  控制天线射频发射场开关
 * @param  enable true=打开 (0x8E), false=关闭 (0x86)
 */
void RC663_SetRfField(bool enable)
{
    if (enable)
    {
        /* 0x8E: Tx2Inv=1, TxEn=1, TxClkMode=110b (启动 13.56MHz 天线发射载波) */
        RC663_WriteRegister(RC663_REG_DRIVERMODE, 0x8EU);
    }
    else
    {
        /* 0x86: 关闭 TxEn，切断天线射频场 */
        RC663_WriteRegister(RC663_REG_DRIVERMODE, 0x86U);
    }
}

/**
 * @brief  从内部 EEPROM 加载 ISO14443-A (106kbps) 协议并打开天线射频场
 * @return RC663_OK = 成功；其它 = 失败
 */
RC663_Status_t RC663_LoadProtocolA(void)
{
    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_IDLE);
    RC663_WriteRegister(RC663_REG_FIFOCONTROL, 0xB0U);

    /* 写入 0x00, 0x00: 指定 RxProtocol=0, TxProtocol=0 (Type A 106kbps) */
    RC663_WriteRegister(RC663_REG_FIFODATA, 0x00U);
    RC663_WriteRegister(RC663_REG_FIFODATA, 0x00U);
    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_LOADPROTOCOL);
    HAL_Delay(10);

    uint8_t err = RC663_ReadRegister(RC663_REG_ERROR);
    if (err != 0U)
    {
        printf("[RC663] LoadProtocol FAIL, ErrorReg = 0x%02X\r\n", err);
        return RC663_ERR_PROTOCOL;
    }

    /* 打开天线 13.56MHz 发射场 */
    RC663_SetRfField(true);
    HAL_Delay(5); /* 射频场建立稳定延时 */
    printf("[RC663] ISO14443-A (106 kbps) loaded, RF Transmitter is ON.\r\n");
    return RC663_OK;
}

/* ============================================================================
 * ISO14443-A 寻卡、防冲撞与选卡核心实现（融合 SW6693 时序与 WUPA 唤醒机制）
 * ============================================================================ */

/**
 * @brief  发送 WUPA (0x52) 或 REQA (0x26) 寻卡测试
 * @param  atqa 输出 2 字节 ATQA 应答
 * @return RC663_OK = 寻到卡片；其它 = 无卡或错误
 */
RC663_Status_t RC663_WakeupCard(uint8_t atqa[2])
{
    if (atqa == NULL) return RC663_ERR_PROTOCOL;

    /* 1. 清空 FIFO 与中断标志 */
    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_IDLE);
    RC663_WriteRegister(RC663_REG_FIFOCONTROL, 0xB0U);
    RC663_WriteRegister(RC663_REG_IRQ0, RC663_IRQ0_CLR_ALL);

    /* 2. REQA/WUPA 命令双向关闭 CRC */
    RC663_WriteRegister(RC663_REG_TXCRCCON, RC663_CRC_OFF_A);
    RC663_WriteRegister(RC663_REG_RXCRCCON, RC663_CRC_OFF_A);

    /* 3. WUPA 为 7 位短帧：TxDataNum = 0x0F (DataEn=1 | 7 bits) */
    RC663_WriteRegister(RC663_REG_TXDATANUM, (uint8_t)(RC663_TXDATANUM_DATAEN | 0x07U));

    /* 4. 写入 WUPA (0x52)，IDLE 与 HALT 状态卡片均响应 */
    RC663_WriteRegister(RC663_REG_FIFODATA, RC663_ISO14443A_WUPA);

    /* 5. 发送并开启接收回路 */
    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_TRANSCEIVE);

    /* 6. 等待卡片应答 */
    RC663_Status_t status = RC663_WaitForResponse(10);
    if (status != RC663_OK)
    {
        /* 若上一轮卡片停留在 ACTIVE 状态可能不理睬 WUPA，补发一次 HLTA 后重试 */
        RC663_HaltCard();

        RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_IDLE);
        RC663_WriteRegister(RC663_REG_FIFOCONTROL, 0xB0U);
        RC663_WriteRegister(RC663_REG_IRQ0, RC663_IRQ0_CLR_ALL);
        RC663_WriteRegister(RC663_REG_TXCRCCON, RC663_CRC_OFF_A);
        RC663_WriteRegister(RC663_REG_RXCRCCON, RC663_CRC_OFF_A);
        RC663_WriteRegister(RC663_REG_TXDATANUM, (uint8_t)(RC663_TXDATANUM_DATAEN | 0x07U));
        RC663_WriteRegister(RC663_REG_FIFODATA, RC663_ISO14443A_WUPA);
        RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_TRANSCEIVE);

        status = RC663_WaitForResponse(10);
        if (status != RC663_OK)
        {
            return status;
        }
    }

    /* 7. 读取 ATQA (2 字节) */
    uint8_t rxLen = RC663_ReadRegister(RC663_REG_FIFOLENGTH);
    if (rxLen >= 2U)
    {
        atqa[0] = RC663_ReadRegister(RC663_REG_FIFODATA);
        atqa[1] = RC663_ReadRegister(RC663_REG_FIFODATA);
        return RC663_OK;
    }

    return RC663_ERR_NO_CARD;
}

/**
 * @brief  执行单级防冲撞命令（如 CL1: 0x93 0x20 或 CL2: 0x95 0x20 或 CL3: 0x97 0x20）
 * @param  cascadeLevel 0x93 (Level 1) / 0x95 (Level 2) / 0x97 (Level 3)
 * @param  uidResp      输出 5 字节数据（前 4 字节为 UID/CT，第 5 字节为卡片返回的 BCC）
 * @return RC663_OK = 校验成功；其它 = 失败或 BCC 错误
 */
RC663_Status_t RC663_Anticollision(uint8_t cascadeLevel, uint8_t uidResp[5])
{
    if (uidResp == NULL) return RC663_ERR_PROTOCOL;

    /* 1. 清空 FIFO 与中断标志 */
    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_IDLE);
    RC663_WriteRegister(RC663_REG_FIFOCONTROL, 0xB0U);
    RC663_WriteRegister(RC663_REG_IRQ0, RC663_IRQ0_CLR_ALL);

    /* 2. SW6693 关键设置：RxBitCtrl = 0x80（碰撞时 1 强于 0，字节对齐） */
    RC663_WriteRegister(RC663_REG_RXBITCTRL, 0x80U);

    /* 3. 防冲撞阶段不带硬件 CRC */
    RC663_WriteRegister(RC663_REG_TXCRCCON, RC663_CRC_OFF_A);
    RC663_WriteRegister(RC663_REG_RXCRCCON, RC663_CRC_OFF_A);

    /* 4. 8-bit 整字节传输 */
    RC663_WriteRegister(RC663_REG_TXDATANUM, RC663_TXDATANUM_DATAEN);

    /* 5. 写入防冲撞命令：级联级别码 + NVB (0x20) */
    RC663_WriteRegister(RC663_REG_FIFODATA, cascadeLevel);
    RC663_WriteRegister(RC663_REG_FIFODATA, RC663_ISO14443A_NVB_AC);

    /* 6. 发射并等待卡片应答 */
    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_TRANSCEIVE);

    RC663_Status_t status = RC663_WaitForResponse(20);
    if (status != RC663_OK)
    {
        return status;
    }

    /* 7. 严格核验应答字节数：完整 UID 片段 + BCC 必须为 5 字节 */
    uint8_t rxLen = RC663_ReadRegister(RC663_REG_FIFOLENGTH);
    if (rxLen != 5U)
    {
        return RC663_ERR_PROTOCOL;
    }

    /* 8. 严格采用 SW6693 单字节循环读取，杜绝 SPI 突发读取地址错位 */
    for (uint8_t i = 0; i < 5; i++)
    {
        uidResp[i] = RC663_ReadRegister(RC663_REG_FIFODATA);
    }

    /* 9. 权威 BCC 异或和校验：BCC = Byte0 ^ Byte1 ^ Byte2 ^ Byte3 */
    uint8_t bccCalc = uidResp[0] ^ uidResp[1] ^ uidResp[2] ^ uidResp[3];
    if (bccCalc != uidResp[4])
    {
        return RC663_ERR_BCC;
    }

    return RC663_OK; /* 防冲撞校验通过 */
}

/**
 * @brief  执行单级选卡命令（如 CL1: 0x93 0x70 或 CL2: 0x95 0x70）
 * @param  cascadeLevel 0x93 (Level 1) / 0x95 (Level 2) / 0x97 (Level 3)
 * @param  uidResp      该级防冲撞得到的 5 字节数据（UID0..3 + BCC）
 * @param  pSak         输出该级卡片返回的 SAK 状态字节
 * @return RC663_OK = 选卡成功；其它 = 失败
 */
RC663_Status_t RC663_Select(uint8_t cascadeLevel, const uint8_t uidResp[5], uint8_t *pSak)
{
    if (uidResp == NULL) return RC663_ERR_PROTOCOL;

    /* 1. 清空 FIFO 与中断标志 */
    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_IDLE);
    RC663_WriteRegister(RC663_REG_FIFOCONTROL, 0xB0U);
    RC663_WriteRegister(RC663_REG_IRQ0, RC663_IRQ0_CLR_ALL);

    /* 2. Select 选卡命令必须开启硬件 CRC-16 (Tx 0x19, Rx 0x19) */
    RC663_WriteRegister(RC663_REG_TXCRCCON, RC663_CRC_ON_A);
    RC663_WriteRegister(RC663_REG_RXCRCCON, RC663_CRC_ON_A);

    /* 3. 8-bit 整字节传输 */
    RC663_WriteRegister(RC663_REG_TXDATANUM, RC663_TXDATANUM_DATAEN);

    /* 4. 组装 Select 帧：级联级别码 + NVB (0x70) + 5 字节 (UID0..3 + BCC) */
    RC663_WriteRegister(RC663_REG_FIFODATA, cascadeLevel);
    RC663_WriteRegister(RC663_REG_FIFODATA, RC663_ISO14443A_NVB_SEL);
    for (uint8_t i = 0; i < 5; i++)
    {
        RC663_WriteRegister(RC663_REG_FIFODATA, uidResp[i]);
    }

    /* 5. 发送并等待 SAK 应答 */
    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_TRANSCEIVE);

    RC663_Status_t status = RC663_WaitForResponse(20);
    if (status != RC663_OK)
    {
        return status;
    }

    /* 6. 开启 Rx CRC 后，CLRC663 硬件自动校验并剥离 2 字节 CRC，FIFO 中仅剩 1 字节 SAK */
    uint8_t rxLen = RC663_ReadRegister(RC663_REG_FIFOLENGTH);
    if (rxLen < 1U)
    {
        return RC663_ERR_PROTOCOL;
    }

    uint8_t sak = RC663_ReadRegister(RC663_REG_FIFODATA);
    if (pSak != NULL)
    {
        *pSak = sak;
    }

    return RC663_OK;
}

/**
 * @brief  完整 ISO14443-A 多级防冲突读卡（统一循环，支持 4B / 7B / 10B UID，保持 ACTIVE 供后续操作）
 * @param  uid    输出 UID 缓冲区（至少 10 字节）
 * @param  uidLen 输出实际 UID 长度（4 / 7 / 10 字节）
 * @param  sak    输出最终选卡成功的 SAK 字节
 * @param  atqa   输出卡片应答的 ATQA（2 字节）
 * @return RC663_OK = 选卡成功；其它 = 场中无卡或读取失败
 */
RC663_Status_t RC663_ReadCardUID(uint8_t *uid, uint8_t *uidLen, uint8_t *sak, uint8_t atqa[2])
{
    uint8_t cascade[5];
    uint8_t sakByte = 0U;
    uint8_t uidBytes = 0U;
    uint8_t selCode = RC663_ISO14443A_SEL_CL1;
    RC663_Status_t status;

    if ((uid == NULL) || (uidLen == NULL))
    {
        return RC663_ERR_PROTOCOL;
    }

    *uidLen = 0U;

    /* 1. 发送 WUPA 寻卡 */
    status = RC663_WakeupCard(atqa);
    if (status != RC663_OK)
    {
        return status;
    }

    /* 2. 统一多级防冲撞循环（兼容 CL1=4B、CL2=7B、CL3=10B UID） */
    for (uint8_t level = 0; level < 3; level++)
    {
        /* 2.1 执行当前级防冲撞 */
        status = RC663_Anticollision(selCode, cascade);
        if (status != RC663_OK)
        {
            return status;
        }

        /* 2.2 执行当前级选卡 */
        status = RC663_Select(selCode, cascade, &sakByte);
        if (status != RC663_OK)
        {
            return status;
        }

        /* 2.3 判断当前 UID 是否已完整：
         * 若 SAK 的 Bit 2 (0x04) 为 1，或首字节为 Cascade Tag (0x88)，说明 UID 尚未完整，需进入下一级 */
        if ((sakByte & 0x04U) || (cascade[0] == RC663_ISO14443A_CASCADE_TAG))
        {
            if (level >= 2U)
            {
                return RC663_ERR_UNSUPPORTED; /* 超过 CL3 的级联不存在 */
            }

            /* 去掉 CT(0x88)，保存实际的 3 字节 UID 片段 */
            memcpy(&uid[uidBytes], &cascade[1], 3U);
            uidBytes = (uint8_t)(uidBytes + 3U);

            /* 准备下一级级联命令码 */
            selCode = (level == 0U) ? RC663_ISO14443A_SEL_CL2 : RC663_ISO14443A_SEL_CL3;
        }
        else
        {
            /* UID 已完整：复制全部 4 字节数据并退出循环 */
            memcpy(&uid[uidBytes], cascade, 4U);
            uidBytes = (uint8_t)(uidBytes + 4U);
            break;
        }
    }

    if (uidBytes == 0U)
    {
        return RC663_ERR_PROTOCOL;
    }

    *uidLen = uidBytes;
    if (sak != NULL)
    {
        *sak = sakByte;
    }

    /* 注意：此时卡片处于 ACTIVE 状态，允许上层应用立即进行数据块读写！ */
    return RC663_OK;
}

/**
 * @brief  向已选中的卡片发送 HLTA 休眠命令（使卡片进入 HALT 状态）
 */
void RC663_HaltCard(void)
{
    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_IDLE);
    RC663_WriteRegister(RC663_REG_FIFOCONTROL, 0xB0U);
    RC663_WriteRegister(RC663_REG_IRQ0, RC663_IRQ0_CLR_ALL);

    /* HLTA 命令带 CRC */
    RC663_WriteRegister(RC663_REG_TXCRCCON, RC663_CRC_ON_A);
    RC663_WriteRegister(RC663_REG_RXCRCCON, RC663_CRC_ON_A);
    RC663_WriteRegister(RC663_REG_TXDATANUM, RC663_TXDATANUM_DATAEN);

    RC663_WriteRegister(RC663_REG_FIFODATA, RC663_ISO14443A_HLTA);
    RC663_WriteRegister(RC663_REG_FIFODATA, 0x00U);

    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_TRANSCEIVE);

    /* HLTA 卡片不应答，仅等待短时发射完成 */
    RC663_WaitForResponse(5);
}

/**
 * @brief  读取卡片存储块（严格遵循 SW6693 Ex3_Write_Block 规范）
 * @param  blockAddr 目标块地址 (如 NTAG213/215/216 的 Page 4 ~ Page 39)
 * @param  blockData 输出 16 字节数据缓冲区
 * @return RC663_OK = 读取成功；其它 = 读取失败
 */
RC663_Status_t RC663_ReadBlock(uint8_t blockAddr, uint8_t blockData[16])
{
    if (blockData == NULL) return RC663_ERR_PROTOCOL;

    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_IDLE);
    RC663_WriteRegister(RC663_REG_FIFOCONTROL, 0xB0U);
    RC663_WriteRegister(RC663_REG_IRQ0, RC663_IRQ0_CLR_ALL);

    /* 读块命令双向开启 CRC-16 */
    RC663_WriteRegister(RC663_REG_TXCRCCON, RC663_CRC_ON_A);
    RC663_WriteRegister(RC663_REG_RXCRCCON, RC663_CRC_ON_A);
    RC663_WriteRegister(RC663_REG_TXDATANUM, RC663_TXDATANUM_DATAEN);

    /* 发送读块指令 (0x30) + 目标块地址 */
    RC663_WriteRegister(RC663_REG_FIFODATA, RC663_ISO14443A_READ_BLOCK);
    RC663_WriteRegister(RC663_REG_FIFODATA, blockAddr);

    RC663_WriteRegister(RC663_REG_COMMAND, RC663_CMD_TRANSCEIVE);

    RC663_Status_t status = RC663_WaitForResponse(30);
    if (status != RC663_OK)
    {
        return status;
    }

    uint8_t rxLen = RC663_ReadRegister(RC663_REG_FIFOLENGTH);
    if (rxLen < 16U)
    {
        return RC663_ERR_PROTOCOL;
    }

    for (uint8_t i = 0; i < 16; i++)
    {
        blockData[i] = RC663_ReadRegister(RC663_REG_FIFODATA);
    }

    return RC663_OK;
}
