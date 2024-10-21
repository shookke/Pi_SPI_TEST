#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_err.h"
#include "esp_spi_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "ping/ping_sock.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"

// Replace these with your actual Wi-Fi credentials
#define WIFI_SSID       "<ssid>"
#define WIFI_PASSWORD   "<password>"

// SPI configuration
#define GPIO_MISO   16         // SPI MISO pin (GPIO 16)
#define GPIO_MOSI   15         // SPI MOSI pin (GPIO 15)
#define GPIO_SCLK   14         // SPI SCLK pin (GPIO 14)
#define GPIO_CS     17         // SPI Chip Select pin (GPIO 17)
#define SPI_HOST SPI2_HOST     // SPI HOST SPI2
#define DMA_CHAN   SPI_DMA_CH_AUTO

// Transaction configuration
#define NUM_PREPARED_TRANSACTIONS 1
#define MAX_TRANSACTION_SIZE      4096  // in bytes

// Image and HTTP configuration
#define MULTIPART_BOUNDARY "----ESP32Boundary7MA4YWxkTrZu0gW"
#define MAX_HTTP_OUTPUT_BUFFER 4096  // Adjust size as needed
#define API_UPLOAD_URL "https://plantpulse.app/api/v1/readings/upload"
#define AUTH_TOKEN  "<JWT Token>" // Replace with your actual JWT token

// Commands and Acknowledgments
#define GET_IMAGE_CMD     0xA1
#define ACK_BYTE          0xAC
#define READY_ACK_BYTE    0xAD
#define CHECKSUM_ACK_BYTE 0xAE
#define FINAL_ACK_BYTE    0xAF

static const char *TAG = "SPI_SLAVE";

// Wi-Fi event group
static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// Image ready semaphore
static SemaphoreHandle_t xImageReadySemaphore;

// Structure to hold image data and size
typedef struct {
    uint8_t *image_data;
    size_t image_size;
} send_image_args_t;

static send_image_args_t send_image_args = {
    .image_data = NULL,
    .image_size = 0,
};

// Forward declarations
void wifi_init_sta(void);
void send_image_task(void *pvParameters);
void spi_slave_task(void *pvParameters);
esp_err_t http_event_handler(esp_http_client_event_t *evt);

