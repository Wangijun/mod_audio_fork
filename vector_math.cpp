/**
 * @file vector_math.cpp
 * @brief SIMD 向量化音频数学运算库实现
 *
 * 为 FreeSWITCH mod_audio_fork 提供高性能 PCM 线性音频向量运算实现:
 * 1. 向量累加 (vector_add)
 * 2. 向量动态限幅 (vector_normalize)
 * 3. 细粒度分贝音量调节 (vector_change_sln_volume_granular)
 *
 * 包含 AVX2 (256-bit)、SSE2 (128-bit) 以及无向量扩展时的通用标量实现.
 */

#include "vector_math.h"
#include <string.h>
#include <cstdlib>

/* 细粒度音量最大调节阶数 (支持 [-50dB, +50dB]) */
#define GRANULAR_VOLUME_MAX (50)

/* 16-bit 有符号 PCM 整数的极值范围 */
#define SMAX 32767
#define SMIN (-32768)

/**
 * @brief 基础 16 位限幅宏 (安全 do-while 结构)
 */
#define normalize_to_16bit_basic(n) \
  do { \
    if ((n) > SMAX) (n) = SMAX; \
    else if ((n) < SMIN) (n) = SMIN; \
  } while (0)

/**
 * @brief 细粒度音量等级范围裁剪宏 [-50, 50]
 */
#define normalize_volume_granular(x) \
  do { \
    if ((x) > GRANULAR_VOLUME_MAX) (x) = GRANULAR_VOLUME_MAX; \
    if ((x) < -GRANULAR_VOLUME_MAX) (x) = -GRANULAR_VOLUME_MAX; \
  } while (0)

namespace {
/* 细粒度正向音量增益放大系数查找表 (+1dB ~ +50dB)
 * 计算公式: rate = 10 ^ (vol / 20.0)
 */
static const float s_volume_pos_table[GRANULAR_VOLUME_MAX] = {
  1.122018f,   1.258925f,   1.412538f,   1.584893f,   1.778279f,
  1.995262f,   2.238721f,   2.511887f,   2.818383f,   3.162278f,
  3.548134f,   3.981072f,   4.466835f,   5.011872f,   5.623413f,
  6.309574f,   7.079458f,   7.943282f,   8.912509f,  10.000000f,
  11.220183f,  12.589254f,  14.125375f,  15.848933f,  17.782795f,
  19.952621f,  22.387213f,  25.118862f,  28.183832f,  31.622776f,
  35.481335f,  39.810719f,  44.668358f,  50.118729f,  56.234131f,
  63.095726f,  70.794586f,  79.432816f,  89.125107f, 100.000000f,
  112.201836f, 125.892517f, 141.253784f, 158.489334f, 177.827942f,
  199.526215f, 223.872070f, 251.188705f, 281.838318f, 316.227753f
};

/* 细粒度负向音量衰减缩减系数查找表 (-1dB ~ -50dB)
 * 计算公式: rate = 10 ^ (-vol / 20.0)
 * 特别说明: 最后一阶 -50 dB 特意硬编码为 0.000000f (完全静音), 避免微小底噪泄露.
 */
static const float s_volume_neg_table[GRANULAR_VOLUME_MAX] = {
  0.891251f, 0.794328f, 0.707946f, 0.630957f, 0.562341f,
  0.501187f, 0.446684f, 0.398107f, 0.354813f, 0.316228f,
  0.281838f, 0.251189f, 0.223872f, 0.199526f, 0.177828f,
  0.158489f, 0.141254f, 0.125893f, 0.112202f, 0.100000f,
  0.089125f, 0.079433f, 0.070795f, 0.063096f, 0.056234f,
  0.050119f, 0.044668f, 0.039811f, 0.035481f, 0.031623f,
  0.028184f, 0.025119f, 0.022387f, 0.019953f, 0.017783f,
  0.015849f, 0.014125f, 0.012589f, 0.011220f, 0.010000f,
  0.008913f, 0.007943f, 0.007079f, 0.006310f, 0.005623f,
  0.005012f, 0.004467f, 0.003981f, 0.003548f, 0.000000f
};
} // namespace

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/* 方案 A: AVX2 256 位宽向量化实现                                            */
/* ========================================================================= */
#if defined(USE_AVX2)
#include <immintrin.h>
#pragma message("使用 AVX2 SIMD.")

