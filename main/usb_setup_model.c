/* SPDX-License-Identifier: Apache-2.0 */
#include "usb_setup_model.h"

#include <string.h>

void usb_setup_model_init(usb_setup_model_t *model, uint64_t now_ms, bool new_key)
{
    memset(model, 0, sizeof(*model));
    model->export_until_ms = new_key ? now_ms + USB_SETUP_FIRST_BOOT_MS : 0;
}

void usb_setup_model_button(usb_setup_model_t *model, uint64_t now_ms, bool pressed)
{
    if (pressed && !model->button_pressed) {
        model->button_since_ms = now_ms;
        model->button_handled = false;
    }
    if (pressed && !model->button_handled &&
        now_ms - model->button_since_ms >= USB_SETUP_BUTTON_HOLD_MS) {
        model->export_until_ms = now_ms + USB_SETUP_UNLOCK_MS;
        model->button_handled = true;
    }
    model->button_pressed = pressed;
}

bool usb_setup_model_key_allowed(const usb_setup_model_t *model, uint64_t now_ms)
{
    return now_ms < model->export_until_ms;
}

void usb_setup_model_lock(usb_setup_model_t *model)
{
    model->export_until_ms = 0;
    /* A button already held down must be released before another unlock. */
    model->button_handled = model->button_pressed;
}

static bool parse_line(const char *line, usb_setup_request_t *request)
{
    const size_t prefix_length = sizeof(USB_SETUP_PREFIX) - 1U;
    const size_t command_offset = prefix_length + 1U + USB_SETUP_ID_LENGTH + 1U;
    if (strlen(line) <= command_offset ||
        strncmp(line, USB_SETUP_PREFIX " ", prefix_length + 1U) != 0 ||
        line[command_offset - 1U] != ' ') {
        return false;
    }
    for (size_t i = 0; i < USB_SETUP_ID_LENGTH; ++i) {
        const char c = line[prefix_length + 1U + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    const char *command = line + command_offset;
    if (strcmp(command, "STATUS") == 0) {
        request->command = USB_SETUP_STATUS;
    } else if (strcmp(command, "KEY") == 0) {
        request->command = USB_SETUP_KEY;
    } else if (strcmp(command, "LOCK") == 0) {
        request->command = USB_SETUP_LOCK;
    } else {
        return false;
    }
    memcpy(request->id, line + prefix_length + 1U, USB_SETUP_ID_LENGTH);
    request->id[USB_SETUP_ID_LENGTH] = '\0';
    return true;
}

bool usb_setup_model_receive(usb_setup_model_t *model, uint8_t byte,
    usb_setup_request_t *request)
{
    if (byte == '\n') {
        bool ready = false;
        if (!model->discard_line) {
            if (model->line_length && model->line[model->line_length - 1U] == '\r') {
                --model->line_length;
            }
            model->line[model->line_length] = '\0';
            ready = parse_line(model->line, request);
        }
        model->discard_line = false;
        model->line_length = 0;
        return ready;
    }
    if (model->discard_line) {
        return false;
    }
    if ((byte < 0x20U && byte != '\r') || byte > 0x7eU ||
        model->line_length >= sizeof(model->line) - 1U) {
        model->discard_line = true;
        return false;
    }
    model->line[model->line_length++] = (char)byte;
    return false;
}
