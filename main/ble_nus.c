// Nordic UART Service on NimBLE - see ble_nus.h for the protocol context.
//
// Structure (top to bottom):
//   1. NUS 128-bit UUIDs
//   2. RX ring buffer  (incoming bytes from the central)
//   3. GATT service definition (RX = WRITE encrypted, TX = NOTIFY encrypted)
//   4. GAP event handler (connect, disconnect, encrypt, passkey, MTU, sub)
//   5. Host sync + advertising start
//   6. Public API (init / connected / secure / passkey / clearBonds / I/O)
//
// The source project's ble_bridge.cpp (Arduino BLEDevice) is the behavioral
// spec. Crucial differences to remember vs Arduino: NimBLE is fully
// asynchronous (no createService/start), characteristic permission flags
// are baked into ble_gatt_chr_def.flags, and the security stack is
// configured globally on ble_hs_cfg before nimble_port_freertos_init().
#include "ble_nus.h"
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_timer.h"

#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ble";

// Provided by NimBLE's ble_store_config component (linked in via `bt`).
// Declared rather than #include'd because the header lives in a private
// dir; this matches what the bleprph example does.
extern void ble_store_config_init(void);

// ─────────────────────────────────────────────────────────────────────
// 1. Nordic UART Service UUIDs (128-bit, little-endian byte order)
// ─────────────────────────────────────────────────────────────────────
// 6e400001-b5a3-f393-e0a9-e50e24dcca9e  (service)
// 6e400002-...                          (RX, write)
// 6e400003-...                          (TX, notify)
static const ble_uuid128_t NUS_SVC_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

static const ble_uuid128_t NUS_RX_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

static const ble_uuid128_t NUS_TX_UUID =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint16_t tx_val_handle;   // populated when the chr is registered

// Live link state
static uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool  link_secure = false;
static volatile uint32_t pending_passkey = 0;
static volatile bool  tx_subscribed = false;
static volatile uint16_t link_mtu = 23;   // ATT default until negotiated

static uint8_t own_addr_type;
static char    adv_name[20];

// 30 s backoff after a stale-triggered disconnect. Prevents CoreBluetooth
// from auto-reconnecting via stored LTK before the Hardware Buddy UI has
// had a chance to notice the link is gone and reset its session state.
static esp_timer_handle_t s_adv_delay_timer = NULL;
static volatile bool      s_stale_disconnect = false;

// Deferred encryption-kick timer. Stored here so the previous timer can be
// stopped and deleted on each new connection instead of leaking one handle
// per reconnect cycle.
static esp_timer_handle_t s_sec_kick_timer = NULL;

static void start_advertising(void);  // forward-decl for adv_delay_cb

static void adv_delay_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "[ble] stale backoff elapsed - re-advertising");
    start_advertising();
}

// ─────────────────────────────────────────────────────────────────────
// 2. RX ring buffer - drained by ble_read() / ble_available()
// ─────────────────────────────────────────────────────────────────────
// Sized to comfortably hold a heartbeat snapshot JSON + headroom.
// Writes that would overrun are dropped (the central re-sends on a
// new heartbeat; we never silently truncate a partial line).
#define RX_CAP 2048
static uint8_t  rx_buf[RX_CAP];
static volatile size_t rx_head = 0, rx_tail = 0;

static void rx_push(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        size_t next = (rx_head + 1) % RX_CAP;
        if (next == rx_tail) return;   // full → drop (upstream should drain)
        rx_buf[rx_head] = p[i];
        rx_head = next;
    }
}

// ─────────────────────────────────────────────────────────────────────
// 3. GATT service definition
// ─────────────────────────────────────────────────────────────────────

static int nus_chr_access(uint16_t conn_h, uint16_t attr_h,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // Drain the inbound mbuf into the RX ring. ble_hs_mbuf_to_flat
        // works for fragmented mbufs without us caring about chains.
        uint8_t tmp[256];
        uint16_t out_len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof(tmp), &out_len);
        if (rc == 0 && out_len > 0) rx_push(tmp, out_len);
        return 0;
    }
    return 0;  // TX is notify-only; no READ/WRITE on it
}

