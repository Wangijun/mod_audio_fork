/*
    ******
    base64.hpp 是将 base64.cpp 和 base64.h 文件重新打包为
    单一头文件的版本，适合作为仅头文件库使用。此转换由
    Peter Thorson (webmaster@zaphoyd.com) 于 2012 年完成。
    所有代码修改均按照与原始代码相同的许可证重新分发，
    原始许可证如下所示。
    ******

   base64.cpp 和 base64.h

   版权所有 (C) 2004-2008 René Nyffenegger

   本源代码按"原样"提供，不附带任何明示或暗示的
   保证。在任何情况下，作者均不对因使用本软件而
   造成的任何损害承担责任。

   特此授权任何人出于任何目的使用本软件，包括
   商业应用，并可自由修改和重新分发，但须遵守
   以下限制：

   1. 不得歪曲本源代码的来源；不得声称您编写了
      原始源代码。如果您在产品中使用本源代码，
      建议在产品文档中致谢，但这并非必须。

   2. 修改后的源代码版本必须明确标注为修改版本，
      且不得歪曲为原始源代码。

   3. 不得从任何源代码分发中删除或修改本声明。

   René Nyffenegger rene.nyffenegger@adp-gmbh.ch

*/

#ifndef _BASE64_HPP_
#define _BASE64_HPP_

#include <string>

namespace drachtio {

static std::string const base64_chars =
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789+/";

/// 测试字符是否为有效的 base64 字符
/**
 * @param c 要测试的字符
 * @return 如果 c 是有效的 base64 字符则返回 true
 */
static inline bool is_base64(unsigned char c) {
    return (c == 43 || // +
           (c >= 47 && c <= 57) || // /-9
           (c >= 65 && c <= 90) || // A-Z
           (c >= 97 && c <= 122)); // a-z
}

/// 将字符缓冲区编码为 base64 字符串
/**
 * @param input 输入数据
 * @param len 输入数据的字节长度
 * @return 表示输入数据的 base64 编码字符串
 */
inline std::string base64_encode(unsigned char const * input, size_t len) {
    std::string ret;
    int i = 0;
    int j = 0;
    unsigned char char_array_3[3];
    unsigned char char_array_4[4];

    while (len--) {
        char_array_3[i++] = *(input++);
        if (i == 3) {
            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) +
                              ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) +
                              ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for(i = 0; (i <4) ; i++) {
                ret += base64_chars[char_array_4[i]];
            }
            i = 0;
        }
    }

    if (i) {
        for(j = i; j < 3; j++) {
            char_array_3[j] = '\0';
        }

        char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
        char_array_4[1] = ((char_array_3[0] & 0x03) << 4) +
                          ((char_array_3[1] & 0xf0) >> 4);
        char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) +
                          ((char_array_3[2] & 0xc0) >> 6);
        char_array_4[3] = char_array_3[2] & 0x3f;

        for (j = 0; (j < i + 1); j++) {
            ret += base64_chars[char_array_4[j]];
        }

        while((i++ < 3)) {
            ret += '=';
        }
    }

    return ret;
}

/// 将字符串编码为 base64 字符串
/**
 * @param input 输入数据
 * @return 表示输入数据的 base64 编码字符串
 */
inline std::string base64_encode(std::string const & input) {
    return base64_encode(
        reinterpret_cast<const unsigned char *>(input.data()),
        input.size()
    );
}

/// 将 base64 编码的字符串解码为原始字节字符串
/**
 * @param input base64 编码的输入数据
 * @return 表示解码后原始字节的字符串
 */
inline std::string base64_decode(std::string const & input) {
    size_t in_len = input.size();
    int i = 0;
    int j = 0;
    int in_ = 0;
    unsigned char char_array_4[4], char_array_3[3];
    std::string ret;

    while (in_len-- && ( input[in_] != '=') && is_base64(input[in_])) {
        char_array_4[i++] = input[in_]; in_++;
        if (i ==4) {
            for (i = 0; i <4; i++) {
                char_array_4[i] = static_cast<unsigned char>(base64_chars.find(char_array_4[i]));
            }

            char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

            for (i = 0; (i < 3); i++) {
                ret += char_array_3[i];
            }
            i = 0;
        }
    }

    if (i) {
        for (j = i; j <4; j++)
            char_array_4[j] = 0;

        for (j = 0; j <4; j++)
            char_array_4[j] = static_cast<unsigned char>(base64_chars.find(char_array_4[j]));

        char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

        for (j = 0; (j < i - 1); j++) {
            ret += static_cast<std::string::value_type>(char_array_3[j]);
        }
    }

    return ret;
}

} // namespace websocketpp

#endif // _BASE64_HPP_
