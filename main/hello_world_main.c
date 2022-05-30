/*
 * Copyright 2010-2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * Additions Copyright 2016 Espressif Systems (Shanghai) PTE LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
/**
 * @file subscribe_publish_sample.c
 * @brief simple MQTT publish and subscribe on the same topic
 *
 * This example takes the parameters from the build configuration and establishes a connection to the AWS IoT MQTT Platform.
 * It subscribes and publishes to the same topic - "test_topic/esp32"
 *
 * Some setup is required. See example README for details.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_mqtt_client_interface.h"

#include <esp_idf_version.h>
#include "esp_task_wdt.h"
#include "esp_mac.h"
#include "esp_spiffs.h"

#include "cJSON.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0)
// Features supported in 4.1+
#define ESP_NETIF_SUPPORTED
#endif

enum AWS_prov_state{
    CERTIFICATE_AND_KEY,
    PROVISIONING,
    THING_ACTIVE
};

enum AWS_prov_state aws_prov_state = CERTIFICATE_AND_KEY;
cJSON *aws_prov_response;

static const char *TAG = "subpub";
static const char *TAG_SPIFF = "SPIFF";

/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.
   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* Semaphore to indicate the start of the start of provisioning process */
SemaphoreHandle_t xSemaphore_provision;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;


/* CA Root certificate, device ("Thing") certificate and device
 * ("Thing") key.
   Example can be configured one of two ways:
   "Embedded Certs" are loaded from files in "certs/" and embedded into the app binary.
   "Filesystem Certs" are loaded from the filesystem (SD card, etc.)
   See example README for more details.
*/
#define CONFIG_EXAMPLE_CERTIFICATE_PATH "/spiffs/certificate_pem_crt"
#define CONFIG_EXAMPLE_PRIVATE_KEY_PATH "/spiffs/private_pem_key"


extern const uint8_t aws_root_ca_pem_start[] asm("_binary_aws_root_ca_pem_start");
extern const uint8_t aws_root_ca_pem_end[] asm("_binary_aws_root_ca_pem_end");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t certificate_pem_crt_end[] asm("_binary_certificate_pem_crt_end");
extern const uint8_t private_pem_key_start[] asm("_binary_private_pem_key_start");
extern const uint8_t private_pem_key_end[] asm("_binary_private_pem_key_end");


static const char * DEVICE_CERTIFICATE_PATH = CONFIG_EXAMPLE_CERTIFICATE_PATH;
static const char * DEVICE_PRIVATE_KEY_PATH = CONFIG_EXAMPLE_PRIVATE_KEY_PATH;
//static const char * ROOT_CA_PATH = CONFIG_EXAMPLE_ROOT_CA_PATH;


/**
 * @brief Default MQTT HOST URL is pulled from the aws_iot_config.h
 */
char HostAddress[255] = AWS_IOT_MQTT_HOST;

/**
 * @brief Default MQTT port is pulled from the aws_iot_config.h
 */
uint32_t port = AWS_IOT_MQTT_PORT;

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        /* Signal main application to continue execution */
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    }
}