// One service, two characteristics. Both encrypted (LE Secure + bonded):
//   RX  WRITE | WRITE_NO_RSP, ENC
//   TX  NOTIFY, ENC  (also marks the CCCD permission)
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type        = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid        = &NUS_SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &NUS_RX_UUID.u,
                .access_cb  = nus_chr_access,
                .flags      = BLE_GATT_CHR_F_WRITE
                            | BLE_GATT_CHR_F_WRITE_NO_RSP
                            | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid       = &NUS_TX_UUID.u,
                .access_cb  = nus_chr_access,
                .val_handle = &tx_val_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY
                            | BLE_GATT_CHR_F_READ_ENC,
            },
            { 0 }   // end of characteristics
        },
    },
    { 0 }           // end of services
};

// ─────────────────────────────────────────────────────────────────────
// 4. GAP event handler
// ─────────────────────────────────────────────────────────────────────

// Deferred encryption-initiate callback (esp_timer one-shot, dispatched
// on the timer task). If the link is still up and NOT yet encrypted,
// kick the GAP security procedure ourselves. Used to avoid racing the
// central - see the BLE_GAP_EVENT_CONNECT handler for the full story.
static void sec_kick_cb(void *arg)
{
    uint16_t conn_h = (uint16_t)(uintptr_t)arg;
    if (conn_handle != conn_h) return;     // link changed already
    if (link_secure) {
        ESP_LOGI(TAG, "[ble] sec kick: link already encrypted, no action");
        return;
    }
    ESP_LOGI(TAG, "[ble] sec kick: initiating encryption (link still plaintext)");
    int rc = ble_gap_security_initiate(conn_h);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "[ble] security_initiate failed: %d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            conn_handle  = event->connect.conn_handle;
            ESP_LOGI(TAG, "[ble] connected (handle=%u)", conn_handle);
            // Give the central ~700ms to initiate encryption on its own.
            // If it doesn't (some stacks don't on reconnect), we'll
            // kick it ourselves from the deferred check below. Doing
            // it immediately races with the central's own request and
            // ends in a key-renegotiation loop the desktop interprets
            // as "stale bond" - it then sends {"cmd":"unpair"} and
            // drops the link with reason=531. The delay lets the
            // central win the encryption race when it intends to.
            // Reuse s_sec_kick_timer across connections; stop and delete
            // any still-running instance before creating a fresh one.
            if (s_sec_kick_timer != NULL) {
                esp_timer_stop(s_sec_kick_timer);
                esp_timer_delete(s_sec_kick_timer);
                s_sec_kick_timer = NULL;
            }
            esp_timer_create_args_t a = {
                .callback = sec_kick_cb,
                .arg = (void *)(uintptr_t)event->connect.conn_handle,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "sec_kick",
            };
            if (esp_timer_create(&a, &s_sec_kick_timer) == ESP_OK) {
                esp_timer_start_once(s_sec_kick_timer, 700 * 1000);
            }
        } else {
            ESP_LOGW(TAG, "[ble] connect failed (status=%d) - re-advertising",
                     event->connect.status);
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "[ble] disconnected (reason=%d)", event->disconnect.reason);
        conn_handle      = BLE_HS_CONN_HANDLE_NONE;
        link_secure      = false;
        pending_passkey  = 0;
        tx_subscribed    = false;
        link_mtu         = 23;
        if (s_stale_disconnect && s_adv_delay_timer != NULL) {
            // Stale-triggered disconnect: hold off advertising for 30 s so
            // CoreBluetooth cannot silently auto-reconnect via stored LTK.
            // The gap forces Hardware Buddy's UI into "Disconnected" state,
            // giving the user a working Connect button when we re-advertise.
            s_stale_disconnect = false;
            esp_timer_start_once(s_adv_delay_timer, 30ULL * 1000 * 1000);
            ESP_LOGI(TAG, "[ble] stale backoff: re-advertise in 30 s");
        } else {
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        // Encryption result (also fires for bonded reconnects that reuse
        // the stored LTK). Update link_secure regardless of pairing path.
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                link_secure = desc.sec_state.encrypted ? true : false;
                ESP_LOGI(TAG, "[ble] encryption %s (auth=%d, bonded=%d)",
                         link_secure ? "ON" : "off",
                         desc.sec_state.authenticated, desc.sec_state.bonded);
            }
            pending_passkey = 0;  // pairing done either way → clear the screen

            // Encryption-change failure means the central's stored LTK is
            // stale (user wiped the bond on their side, mismatching keys).
            // The link stays up but NUS chars - flagged WRITE_ENC - are
            // unusable, leaving us "connected but mute" until either side
            // drops the conn. Force a clean teardown so the next attempt
            // re-pairs from scratch. Matches the Espressif reference.
            if (event->enc_change.status != 0) {
                ESP_LOGW(TAG, "[ble] enc_change failed (status=%d) - terminating",
                         event->enc_change.status);
                ble_gap_terminate(event->enc_change.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
            }
        }
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        // DisplayOnly IO: stack hands us a passkey to show on screen.
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            // Random 6-digit (000000-999999). The source uses the stack's
            // suggested key; we generate one so it's unpredictable per
            // pair attempt even across reboots.
            uint32_t pk = esp_random() % 1000000UL;
            struct ble_sm_io io = {
                .action  = BLE_SM_IOACT_DISP,
                .passkey = pk,
            };
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            if (rc == 0) {
                pending_passkey = pk;
                // Defense in depth: never log the passkey digits. It's on
                // the device's own display (the source of truth); putting
                // it in the in-RAM log ring meant /debug/log leaked the
                // active pair window to any LAN peer with HTTP access.
                // The trust gate already hides /debug, but the device UX
                // is the only place this value belongs.
                ESP_LOGI(TAG, "[ble] passkey ****** - see device screen");
            } else {
                ESP_LOGE(TAG, "[ble] ble_sm_inject_io failed: %d", rc);
            }
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == tx_val_handle) {
            tx_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "[ble] TX %s", tx_subscribed ? "subscribed" : "unsubscribed");
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        link_mtu = event->mtu.value;
        ESP_LOGI(TAG, "[ble] MTU = %u", link_mtu);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // Bonded peer is re-pairing (e.g. user clicked "Forget" on the
        // desktop). Drop the old bond and accept the fresh pairing -
        // matches the source's behavior and the desktop's expectations.
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        return 0;
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────
// 5. Host sync + advertising
// ─────────────────────────────────────────────────────────────────────

