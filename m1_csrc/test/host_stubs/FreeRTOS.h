/* FreeRTOS.h — host-test stub: allocator surface + handle typedefs the real
 * m1_log_debug.h expects to already have in scope. */
#ifndef HOST_FREERTOS_STUB_H
#define HOST_FREERTOS_STUB_H
#include <stdlib.h>
#include <stdint.h>

typedef void *TaskHandle_t;
typedef void *QueueHandle_t;

#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
static inline void *pvPortMalloc(size_t n) { return malloc(n); }
static inline void vPortFree(void *p) { free(p); }
static inline size_t xPortGetFreeHeapSize(void) { return 64 * 1024; }

#endif
