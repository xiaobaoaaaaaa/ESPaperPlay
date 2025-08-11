#ifndef _TCPSERVER_H
#define _TCPSERVER_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

extern QueueHandle_t tcp_msg_queue;

void tcp_server_stop(void);
void tcpserver_create(void);

#endif