void vector_add(int16_t* a, int16_t* b, size_t len) {
  size_t i = 0;
  /* 每次循环并行处理 16 个 int16 采样点 (16 * 16 bits = 256 bits) */
  for (; i + 15 < len; i += 16) {
    __m256i va = _mm256_loadu_si256((const __m256i*)(a + i));
    __m256i vb = _mm256_loadu_si256((const __m256i*)(b + i));
    __m256i vc = _mm256_add_epi16(va, vb);
    _mm256_storeu_si256((__m256i*)(a + i), vc);
  }
  /* 标量循环处理剩余不足 16 个的尾部采样 */
  for (; i < len; ++i) {
    a[i] += b[i];
  }
}

void vector_normalize(int16_t* a, size_t len) {
  __m256i max_val = _mm256_set1_epi16(SMAX);
  __m256i min_val = _mm256_set1_epi16(SMIN);

  size_t i = 0;
  /* 每次循环并行检查并限幅 16 个采样点 */
  for (; i + 15 < len; i += 16) {
    __m256i values = _mm256_loadu_si256((__m256i*)(a + i));
    __m256i gt_max = _mm256_cmpgt_epi16(values, max_val);
    __m256i lt_min = _mm256_cmpgt_epi16(min_val, values);
    values = _mm256_blendv_epi8(values, max_val, gt_max);
    values = _mm256_blendv_epi8(values, min_val, lt_min);
    _mm256_storeu_si256((__m256i*)(a + i), values);
  }

  /* 处理尾部剩余采样点 */
  for (; i < len; ++i) {
    int32_t val = a[i];
    if (val > SMAX) a[i] = SMAX;
    else if (val < SMIN) a[i] = SMIN;
  }
}

void vector_change_sln_volume_granular(int16_t* data, uint32_t samples, int32_t vol) {
  if (vol == 0) return;
  normalize_volume_granular(vol);

  const float* chart = (vol > 0) ? s_volume_pos_table : s_volume_neg_table;
  uint32_t index = (uint32_t)abs(vol) - 1;
  float newrate = chart[index];

  if (newrate != 0.0f) {
    __m256 scale_factor_reg = _mm256_set1_ps(newrate);
    /* 每次循环处理 8 个采样点: 先由 16 位解包扩充为 32 位浮点计算, 再压缩截断回 16 位整数 */
    uint32_t processed_samples = samples - (samples % 8);
    for (uint32_t i = 0; i < processed_samples; i += 8) {
      __m128i data_ = _mm_loadu_si128((__m128i*)(data + i));
      __m256i data_32 = _mm256_cvtepi16_epi32(data_);

      __m256 data_float = _mm256_cvtepi32_ps(data_32);
      __m256 result = _mm256_mul_ps(data_float, scale_factor_reg);

      __m256i result_32 = _mm256_cvtps_epi32(result);

      /* 处理数值饱和范围 [-32768, 32767] */
      __m256i min_val = _mm256_set1_epi32(SMIN);
      __m256i max_val = _mm256_set1_epi32(SMAX);
      result_32 = _mm256_min_epi32(result_32, max_val);
      result_32 = _mm256_max_epi32(result_32, min_val);

      /* 32 位饱和压回 16 位 */
      __m128i result_16 = _mm_packs_epi32(_mm256_castsi256_si128(result_32), _mm256_extractf128_si256(result_32, 1));
      _mm_storeu_si128((__m128i*)(data + i), result_16);
    }

    /* 处理剩余尾部样本 */
    for (uint32_t i = processed_samples; i < samples; i++) {
      int32_t tmp = (int32_t)(data[i] * newrate);
      tmp = tmp > SMAX ? SMAX : (tmp < SMIN ? SMIN : tmp);
      data[i] = (int16_t)tmp;
    }
  } else {
    /* newrate 为 0 (例如 -50dB 完全静音) */
    memset(data, 0, samples * sizeof(int16_t));
  }
}

