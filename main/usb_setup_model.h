/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USB_SETUP_PREFIX "BACNET-USB/1"
#define USB_SETUP_ID_LENGTH 16U
#define USB_SETUP_LINE_SIZE 96U
#define USB_SETUP_FIRST_BOOT_MS 300000ULL
#define USB_SETUP_UNLOCK_MS 60000ULL
#define USB_SETUP_BUTTON_HOLD_MS 3000ULL

typedef enum {
    USB_SETUP_STATUS,
    USB_SETUP_KEY,
    USB_SETUP_LOCK,
} usb_setup_command_t;

typedef struct {
    char id[USB_SETUP_ID_LENGTH + 1U];
    usb_setup_command_t command;
} usb_setup_request_t;

typedef struct {
    uint64_t export_until_ms;
    uint64_t button_since_ms;
    bool button_pressed;
    bool button_handled;
    bool discard_line;
    size_t line_length;
    char line[USB_SETUP_LINE_SIZE];
} usb_setup_model_t;

void usb_setup_model_init(usb_setup_model_t *model, uint64_t now_ms, bool new_key);
void usb_setup_model_button(usb_setup_model_t *model, uint64_t now_ms, bool pressed);
bool usb_setup_model_key_allowed(const usb_setup_model_t *model, uint64_t now_ms);
void usb_setup_model_lock(usb_setup_model_t *model);
bool usb_setup_model_receive(usb_setup_model_t *model, uint8_t byte,
    usb_setup_request_t *request);
