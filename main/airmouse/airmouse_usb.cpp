#include "airmouse_usb.h"

extern "C" {
#include "tinyusb.h"
#include "class/hid/hid_device.h"
}

#include <cstdint>

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

static bool s_usb_inited = false;
static bool s_usb_active = false;

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(HID_ITF_PROTOCOL_MOUSE))
};

static const char *s_hid_string_descriptor[5] = {
    (const char[]){0x09, 0x04}, // 0: English (0x0409)
    "M5StickS3",                // 1: Manufacturer
    "StickS3 AirMouse",         // 2: Product
    "S3AIR001",                 // 3: Serial
    "StickS3 HID Mouse",        // 4: HID Interface
};

static const uint8_t s_hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(s_hid_report_descriptor), 0x81, 16, 4),
};

extern "C" {

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

} // extern "C"

bool airmouse_usb_start() {
    s_usb_active = true;
    if (s_usb_inited) return true;

    tinyusb_config_t tusb_cfg = {};
    tusb_cfg.device_descriptor = nullptr;
    tusb_cfg.string_descriptor = s_hid_string_descriptor;
    tusb_cfg.string_descriptor_count =
        sizeof(s_hid_string_descriptor) / sizeof(s_hid_string_descriptor[0]);
    tusb_cfg.external_phy = false;
    tusb_cfg.configuration_descriptor = s_hid_configuration_descriptor;
    tusb_cfg.self_powered = false;
    tusb_cfg.vbus_monitor_io = -1;

    if (tinyusb_driver_install(&tusb_cfg) != ESP_OK) {
        return false;
    }
    s_usb_inited = true;
    return true;
}

void airmouse_usb_stop() {
    if (s_usb_inited && tud_mounted() && tud_hid_ready()) {
        tud_hid_mouse_report(HID_ITF_PROTOCOL_MOUSE, 0, 0, 0, 0, 0);
    }
    s_usb_active = false;
}

bool airmouse_usb_was_inited() {
    return s_usb_inited;
}

bool airmouse_usb_is_connected() {
    return s_usb_inited && s_usb_active && tud_mounted();
}

bool airmouse_usb_is_suspended() {
    return s_usb_inited && tud_suspended();
}

void airmouse_usb_send(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel) {
    if (!s_usb_inited || !s_usb_active) return;
    if (!tud_mounted()) return;
    if (tud_suspended()) {
        if (buttons != 0) {
            tud_remote_wakeup();
        }
        return;
    }
    if (!tud_hid_ready()) return;
    tud_hid_mouse_report(HID_ITF_PROTOCOL_MOUSE, buttons, dx, dy, wheel, 0);
}
