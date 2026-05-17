/* Host-test stub — provides the minimum types cmd.c pulls in via FreeRTOS.h */
#pragma once
#include <stdint.h>
typedef unsigned int  UBaseType_t;
typedef int           BaseType_t;
typedef uint32_t      TickType_t;
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define pdTRUE  1
#define pdFALSE 0
