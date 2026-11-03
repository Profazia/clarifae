#include "config.h"
#include "log.h"
#include "host-stubs/properties.h"
#include "test_common.h"
#include <stdatomic.h>
#include <string.h>

static void disable_during_load(void) { host_property_set("persist.clarifae.enabled", "0"); }

int main(void) {
    host_properties_reset();
    clarifae_config_watch_t a = {0}, b = {0};
    clarifae_config_t cfg;
    CHECK(clarifae_config_changed(&a));
    CHECK(clarifae_config_changed(&b));
    CHECK(!clarifae_config_changed(&a));
    CHECK(!clarifae_config_changed(&b));
    clarifae_config_load(&cfg);
    CHECK(cfg.enabled == 1 && cfg.in_enabled == 1 && cfg.out_enabled == 1);
    CHECK(cfg.strength == 70 && cfg.out_strength == 60 && cfg.mode == 1);

    /* Property presence matters even for zero serial and empty value. */
    host_property_set_serial("persist.clarifae.targets", "", 0);
    CHECK(clarifae_config_changed(&a));
    CHECK(clarifae_config_changed(&b));
    CHECK(!clarifae_config_changed(&a));

    host_property_set_serial("persist.clarifae.strength", "20", 100);
    host_property_set_serial("persist.clarifae.out_strength", "30", 200);
    CHECK(clarifae_config_changed(&a));
    CHECK(clarifae_config_changed(&b));
    /* Same serial sum, distinct property snapshots. */
    host_property_set_serial("persist.clarifae.strength", "21", 101);
    host_property_set_serial("persist.clarifae.out_strength", "31", 199);
    CHECK(clarifae_config_changed(&a));
    CHECK(!clarifae_config_changed(&a));
    CHECK(clarifae_config_changed(&b));
    /* A repeated/wrapped serial cannot hide a changed value. */
    host_property_set_serial("persist.clarifae.strength", "22", 101);
    CHECK(clarifae_config_changed(&a));
    CHECK(clarifae_config_changed(&b));
    clarifae_config_load(&cfg);
    CHECK(cfg.strength == 22 && cfg.out_strength == 31);

    host_property_set("persist.clarifae.strength", "999999999999999999999999999");
    host_property_set("persist.clarifae.out_strength", "-9");
    host_property_set("persist.clarifae.mode", "not-a-number");
    host_property_set("persist.clarifae.loglevel", "3");
    clarifae_config_load(&cfg);
    CHECK(cfg.strength == 100 && cfg.out_strength == 0 && cfg.mode == 1);
    CHECK(atomic_load_explicit(&clarifae_log_level, memory_order_relaxed) == 3);
    char long_value[CLARIFAE_CONFIG_VALUE_MAX];
    memset(long_value, 'x', sizeof(long_value) - 1);
    long_value[sizeof(long_value) - 1] = '\0';
    host_property_set("persist.clarifae.targets", long_value);
    CHECK(clarifae_config_changed(&a));
    clarifae_config_load(&cfg);
    CHECK(strlen(cfg.targets) == sizeof(cfg.targets) - 1);
    CHECK(cfg.targets[sizeof(cfg.targets) - 1] == '\0');
    CHECK(!clarifae_config_changed(&a));

    /* Watch before load: a disable after enabled was read is not consumed. */
    host_property_set("persist.clarifae.enabled", "1");
    CHECK(clarifae_config_changed(&a));
    host_property_after_get("persist.clarifae.enabled", disable_during_load);
    clarifae_config_load(&cfg);
    CHECK(cfg.enabled == 1);
    CHECK(clarifae_config_changed(&a));
    clarifae_config_load(&cfg);
    CHECK(cfg.enabled == 0);
    CHECK(!clarifae_config_changed(&a));
    CHECK(clarifae_config_changed(&b));
    printf("PASS config snapshots/interleaving checks=%u\n", checks);
    return 0;
}
