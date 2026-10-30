/* test_m1_capsrc.c — pure parts of the capture source (meta derivation). */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "m1_capture_source.h"
#include "m1_esp_client.h"

/* stub brain client surface (device: m1_esp_client.c) */
void m1_esp32_init(void) { }         /* link bring-up stub */
int  m1_esp_client_monitor_start(uint8_t c) { (void)c; return 0; }
bool m1_esp_client_monitor_stop(void) { return true; }
bool m1_esp_client_monitor_stats(uint32_t *t, uint32_t *d, uint8_t *b)
{ *t = 0; *d = 0; *b = 0; return true; }
uint16_t m1_esp_client_monitor_read(uint8_t *o, uint16_t cap, int8_t *r, uint8_t *c)
{ (void)o;(void)cap;(void)r;(void)c; return 0; }

static int g_failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d\n", __FILE__, __LINE__); g_failures++; } } while (0)

int main(void)
{
    uint8_t klass; uint32_t ts;
    uint8_t mgmt[2]  = { 0x80, 0x00 };   /* type bits 00 -> mgmt  */
    uint8_t ctrl[2]  = { 0xD4, 0x00 };   /* type bits 01 -> ctrl  */
    uint8_t data[2]  = { 0x08, 0x00 };   /* type bits 10 -> data  */

    m1_capsrc_derive_meta(mgmt, 2, 6, -55, 1234, &klass, &ts);
    CHECK(klass == 0); CHECK(ts == 1234000u);
    m1_capsrc_derive_meta(ctrl, 2, 6, -70, 0, &klass, &ts);
    CHECK(klass == 1); CHECK(ts == 0);
    m1_capsrc_derive_meta(data, 2, 11, -42, 5, &klass, &ts);
    CHECK(klass == 2);
    if (!g_failures) printf("test_m1_capsrc: ALL PASS\n");
    else printf("test_m1_capsrc: %d FAILURES\n", g_failures);
    return g_failures ? 1 : 0;
}