// Wi-Fi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT)
    {
        if (event_id == WIFI_EVENT_STA_START)
        {
            esp_wifi_connect();
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
        {
            ESP_LOGI(TAG, "Disconnected from Wi-Fi. Attempting to reconnect...");
            esp_wifi_connect();
            xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
    else if (event_base == IP_EVENT)
    {
        if (event_id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
            ESP_LOGI(TAG, "Got IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}


// Initialize Wi-Fi in station mode
void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        &instance_any_id);

    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(IP_EVENT,
                                        IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler,
                                        NULL,
                                        &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi initialization completed. Connecting to SSID:%s", WIFI_SSID);

    // Wait until the connection is established
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to Wi-Fi network: %s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "Failed to connect to Wi-Fi network: %s", WIFI_SSID);
    }
}

// HTTP Event Handler
#define MAX_RESPONSE_SIZE 2048
static char response_buffer[MAX_RESPONSE_SIZE];
static size_t response_length = 0;

// HTTP Event Handler
esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                if (response_length + evt->data_len < MAX_RESPONSE_SIZE) {
                    memcpy(response_buffer + response_length, evt->data, evt->data_len);
                    response_length += evt->data_len;
                    response_buffer[response_length] = '\0'; // Null-terminate
                } else {
                    ESP_LOGW(TAG, "Response buffer overflow");
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            ESP_LOGI(TAG, "Server Response: %s", response_buffer);
            // Reset the buffer for the next request
            memset(response_buffer, 0, MAX_RESPONSE_SIZE);
            response_length = 0;
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
        default:
            ESP_LOGW(TAG, "Unknown event id: %d", evt->event_id);
            break;
    }
    return ESP_OK;
}


// Function to send image to API endpoint
void send_image_to_server_lwip(uint8_t *image_data, size_t image_size) {
    ESP_LOGI(TAG, "Starting HTTP request to send image of size %u bytes", image_size);

    // Prepare multipart form-data headers and footer
    const char *header_format = "--" MULTIPART_BOUNDARY "\r\n"
                                "Content-Disposition: form-data; name=\"file\"; filename=\"image.jpg\"\r\n"
                                "Content-Type: image/jpeg\r\n\r\n";
    const char *footer = "\r\n--" MULTIPART_BOUNDARY "--\r\n";

    // Calculate total content length
    size_t header_size = strlen(header_format);
    size_t footer_size = strlen(footer);
    size_t multipart_size = header_size + image_size + footer_size;

    // Allocate memory for multipart data
    uint8_t *multipart_data = (uint8_t *)heap_caps_malloc(multipart_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (multipart_data == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for multipart data");
        return;
    }

    // Construct the multipart data
    memcpy(multipart_data, header_format, header_size);
    memcpy(multipart_data + header_size, image_data, image_size);
    memcpy(multipart_data + header_size + image_size, footer, footer_size);

    // Configure the HTTP client
    esp_http_client_config_t config = {
        .url = API_UPLOAD_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .timeout_ms = 20000,
        .buffer_size = 2048,        // Receive buffer size
        .buffer_size_tx = 2048,     // Transmit buffer size
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,  // Attach certificate bundle for HTTPS
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        heap_caps_free(multipart_data);
        return;
    }

    // Set headers
    char content_type_header[128];
    snprintf(content_type_header, sizeof(content_type_header), "multipart/form-data; boundary=%s", MULTIPART_BOUNDARY);
    esp_http_client_set_header(client, "Content-Type", content_type_header);
    esp_http_client_set_header(client, "Authorization", AUTH_TOKEN);

    // Set the multipart data as the POST field
    esp_http_client_set_post_field(client, (const char *)multipart_data, multipart_size);

    // Perform the HTTP request
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Image sent successfully. HTTP POST Status = %d", status);
    } else {
        ESP_LOGE(TAG, "Failed to send image. HTTP error: %s", esp_err_to_name(err));
    }

    // Clean up
    esp_http_client_cleanup(client);
    heap_caps_free(multipart_data);
}

// Task to send the image once it's fully received
void send_image_task(void *pvParameters) {
    while (1) {
        if (xSemaphoreTake(xImageReadySemaphore, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Image is ready, preparing to send to server...");

            // Check Wi-Fi connection
            EventBits_t bits = xEventGroupGetBits(wifi_event_group);
            if (!(bits & WIFI_CONNECTED_BIT)) {
                ESP_LOGW(TAG, "Wi-Fi not connected. Waiting for connection...");
                xEventGroupWaitBits(wifi_event_group,
                                    WIFI_CONNECTED_BIT,
                                    pdFALSE,
                                    pdFALSE,
                                    portMAX_DELAY);
            }

            // Make a local copy of the image data pointer and size
            uint8_t *image_data = send_image_args.image_data;
            size_t image_size = send_image_args.image_size;

            // Send the image to the server
            send_image_to_server_lwip(image_data, image_size);

            // After sending, free the image data
            if (image_data != NULL) {
                heap_caps_free(image_data);
                send_image_args.image_data = NULL;
                send_image_args.image_size = 0;
                ESP_LOGI(TAG, "Image data memory freed.");
            }
        }
    }
}

// SPI Slave Task
void spi_slave_task(void *pvParameters) {
    esp_err_t err;

    // SPI slave configuration
    spi_bus_config_t buscfg = {
        .mosi_io_num = GPIO_MOSI,
        .miso_io_num = GPIO_MISO,
        .sclk_io_num = GPIO_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MAX_TRANSACTION_SIZE,
    };

    spi_slave_interface_config_t slvcfg = {
        .spics_io_num = GPIO_CS,
        .flags = 0,
        .queue_size = NUM_PREPARED_TRANSACTIONS + 3,  // +3 for initial command, READY ACK, and final ACK
        .mode = 0,
    };

    err = spi_slave_initialize(SPI_HOST, &buscfg, &slvcfg, DMA_CHAN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI slave: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    uint8_t *image_buffer = NULL;
    size_t expected_size = 0;
    size_t total_received = 0;
    uint32_t esp32_checksum = 0;

    // Command buffer
    static uint8_t cmd_buffer[4] __attribute__((aligned(4))) = {0};

    // Prepare command transaction
    spi_slave_transaction_t cmd_trans = {0};
    cmd_trans.length = 8;  // 1 byte
    cmd_trans.rx_buffer = cmd_buffer;

    // Ack buffer
    static uint8_t ack_buffer[4] __attribute__((aligned(4))) = {ACK_BYTE, 0x00, 0x00, 0x00};

    // Prepare ack transaction
    spi_slave_transaction_t ack_trans = {0};
    ack_trans.length = 8;
    ack_trans.tx_buffer = ack_buffer;

    spi_slave_transaction_t *completed_trans;

    // Prepare transaction buffers
    uint8_t *rx_buffers[NUM_PREPARED_TRANSACTIONS];


    // Allocate rx_buffers
    for (int i = 0; i < NUM_PREPARED_TRANSACTIONS; i++) {
        rx_buffers[i] = (uint8_t *)heap_caps_malloc(MAX_TRANSACTION_SIZE, MALLOC_CAP_DMA);
        if (rx_buffers[i] == NULL) {
            ESP_LOGE(TAG, "Failed to allocate rx_buffer %d", i);
            // Handle memory allocation failure (free previously allocated buffers, etc.)
            for (int j = 0; j < i; j++) {
                heap_caps_free(rx_buffers[j]);
                rx_buffers[j] = NULL;
            }
            break;
        }
    }

    while (1) {
        // **Step 1: Queue Transaction to Receive Command**
        // Queue command transaction
        err = spi_slave_queue_trans(SPI_HOST, &cmd_trans, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to queue command transaction: %s", esp_err_to_name(err));
            continue;
        }

        // Wait for command transaction to complete
        err = spi_slave_get_trans_result(SPI_HOST, &completed_trans, portMAX_DELAY);
        if (err != ESP_OK || completed_trans != &cmd_trans) {
            ESP_LOGE(TAG, "Failed to get command transaction result: %s", esp_err_to_name(err));
            continue;
        }

        uint8_t command = cmd_buffer[0];
        ESP_LOGD(TAG, "Command received: 0x%02x", command);

        if (command == GET_IMAGE_CMD) {
            ESP_LOGD(TAG, "Received 'receive image' command.");

            // **Step 2: Queue ACK Transaction**

            // Queue ACK transaction
            err = spi_slave_queue_trans(SPI_HOST, &ack_trans, portMAX_DELAY);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to queue ACK transaction: %s", esp_err_to_name(err));
                continue;
            }

            // Wait for ACK transaction to complete
            err = spi_slave_get_trans_result(SPI_HOST, &completed_trans, portMAX_DELAY);
            if (err != ESP_OK || completed_trans != &ack_trans) {
                ESP_LOGE(TAG, "Failed to get ACK transaction result: %s", esp_err_to_name(err));
                continue;
            }

            // **Step 3: Queue Transaction to Receive Image Size**

            static uint8_t size_buffer[4] __attribute__((aligned(4))) = {0};

            // Prepare image size transaction
            spi_slave_transaction_t size_trans = {0};
            size_trans.length = 32;  // 4 bytes
            size_trans.rx_buffer = size_buffer;

            // Queue image size transaction
            err = spi_slave_queue_trans(SPI_HOST, &size_trans, portMAX_DELAY);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to queue image size transaction: %s", esp_err_to_name(err));
                continue;
            }

            // Wait for image size transaction to complete
            err = spi_slave_get_trans_result(SPI_HOST, &completed_trans, portMAX_DELAY);
            if (err != ESP_OK || completed_trans != &size_trans) {
                ESP_LOGE(TAG, "Failed to get image size transaction result: %s", esp_err_to_name(err));
                continue;
            }

            expected_size = (size_buffer[0] << 24) | (size_buffer[1] << 16) |
                            (size_buffer[2] << 8) | size_buffer[3];

            ESP_LOGD(TAG, "Received image size: %u bytes", expected_size);

            // Allocate memory for the image buffer
            image_buffer = (uint8_t *)heap_caps_malloc(expected_size, MALLOC_CAP_SPIRAM);
            if (image_buffer == NULL) {
                ESP_LOGE(TAG, "Failed to allocate memory for image of size %u bytes", expected_size);
                continue;
            }

            // Initialize variables
            total_received = 0;
            esp32_checksum = 0;

            // Prepare and queue the READY ACK transaction
            static uint8_t ready_ack_buffer[4] __attribute__((aligned(4))) = {READY_ACK_BYTE, 0x00, 0x00, 0x00};
            spi_slave_transaction_t ready_ack_trans = {0};
            ready_ack_trans.length = 8;
            ready_ack_trans.tx_buffer = ready_ack_buffer;

            // Prepare the Data transaction
            spi_slave_transaction_t data_trans = {0};
            data_trans.length = MAX_TRANSACTION_SIZE * 8;
            data_trans.rx_buffer = rx_buffers[0];

            // **Step 4: Queue Transaction to Receive Image**

            // Send ready signal for image transmition
            err = spi_slave_queue_trans(SPI_HOST, &ready_ack_trans, portMAX_DELAY);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to queue READY ACK: %s", esp_err_to_name(err));
                continue;
            }
            err = spi_slave_get_trans_result(SPI_HOST, &completed_trans, portMAX_DELAY);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to get SPI transaction result: %s", esp_err_to_name(err));
                break;
            }

            // Queue the data receive transaction
            err = spi_slave_queue_trans(SPI_HOST, &data_trans, portMAX_DELAY);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to queue DATA Receive Transaction: %s", esp_err_to_name(err));
                continue;
            }

            // Proceed to receive transactions
            while (total_received < expected_size) {
                err = spi_slave_get_trans_result(SPI_HOST, &completed_trans, portMAX_DELAY);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to get SPI transaction result: %s", esp_err_to_name(err));
                    break;
                }

                // Process data receive transactions
                int bytes_received = completed_trans->trans_len / 8;
                ESP_LOGD(TAG, "Bytes received: %d", bytes_received);

                if (bytes_received <= 0) {
                    ESP_LOGW(TAG, "No data received in this transaction");
                    break;
                }

                // Copy received data to image buffer
                if (total_received + bytes_received > expected_size) {
                    bytes_received = expected_size - total_received;
                }

                memcpy(image_buffer + total_received, completed_trans->rx_buffer, bytes_received);
                total_received += bytes_received;


                ESP_LOGD(TAG, "Received %d bytes, total received: %u", bytes_received, total_received);

                // Re-queue the transaction
                memset(completed_trans->rx_buffer, 0, MAX_TRANSACTION_SIZE);
                completed_trans->length = MAX_TRANSACTION_SIZE * 8;

                err = spi_slave_queue_trans(SPI_HOST, completed_trans, portMAX_DELAY);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to re-queue SPI transaction: %s", esp_err_to_name(err));
                    break;
                }
            }

            // **Step 5: Clean up and Prepare for Wifi Transmission**

            // Clean up rx_buffers
            for (int i = 0; i < NUM_PREPARED_TRANSACTIONS; i++) {
                if (rx_buffers[i] != NULL) {
                    heap_caps_free(rx_buffers[i]);
                    rx_buffers[i] = NULL;
                }
            }

            // Clean up pending transactions
            spi_slave_transaction_t *unused_trans;
            while (spi_slave_get_trans_result(SPI_HOST, &unused_trans, 0) == ESP_OK) {
                ESP_LOGI(TAG, "Cleaned up pending transaction.");
            }

            // After receiving the full image
            if (total_received == expected_size) {
                ESP_LOGI(TAG, "Full image received (%u bytes).", total_received);

                // Signal that the image is ready to be sent over HTTP
                send_image_args.image_data = image_buffer;
                send_image_args.image_size = expected_size;

                // Grant the semaphore
                xSemaphoreGive(xImageReadySemaphore);

            } else {
                // Handle incomplete reception
                ESP_LOGE(TAG, "Failed to receive full image, received %u of %u bytes", total_received, expected_size);
                if (image_buffer != NULL) {
                    heap_caps_free(image_buffer);
                    image_buffer = NULL;
                }
            }

        } else {
            ESP_LOGW(TAG, "Unknown command received: 0x%02x", command);
        }

        vTaskDelay(pdMS_TO_TICKS(10));  // Small delay to prevent tight loop
    }

    // **Clean up**
    spi_slave_free(SPI_HOST);
    vTaskDelete(NULL);
}


void app_main(void)
{
    // Initialize NVS (required by Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize Wi-Fi
    wifi_init_sta();

    // Create the semaphore for image readiness
    xImageReadySemaphore = xSemaphoreCreateBinary();

    // Create tasks
    xTaskCreate(spi_slave_task, "spi_slave_task", 8192, NULL, 5, NULL);
    xTaskCreate(send_image_task, "send_image_task", 8192, NULL, 5, NULL);

    // Prevent app_main from returning
    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}
