/* In-memory test controls, never linked into the Android library. */
#ifndef CLF_TEST_PROPERTIES_H
#define CLF_TEST_PROPERTIES_H
#include <stdint.h>
void host_properties_reset(void);
unsigned host_property_snapshot_reads(void);
void host_property_set(const char* name, const char* value);
void host_property_set_serial(const char* name, const char* value, uint32_t serial);
/* One-shot deterministic interleaving after the selected value was copied. */
void host_property_after_get(const char* name, void (*hook)(void));
#endif
