/* task.h — host-test stub. */
#ifndef HOST_TASK_STUB_H
#define HOST_TASK_STUB_H
#include <stdint.h>
#include "FreeRTOS.h"
static inline void vTaskDelay(uint32_t ms) { (void)ms; }
static inline uint32_t xTaskGetTickCount(void) { return 0; }
typedef void *TaskHandle_t2;
#define tskNORMAL_PRIORITY 3
static inline int xTaskCreate(void (*fn)(void*), const char *n, uint16_t st,
                              void *arg, unsigned prio, void **h)
{ (void)fn;(void)n;(void)st;(void)arg;(void)prio;(void)h; return pdPASS; }
static inline void vTaskDelete(void *h) { (void)h; }
/* Tests steer "which task am I" through this global (defined in host_stubs.c). */
extern void *g_host_current_task;
static inline void *xTaskGetCurrentTaskHandle(void) { return g_host_current_task; }
#endif