void iot_subscribe_callback_handler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
                                    IoT_Publish_Message_Params *params, void *pData) {
    
    char topic_name[128];
    sprintf(topic_name,"%.*s", topicNameLen, topicName);

    switch(aws_prov_state){
        case CERTIFICATE_AND_KEY:

            if(strcmp(topic_name,"$aws/certificates/create/json/accepted") == 0){
                ESP_LOGI(TAG, "Recieved unique certificate and private key ");
                /* We have received unique certificate and key so we save credentials for provisioning */
                aws_prov_response = cJSON_Parse(params->payload);

                // Use POSIX and C standard library functions to work with files.
                // First create a file.
                ESP_LOGI(TAG_SPIFF, "Opening file");
                FILE* f = fopen("/spiffs/certificate_pem_crt", "w");
                if (f == NULL) {
                    ESP_LOGE(TAG_SPIFF, "Failed to open file for writing");
                    abort();
                }
                fprintf(f, "%s",cJSON_GetObjectItem(aws_prov_response,"certificatePem")->valuestring);
                fclose(f);
                ESP_LOGI(TAG_SPIFF, "Certificate File written");

                f = fopen("/spiffs/private_pem_key", "w");
                if (f == NULL) {
                    ESP_LOGE(TAG_SPIFF, "Failed to open file for writing");
                    abort();
                }
                fprintf(f, "%s",cJSON_GetObjectItem(aws_prov_response,"privateKey")->valuestring);
                fclose(f);
                ESP_LOGI(TAG_SPIFF, "Private Key written");

                aws_prov_state = PROVISIONING;

                static BaseType_t xHigherPriorityTaskWoken;
                xHigherPriorityTaskWoken = pdFALSE;
                xSemaphoreGiveFromISR( xSemaphore_provision, &xHigherPriorityTaskWoken );
                portYIELD_FROM_ISR( xHigherPriorityTaskWoken );
            }
            else{
                /* Claim certificate was rejected */
                abort();
            }
            break;
        case PROVISIONING:
            if(strcmp(topic_name,"$aws/provisioning-templates/custom_esp_prov_template/provision/json/accepted") == 0){
                ESP_LOGI(TAG, "THING was created sucessfully");
                aws_prov_state = THING_ACTIVE;
                /* Restart device to use updated certificate and private key */
                esp_restart();
            }
        default:
            break;
    };

    ESP_LOGI(TAG, "Subscribe callback");
    ESP_LOGI(TAG, "Topic Name :  %.*s", topicNameLen, topicName);   
    ESP_LOGI(TAG, "%.*s\t%.*s", topicNameLen, topicName, (int) params->payloadLen, (char *)params->payload);

}

void disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data) {
    ESP_LOGW(TAG, "MQTT Disconnect");
    IoT_Error_t rc = FAILURE;

    if(NULL == pClient) {
        return;
    }

    if(aws_iot_is_autoreconnect_enabled(pClient)) {
        ESP_LOGI(TAG, "Auto Reconnect is enabled, Reconnecting attempt will start now");
    } else {
        ESP_LOGW(TAG, "Auto Reconnect not enabled. Starting manual reconnect...");
        rc = aws_iot_mqtt_attempt_reconnect(pClient);
        if(NETWORK_RECONNECTED == rc) {
            ESP_LOGW(TAG, "Manual Reconnect Successful");
        } else {
            ESP_LOGW(TAG, "Manual Reconnect Failed - %d", rc);
        }
    }
}

void aws_iot_task(void *param) {

    /* Create the provisioning semaphore */
    xSemaphore_provision = xSemaphoreCreateBinary();

    char cPayload[1024];


    IoT_Error_t rc = FAILURE;

    AWS_IoT_Client client;
    IoT_Client_Init_Params mqttInitParams = iotClientInitParamsDefault;
    IoT_Client_Connect_Params connectParams = iotClientConnectParamsDefault;

    IoT_Publish_Message_Params paramsQOS0;
    IoT_Publish_Message_Params paramsQOS1;

    ESP_LOGI(TAG, "AWS IoT SDK Version %d.%d.%d-%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

    mqttInitParams.enableAutoReconnect = false; // We enable this later below
    mqttInitParams.pHostURL = HostAddress;
    mqttInitParams.port = port;

    /* Check whether device unique certificate and private key are present in SPIFFS */
    /* This will help determine whether it has been provisioned or not */

    struct stat st_cert, st_key;
    if (stat(CONFIG_EXAMPLE_CERTIFICATE_PATH, &st_cert) != 0 || stat(CONFIG_EXAMPLE_PRIVATE_KEY_PATH, &st_key) != 0 ) {
        /* Credentials not available in SPIFFS, proceed to provision */
        ESP_LOGI(TAG, "Device not provisioned, initiating using claim certificate and key");
        aws_prov_state = CERTIFICATE_AND_KEY;
        mqttInitParams.pRootCALocation = (const char *)aws_root_ca_pem_start;
        mqttInitParams.pDeviceCertLocation = (const char *)certificate_pem_crt_start;
        mqttInitParams.pDevicePrivateKeyLocation = (const char *)private_pem_key_start;
    }
    else{
        /* Device is already provisioned and credentials available in SPIFFS */
        ESP_LOGI(TAG, "Device already provisioned, retreiving certificate and key from SPIFFS");
        aws_prov_state = THING_ACTIVE;
        mqttInitParams.pRootCALocation = (const char *)aws_root_ca_pem_start;
        mqttInitParams.pDeviceCertLocation = DEVICE_CERTIFICATE_PATH;
        mqttInitParams.pDevicePrivateKeyLocation = DEVICE_PRIVATE_KEY_PATH;

    }

    mqttInitParams.mqttCommandTimeout_ms = 20000;
    mqttInitParams.tlsHandshakeTimeout_ms = 5000;
    mqttInitParams.isSSLHostnameVerify = true;
    mqttInitParams.disconnectHandler = disconnectCallbackHandler;
    mqttInitParams.disconnectHandlerData = NULL;

#ifdef CONFIG_EXAMPLE_SDCARD_CERTS
    ESP_LOGI(TAG, "Mounting SD card...");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 3,
    };
    sdmmc_card_t* card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card VFAT filesystem. Error: %s", esp_err_to_name(ret));
        abort();
    }
