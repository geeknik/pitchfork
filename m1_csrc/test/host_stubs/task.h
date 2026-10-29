/* task.h — host-test stub. */
#ifndef HOST_TASK_STUB_H
#define HOST_TASK_STUB_H
#include <stdint.h>
static inline void vTaskDelay(uint32_t ms) { (void)ms; }
/* Tests steer "which task am I" through this global (defined in host_stubs.c). */
extern void *g_host_current_task;
static inline void *xTaskGetCurrentTaskHandle(void) { return g_host_current_task; }
#endif