static void start_advertising(void)
{
    if (ble_gap_adv_active()) return;

    // Advertising payload is capped at 31 bytes. Flags(3) + name "Claude-XXXX"(13)
    // + 128-bit NUS UUID(18) = 34, which overflows. Standard split:
    //   primary adv  = flags + complete name      (~16 B)
    //   scan rsp     = 128-bit service UUID       (~18 B)
    // Both go out together; the desktop's NUS-filtered scan picks us up
    // via the scan response.
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)adv_name;
    fields.name_len = strlen(adv_name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&NUS_SVC_UUID;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as '%s'", adv_name);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    assert(rc == 0);
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "[ble] host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    ESP_LOGI(TAG, "[ble] host task started");
    nimble_port_run();           // returns only on stop
    nimble_port_freertos_deinit();
}

// ─────────────────────────────────────────────────────────────────────
// 6. Public API
// ─────────────────────────────────────────────────────────────────────

void ble_init(const char *name)
{
    if (name && *name) {
        strncpy(adv_name, name, sizeof(adv_name) - 1);
        adv_name[sizeof(adv_name) - 1] = '\0';
    } else {
        strncpy(adv_name, "Claude", sizeof(adv_name));
    }

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return;
    }

    // --- Host config: callbacks + security ---
    ble_hs_cfg.reset_cb        = on_reset;
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // LE Secure Connections + bonding + MITM, DisplayOnly IO.
    // REFERENCE.md: NUS chars are encrypted-only → first GATT access
    // triggers pairing; the desktop prompts the user for the passkey
    // we display. Reconnects reuse the stored LTK silently.
    ble_hs_cfg.sm_io_cap        = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding       = 1;
    ble_hs_cfg.sm_mitm          = 1;
    ble_hs_cfg.sm_sc            = 1;
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    // --- Services: GAP + GATT (auto), then NUS ---
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(gatt_svcs);
    assert(rc == 0);

    rc = ble_svc_gap_device_name_set(adv_name);
    assert(rc == 0);

    // Persistent bond storage (LTKs, IRKs in NVS namespace nimble_bonds).
    ble_store_config_init();

    // One-shot timer for the stale-disconnect advertising backoff.
    esp_timer_create_args_t adv_ta = {
        .callback        = adv_delay_cb,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "adv_delay",
    };
    esp_timer_create(&adv_ta, &s_adv_delay_timer);

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "[ble] initialized, will advertise on host sync");
}

