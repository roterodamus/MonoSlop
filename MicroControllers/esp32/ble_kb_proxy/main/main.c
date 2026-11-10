/*
 * USB keyboard -> BLE keyboard proxy for YD-ESP32-S3 (ESP32-S3, ESP-IDF 5.x)
 *
 * Plug the keyboard into the native "USB" port (GPIO19/20). The keyboard's
 * boot-protocol interface is switched to the fixed 8-byte report format and
 * every report is forwarded as a BLE HID (HOGP) notification.
 * Vial's raw-HID interface is ignored, so Vial itself still needs a wired
 * connection for configuration.
 *
 * Hold BOOT (GPIO0) while resetting to wipe all BLE bonds.
 */
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/usb_host.h"
#include "usb/hid_host.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define DEVICE_NAME   "Vial BLE Keyboard"
#define BOOT_BTN_GPIO GPIO_NUM_0
#define REPORT_LEN    8

static const char *TAG = "kbproxy";

/* ------------------------------------------------------------------ */
/* BLE HID state                                                       */
/* ------------------------------------------------------------------ */
static uint8_t  own_addr_type;
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t input_handle;
static bool     notify_enabled;
static uint8_t  protocol_mode = 1;        /* 1 = report protocol */
static uint8_t  led_state;                /* host -> keyboard LEDs */
static uint8_t  battery_level = 100;

static volatile hid_host_device_handle_t s_kbd;  /* active USB keyboard iface */

static const uint8_t hid_info[4] = { 0x11, 0x01, 0x00, 0x02 };

static const uint8_t report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,             /* Keyboard, Application   */
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,             /*  Modifiers              */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
    0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,             /*  Reserved byte          */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08,             /*  LEDs (output)          */
    0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,             /*  LED padding            */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, /*  6 keys                 */
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00,
    0xC0
};

/* Report Reference descriptor values: {report id, type} (1=input, 2=output) */
static const uint8_t ref_input[2]  = { 0, 1 };
static const uint8_t ref_output[2] = { 0, 2 };

static void start_adv(void);

static int hid_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc = 0;
    switch (ble_uuid_u16(ctxt->chr->uuid)) {
    case 0x2A4A: /* HID Information */
        rc = os_mbuf_append(ctxt->om, hid_info, sizeof(hid_info));
        break;
    case 0x2A4B: /* Report Map */
        rc = os_mbuf_append(ctxt->om, report_map, sizeof(report_map));
        break;
    case 0x2A4C: /* Control Point (suspend/resume) - ignored */
        return 0;
    case 0x2A4E: /* Protocol Mode */
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            rc = os_mbuf_append(ctxt->om, &protocol_mode, 1);
        } else if (OS_MBUF_PKTLEN(ctxt->om) >= 1) {
            os_mbuf_copydata(ctxt->om, 0, 1, &protocol_mode);
            return 0;
        }
        break;
    case 0x2A4D: /* Report (input or output) */
        if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
            static const uint8_t zeros[REPORT_LEN];
            rc = (attr == input_handle)
                     ? os_mbuf_append(ctxt->om, zeros, REPORT_LEN)
                     : os_mbuf_append(ctxt->om, &led_state, 1);
        } else if (OS_MBUF_PKTLEN(ctxt->om) >= 1) {
            os_mbuf_copydata(ctxt->om, 0, 1, &led_state);
            hid_host_device_handle_t k = s_kbd;
            if (k) {
                hid_class_request_set_report(k, HID_REPORT_TYPE_OUTPUT, 0, &led_state, 1);
            }
            return 0;
        }
        break;
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int report_ref_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, arg, 2) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int battery_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, &battery_level, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {   /* Battery Service */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = battery_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC },
            { 0 }
        },
    },
    {   /* HID Service */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = hid_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = hid_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = hid_access,
              .flags = BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = hid_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access,
              .val_handle = &input_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908),
                    .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                    .access_cb = report_ref_access, .arg = (void *)ref_input },
                  { 0 } } },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                       BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                       BLE_GATT_CHR_F_WRITE_NO_RSP,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908),
                    .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                    .access_cb = report_ref_access, .arg = (void *)ref_output },
                  { 0 } } },
            { 0 }
        },
    },
    { 0 }
};

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    struct ble_gap_conn_desc desc;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            conn_handle = ev->connect.conn_handle;
            ESP_LOGI(TAG, "BLE connected");
            ble_gap_security_initiate(conn_handle);
        } else {
            start_adv();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected (reason %d)", ev->disconnect.reason);
        conn_handle = BLE_HS_CONN_HANDLE_NONE;
        notify_enabled = false;
        start_adv();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_adv();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == input_handle) {
            notify_enabled = ev->subscribe.cur_notify;
            ESP_LOGI(TAG, "Input report notifications %s", notify_enabled ? "on" : "off");
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption change, status=%d", ev->enc_change.status);
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Peer lost its bond: drop ours and let it re-pair */
        ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc);
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    default:
        break;
    }
    return 0;
}

