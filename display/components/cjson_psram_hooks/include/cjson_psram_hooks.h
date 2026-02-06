//
// Created by Shane_Hwang on 2025/7/11.
//

#ifndef CJSON_PSRAM_HOOKS_H
#define CJSON_PSRAM_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

	/**
	 * @brief 初始化 cJSON，讓它使用 PSRAM 作為記憶體配置。
	 */
	void cjson_init_with_psram(void);

#ifdef __cplusplus
}
#endif

#endif // CJSON_PSRAM_HOOKS_H