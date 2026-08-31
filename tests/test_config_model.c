/* SPDX-License-Identifier: Apache-2.0 */
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
    strcpy(invalid.location, "bad\nlocation");
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    invalid = config;
    memset(invalid.device_name, 'x', sizeof(invalid.device_name));
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    invalid = config;
    invalid.input_names[3][0] = '\0';
    assert(!config_model_validate(&invalid, reason, sizeof(reason)));

    return 0;
}
