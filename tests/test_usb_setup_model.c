/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usb_setup_model.h"

static bool feed(usb_setup_model_t *model, const char *text, usb_setup_request_t *request)
{
    bool received = false;
    for (size_t i = 0; i < strlen(text); ++i) {
        received |= usb_setup_model_receive(model, (uint8_t)text[i], request);
    }
    return received;
}

int main(void)
{
    usb_setup_model_t model;
    usb_setup_request_t request;
    usb_setup_model_init(&model, 100, true);
    assert(usb_setup_model_key_allowed(&model, 100));
    assert(usb_setup_model_key_allowed(&model, 300099));
    assert(!usb_setup_model_key_allowed(&model, 300100));
    usb_setup_model_init(&model, 0, false);
    assert(!usb_setup_model_key_allowed(&model, 0));
    usb_setup_model_button(&model, 100, true);
    usb_setup_model_button(&model, 3099, true);
    assert(!usb_setup_model_key_allowed(&model, 3099));
    usb_setup_model_button(&model, 3100, true);
    assert(usb_setup_model_key_allowed(&model, 3100));
    usb_setup_model_button(&model, 63100, true);
    assert(!usb_setup_model_key_allowed(&model, 63100));
    usb_setup_model_button(&model, 63200, false);
    usb_setup_model_button(&model, 64000, true);
    usb_setup_model_button(&model, 67000, true);
    assert(usb_setup_model_key_allowed(&model, 67000));
    usb_setup_model_lock(&model);
    usb_setup_model_button(&model, 70000, true);
    assert(!usb_setup_model_key_allowed(&model, 70000));
    usb_setup_model_button(&model, 71000, false);
    usb_setup_model_button(&model, 72000, true);
    usb_setup_model_button(&model, 73000, false);
    usb_setup_model_button(&model, 74000, true);
    usb_setup_model_button(&model, 75999, true);
    assert(!usb_setup_model_key_allowed(&model, 75999));

    assert(feed(&model, "BACNET-USB/1 0123456789abcdef STATUS\r\n", &request));
    assert(request.command == USB_SETUP_STATUS);
    assert(strcmp(request.id, "0123456789abcdef") == 0);
    assert(feed(&model, "BACNET-USB/1 abcdef0123456789 KEY\n", &request));
    assert(request.command == USB_SETUP_KEY);
    assert(feed(&model, "BACNET-USB/1 0123456789abcdef LOCK\n", &request));
    assert(request.command == USB_SETUP_LOCK);
    const char *bad[] = {
        "\n", "I (12) boot: hello\n", "BACNET-USB/2 0123456789abcdef KEY\n",
        "BACNET-USB/1 0123456789abcde KEY\n", "BACNET-USB/1 0123456789abcdefg KEY\n",
        "BACNET-USB/1 0123456789abcdeg KEY\n", "BACNET-USB/1 0123456789abcdef KEY extra\n",
        "BACNET-USB/1 0123456789abcdef ERASE\n", "BACNET-USB/1 0123456789abcdef \n",
        "BACNET-USB/1 0123456789abcdef\rKEY\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(!feed(&model, bad[i], &request));
    }
    for (unsigned i = 0; i < 10000; ++i) {
        assert(!usb_setup_model_receive(&model, 'x', &request));
    }
    assert(!feed(&model, "BACNET-USB/1 0123456789abcdef KEY\n", &request));
    assert(feed(&model, "BACNET-USB/1 0123456789abcdef STATUS\n", &request));
    assert(!feed(&model, "BACNET-USB/1 0123456789abcdef ", &request));
    assert(!usb_setup_model_receive(&model, 0, &request));
    assert(!feed(&model, "KEY\n", &request));
    assert(feed(&model, "BACNET-USB/1 0123456789abcdef STATUS\n", &request));
    puts("USB setup model: parser boundaries, recovery, timed export, and physical unlock passed");
    return 0;
}
