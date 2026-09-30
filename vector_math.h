/**
 * @file vector_math.h
 * @brief SIMD 向量化音频数学运算库头文件
 *
 * 为 FreeSWITCH mod_audio_fork 提供高性能 PCM 线性音频向量运算支持:
 * 1. 向量累加 (vector_add): 用于多路音频流线性叠加混音;
 * 2. 向量限幅归一化 (vector_normalize): 保证 16-bit 线性 PCM 幅度在 [-32768, 32767] 内, 杜绝整型溢出爆音;
 * 3. 细粒度分贝音量调节 (vector_change_sln_volume_granular): 提供 [-50dB, +50dB] 细粒度对数音量增益/衰减.
 *
 * 底层实现根据编译期配置与 CPU 指令集自动选择:
 * - AVX2: 单指令并行处理 16 个 int16 采样点 (256-bit 向量寄存器);
 * - SSE2: 单指令并行处理 8 个 int16 采样点 (128-bit 向量寄存器);
 * - Fallback: 标量循环处理 (无 SIMD 支持平台回退).
 */

#ifndef VECTOR_MATH_H
#define VECTOR_MATH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 向量累加: a[i] += b[i]
 *
 * 将两路等长的 S16LE PCM 音频数据逐采样点相加并覆盖写回源数组 a.
 *
 * @param a 目标/第一路音频采样数组指针 (原地累加并存储结果)
 * @param b 第二路音频采样数组指针
 * @param len 数组中包含的采样点总数 (samples = bytes / 2)
 */
void vector_add(int16_t* a, int16_t* b, size_t len);

/**
 * @brief 向量限幅归一化
 *
 * 将采样数组 a 中的每个元素饱和截断至 [-32768, 32767] 范围, 防止混音后爆音.
 *
 * @param a 音频采样数组指针 (原地限幅修改)
 * @param len 采样点总数
 */
void vector_normalize(int16_t* a, size_t len);

/**
 * @brief 细粒度音量分贝缩放
 *
 * 按照预计算的指数衰减/放大查找表调节 PCM 音频音量.
 *
 * @param data 音频采样数组指针 (原地调节)
 * @param samples 采样点总数
 * @param vol 细粒度音量等级, 范围 [-50, 50], 0 表示无改变 (直通), -50 表示静音 (比率 0.0)
 */
void vector_change_sln_volume_granular(int16_t* data, uint32_t samples, int32_t vol);

#ifdef __cplusplus
}
#endif

#endif /* VECTOR_MATH_H */
