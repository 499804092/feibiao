/**
 ******************************************************************************
 * @file    vision_sensor.c
 * @author  RobotPilots
 * @brief   Vision sensor driver
 ******************************************************************************
 * @attention
 * 
 * Copyright 2024 RobotPilots
 * 
 * @Version     V1.0
 * @date        August-2024
 ******************************************************************************
 */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __VISION_SENSOR_H
#define __VISION_SENSOR_H

/* Includes ------------------------------------------------------------------*/
#include "rp_config.h"
#include "rp_math.h"
#include "string.h"
#include "crc.h"

/* Exported macro ------------------------------------------------------------*/
/* Exported types ------------------------------------------------------------*/
typedef enum {
	AIM_OFF 		    = 0x00,	// 不启动自瞄
	AIM_ON			    = 0x01,	// 启动自瞄
	RECORD				= 0x02,
} vision_cmd_e;

typedef __packed struct 
{
	uint8_t  			SOF;
	uint8_t		  		mode;        		// 0:不开自瞄，1：开自瞄 2；仅录像
	uint8_t  			CRC8;
	uint8_t		  		state;
	uint8_t				number;				// 第几发飞镖，取值0--4
	uint8_t				done;
	uint16_t 			CRC16;
}vision_tx_info_t;

/* ==========================================================================
 * 【本任务核心改动 4】视觉下发给飞控板的接收结构体（必须与视觉端
 *   Vision_Code/src/utility/inc/global/Struct.hpp 里的 VisionData 逐字节一致）
 * --------------------------------------------------------------------------
 * 字节布局（共 22 字节，__packed 保证无填充）：
 *   偏移  0 : uint8_t  SOF            帧头 0xA5
 *   偏移  1 : uint8_t  CRC8           帧头区校验字节1
 *   偏移  2 : uint8_t  CRC8_reserved  帧头区校验字节2（视觉端把 CRC8 写在这里）
 *   偏移  3 : float    pitch_error    俯仰角误差(rad)   【新增】
 *   偏移  7 : float    yaw_error      偏航角误差(rad)   【原有】
 *   偏移 11 : float    roll_error     横滚角误差(rad)   【新增】
 *   偏移 15 : float    distance       到目标直线距离(m) 【新增】
 *   偏移 19 : uint8_t  target_status  0:lost 1:find     【原有】
 *   偏移 20 : uint16_t CRC16          整帧 CRC16 校验
 * 注意：新增字段全部插在帧头区与 target_status 之间，解析逻辑不用改，
 *       重新编译即可自动适配新的帧长。
 * ========================================================================== */
typedef __packed struct 
{
	uint8_t			    SOF;			/* 帧头，固定 0xA5 */
	//vision_cmd_e  		mode;           // 
	uint8_t 			CRC8;			/* 帧头区校验字节1 */
	uint8_t 			CRC8_reserved;	/* 【新增】帧头区校验字节2，凑满 3 字节帧头区 */
	float    			pitch_error;	/* 【新增】俯仰角误差(rad)，正=目标偏上，需抬高枪口 */
	float    			yaw_error;		/* 【原有】偏航角误差(rad)，正=目标偏右，需右转 */
	float    			roll_error;		/* 【新增】横滚角误差(rad)，由视觉按目标框倾斜量解算 */
	float    			distance;		/* 【新增】到目标的直线距离(m)，视觉单目测距给出 */
	uint8_t  			target_status; 	/* 0:lost 1;find 目标是否可见 */
	uint16_t 			CRC16;			/* 整帧 CRC16 */
}vision_rx_info_t;

/* 编译期自检：接收结构体必须是 22 字节；以后加字段忘了同步，这里立刻编译报错 */
_Static_assert(sizeof(vision_rx_info_t) == 22, "vision_rx_info_t size mismatch with vision side!");


typedef struct
{
	vision_tx_info_t  *tx_info;
	vision_rx_info_t  *rx_info;

	float    pitch_error;	/* 【新增】俯仰角误差(rad)，接收帧解析后镜像到这里 */
	float    yaw_error;		/* 【原有】偏航角误差(rad) */
	float    roll_error;		/* 【新增】横滚角误差(rad) */
	float    distance;		/* 【新增】到目标直线距离(m) */
	uint8_t  target_status;	/* 目标状态 0:lost 1:find */
	
	uint8_t  rx_flag;
	int16_t	 offline_cnt;
	int16_t	 offline_max_cnt;
}vision_info_t;

typedef struct vision_sensor_struct {
	vision_info_t	    *info;
	drv_uart_t		    *driver;
	void				     (*init)(struct vision_sensor_struct *self);
	void				     (*update)(struct vision_sensor_struct *self, uint8_t *rxBuf);
	void				     (*check)(struct vision_sensor_struct *self);	
	void				     (*heart_beat)(struct vision_sensor_struct *self);
	dev_work_state_t	work_state;
	dev_errno_t			  errno;
	dev_id_t			    id;
} vision_sensor_t;


extern vision_tx_info_t vision_tx_info;
extern vision_rx_info_t vision_rx_info;
extern vision_sensor_t vision_sensor;

/* Exported functions --------------------------------------------------------*/
extern bool vision_send_data(void);

#endif
