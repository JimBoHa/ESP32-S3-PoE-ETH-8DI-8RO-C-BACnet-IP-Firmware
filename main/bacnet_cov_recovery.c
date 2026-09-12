/* SPDX-License-Identifier: 0BSD */
#include "bacnet_cov_recovery.h"

#include <string.h>
#include "bacnet/bacapp.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/cov.h"
#include "bacnet/npdu.h"

/* More than the 26 COV-capable objects in this firmware. No heap allocation.
   A one-second pause lets already queued notifications use the freed TSM
   slots before a failed subscriber can start a new transaction. */
#define RECOVERY_OBJECTS 32U
#define RECOVERY_PAUSE_MS 1000U

typedef struct {
    bool used;
    BACNET_OBJECT_TYPE type;
    uint32_t instance;
    uint32_t remaining_ms;
} recovery_object_t;

static recovery_object_t s_pending[RECOVERY_OBJECTS];
static bacnet_cov_recovery_stats_t s_stats;
/* tsm_get_transaction_pdu requires a MAX_PDU buffer. Avoid a large transient
   stack allocation in the timeout callback nested inside the BACnet loop. */
static uint8_t s_failed_pdu[MAX_PDU];

static recovery_object_t *find(BACNET_OBJECT_TYPE type, uint32_t instance)
{
    for (unsigned i = 0; i < RECOVERY_OBJECTS; ++i) {
        if (s_pending[i].used && s_pending[i].type == type &&
            s_pending[i].instance == instance) {
            return &s_pending[i];
        }
    }
    return NULL;
}

static void confirmed_timeout(uint8_t invoke_id)
{
    BACNET_ADDRESS destination = {0}, decoded_dest = {0}, decoded_src = {0};
    BACNET_NPDU_DATA npdu = {0};
    uint16_t length = 0;
    BACNET_PROPERTY_VALUE values[2] = {0};
    BACNET_COV_DATA cov = {0};

    if (!tsm_get_transaction_pdu(invoke_id, &destination, &npdu,
            s_failed_pdu, &length)) {
        return;
    }
    int offset = bacnet_npdu_decode(s_failed_pdu, length,
        &decoded_dest, &decoded_src, &npdu);
    if (offset <= 0 || offset + 4 > length || npdu.network_layer_message ||
        npdu.protocol_version != BACNET_PROTOCOL_VERSION ||
        s_failed_pdu[offset] != PDU_TYPE_CONFIRMED_SERVICE_REQUEST ||
        s_failed_pdu[offset + 2] != invoke_id ||
        s_failed_pdu[offset + 3] != SERVICE_CONFIRMED_COV_NOTIFICATION) {
        return;
    }
    bacapp_property_value_list_init(values, 2);
    cov.listOfValues = values;
    int decoded = cov_notify_decode_service_request(s_failed_pdu + offset + 4,
        length - offset - 4, &cov);
    if (decoded <= 0 || decoded != length - offset - 4) {
        return;
    }
    s_stats.confirmed_timeouts++;
    BACNET_OBJECT_TYPE type = (BACNET_OBJECT_TYPE)cov.monitoredObjectIdentifier.type;
    uint32_t instance = cov.monitoredObjectIdentifier.instance;
    recovery_object_t *entry = find(type, instance);
    if (entry) {
        /* Another failed recipient must not postpone a refresh already due. */
        return;
    }
    for (unsigned i = 0; i < RECOVERY_OBJECTS; ++i) {
        if (!s_pending[i].used) {
            s_pending[i] = (recovery_object_t){.used = true, .type = type,
                .instance = instance, .remaining_ms = RECOVERY_PAUSE_MS};
            s_stats.pending_objects++;
            return;
        }
    }
    s_stats.capacity_errors++;
}

void bacnet_cov_recovery_init(void)
{
    memset(s_pending, 0, sizeof(s_pending));
    memset(&s_stats, 0, sizeof(s_stats));
    tsm_set_timeout_handler(confirmed_timeout);
}

void bacnet_cov_recovery_timer_milliseconds(uint32_t elapsed_ms)
{
    for (unsigned i = 0; i < RECOVERY_OBJECTS; ++i) {
        recovery_object_t *entry = &s_pending[i];
        if (entry->used) {
            entry->remaining_ms = elapsed_ms >= entry->remaining_ms ? 0 :
                entry->remaining_ms - elapsed_ms;
        }
    }
}

bool bacnet_cov_recovery_pending(BACNET_OBJECT_TYPE type, uint32_t instance)
{
    recovery_object_t *entry = find(type, instance);
    return entry && entry->remaining_ms == 0;
}

void bacnet_cov_recovery_clear(BACNET_OBJECT_TYPE type, uint32_t instance)
{
    recovery_object_t *entry = find(type, instance);
    if (entry) {
        if (entry->remaining_ms == 0) {
            s_stats.refresh_requests++;
        }
        entry->used = false;
        s_stats.pending_objects--;
    }
}

void bacnet_cov_recovery_stats(bacnet_cov_recovery_stats_t *stats)
{
    if (stats) {
        *stats = s_stats;
    }
}
