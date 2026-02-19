//
// Created by Shane_Hwang on 2025/5/6.
//

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_peripherals.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "audio_mem.h"
#include "manual_ota.h"
#include "ota_proc_default.h"
#include "ota_service.h"
#include "s3_nvs_item.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

static const char *TAG = "MANUAL_OTA";
static EventGroupHandle_t events = NULL;

#define OTA_FINISH (BIT0)
static esp_err_t ota_result_status = ESP_FAIL;

extern void cli_disable_console(void);
extern void Shotdown_Task_ForOTA(void);
esp_err_t set_sn(char *sn) {
    if (sn == NULL || strlen(sn) > 16) {
        ESP_LOGE(TAG, "sn address param is NULL or len > 16");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        goto exit;
    } else {
        ESP_LOGI(TAG, "Write %s to NVS ... ", sn);
        err = nvs_set_str(my_handle, "sn", sn);
        if (err != ESP_OK) {
            goto exit;
        }
        err = nvs_commit(my_handle);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "set_sn:success");
        return ESP_OK;
    }
exit:
    ESP_LOGE(TAG, "set_sn:failed");
    return err;
}

esp_err_t get_sn(char *sn) {
    if (sn == NULL) {
        ESP_LOGE(TAG, "sn address param is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        goto exit;
    } else {
        size_t len = 16;
        err = nvs_get_str(my_handle, "sn", sn, &len);
        if (err != ESP_OK) {
            goto exit;
        }
        nvs_close(my_handle);
        ESP_LOGI(TAG, "get_sn:%s", sn);
        return ESP_OK;
    }
exit:
    ESP_LOGE(TAG, "get_sn:failed");
    return err;
}

static esp_err_t ota_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx) {
    if (evt->type == OTA_SERV_EVENT_TYPE_RESULT) {
        ota_result_t *result_data = evt->data;
        ota_result_status = result_data->result;
        if (result_data->result != ESP_OK) {
            ESP_LOGE(TAG, "List id: %d, OTA failed", result_data->id);
        } else {
            ESP_LOGI(TAG, "List id: %d, OTA success", result_data->id);
        }
    } else if (evt->type == OTA_SERV_EVENT_TYPE_FINISH) {
        xEventGroupSetBits(events, OTA_FINISH);
    }

    return ESP_OK;
}

void print_app_version(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_app_desc_t app_desc;
    esp_ota_get_partition_description(running, &app_desc);

    ESP_LOGW(TAG, "fw_version:%s", app_desc.version);
}

esp_err_t fw_version(esp_periph_handle_t periph, int argc, char *argv[]) {
    print_app_version();
    return ESP_OK;
}

esp_err_t ota_main(char *uri, int checkVer) {
    //    ESP_LOGI(TAG, "shane version: %s", CONFIG_APP_PROJECT_VER);
    // NOTE: NVS should already be initialized in main.c, so we skip NVS init here
    // to prevent accidental NVS erase during OTA which would wipe all stored values
    // If NVS is not initialized, it will be handled in the main program, not during OTA
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
    ESP_ERROR_CHECK(esp_netif_init());
#else
    tcpip_adapter_init();
#endif

    ESP_LOGI(TAG, "[1.0] Initialize peripherals management");
    //    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    //    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

    ESP_LOGI(TAG, "[1.1] check Wi-Fi connected");
    wifi_ap_record_t ap_info = {0};
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    if (ret == ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGI(TAG, "Not connected Wi-Fi");
        return ESP_FAIL;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);
    AUDIO_MEM_SHOW(TAG);
    if (checkVer) {
        cli_disable_console();
        Shotdown_Task_ForOTA();
    }
    AUDIO_MEM_SHOW(TAG);

    //    ESP_LOGI(TAG, "[1.2] Mount SDCard");
    //    audio_board_sdcard_init(set, SD_MODE_1_LINE);
    print_app_version();

    ESP_LOGI(TAG, "[2.0] Create OTA service");
    ota_service_config_t ota_service_cfg = OTA_SERVICE_DEFAULT_CONFIG();
    ota_service_cfg.task_stack = 4 * 1024;
	ota_service_cfg.task_prio = 21;
    ota_service_cfg.evt_cb = ota_service_cb;
    ota_service_cfg.cb_ctx = NULL;
    periph_service_handle_t ota_service = ota_service_create(&ota_service_cfg);
    events = xEventGroupCreate();

    ESP_LOGI(TAG, "[2.1] Set upgrade list %s", uri);
    ota_upgrade_ops_t upgrade_list[] = {{{ESP_PARTITION_TYPE_APP, NULL, uri, NULL}, NULL, NULL, NULL, NULL, true, false}};

    cei_ota_app_get_default_proc(&upgrade_list[0], checkVer);

    ota_service_set_upgrade_param(ota_service, upgrade_list, sizeof(upgrade_list) / sizeof(ota_upgrade_ops_t));

    ESP_LOGI(TAG, "[2.2] Start OTA service");
    AUDIO_MEM_SHOW(TAG);
    ret = periph_service_start(ota_service);
	if (ret != ESP_OK)
	{
		ESP_LOGI(TAG, "[2.2] Start OTA service ret=%d",ret);
	}else
	{
		EventBits_t bits = xEventGroupWaitBits(events, OTA_FINISH, true, false, portMAX_DELAY);
		if (bits & OTA_FINISH) {
			ESP_LOGI(TAG, "[2.3] Finish OTA service");
		    ret = ota_result_status;
		}else
		{
			ESP_LOGI(TAG, "[2.3] OTA service Fail timeout");
			ret = ESP_FAIL;
		}
	}

    ESP_LOGI(TAG, "[2.4] Clear OTA service");
    periph_service_destroy(ota_service);
    vEventGroupDelete(events);
	return ret;
}

esp_err_t manual_ota(esp_periph_handle_t periph, int argc, char *argv[]) {
    if (argc == 2) {
        // howto manual ota : https://redmine.pixseecare.com:8081/projects/ipg24d_fw/wiki/HowToOTA
        ESP_LOGI(TAG, "argc=%d,http_server=%s,check version=%s", argc, argv[0], argv[1]);
        if (strcmp(argv[1], "off") == 0)
            ota_main(argv[0], 0);
        else
            ota_main(argv[0], 1);
    } else {
        ESP_LOGI(TAG, "download dc image from cloud:");

    	int domain = DOMAIN_PRODUCTION;
    	if (s3_nvs_get(NVS_S3_SW_CLOUD_DOMAIN, &domain) != ESP_OK)
    	{
    		ESP_LOGE(TAG, "Failed to get DOMAIN from NVS, using default value");
    		domain = DOMAIN_PRODUCTION;
    	}

    	if (domain == DOMAIN_PRODUCTION)
    		ota_main("https://s3stgcontent.blob.core.windows.net/s3-firmware/cli_app.bin", 1);
    	else if (domain == DOMAIN_STAGING)
    		ota_main("https://s3stgcontent.blob.core.windows.net/s3-firmware/cli_app.bin", 1);
    	else if (domain == DOMAIN_DEVELOPER)
    		ota_main("https://s3devcontent.blob.core.windows.net/s3-firmware/cli_app.bin", 1);

    }
    return ESP_OK;
}