/* ========================================================================= */
/* 方案 B: SSE2 128 位宽向量化实现                                            */
/* ========================================================================= */
#elif defined(USE_SSE2)
#include <emmintrin.h>
#pragma message("使用 SSE2 SIMD.")

void vector_add(int16_t* a, int16_t* b, size_t len) {
  size_t i = 0;
  /* 每次循环并行处理 8 个 int16 采样点 (8 * 16 bits = 128 bits) */
  for (; i + 7 < len; i += 8) {
    __m128i va = _mm_loadu_si128((const __m128i*)(a + i));
    __m128i vb = _mm_loadu_si128((const __m128i*)(b + i));
    __m128i vc = _mm_add_epi16(va, vb);
    _mm_storeu_si128((__m128i*)(a + i), vc);
  }
  /* 标量循环处理剩余不足 8 个的尾部采样 */
  for (; i < len; ++i) {
    a[i] += b[i];
  }
}

void vector_normalize(int16_t* a, size_t len) {
  __m128i max_val = _mm_set1_epi16(SMAX);
  __m128i min_val = _mm_set1_epi16(SMIN);

  size_t i = 0;
  /* 每次循环并行检查并限幅 8 个采样点 */
  for (; i + 7 < len; i += 8) {
    __m128i values = _mm_loadu_si128((__m128i*)(a + i));
    __m128i gt_max = _mm_cmpgt_epi16(values, max_val);
    __m128i lt_min = _mm_cmpgt_epi16(min_val, values);
    __m128i max_masked = _mm_and_si128(gt_max, max_val);
    __m128i min_masked = _mm_and_si128(lt_min, min_val);
    __m128i other_masked = _mm_andnot_si128(_mm_or_si128(gt_max, lt_min), values);
    values = _mm_or_si128(_mm_or_si128(max_masked, min_masked), other_masked);
    _mm_storeu_si128((__m128i*)(a + i), values);
  }

  /* 处理剩余尾部采样 */
  for (; i < len; ++i) {
    if (a[i] > SMAX) a[i] = SMAX;
    else if (a[i] < SMIN) a[i] = SMIN;
  }
}

void vector_change_sln_volume_granular(int16_t* data, uint32_t samples, int32_t vol) {
  if (vol == 0) return;
  normalize_volume_granular(vol);

  const float* chart = (vol > 0) ? s_volume_pos_table : s_volume_neg_table;
  uint32_t index = (uint32_t)abs(vol) - 1;
  float newrate = chart[index];

  if (newrate != 0.0f) {
    int32_t tmp;
    int16_t *fp = data;
    for (uint32_t x = 0; x < samples; x++) {
      tmp = (int32_t)(fp[x] * newrate);
      normalize_to_16bit_basic(tmp);
      fp[x] = (int16_t)tmp;
    }
  } else {
    memset(data, 0, samples * sizeof(int16_t));
  }
}

/* ========================================================================= */
/* 方案 C: 无 SIMD 扩展时的通用标量 Fallback 实现                              */
/* ========================================================================= */
#else
#pragma message("不使用向量数学支持进行构建 (纯标量 Fallback).")

void vector_add(int16_t* a, int16_t* b, size_t len) {
  for (size_t i = 0; i < len; i++) {
    a[i] += b[i];
  }
}

void vector_normalize(int16_t* a, size_t len) {
  for (size_t i = 0; i < len; i++) {
    int32_t val = a[i];
    normalize_to_16bit_basic(val);
    a[i] = (int16_t)val;
  }
}

void vector_change_sln_volume_granular(int16_t* data, uint32_t samples, int32_t vol) {
  if (vol == 0) return;
  normalize_volume_granular(vol);

  const float* chart = (vol > 0) ? s_volume_pos_table : s_volume_neg_table;
  uint32_t index = (uint32_t)abs(vol) - 1;
  float newrate = chart[index];

  if (newrate != 0.0f) {
    int32_t tmp;
    int16_t *fp = data;
    for (uint32_t x = 0; x < samples; x++) {
      tmp = (int32_t)(fp[x] * newrate);
      normalize_to_16bit_basic(tmp);
      fp[x] = (int16_t)tmp;
    }
  } else {
    memset(data, 0, samples * sizeof(int16_t));
  }
}

#endif

#ifdef __cplusplus
}
#endif