static void start_adv(void)
{
    static ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    struct ble_hs_adv_fields f = { 0 };
    const char *name = ble_svc_gap_device_name();

    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.appearance = 0x03C1; /* HID keyboard */
    f.appearance_is_present = 1;
    f.uuids16 = &hid_uuid;
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    f.name = (uint8_t *)name;
    f.name_len = strlen(name);
    f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);

    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    start_adv();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_send_report(const uint8_t *rpt)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || !notify_enabled) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(rpt, REPORT_LEN);
    if (om) {
        int rc = ble_gatts_notify_custom(conn_handle, input_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify failed: %d", rc);
        }
    }
}

static void ble_init(void)
{
    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;   /* Just Works */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_svc_gap_device_appearance_set(0x03C1);

    extern void ble_store_config_init(void);
    ble_store_config_init();

    nimble_port_freertos_init(host_task);
}

/* ------------------------------------------------------------------ */
/* USB host side                                                       */
/* ------------------------------------------------------------------ */
static void hid_iface_cb(hid_host_device_handle_t h, const hid_host_interface_event_t event, void *arg)
{
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[64];
        size_t len = 0;
        if (hid_host_device_get_raw_input_report_data(h, data, sizeof(data), &len) != ESP_OK) {
            break;
        }
        uint8_t rpt[REPORT_LEN] = { 0 };
        memcpy(rpt, data, len < REPORT_LEN ? len : REPORT_LEN);
        ble_send_report(rpt);
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED: {
        ESP_LOGI(TAG, "USB keyboard removed");
        if (s_kbd == h) {
            s_kbd = NULL;
        }
        hid_host_device_close(h);
        uint8_t release[REPORT_LEN] = { 0 };   /* don't leave keys stuck */
        ble_send_report(release);
        break;
    }
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "USB transfer error");
        break;
    default:
        break;
    }
}

typedef struct {
    hid_host_device_handle_t h;
    hid_subclass_t sub_class;
} kbd_setup_t;

/* Runs outside the HID driver task so control transfers can complete */
static void kbd_setup_task(void *arg)
{
    kbd_setup_t s = *(kbd_setup_t *)arg;
    free(arg);
    vTaskDelay(pdMS_TO_TICKS(100));

    if (s.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
        esp_err_t e = hid_class_request_set_protocol(s.h, HID_REPORT_PROTOCOL_BOOT);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "set_protocol(boot) failed: %s (continuing)", esp_err_to_name(e));
        }
        e = hid_class_request_set_idle(s.h, 0, 0);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "set_idle failed: %s (continuing)", esp_err_to_name(e));
        }
    }

    esp_err_t e = hid_host_device_start(s.h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "hid_host_device_start failed: %s", esp_err_to_name(e));
        vTaskDelete(NULL);
    }
    s_kbd = s.h;
    ESP_LOGI(TAG, "USB keyboard streaming");

    if (led_state) {
        hid_class_request_set_report(s.h, HID_REPORT_TYPE_OUTPUT, 0, &led_state, 1);
    }
    vTaskDelete(NULL);
}

static void hid_driver_cb(hid_host_device_handle_t h, const hid_host_driver_event_t event, void *arg)
{
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) {
        return;
    }
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(h, &params) != ESP_OK) {
        return;
    }

    /* Only the keyboard interface; skips Vial raw HID, mouse, consumer, etc. */
    if (params.proto != HID_PROTOCOL_KEYBOARD) {
        ESP_LOGI(TAG, "Ignoring HID iface %d (proto %d)", params.iface_num, params.proto);
        return;
    }
    ESP_LOGI(TAG, "USB keyboard found (iface %d)", params.iface_num);

    const hid_host_device_config_t cfg = {
        .callback = hid_iface_cb,
        .callback_arg = NULL,
    };
    if (hid_host_device_open(h, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "device_open failed");
        return;
    }

    kbd_setup_t *s = malloc(sizeof(*s));
    if (!s) {
        return;
    }
    s->h = h;
    s->sub_class = params.sub_class;
    xTaskCreate(kbd_setup_task, "kbd_setup", 3072, s, 4, NULL);
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&cfg));
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void usb_init(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096,
                                            xTaskGetCurrentTaskHandle(), 2, NULL, 0);
    assert(ok == pdTRUE);
    ulTaskNotifyTake(pdFALSE, portMAX_DELAY);   /* wait for usb_host_install */

    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_driver_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_cfg));
}

/* ------------------------------------------------------------------ */
void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOOT_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    bool wipe_bonds = (gpio_get_level(BOOT_BTN_GPIO) == 0);

    ble_init();
    if (wipe_bonds) {
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP_LOGW(TAG, "BOOT held: clearing all bonds");
        ble_store_clear();
    }
    usb_init();
    ESP_LOGI(TAG, "Ready. Pair with \"%s\" from your host.", DEVICE_NAME);
}
