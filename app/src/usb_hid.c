/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <device.h>
#include <init.h>

#include <usb/usb_device.h>
#include <usb/class/usb_hid.h>

#include <zmk/usb.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/led_indicators.h>
#include <zmk/event_manager.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static const struct device *hid_dev;

static K_SEM_DEFINE(hid_sem, 1, 1);

static void in_ready_cb(const struct device *dev) { k_sem_give(&hid_sem); }

static void out_ready_cb(const struct device *dev) {
    size_t report_length;
    int rc = hid_int_ep_read(hid_dev, NULL, 0, &report_length);
    if (rc != 0) {
        LOG_ERR("Failed to read USB report length: %d", rc);
        return;
    }

    uint8_t *report = k_malloc(report_length);
    if (report == NULL) {
        LOG_ERR("Failed to allocate memory");
        return;
    }

    rc = hid_int_ep_read(hid_dev, report, report_length, &report_length);
    if (rc != 0) {
        LOG_ERR("Failed to read USB report: %d", rc);
        goto free;
    }

    uint8_t report_id = report[0];

    switch (report_id) {
    case HID_REPORT_ID_LEDS: {
        if (report_length != sizeof(struct zmk_hid_led_report)) {
            LOG_ERR("LED report is malformed: length=%d", report_length);
            goto free;
        }
        struct zmk_hid_led_report *led_report = (struct zmk_hid_led_report *)report;
        zmk_leds_process_report(&led_report->body, ZMK_ENDPOINT_USB, 0);
        break;
    }
    default:
        LOG_WRN("Unsupported host report: %d", report_id);
        break;
    }

free:
    k_free(report);
}

#define HID_GET_REPORT_TYPE_MASK 0xff00
#define HID_GET_REPORT_ID_MASK 0x00ff

#define HID_REPORT_TYPE_INPUT 0x100
#define HID_REPORT_TYPE_OUTPUT 0x200
#define HID_REPORT_TYPE_FEATURE 0x300

static int set_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    if ((setup->wValue & HID_GET_REPORT_TYPE_MASK) != HID_REPORT_TYPE_OUTPUT) {
        LOG_ERR("Unsupported report type %d requested",
                (setup->wValue & HID_GET_REPORT_TYPE_MASK) >> 8);
        return -ENOTSUP;
    }

    switch (setup->wValue & HID_GET_REPORT_ID_MASK) {
    case HID_REPORT_ID_LEDS:
        if (*len != sizeof(struct zmk_hid_led_report_body)) {
            LOG_ERR("LED set report is malformed: length=%d", *len);
        } else {
            struct zmk_hid_led_report_body *report = (struct zmk_hid_led_report_body *)*data;
            zmk_leds_process_report(report, ZMK_ENDPOINT_USB, 0);
        }
        break;
    default:
        LOG_ERR("Invalid report ID %d requested", setup->wValue & HID_GET_REPORT_ID_MASK);
        return -EINVAL;
    }

    return 0;
}

static const struct hid_ops ops = {
    .int_in_ready = in_ready_cb,
    .int_out_ready = out_ready_cb,
    .set_report = set_report_cb,
};

int zmk_usb_hid_send_report(const uint8_t *report, size_t len) {
    switch (zmk_usb_get_status()) {
    case USB_DC_SUSPEND:
        return usb_wakeup_request();
    case USB_DC_ERROR:
    case USB_DC_RESET:
    case USB_DC_DISCONNECTED:
    case USB_DC_UNKNOWN:
        return -ENODEV;
    default:
        k_sem_take(&hid_sem, K_MSEC(30));
        int err = hid_int_ep_write(hid_dev, report, len, NULL);

        if (err) {
            k_sem_give(&hid_sem);
        }

        return err;
    }
}

static int zmk_usb_hid_init(const struct device *_arg) {
    hid_dev = device_get_binding("HID_0");
    if (hid_dev == NULL) {
        LOG_ERR("Unable to locate HID device");
        return -EINVAL;
    }

    usb_hid_register_device(hid_dev, zmk_hid_report_desc, sizeof(zmk_hid_report_desc), &ops);
    usb_hid_init(hid_dev);

    return 0;
}

SYS_INIT(zmk_usb_hid_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
