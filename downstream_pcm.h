#ifndef AUDIO_FORK_DOWNSTREAM_PCM_H
#define AUDIO_FORK_DOWNSTREAM_PCM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct af_pcm af_pcm_t;
/* sink 接收完整的输出率交错 PCM16 帧，成功返回 0；失败后本代际不可继续使用。 */
typedef int (*af_pcm_sink_t)(void *user, const int16_t *pcm, size_t bytes);
enum { AF_PCM_OK = 0, AF_PCM_FORMAT = -1, AF_PCM_RESAMPLE = -2,
       AF_PCM_SINK = -3, AF_PCM_PARTIAL = -4, AF_PCM_ENDED = -5 };

af_pcm_t *af_pcm_create(uint32_t source_rate, uint32_t output_rate, uint32_t channels, int quality);
void af_pcm_destroy(af_pcm_t **pcm);
int af_pcm_reset(af_pcm_t *pcm, uint32_t source_rate, uint32_t output_rate, uint32_t channels);
int af_pcm_write(af_pcm_t *pcm, const void *data, size_t bytes, af_pcm_sink_t sink, void *user);
/* 幂等结束；输出帧数为 ceil(输入帧数 * 输出率 / 源率)，排空滤波延迟但不延长时长。 */
int af_pcm_finish(af_pcm_t *pcm, af_pcm_sink_t sink, void *user);
const char *af_pcm_error(int error);

uint64_t af_monotonic_us(void);
typedef struct af_watchdog {
  uint64_t since_us;
  uint64_t seen_audio;
} af_watchdog_t;
/* audio_serial 在生产者写入有效 PCM 时递增，起播门控期间也会复位欠载。 */
int af_watchdog_expired(af_watchdog_t *watchdog, uint64_t audio_serial,
                        int received, int missing, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
