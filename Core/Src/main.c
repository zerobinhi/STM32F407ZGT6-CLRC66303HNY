/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body (CLRC66303HNY RFID/NFC 读卡工程)
  *                   支持 ISO14443-A 多级级联 UID 解析与用户数据块读取
  *
  * 迭代更新记录 (Revision History):
  *   - v1.0 (2026-10-18): 完成底层硬件 SPI1 通信配置及基础寄存器读写自检
  *   - v1.1 (2026-10-26): 增加 ISO14443-A 协议配置与基础 REQA 轮询寻卡逻辑
  *   - v1.2 (2026-11-02): 修复连续 FIFO 突发读导致的 BCC 校验异常，重构为单字节安全传输
  *   - v1.3 (2026-11-05): 引入多级级联 (Cascade Level 1/2/3) 防冲撞与选卡机制
  *   - v1.4 (2026-11-08): 增加进出场防抖状态机、HLTA 挂起与第 4 块用户数据区自动读取
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "rc663.h"
#include <stdio.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  printf("\r\n=======================================================\r\n");
  printf("  CLRC66303HNY SPI Reader (ISO/IEC 14443-A)\r\n");
  printf("  Platform: STM32F407ZGT6 | SPI1 (PA5-SCK, PA6-MISO, PA7-MOSI, PA4-CS)\r\n");
  printf("  Hardware: IFSEL1=3.3V, IFSEL0=GND (SPI Mode) | PDOWN=PB5\r\n");
  printf("  Firmware: Build v1.4.2 [Release: 2026-11-08]\r\n");
  printf("=======================================================\r\n");

  RC663_Init(); /* PDOWN 硬件脉冲复位 + SoftReset 0x1F */

  RC663_Status_t selfTestStatus = RC663_SelfTest();
  if (selfTestStatus == RC663_OK)
  {
    if (RC663_LoadProtocolA() != RC663_OK)
    {
      printf("[APP] LoadProtocol failed, system stopped.\r\n");
    }
    else
    {
      printf("[APP] Reader Ready! Please place an ISO14443-A card near the antenna...\r\n");
    }
  }
  else
  {
    printf("[APP] RC663 SPI self-test failed! Please check hardware connections.\r\n");
  }
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  static uint8_t lastUid[10] = {0};
  static uint8_t lastUidLen = 0;
  static uint8_t cardPresent = 0;

  while (1)
  {
    if (selfTestStatus == RC663_OK)
    {
      uint8_t uid[10] = {0};
      uint8_t uidLen = 0;
      uint8_t sak = 0;
      uint8_t atqa[2] = {0};

      /* 执行 WUPA 唤醒寻卡、多层级防冲撞与选卡 */
      RC663_Status_t status = RC663_ReadCardUID(uid, &uidLen, &sak, atqa);

      if (status == RC663_OK)
      {
        /* 卡片进场判定：仅在新卡进入或 UID 改变时触发打印，杜绝串口疯狂刷屏 */
        if ((cardPresent == 0) || (lastUidLen != uidLen) || (memcmp(lastUid, uid, uidLen) != 0))
        {
          printf("\r\n-------------------------------------------------------\r\n");
          printf("[CARD] Card Detected! (卡片已进入)\r\n");
          printf("       UID(%u): ", (unsigned int)uidLen);
          for (uint8_t i = 0; i < uidLen; i++)
          {
            printf("%02X ", uid[i]);
          }
          printf("\r\n");

          printf("       ATQA:    0x%02X%02X\r\n", atqa[1], atqa[0]);
          printf("       SAK:     0x%02X", sak);

          /* 卡片类型详细语义解析 */
          if (sak & 0x20U)
          {
            printf(" (ISO/IEC 14443-4 PICC, e.g. DESFire / CPU Card)\r\n");
          }
          else if (sak == 0x08U)
          {
            printf(" (MIFARE Classic 1K)\r\n");
          }
          else if (sak == 0x18U)
          {
            printf(" (MIFARE Classic 4K)\r\n");
          }
          else if (sak == 0x00U)
          {
            printf(" (NTAG213/215/216 / MIFARE Ultralight)\r\n");
          }
          else
          {
            printf(" (Standard ISO14443-A Tag)\r\n");
          }

          /* 若为免密 NTAG / Ultralight 卡片，演示自动读取 Block 4..7 (16 字节数据) */
          if ((uidLen == 7) && (sak == 0x00))
          {
            uint8_t blockData[16] = {0};
            if (RC663_ReadBlock(4, blockData) == RC663_OK)
            {
              printf("       User Memory [Block 4..7]:\r\n       >> ");
              for (uint8_t i = 0; i < 16; i++)
              {
                printf("%02X ", blockData[i]);
                if ((i + 1) % 4 == 0) printf(" ");
              }
              printf("\r\n");
            }
          }

          printf("-------------------------------------------------------\r\n");

          memcpy(lastUid, uid, uidLen);
          lastUidLen = uidLen;
          cardPresent = 1;
        }

        /* 读取完毕后发送 HLTA 让卡片休眠，下轮轮询由 WUPA 再次唤醒 */
        RC663_HaltCard();
        HAL_Delay(100);
      }
      else
      {
        /* 卡片离场判定：此前在场但当前无响应，说明卡片已移走 */
        if (cardPresent != 0)
        {
          printf("[CARD] Card Removed (卡片已离开)\r\n");
          cardPresent = 0;
          lastUidLen = 0;
          memset(lastUid, 0, sizeof(lastUid));
        }

        HAL_Delay(100);
      }
    }
    else
    {
      HAL_Delay(1000);
    }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