#endif

    rc = aws_iot_mqtt_init(&client, &mqttInitParams);
    if(SUCCESS != rc) {
        ESP_LOGE(TAG, "aws_iot_mqtt_init returned error : %d ", rc);
        abort();
    }

    /* Wait for WiFI to show as connected */
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
                        false, true, portMAX_DELAY);

    connectParams.keepAliveIntervalInSec = 10;
    connectParams.isCleanSession = true;
    connectParams.MQTTVersion = MQTT_3_1_1;
    /* Client ID is set in the menuconfig of the example */
    uint8_t chipId[6];
    char UUID[20];
    esp_efuse_mac_get_default(chipId);
    sprintf(UUID,"%02x%02x%02x%02x%02x%02x", chipId[0], chipId[1], chipId[2], chipId[3], chipId[4], chipId[5]);
    connectParams.pClientID = UUID;
    connectParams.clientIDLen = (uint16_t) strlen(UUID);
    connectParams.isWillMsgPresent = false;

    ESP_LOGI(TAG, "Connecting to AWS...");
    do {
        rc = aws_iot_mqtt_connect(&client, &connectParams);
        if(SUCCESS != rc) {
            ESP_LOGE(TAG, "Error(%d) connecting to %s:%d", rc, mqttInitParams.pHostURL, mqttInitParams.port);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
    } while(SUCCESS != rc);

    /*
     * Enable Auto Reconnect functionality. Minimum and Maximum time of Exponential backoff are set in aws_iot_config.h
     *  #AWS_IOT_MQTT_MIN_RECONNECT_WAIT_INTERVAL
     *  #AWS_IOT_MQTT_MAX_RECONNECT_WAIT_INTERVAL
     */
    rc = aws_iot_mqtt_autoreconnect_set_status(&client, true);
    if(SUCCESS != rc) {
        ESP_LOGE(TAG, "Unable to set Auto Reconnect to true - %d", rc);
        abort();
    }

    const char *TOPIC = "test_topic/esp32";
    const int TOPIC_LEN = strlen(TOPIC);

    paramsQOS0.payload = (void *) cPayload;
    paramsQOS1.payload = (void *) cPayload;

    if(aws_prov_state == CERTIFICATE_AND_KEY){
        /* Use sing claim certificate and key to obtain unique cert and key and initiate prvisioning */
        /* 1. Subscribe to create certificate response topic accepted */
        const char *CREATE_CERT_RESPONSE_TOPIC_ACCEPTED = "$aws/certificates/create/json/accepted";
        const int CREATE_CERT_RESPONSE_TOPIC_ACCEPTED_LEN = strlen(CREATE_CERT_RESPONSE_TOPIC_ACCEPTED);
        
        ESP_LOGI(TAG, "Subscribing to  create certificate response accepted topic");
        rc = aws_iot_mqtt_subscribe(&client, CREATE_CERT_RESPONSE_TOPIC_ACCEPTED, CREATE_CERT_RESPONSE_TOPIC_ACCEPTED_LEN, QOS1, iot_subscribe_callback_handler, NULL);
        if(SUCCESS != rc) {
            ESP_LOGE(TAG, "Error subscribing to  create certificate response accepted topic: %d ", rc);
            abort();
        }
        
        
        /* 2. Subscribe to create certificate response rejected */
        const char *CREATE_CERT_RESPONSE_TOPIC_REJECTED = "$aws/certificates/create/json/rejected";
        const int CREATE_CERT_RESPONSE_TOPIC_REJECTED_LEN = strlen(CREATE_CERT_RESPONSE_TOPIC_REJECTED);
        
        ESP_LOGI(TAG, "Subscribing to  create certificate response  rejected topic");
        rc = aws_iot_mqtt_subscribe(&client, CREATE_CERT_RESPONSE_TOPIC_REJECTED, CREATE_CERT_RESPONSE_TOPIC_REJECTED_LEN, QOS1, iot_subscribe_callback_handler, NULL);
        if(SUCCESS != rc) {
            ESP_LOGE(TAG, "Error subscribing to  create certificate response  rejected topic: %d ", rc);
            abort();
        }

        /* 3. Subscribe to register thing response topics rejected */
        const char *PROVISIONING_RESPONSE_TOPIC_REJECTED = "$aws/provisioning-templates/custom_esp_prov_template/provision/json/rejected";
        const int PROVISIONING_RESPONSE_TOPIC_REJECTED_LEN = strlen(PROVISIONING_RESPONSE_TOPIC_REJECTED);
                
        ESP_LOGI(TAG, "Subscribing to  provisioning response  rejected topic");
        rc = aws_iot_mqtt_subscribe(&client, PROVISIONING_RESPONSE_TOPIC_REJECTED, PROVISIONING_RESPONSE_TOPIC_REJECTED_LEN, QOS1, iot_subscribe_callback_handler, NULL);
        if(SUCCESS != rc) {
            ESP_LOGE(TAG, "Error subscribing to provisioning response rejected topic: %d ", rc);
            abort();
        }

        /* 4. Subscribe to register thing response topics accepted */
        const char *PROVISIONING_RESPONSE_TOPIC_ACCEPTED = "$aws/provisioning-templates/custom_esp_prov_template/provision/json/accepted";
        const int PROVISIONING_RESPONSE_TOPIC_ACCEPTED_LEN = strlen(PROVISIONING_RESPONSE_TOPIC_ACCEPTED);
        
        ESP_LOGI(TAG, "Subscribing to  provisioning response  accepted topic");
        rc = aws_iot_mqtt_subscribe(&client, PROVISIONING_RESPONSE_TOPIC_ACCEPTED, PROVISIONING_RESPONSE_TOPIC_ACCEPTED_LEN, QOS1, iot_subscribe_callback_handler, NULL);
        if(SUCCESS != rc) {
            ESP_LOGE(TAG, "Error subscribing to provisioning response accepted topic: %d ", rc);
            abort();
        }

        /* 5. Publish to create a topic with empty payload */
        ESP_LOGI(TAG, "Requested for unique certificate");
        const char *CREATE_CERT_TOPIC = "$aws/certificates/create/json";
        const int CREATE_CERT_TOPIC_LEN = strlen(CREATE_CERT_TOPIC);

        paramsQOS0.qos = QOS0;
        sprintf(cPayload, "{}");
        paramsQOS0.payloadLen = strlen(cPayload);
        rc = aws_iot_mqtt_publish(&client, CREATE_CERT_TOPIC, CREATE_CERT_TOPIC_LEN, &paramsQOS0);
        if (rc == MQTT_REQUEST_TIMEOUT_ERROR) {
            ESP_LOGW(TAG, "QOS0 publish ack not received.");
            rc = SUCCESS;
        }
    }
    else if(aws_prov_state == THING_ACTIVE){
        ESP_LOGI(TAG, "Subscribing...");
        rc = aws_iot_mqtt_subscribe(&client, TOPIC, TOPIC_LEN, QOS0, iot_subscribe_callback_handler, NULL);
        if(SUCCESS != rc) {
            ESP_LOGE(TAG, "Error subscribing : %d ", rc);
            abort();
        }
    }

    while(1) {
        esp_task_wdt_reset();

        if( xSemaphoreTake( xSemaphore_provision, ( TickType_t ) 0 ) == pdTRUE ){

            esp_task_wdt_reset();

            /* 6. Publish to register thing topic with parameters */
            ESP_LOGI(TAG, "Publishing to register thing in AWS IoT core");
            const char *PROVISION_TOPIC = "$aws/provisioning-templates/custom_esp_prov_template/provision/json";
            const int PROVISION_TOPIC_LEN = strlen(PROVISION_TOPIC);

        /*    
            cJSON *provision_payload,*fmt ;
	        provision_payload = cJSON_CreateObject();
            cJSON_AddStringToObject(provision_payload, "certificateOwnershipToken", cJSON_GetObjectItem(aws_prov_response,"certificateOwnershipToken")->valuestring);
            //cJSON_AddStringToObject(provision_payload, "certificateId", cJSON_GetObjectItem(aws_prov_response,"certificateId")->valuestring);
            cJSON_AddItemToObject(provision_payload, "parameters", fmt=cJSON_CreateObject());
            cJSON_AddStringToObject(fmt,"SerialNumber",	UUID);
            sprintf(cPayload, "%s",cJSON_Print(provision_payload));
        */
            paramsQOS1.qos = QOS1;
            sprintf(cPayload, "{\"certificateOwnershipToken\":\"%s\",\"parameters\":{\"SerialNumber\":\"%s\"}}", cJSON_GetObjectItem(aws_prov_response,"certificateOwnershipToken")->valuestring,UUID);
            ESP_LOGI(TAG, "Payload : %s",cPayload);
            //cJSON_Delete(provision_payload);
            cJSON_Delete(aws_prov_response);
            paramsQOS1.payloadLen = strlen(cPayload);
            rc = aws_iot_mqtt_publish(&client, PROVISION_TOPIC, PROVISION_TOPIC_LEN, &paramsQOS1);
            if (rc == MQTT_REQUEST_TIMEOUT_ERROR) {
                ESP_LOGW(TAG, "QOS1 publish ack not received.");
                rc = SUCCESS;
            }
        }

        //Max time the yield function will wait for read messages
        rc = aws_iot_mqtt_yield(&client, 100);
        if(NETWORK_ATTEMPTING_RECONNECT == rc) {
            // If the client is attempting to reconnect we will skip the rest of the loop.
            //continue;
        }
        ESP_LOGI(TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetName(NULL), uxTaskGetStackHighWaterMark(NULL));

        if(aws_prov_state == THING_ACTIVE){
            /* Thing is active, Do stuff here */
            paramsQOS1.qos = QOS1;
            sprintf(cPayload, "{\"Device_ID\":\"%s\",\"Data\":\"%d\"}",UUID,56);
            paramsQOS1.payloadLen = strlen(cPayload);
            rc = aws_iot_mqtt_publish(&client, TOPIC, TOPIC_LEN, &paramsQOS1);
            if (rc == MQTT_REQUEST_TIMEOUT_ERROR) {
                ESP_LOGW(TAG, "QOS1 publish ack not received.");
                rc = SUCCESS;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
       
    }

    ESP_LOGE(TAG, "An error occurred in the main loop.");
   
}

static void initialise_wifi(void)
{
    /* Initialize TCP/IP */
#ifdef ESP_NETIF_SUPPORTED
    esp_netif_init();
#else
    tcpip_adapter_init();
#endif
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    /* Initialize Wi-Fi including netif with default config */
#ifdef ESP_NETIF_SUPPORTED
    esp_netif_create_default_wifi_sta();
#endif

    wifi_event_group = xEventGroupCreate();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}


void app_main()
{
    // Initialize NVS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK( err );

    esp_task_wdt_init(60000,0);

    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true
    };
    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG_SPIFF, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG_SPIFF, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG_SPIFF, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }

        /* abort */
    }
    else{
       ESP_LOGI(TAG_SPIFF, "SPIFFS mounted successfully"); 
    }

    ESP_LOGI(TAG_SPIFF, "Performing SPIFFS_check().");
    ret = esp_spiffs_check(conf.partition_label);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_SPIFF, "SPIFFS_check() failed (%s)", esp_err_to_name(ret));
        return;
    } else {
        ESP_LOGI(TAG_SPIFF, "SPIFFS_check() successful");
    }


    initialise_wifi();
    xTaskCreatePinnedToCore(&aws_iot_task, "aws_iot_task", 16216, NULL, 5, NULL, 1);
}