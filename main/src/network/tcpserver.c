#include <string.h>
#include <sys/param.h>
#include "esp_system.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "tcpserver.h"

#define QUEUE_LEN 10
#define MSG_MAX_LEN 128

#define PORT 8080
#define TAG "tcp_server"

static volatile bool tcp_server_running = false;
TaskHandle_t tcp_server_task_handler = NULL;
static int listen_sock = -1;
QueueHandle_t tcp_msg_queue;

static void tcp_server_task(void *param)
{
    char rx_buffer[128];
    char addr_str[128];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;

    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);

    listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if(listen_sock < 0)
    {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket created");

    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket listening");

    while (tcp_server_running)
    {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);

        if (!tcp_server_running) {
            ESP_LOGI(TAG, "TCP server task stopped");
            break;
        }

        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(TAG, "Socket accepted from %s", addr_str);

        while (tcp_server_running) 
        {
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len < 0) {
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                break;
            } else if (len == 0) {
                ESP_LOGI(TAG, "Connection closed");
                break;
            }

            rx_buffer[len] = 0; // Null-terminate whatever we received and treat as a string
            ESP_LOGI(TAG, "Received %d bytes: %s", len, rx_buffer);
            if(xQueueSend(tcp_msg_queue, rx_buffer, 0) != pdPASS)
            {
                ESP_LOGW(TAG, "Tcp Message Queue full, messege dropped");
            }
        }

        shutdown(sock, 0);
        close(sock);
        ESP_LOGI(TAG, "Socket closed");
    }

    if (listen_sock >= 0) 
    {
        close(listen_sock);
        listen_sock = -1;
    }

    ESP_LOGI(TAG, "TCP server stopped");
    vTaskDelete(NULL);
    tcp_server_task_handler = NULL;
}

void tcp_server_stop(void) {
    tcp_server_running = false;

    if (listen_sock >= 0) {
        shutdown(listen_sock, 0);
        close(listen_sock);
        listen_sock = -1;
    }
}


void tcpserver_create(void)
{
    if(!tcp_msg_queue) tcp_msg_queue = xQueueCreate(QUEUE_LEN, MSG_MAX_LEN);

    if(!tcp_server_task_handler)
    {
        tcp_server_running = true;
        xTaskCreate(tcp_server_task, "tcp_server_task", 4096, NULL, 5, &tcp_server_task_handler);
        ESP_LOGI(TAG, "TCP server task created");
    }
    else
    {
        ESP_LOGW(TAG, "Tcp server already running");
    }
}