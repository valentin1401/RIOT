/*
 * Copyright (C) 2016 Unwired Devices [info@unwds.com]
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @defgroup
 * @ingroup
 * @brief
 * @{
 * @file		umdk-uart.c
 * @brief       umdk-uart module implementation
 * @author      EP
 */

#ifdef __cplusplus
extern "C" {
#endif

/* define is autogenerated, do not change */
#undef _UMDK_MID_
#define _UMDK_MID_ UNWDS_UART_MODULE_ID

/* define is autogenerated, do not change */
#undef _UMDK_NAME_
#define _UMDK_NAME_ "uart"

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "periph/gpio.h"
#include "periph/uart.h"

#include "board.h"

#include "unwds-common.h"
#include "include/umdk-uart.h"

#include "thread.h"
#include "xtimer.h"

static uwnds_cb_t *callback;
static uint8_t rxbuf[UMDK_UART_RXBUF_SIZE] = {};

static volatile uint8_t num_bytes_received;

static kernel_pid_t writer_pid;

static msg_t send_msg;
static msg_t send_msg_ovf;

static xtimer_t send_timer;

typedef struct {
	uint8_t is_valid;
	uint8_t uart_dev;
	uint32_t baudrate;
    uint8_t databits;
    uint8_t parity;
    uint8_t stopbits;
} umdk_uart_config_t;

static umdk_uart_config_t umdk_uart_config = { 0, UMDK_UART_DEV, 115200U, \
                                               UART_DATABITS_8, UART_PARITY_NOPARITY, \
                                               UART_STOPBITS_10 };

void *writer(void *arg) {
  msg_t msg;
  msg_t msg_queue[128];
  msg_init_queue(msg_queue, 128);

  while (1) {
    msg_receive(&msg);

    module_data_t data;
    data.data[0] = _UMDK_MID_;
    data.length = 2;

    /* Received payload, send it */
    if (msg.content.value == send_msg.content.value) {
      data.length += num_bytes_received;
      data.data[1] = UMDK_UART_REPLY_RECEIVED;

      memcpy(data.data + 2, rxbuf, num_bytes_received);

      num_bytes_received = 0;
    } else if (msg.content.value == send_msg_ovf.content.value) { /* RX buffer overflowed, send error message */
      data.length = 2;
      data.data[1] = UMDK_UART_REPLY_ERR_OVF;

      num_bytes_received = 0;
    }
    
    char buf[200];
    char *pos = buf;
    int k = 0;
    for (k = 2; k < data.length; k++) {
        snprintf(pos, 3, "%02x", data.data[k]);
        pos += 2;
    }
    
    printf("[umdk-" _UMDK_NAME_ "] received 0x%s\n", buf);

    callback(&data);
  }

  return NULL;
}

void rx_cb(void *arg, uint8_t data)
{
	/* Buffer overflow */
	if (num_bytes_received == UMDK_UART_RXBUF_SIZE) {
		num_bytes_received = 0;

		msg_send(&send_msg_ovf, writer_pid);

		return;
	}

	rxbuf[num_bytes_received++] = data;

	/* Schedule sending after timeout */
	xtimer_set_msg(&send_timer, 1e3 * UMDK_UART_SYMBOL_TIMEOUT_MS, &send_msg, writer_pid);
}

static void reset_config(void) {
	umdk_uart_config.is_valid = 0;
	umdk_uart_config.baudrate = 115200U;
    umdk_uart_config.databits = UART_DATABITS_8;
    umdk_uart_config.parity = UART_PARITY_NOPARITY;
    umdk_uart_config.stopbits = UART_STOPBITS_10;
	umdk_uart_config.uart_dev = UMDK_UART_DEV;
}

static void init_config(void) {
	reset_config();

	if (!unwds_read_nvram_config(_UMDK_MID_, (uint8_t *) &umdk_uart_config, sizeof(umdk_uart_config)))
		return;

	if ((umdk_uart_config.is_valid == 0xFF) || (umdk_uart_config.is_valid == 0))  {
		reset_config();
		return;
	}
    
    /* simple check if we're upgrading from previous version */
    if (umdk_uart_config.stopbits > UART_STOPBITS_20) {
		reset_config();
		return;
    }

	if (umdk_uart_config.uart_dev >= UART_NUMOF) {
		reset_config();
		return;
	}
}

static inline void save_config(void) {
	umdk_uart_config.is_valid = 1;
	unwds_write_nvram_config(_UMDK_MID_, (uint8_t *) &umdk_uart_config, sizeof(umdk_uart_config));
}

int umdk_uart_shell_cmd(int argc, char **argv) {
    if (argc == 1) {
        puts (_UMDK_NAME_ " send <hex> - send data to UART port");
        puts (_UMDK_NAME_ " baud <baud> - set baudrate");
        puts (_UMDK_NAME_ " reset - reset settings to default");
        return 0;
    }
    
    char *cmd = argv[1];
    
    if (strcmp(cmd, "send") == 0) {
        char *pos = argv[2];
        
        if ((strlen(pos) % 2) != 0 ) {
            puts("[umdk-" _UMDK_NAME_ "] Error: hex number length must be even");
            return 0;
        }
        
        if ((strlen(pos)) > 400 ) {
            puts("[umdk-" _UMDK_NAME_ "] Error: over 200 bytes of data");
            return 0;
        }

        uint8_t data[200];
        uint8_t count = 0;
        uint8_t i = 0;
        char buf[3] = { 0 };
        for(i = 0; i < strlen(argv[2])/2; i++) {
            /* copy 2 hex symbols to a new array */
            memcpy(buf, pos, 2);
            pos += 2;

            if (strcmp(buf, "0x") && strcmp(buf, "0X")) {
                data[count] = strtol(buf, NULL, 16);
                count++;
            }
        }
        
        /* Send data */
        gpio_set(RE_PIN);
        gpio_set(DE_PIN);

        uart_write(UART_DEV(umdk_uart_config.uart_dev), (uint8_t *) data, count);
        
        gpio_clear(RE_PIN);
        gpio_clear(DE_PIN);
    }
    
    if (strcmp(cmd, "baud") == 0) {
        char *val = argv[2];
        
        uart_params_t uart_params;
        uart_params.baudrate = atoi(val);
        uart_params.parity = umdk_uart_config.parity;
        uart_params.stopbits = umdk_uart_config.stopbits;
        uart_params.databits = umdk_uart_config.databits;
        
        if (!uart_init_ext(UART_DEV(umdk_uart_config.uart_dev), &uart_params, rx_cb, NULL)){
            umdk_uart_config.baudrate = uart_params.baudrate;
            save_config();
        }
    }
    
    if (strcmp(cmd, "reset") == 0) {
        reset_config();
        save_config();
    }
    
    return 1;
}

void umdk_uart_init(uint32_t *non_gpio_pin_map, uwnds_cb_t *event_callback)
{
    (void) non_gpio_pin_map;
    callback = event_callback;

    init_config();

    uint8_t databits;
    char parity;
    uint8_t stopbits;
    switch (umdk_uart_config.parity) {
        case UART_PARITY_NOPARITY:
            parity = 'N';
            break;
        case UART_PARITY_ODD:
            parity = 'O';
            break;
        case UART_PARITY_EVEN:
            parity = 'E';
            break;
        default:
            parity = 'N';
            umdk_uart_config.parity = UART_PARITY_NOPARITY;
            break;
    }
    
    switch (umdk_uart_config.stopbits) {
        case UART_STOPBITS_10:
            stopbits = 1;
            break;
        case UART_STOPBITS_20:
            stopbits = 2;
            break;
        default:
            umdk_uart_config.stopbits = UART_STOPBITS_10;
            stopbits = 1;
            break;
    }
    
    switch (umdk_uart_config.databits) {
        case UART_DATABITS_8:
            databits = 8;
            break;
        case UART_DATABITS_9:
        /* not an error!!! */
        /* 9 bits are used with parity only, so it will be 8 data bits + 1 parity bit */
            databits = 8;
            break;
        default:
            databits = 8;
            if (umdk_uart_config.parity == UART_PARITY_NOPARITY) {
                umdk_uart_config.databits = UART_DATABITS_8;
            } else {
                umdk_uart_config.databits = UART_DATABITS_9;                
            }
            break;
    }
    
    printf("[umdk-" _UMDK_NAME_ "] Mode: %lu-%u%c%u\n", umdk_uart_config.baudrate, databits, parity, stopbits);

    uart_params_t uart_params;
    uart_params.baudrate = umdk_uart_config.baudrate;
    uart_params.parity = umdk_uart_config.parity;
    uart_params.stopbits = umdk_uart_config.stopbits;
    uart_params.databits = umdk_uart_config.databits;
    
    /* Initialize UART */
    if (uart_init_ext(UART_DEV(umdk_uart_config.uart_dev), &uart_params, rx_cb, NULL)) {
        return;
    }

    /* Initialize DE/RE pins */
    gpio_init(DE_PIN, GPIO_OUT);
    gpio_init(RE_PIN, GPIO_OUT);

    gpio_clear(DE_PIN);
    gpio_clear(RE_PIN);

    send_msg.content.value = 0;
    send_msg_ovf.content.value = 1;

    char *stack = (char *) allocate_stack();
    if (!stack) {
    	puts("umdk-" _UMDK_NAME_ ": unable to allocate memory. Is too many modules enabled?");
    	return;
    }

    unwds_add_shell_command(_UMDK_NAME_, "type '" _UMDK_NAME_ "' for commands list", umdk_uart_shell_cmd);
    
	/* Create handler thread */
	writer_pid = thread_create(stack, UNWDS_STACK_SIZE_BYTES, THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, writer, NULL, "umdk-uart thread");
}

static void do_reply(module_data_t *reply, umdk_uart_reply_t r)
{
    reply->length = 2;
    reply->data[0] = _UMDK_MID_;
    reply->data[1] = r;
}

bool umdk_uart_cmd(module_data_t *data, module_data_t *reply)
{
    if (data->length < 1) {
        do_reply(reply, UMDK_UART_REPLY_ERR_FMT);
        printf("umdk-" _UMDK_NAME_ ": no command received\n");
        return true;
    }

    umdk_uart_prefix_t prefix = data->data[0];
    switch (prefix) {
        case UMDK_UART_SEND_ALL:
            /* Cannot send nothing */
            if (data->length == 1) {
                do_reply(reply, UMDK_UART_REPLY_ERR_FMT);
                printf("umdk-" _UMDK_NAME_ ": incorrect data length: %d\n", data->length);
                break;
            }

            /* Send data */
            gpio_set(RE_PIN);
            gpio_set(DE_PIN);

            uart_write(UART_DEV(umdk_uart_config.uart_dev), (uint8_t *) data->data + 1, data->length - 1);

            gpio_clear(RE_PIN);
            gpio_clear(DE_PIN);

            do_reply(reply, UMDK_UART_REPLY_SENT);
            break;

        /* set UART parameters */
        case UMDK_UART_SET_PARAMETERS:
            /* 1 byte prefix and a string like 115200-8N1 */
            
            if (data->length < 8) { /* Must be one byte of prefix and one byte of BR index */
                do_reply(reply, UMDK_UART_REPLY_ERR_FMT);
                printf("umdk-" _UMDK_NAME_ ": incorrect data length: %d, should be >= 8\n", data->length);
                break;
            }

            uint32_t baud;
            int databits;
            int stopbits;
            char parity;
            
            uart_params_t uart_params;
            
            if (sscanf((char *)&data->data[1], "%lu-%d%c%d", &baud, &databits, &parity, &stopbits) != 4) {
                do_reply(reply, UMDK_UART_REPLY_ERR_FMT);
                printf("umdk-" _UMDK_NAME_ ": error parsing parameters string: %s\n", (char *)&data->data[1]);
                return true;
            }
            
            uart_params.baudrate = baud;
            
            switch (databits) {
                case 8:
                    uart_params.databits = UART_DATABITS_8;
                    break;
                default:
                    puts("umdk-" _UMDK_NAME_ ": invalid number of data bits, must be 8");
                    do_reply(reply, UMDK_UART_REPLY_ERR_FMT);
                    return true;
            }
            
            switch (parity) {
                case 'N':
                    uart_params.parity = UART_PARITY_NOPARITY;
                    break;
                case 'E':
                    uart_params.parity = UART_PARITY_EVEN;
                    uart_params.databits = UART_DATABITS_9;
                    break;
                case 'O':
                    uart_params.parity = UART_PARITY_ODD;
                    uart_params.databits = UART_DATABITS_9;
                    break;
                default:
                    puts("umdk-" _UMDK_NAME_ ": invalid parity value, must be N, O or E");
                    do_reply(reply, UMDK_UART_REPLY_ERR_FMT);
                    return true;
            }
            
            switch (stopbits) {
                case 1:
                    uart_params.parity = UART_STOPBITS_10;
                    break;
                case 2:
                    uart_params.parity = UART_STOPBITS_20;
                    break;
                default:
                    puts("umdk-" _UMDK_NAME_ ": invalid number of stop bits, must be 1 or 2");
                    do_reply(reply, UMDK_UART_REPLY_ERR_FMT);
                    return true;
            }

            /* Set baudrate and reinitialize UART */
            gpio_clear(RE_PIN);

            if (uart_init_ext(umdk_uart_config.uart_dev, &uart_params, rx_cb, NULL)) {
                do_reply(reply, UMDK_UART_ERR); /* UART error, baud rate not supported? */
                puts("umdk-" _UMDK_NAME_ ": baud rate not supported");
                break;
            }
            
            umdk_uart_config.baudrate = baud;
            umdk_uart_config.databits = databits;
            umdk_uart_config.parity = parity;
            umdk_uart_config.stopbits = stopbits;
            
            printf("[umdk-" _UMDK_NAME_ "] Mode: %lu-%u%c%u\n", baud, databits, parity, stopbits);
            save_config();

            gpio_set(RE_PIN);

            do_reply(reply, UMDK_UART_REPLY_BAUDRATE_SET);
        	break;

        default:
        	do_reply(reply, UMDK_UART_REPLY_ERR_FMT);
        	break;
    }

    return true;
}

#ifdef __cplusplus
}
#endif