bool ble_connected(void)
{
    return conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_secure(void)
{
    return link_secure;
}

uint32_t ble_passkey(void)
{
    return pending_passkey;
}

void ble_clear_bonds(void)
{
    // Wipe every stored bond, then terminate any active link so it doesn't
    // sit there encrypted-but-orphaned (NVS LTKs gone, link still trusts
    // them in stack state). The disconnect handler will re-advertise; the
    // next pair attempt yields a fresh passkey on both sides.
    int rc = ble_store_util_delete_peer(NULL);    // delete-all variant
    ESP_LOGI(TAG, "[ble] cleared bonds (rc=%d)", rc);
    if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

bool ble_disconnect(void)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
    // BLE_ERR_REM_USER_CONN_TERM (0x13) = "remote user terminated" - the
    // standard reason code for an app-initiated disconnect. The GAP
    // disconnect handler will fire and start_advertising() will be
    // called from there, so the link comes back up cleanly.
    int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    ESP_LOGI(TAG, "[ble] forced disconnect requested (rc=%d)", rc);
    return rc == 0;
}

void ble_arm_stale_delay(void)
{
    s_stale_disconnect = true;
}

size_t ble_available(void)
{
    return (rx_head + RX_CAP - rx_tail) % RX_CAP;
}

int ble_read(void)
{
    if (rx_head == rx_tail) return -1;
    int b = rx_buf[rx_tail];
    rx_tail = (rx_tail + 1) % RX_CAP;
    return b;
}

size_t ble_write(const uint8_t *data, size_t len)
{
    // tx_ready = connected + subscribed + encrypted. Between CONNECT and
    // ENC_CHANGE there's a window where the central may have already
    // subscribed but the link isn't encrypted yet; notifying in that
    // window returns BLE_HS_EAUTHEN. Refuse the send up-front so callers
    // see a clean 0 instead of a partial-send + warning log.
    if (!ble_connected() || !tx_subscribed || !link_secure ||
        tx_val_handle == 0) return 0;

    // Per-notify max is (MTU - 3). Cap at 244 (Espressif reference's
    // NOTIFY_CHUNK_CAP) so we ride the upper limit of negotiated MTUs;
    // macOS Sequoia + DLE can push to 251. Pre-MTU-exchange the link
    // is still at the 23-byte ATT default, hence the 20-byte floor.
    size_t chunk = (link_mtu > 3) ? (size_t)(link_mtu - 3) : 20;
    if (chunk > 244) chunk = 244;

    // The 4 ms inter-chunk delay was an early defensive sleep to let the
    // host drain. With a negotiated MTU > 100 the link is healthy and the
    // notify completion already provides flow control; the sleep just
    // adds ~12 ms latency to a 600-byte status ack. Skip it past the
    // ATT-default stage.
    const TickType_t chunk_delay = (link_mtu > 100) ? 0 : pdMS_TO_TICKS(2);

    size_t sent = 0;
    while (sent < len) {
        size_t n = len - sent;
        if (n > chunk) n = chunk;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data + sent, n);
        if (!om) break;   // no mbuf - out of memory; bail honestly
        int rc = ble_gatts_notify_custom(conn_handle, tx_val_handle, om);
        if (rc != 0) {
            // mbuf is consumed by ble_gatts_notify_custom even on error
            ESP_LOGW(TAG, "[ble] notify failed: %d", rc);
            break;
        }
        sent += n;
        if (chunk_delay) vTaskDelay(chunk_delay);
    }
    return sent;
}
