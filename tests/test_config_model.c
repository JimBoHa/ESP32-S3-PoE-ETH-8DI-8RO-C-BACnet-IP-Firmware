/* SPDX-License-Identifier: 0BSD */
#include <assert.h>
#include <string.h>

#include "config_model.h"

int main(void)
{
    firmware_config_t config;
    char reason[128];

    config_model_defaults(&config);
    assert(config_model_validate(&config, reason, sizeof(reason)));
    assert(config_model_is_valid_blob(&config));
    assert(config.device_instance == FW_DEFAULT_DEVICE_INSTANCE);
    assert(config_model_instance_auto(&config) && config_model_instance_pending(&config));
    /* Preserve the on-flash v1 layout, including all existing names and keys. */
    assert(sizeof(config) == 1052 && offsetof(firmware_config_t, device_instance_mode) == 19);
    assert(config.bacnet_port == FW_DEFAULT_BACNET_PORT);
    assert(config.vendor_id == FW_DEFAULT_VENDOR_ID);
    assert(strcmp(config.vendor_name, FW_DEFAULT_VENDOR_NAME) == 0);
    assert(config.input_invert_mask == 0xFFU);
    assert(config.dhcp_enabled);
    assert(!config.restore_relay_state);

    firmware_config_t changed = config;
    changed.input_invert_mask = 0;
    assert(!config_model_is_valid_blob(&changed));
    config_model_finalize(&changed);
    assert(config_model_is_valid_blob(&changed));

    firmware_config_t invalid = config;
    invalid.device_instance = 4194303U;
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    invalid = config;
    invalid.bacnet_port = 0;
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    invalid = config;
    strcpy(invalid.hostname, "-invalid");
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    invalid = config;
    invalid.dhcp_enabled = false;
    strcpy(invalid.ip_address, "999.1.2.3");
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    firmware_config_t static_config = config;
    static_config.dhcp_enabled = false;
    assert(config_model_validate(&static_config, reason, sizeof(reason)));

    invalid = config;
    invalid.dhcp_enabled = false;
    strcpy(invalid.netmask, "255.0.255.0");
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    invalid = static_config;
    strcpy(invalid.gateway, "192.168.76.1");
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    invalid = static_config;
    strcpy(invalid.ip_address, "192.168.75.255");
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    invalid = config;
    strcpy(invalid.relay_names[0], invalid.input_names[0]);
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    invalid = config;
    strcpy(invalid.input_names[0], FW_CONFIG_CSV_HOSTNAME_NAME);
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    invalid = config;
    strcpy(invalid.relay_names[0], FW_CONFIG_BV_RELAY_RESTORE_NAME);
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    invalid = config;
    strcpy(invalid.location, "bad\nlocation");
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    invalid = config;
    memset(invalid.device_name, 'x', sizeof(invalid.device_name));
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    invalid = config;
    invalid.input_names[3][0] = '\0';
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    firmware_config_t legacy = config;
    legacy.device_instance_mode = 0;
    strcpy(legacy.location, "Existing installation");
    config_model_finalize(&legacy);
    assert(config_model_is_valid_blob(&legacy));
    assert(config_model_migrate_instance_mode(&legacy));
    assert(legacy.device_instance_mode == FW_INSTANCE_AUTO_CHECK_SAVED);
    assert(strcmp(legacy.location, "Existing installation") == 0);
    assert(config_model_is_valid_blob(&legacy));
    assert(!config_model_migrate_instance_mode(&legacy));
    legacy.device_instance = 123456;
    legacy.device_instance_mode = 0;
    config_model_finalize(&legacy);
    assert(config_model_migrate_instance_mode(&legacy));
    assert(!config_model_instance_auto(&legacy) && legacy.device_instance == 123456);

    /* A downgrade can preserve the mode byte while changing the numeric ID. */
    legacy.device_instance_mode = FW_INSTANCE_AUTO_LOCKED;
    config_model_finalize(&legacy);
    assert(config_model_migrate_instance_mode(&legacy));
    assert(legacy.device_instance_mode == FW_INSTANCE_MANUAL);
    assert(legacy.device_instance == 123456 && config_model_is_valid_blob(&legacy));

    firmware_config_t assigned = config;
    assert(config_model_assign_instance(&assigned, 1, 599155));
    assert(assigned.device_instance_mode == FW_INSTANCE_AUTO_LOCKED);
    assert(assigned.device_instance == 599155 && assigned.database_revision == 2);
    assert(strcmp(assigned.device_name, "BACnet IO 599155") == 0);
    assert(config_model_is_valid_blob(&assigned));
    assert(!config_model_instance_pending(&assigned));
    firmware_config_t unchanged = assigned;
    assert(!config_model_assign_instance(&assigned, 2, 599156));
    assert(memcmp(&assigned, &unchanged, sizeof(assigned)) == 0);
    /* Do not create duplicate object names while updating a factory name. */
    assigned = config;
    strcpy(assigned.input_names[0], "BACnet IO 599155");
    assert(config_model_assign_instance(&assigned, 1, 599155));
    assert(strcmp(assigned.device_name, "BACnet IO 599153") == 0);
    assert(config_model_is_valid_blob(&assigned));
    /* A manual edit or any intervening config save defeats the assignment CAS. */
    assigned = config;
    assigned.database_revision++;
    unchanged = assigned;
    assert(!config_model_assign_instance(&assigned, 1, 599155));
    assert(memcmp(&assigned, &unchanged, sizeof(assigned)) == 0);
    assert(config_model_instance_options(&assigned, false, false, true, 12, false));
    assert(assigned.device_instance_mode == FW_INSTANCE_MANUAL && assigned.device_instance == 12);
    assert(!config_model_assign_instance(&assigned, 2, 599155));

    assigned = config;
    strcpy(assigned.device_name, "Pump room");
    assert(!config_model_assign_instance(&assigned, 1, 598999));
    assert(!config_model_assign_instance(&assigned, 1, 600000));
    assert(config_model_assign_instance(&assigned, 1, 599999));
    assert(strcmp(assigned.device_name, "Pump room") == 0);
    assert(config_model_instance_options(&assigned, true, true, true, 599153, false));
    assert(assigned.device_instance == 599999 && !config_model_instance_pending(&assigned));
    assert(!config_model_instance_options(&assigned, false, true, false, 0, true));
    assert(config_model_instance_options(&assigned, true, true, false, 0, true));
    assert(config_model_instance_pending(&assigned));
    assert(config_model_instance_options(&assigned, true, false, true, 4194302, false));
    assert(config_model_validate(&assigned, reason, sizeof(reason)));
    assert(assigned.device_instance == 4194302 && !config_model_instance_auto(&assigned));
    return 0;
}
