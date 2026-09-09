/* SPDX-License-Identifier: 0BSD */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bacnet_instance.h"

static unsigned advance(bacnet_instance_t *self, uint64_t end)
{
    unsigned actions = 0;
    for (uint64_t t = 0; t <= end; t += 10) {
        actions |= bacnet_instance_tick(self, t);
    }
    return actions;
}

static void selection_and_lock(void)
{
    bacnet_instance_t self;
    bacnet_instance_start(&self, true, 599153, 42, 155, 0);
    bacnet_instance_observe(&self, 599153, 154, 100);
    bacnet_instance_observe(&self, 42, 156, 100);
    unsigned actions = advance(&self, 7000);
    assert((actions & (INSTANCE_QUERY | INSTANCE_ANNOUNCE | INSTANCE_SAVE)) == 7U);
    assert(self.state == INSTANCE_SAVING);
    assert(self.candidate >= BACNET_INSTANCE_FIRST && self.candidate <= BACNET_INSTANCE_LAST);
    assert(self.candidate != 599153);
    uint32_t selected = self.candidate;
    bacnet_instance_saved(&self, true, 7000);
    assert(self.state == INSTANCE_READY && !self.automatic);
    bacnet_instance_observe(&self, selected, 157, 7200);
    assert(self.conflict && self.candidate == selected && self.state == INSTANCE_READY);
    assert(strcmp(bacnet_instance_state_name(&self), "locked-conflict") == 0);
    for (uint64_t t = 7200; t < 180000; t += 1000) {
        assert(!(bacnet_instance_tick(&self, t) & (INSTANCE_SAVE | INSTANCE_ANNOUNCE)));
    }
    /* A reboot uses the persisted ID in locked mode, even if occupied. */
    bacnet_instance_start(&self, false, selected, 999, 155, 0);
    bacnet_instance_observe(&self, selected, 154, 100);
    assert(self.state == INSTANCE_READY && self.candidate == selected && self.conflict);
}

static void manual_and_saved_ids(void)
{
    bacnet_instance_t self;
    bacnet_instance_start(&self, false, 4194302, 42, 155, 0);
    assert(advance(&self, 120000) == INSTANCE_QUERY);
    bacnet_instance_observe(&self, 4194302, 154, 120001);
    assert(self.candidate == 4194302 && self.conflict);
    bacnet_instance_start(&self, true, 599153, 42, 155, 0);
    bacnet_instance_observe(&self, 599153, 155, 100); /* own/forwarded echo */
    advance(&self, 7000);
    assert(self.candidate == 599153 && !self.conflict);
    bacnet_instance_saved(&self, false, 7000);
    assert(self.state == INSTANCE_SAVE_FAILED);
    assert(bacnet_instance_tick(&self, 100000) == 0);
}

static void range_limits(void)
{
    bacnet_instance_t self;
    bacnet_instance_start(&self, true, BACNET_INSTANCE_NONE, 0, 155, 0);
    for (uint32_t id = BACNET_INSTANCE_FIRST; id < BACNET_INSTANCE_LAST; id++) {
        bacnet_instance_observe(&self, id, 154, 100);
    }
    advance(&self, 7000);
    assert(self.candidate == BACNET_INSTANCE_LAST && self.state == INSTANCE_SAVING);
    bacnet_instance_start(&self, true, 599153, 1, 155, 0);
    for (uint32_t id = BACNET_INSTANCE_FIRST; id <= BACNET_INSTANCE_LAST; id++) {
        bacnet_instance_observe(&self, id, 154, 100);
    }
    assert(!(advance(&self, 7000) & (INSTANCE_SAVE | INSTANCE_ANNOUNCE)));
    assert(self.state == INSTANCE_RANGE_FULL);
    uint64_t retry = self.deadline_ms;
    bacnet_instance_tick(&self, retry);
    assert(self.state == INSTANCE_DISCOVERING);
    for (uint64_t t = retry; t <= retry + 7000; t += 10) {
        bacnet_instance_tick(&self, t);
    }
    assert(self.state == INSTANCE_SAVING); /* departed devices release the range */
}

static void late_manual_claim(void)
{
    bacnet_instance_t self;
    bacnet_instance_start(&self, true, 599153, 42, 1, 0);
    advance(&self, 4000);
    assert(self.state == INSTANCE_CLAIMING);
    /* A higher address must also win over our not-yet-locked candidate. */
    bacnet_instance_observe(&self, self.candidate, 200, 4100);
    assert(self.state == INSTANCE_DISCOVERING);
    for (uint64_t t = 4100; t < 12000; t += 10) {
        bacnet_instance_tick(&self, t);
    }
    assert(self.state == INSTANCE_SAVING && self.candidate != 599153);
}

static void simultaneous_startup(unsigned count)
{
    bacnet_instance_t nodes[100];
    assert(count <= 100);
    for (unsigned i = 0; i < count; i++) {
        bacnet_instance_start(&nodes[i], true, 599153, 42, i + 1, 0);
        nodes[i].deadline_ms = 3000; /* force competing claims in the same tick */
    }
    for (uint64_t t = 0; t < 120000; t += 20) {
        unsigned actions[100];
        uint32_t announced[100];
        for (unsigned i = 0; i < count; i++) {
            actions[i] = bacnet_instance_tick(&nodes[i], t);
            announced[i] = nodes[i].candidate;
            if (actions[i] & INSTANCE_SAVE) {
                bacnet_instance_saved(&nodes[i], true, t);
                actions[i] |= INSTANCE_ANNOUNCE;
            }
        }
        for (unsigned sender = 0; sender < count; sender++) {
            if (actions[sender] & INSTANCE_ANNOUNCE) {
                for (unsigned receiver = 0; receiver < count; receiver++) {
                    bacnet_instance_observe(&nodes[receiver], announced[sender], sender + 1, t);
                }
            }
            if (actions[sender] & INSTANCE_QUERY) {
                for (unsigned responder = 0; responder < count; responder++) {
                    if (nodes[responder].state == INSTANCE_READY ||
                        nodes[responder].state == INSTANCE_CLAIMING) {
                        bacnet_instance_observe(&nodes[sender], nodes[responder].candidate,
                            responder + 1, t);
                    }
                }
            }
        }
    }
    for (unsigned i = 0; i < count; i++) {
        assert(nodes[i].state == INSTANCE_READY && !nodes[i].automatic);
        for (unsigned j = 0; j < i; j++) {
            assert(nodes[i].candidate != nodes[j].candidate);
        }
    }
}

int main(void)
{
    selection_and_lock();
    manual_and_saved_ids();
    range_limits();
    late_manual_claim();
    simultaneous_startup(2);
    simultaneous_startup(10);
    simultaneous_startup(100);
    puts("BACnet instance discovery, collisions, persistence and startup simulation passed");
    return 0;
}
