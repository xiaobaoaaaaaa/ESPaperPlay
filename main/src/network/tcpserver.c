#include <string.h>
#include <sys/param.h>
#include <errno.h>

#include "esp_system.h"
#include "esp_log.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"

#include "tcpserver.h"

// 消息队列长度
#define QUEUE_LEN 10
// 单条消息最大长度
#define MSG_MAX_LEN 128

// 监听端口
#define PORT 8080
#define TAG "tcp_server"

// TCP服务器运行标志
static volatile bool tcp_server_running = false;
// TCP服务器任务句柄
TaskHandle_t tcp_server_task_handler = NULL;
// 监听socket
static int listen_sock = -1;
// TCP消息队列
QueueHandle_t tcp_msg_queue;

/**
 * @brief TCP服务器任务主循环
 * @param param 未使用
 */
static void tcp_server_task(void *param)
{
    char rx_buffer[128];      // 接收缓冲区
    char addr_str[128];       // 存储客户端地址字符串
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;

    // 配置服务器地址结构
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);

    // 创建socket
    listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
    if(listen_sock < 0)
    {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        tcp_server_task_handler = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket created");

    // 绑定socket到指定端口
    int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        tcp_server_task_handler = NULL;
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket bound, port %d", PORT);

    // 设置监听超时时间为1秒
    struct timeval listen_timeout;
    listen_timeout.tv_sec = 1;  // 1秒超时
    listen_timeout.tv_usec = 0;
    setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO, &listen_timeout, sizeof(listen_timeout));

    // 开始监听
    err = listen(listen_sock, 1);
    if (err != 0) {
        ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
        tcp_server_task_handler = NULL;
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Socket listening");

    // 主循环，处理客户端连接
    while (tcp_server_running)
    {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        // 等待客户端连接
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);

        if (!tcp_server_running) {
            ESP_LOGI(TAG, "TCP server task stopped");
            break;
        }

        if (sock < 0) {
            // 超时或错误
            if (errno == EAGAIN || errno == EWOULDBLOCK) 
            {
                // accept超时，继续循环
                continue;
            }
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            break;
        }

        // 打印客户端IP地址
        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
        ESP_LOGI(TAG, "Socket accepted from %s", addr_str);

        // 设置接收超时时间为1秒
        struct timeval timeout;
        timeout.tv_sec = 1;  // 1秒超时
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        // 处理客户端数据
        while (tcp_server_running) 
        {
            int len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
            if (len < 0) {
                // 超时或错误
                if (errno == EAGAIN || errno == EWOULDBLOCK) 
                {
                    // 超时，继续循环
                    continue;
                }
                ESP_LOGE(TAG, "recv failed: errno %d", errno);
                break;
            } else if (len == 0) {
                // 客户端关闭连接
                ESP_LOGI(TAG, "Connection closed");
                break;
            }

            rx_buffer[len] = 0; // 接收到的数据以字符串形式处理
            ESP_LOGI(TAG, "Received %d bytes: %s", len, rx_buffer);
            // 将消息发送到队列
            if(xQueueSend(tcp_msg_queue, rx_buffer, 0) != pdPASS)
            {
                ESP_LOGW(TAG, "Tcp Message Queue full, messege dropped");
            }
        }

        // 关闭客户端socket
        shutdown(sock, 0);
        close(sock);
        ESP_LOGI(TAG, "Socket closed");
    }

    // 关闭监听socket
    if (listen_sock >= 0) 
    {
        close(listen_sock);
        listen_sock = -1;
    }

    ESP_LOGI(TAG, "TCP server stopped");
    tcp_server_task_handler = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief 停止TCP服务器
 */
void tcp_server_stop(void) 
{
    tcp_server_running = false;

    if (listen_sock >= 0) {
        shutdown(listen_sock, 0);
        close(listen_sock);
        listen_sock = -1;
    }
}

/**
 * @brief 创建并启动TCP服务器任务
 */
void tcpserver_create(void)
{
    // 创建消息队列
    if(!tcp_msg_queue) tcp_msg_queue = xQueueCreate(QUEUE_LEN, MSG_MAX_LEN);

    // 启动TCP服务器任务
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