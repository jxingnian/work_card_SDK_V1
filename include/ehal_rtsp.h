/******************************************************************************
Copyright (C), 2023-2028, Ebaina Technology Community (www.ebaina.com)
FilePath: ehal_rtsp.h
Version: 0.1.0
Description: RTSP server封装接口，提供简化的RTSP推流功能
******************************************************************************/

#ifndef EHAL_RTSP_H
#define EHAL_RTSP_H

#include "ehal_common.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RTSP服务器配置
 */
typedef struct ehal_rtsp_config {
    int port;                           /* RTSP服务端口，默认8554 */
    const char *stream_name;            /* 流名称，默认"live" */
    const char *config_path;            /* 配置文件路径，可选 */
} ehal_rtsp_config_t;

/**
 * @brief RTSP服务器句柄（不透明指针）
 */
typedef struct ehal_rtsp_server ehal_rtsp_server_t;

/**
 * @brief 创建并启动RTSP服务器
 *
 * @param server 输出RTSP服务器句柄
 * @param config RTSP配置，如果为NULL则使用默认配置
 * @return EHAL_OK成功，其他失败
 */
int ehal_rtsp_server_create(ehal_rtsp_server_t **server, const ehal_rtsp_config_t *config);

/**
 * @brief 推送视频帧到RTSP服务器
 *
 * @param server RTSP服务器句柄
 * @param stream_index 流索引，通常为0
 * @param data H.264/H.265编码数据
 * @param size 数据大小
 * @return EHAL_OK成功，其他失败
 */
int ehal_rtsp_server_push_video(ehal_rtsp_server_t *server,
                                int stream_index,
                                const uint8_t *data,
                                uint32_t size);

/**
 * @brief 推送音频帧到RTSP服务器
 *
 * @param server RTSP服务器句柄
 * @param stream_index 流索引，通常为0
 * @param data 音频编码数据
 * @param size 数据大小
 * @param nb_samples 采样点数
 * @return EHAL_OK成功，其他失败
 */
int ehal_rtsp_server_push_audio(ehal_rtsp_server_t *server,
                                int stream_index,
                                const uint8_t *data,
                                uint32_t size,
                                uint32_t nb_samples);

/**
 * @brief 停止并销毁RTSP服务器
 *
 * @param server RTSP服务器句柄
 */
void ehal_rtsp_server_destroy(ehal_rtsp_server_t *server);

#ifdef __cplusplus
}
#endif

#endif /* EHAL_RTSP_H */
