#ifndef TIRTC_STAT_H_3PVFXZHS
#define TIRTC_STAT_H_3PVFXZHS

#include "tgtrp.h"
#include "tiRTC.h"

#ifdef __cplusplus
extern "C" {
#endif


/** 同步获取连接层耗时统计信息。
 *
 * 每个统计分组只保留最近 TGTRP_CONNECTION_TIME_STATS_WINDOW_SIZE 条有效样本，
 * 返回的是该窗口的汇总值（latest/min/max/avg）。
 *
 * \param hconn      连接句柄
 * \param time_stats  输出统计快照，成功时被完整填充
 * \return 0 表示成功；<0 为错误码
 */
TiRTC_EXPORT int TiRtcConnGetTimeStats(tirtc_conn_t hconn,
                                        tgtrp_connection_time_stats_t *time_stats);



typedef void (*TIRTCVIDEOSENDMONITOR)(const tgtrp_video_send_event_t* event, void *context);
TiRTC_EXPORT int TiRtcSetVideoSendMonitor(tirtc_conn_t hconn, uint8_t stream_id, TIRTCVIDEOSENDMONITOR cb, void *context);

/** 设置全局的连接方式选择标志 */
TiRTC_EXPORT void TiRtcSetConnFlag(int flag);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* end of include guard: TIRTC_STAT_H_3PVFXZHS */
