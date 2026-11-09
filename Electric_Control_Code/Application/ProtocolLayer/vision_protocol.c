/* Includes ------------------------------------------------------------------*/
#include "vision_protocol.h"

/* Private macro -------------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private typedef -----------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
extern uint32_t micros(void);

uint8_t vision_txBuf[64];	/* [CHANGED] was 40: tx frame struct only; RX frame is 22B, 64B leaves headroom */

/* Exported variables --------------------------------------------------------*/
extern UART_HandleTypeDef huart1;

vision_tx_info_t vision_tx_info = {
	.SOF = 0xA5,

};
vision_rx_info_t vision_rx_info;

/* Private functions ---------------------------------------------------------*/
/* Exported functions --------------------------------------------------------*/
/**
 * @brief  Send the vision sensor data
 * @param  vis_sen: Pointer to a vision_sensor_t structure
 * @retval None
 */
bool vision_send_data(void)
{
	memcpy(vision_txBuf, &vision_tx_info, sizeof(vision_tx_info_t));
	Append_CRC8_Check_Sum(vision_txBuf, 3);
	vision_tx_info.CRC8 = vision_txBuf[2];
	Append_CRC16_Check_Sum(vision_txBuf, sizeof(vision_tx_info_t));
	vision_tx_info.CRC16 = (uint16_t)((vision_txBuf[sizeof(vision_tx_info_t)-2] << 8)
                                   | vision_txBuf[sizeof(vision_tx_info_t)-1]);
	
	if(HAL_UART_Transmit_DMA(&huart1,vision_txBuf,sizeof(vision_tx_info_t)) == HAL_OK)
	{
			return true;
	}
	return false;
}


/*
 * [CHANGED] RX frame layout, MUST stay byte-identical with vision side:
 *   off  0 : uint8_t  SOF            = 0xA5
 *   off  1 : uint8_t  CRC8           header-area crc byte 1
 *   off  2 : uint8_t  CRC8_reserved  header-area crc byte 2 (written by vision)
 *   off  3 : float    pitch_error    pitch angle error (rad)   [NEW]
 *   off  7 : float    yaw_error      yaw angle error (rad)     [orig]
 *   off 11 : float    roll_error     roll angle error (rad)    [NEW]
 *   off 15 : float    distance       straight-line distance (m)[NEW]
 *   off 19 : uint8_t  target_status  0 = lost, 1 = found       [orig]
 *   off 20 : uint16_t CRC16          whole-frame crc
 *   total  : 22 bytes  (sizeof(vision_rx_info_t))
 * All lengths below use sizeof(), so no manual change is needed here.
 */
/**
 * @brief  Vision sensor data processing
 * @param  vis_sen: Pointer to a vision_sensor_t structure
 * @param  rxBuf: Pointer to the received data
 * @retval None
 */
void vision_update(vision_sensor_t *vis_sen, uint8_t *rxBuf)
{
	
	if(rxBuf[0] == 0xA5)
	{
		if(Verify_CRC8_Check_Sum(rxBuf, 3) == true)
		{
			if(Verify_CRC16_Check_Sum(rxBuf, sizeof(vision_rx_info_t)) == true)
			{
				memcpy(&vision_rx_info, rxBuf, sizeof(vision_rx_info_t));
				vis_sen->info->rx_flag = 1;
				
				return;
			}
		}
	}
	vis_sen->info->rx_flag = 0;
}

void vision_check(vision_sensor_t *vis_sen)
{
	vision_rx_info_t *rx_info = vis_sen->info->rx_info;
	vision_info_t *info = vis_sen->info;
	
	info->offline_cnt = 0;
	
	if (info->rx_flag == 1)
	{
		memcpy(&info->yaw_error, (void*)&rx_info->yaw_error, 4);		/* [orig] mirror yaw error (rad) */
		memcpy(&info->pitch_error, (void*)&rx_info->pitch_error, 4);	/* [NEW] mirror pitch error (rad) */
		memcpy(&info->roll_error, (void*)&rx_info->roll_error, 4);		/* [NEW] mirror roll error (rad) */
		memcpy(&info->distance, (void*)&rx_info->distance, 4);		/* [NEW] mirror straight-line distance (m) */
		memcpy(&info->target_status, &rx_info->target_status, 1);	/* [orig] mirror target status 0/1 */
	}
}

void USART1_rxDataHandler(uint8_t *rxBuf)
{
	vision_sensor.update(&vision_sensor, rxBuf);
	vision_sensor.check(&vision_sensor);

}
