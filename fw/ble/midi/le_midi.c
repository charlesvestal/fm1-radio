/* BLE MIDI peripheral on JieLi's BLE stack: the MIDI service (03B80E5A-EDE8-4B33-A751-6CE34EC4C700) with
 * its one characteristic (7772E5DB-3868-4112-A1A9-F2669D106BF3: read, write without response, notify),
 * advertised with the service UUID and the name "FM-1". Built on the structure of the SDK's
 * le_trans_data.c (its event handling and connection-parameter requests), without its test profile.
 *
 *   bt_ble_init()                 the stack is up (BT_STATUS_INIT_OK): profile, advertising
 *   ble_midi_send_many(pkts, n)   USB-MIDI packets (cin | status << 8 | d1 << 16 | d2 << 24) to the
 *                                 central, as many as fit one notification; returns how many went; from
 *                                 the main loop (the stack's context)
 *   ble_midi_receive(pkt)         the firmware's: every MIDI message written by the central
 *   ble_midi_connected()          a central is connected and listens (CCC on) */
#include "system/app_core.h"
#include "system/includes.h"
#include "app_config.h"
#include "btstack/btstack_task.h"
#include "btstack/bluetooth.h"
#include "btcontroller_modules.h"
#include "bt_common.h"
#include "le_common.h"
#include "third_party/common/ble_user.h"

#define LOG_TAG_CONST       BLE_MIDI
#define LOG_TAG             "[BLE_MIDI]"
#define LOG_INFO_ENABLE
#include "debug.h"
const char log_tag_const_i_BLE_MIDI = 1;           /* the SDK keeps these in log_config; ours here */

extern void ble_midi_receive(uint32_t pkt);
extern uint32_t shim_ms(void);

/* ---- the GATT database (the SDK's binary form: size, flags, handle, uuid, value). Flags: bit 1 read,
 * bit 2 write without response, bit 3 write, bit 4 notify, 0x100 dynamic, 0x200 128-bit uuid. */
#define MIDI_SVC_UUID  0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03
#define MIDI_CHR_UUID  0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77
#define H_NAME  0x0003
#define H_MIDI  0x0006
#define H_CCC   0x0007
static const uint8_t profile_data[] = {
    /* 0x0001 primary service 1800 (GAP) */
    0x0a, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18,
    /* 0x0002 characteristic 2a00 (device name) read, dynamic; 0x0003 its value */
    0x0d, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03, 0x28, 0x02, 0x03, 0x00, 0x00, 0x2a,
    0x08, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x2a,
    /* 0x0004 primary service MIDI */
    0x18, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x28, MIDI_SVC_UUID,
    /* 0x0005 characteristic MIDI: read | write without response | write | notify (0x1e); 0x0006 its value */
    0x1b, 0x00, 0x02, 0x00, 0x05, 0x00, 0x03, 0x28, 0x1e, 0x06, 0x00, MIDI_CHR_UUID,
    0x16, 0x00, 0x1e, 0x03, 0x06, 0x00, MIDI_CHR_UUID,
    /* 0x0007 client characteristic configuration */
    0x0a, 0x00, 0x0a, 0x01, 0x07, 0x00, 0x02, 0x29, 0x00, 0x00,
    0x00, 0x00,
};

#define ADV_INTERVAL_MIN     (160)              /* 0.625 ms units: 100 ms (the SDK demo: 500 ms) */
#define ATT_LOCAL_MTU_SIZE   (64)
#define ATT_SEND_CBUF_SIZE   (512)
#define ATT_RAM_BUFSIZE      (ATT_CTRL_BLOCK_SIZE + ATT_LOCAL_MTU_SIZE + ATT_SEND_CBUF_SIZE)
static u8 att_ram_buffer[ATT_RAM_BUFSIZE] __attribute__((aligned(4)));

static const char gap_device_name[] = "FM-1";
static volatile hci_con_handle_t con_handle;
static u8 adv_data[ADV_RSP_PACKET_MAX], scan_rsp_data[ADV_RSP_PACKET_MAX];
static u8 ble_work_state, adv_ctrl_en;
static OS_MUTEX mutex;

/* connection parameters: ask for a short interval (MIDI latency), as the SDK demo does */
static const uint8_t connection_update_enable = 1;
static uint8_t connection_update_cnt;
static const struct conn_update_param_t connection_param_table[] = {
    {12, 24, 0, 400},               /* 15..30 ms, no latency, 4 s timeout */
    {16, 32, 0, 400},
};
#define CONN_PARAM_TABLE_CNT (sizeof(connection_param_table) / sizeof(connection_param_table[0]))

static void set_ble_work_state(ble_state_e state)
{
    if (state != ble_work_state) {
        log_info("ble_work_st:%x->%x\n", ble_work_state, state);
        ble_work_state = state;
    }
}
static ble_state_e get_ble_work_state(void) { return ble_work_state; }

static void send_request_connect_parameter(u8 table_index)
{
    const struct conn_update_param_t *param = &connection_param_table[table_index];
    log_info("update_request:-%d-%d-%d-%d-\n", param->interval_min, param->interval_max, param->latency, param->timeout);
    if (con_handle)
        ble_op_conn_param_request(con_handle, (void *)param);
}
static void check_connetion_updata_deal(void)
{
    if (connection_update_enable && connection_update_cnt < CONN_PARAM_TABLE_CNT)
        send_request_connect_parameter(connection_update_cnt);
}
static void connection_update_complete_success(u8 *packet)
{
    int conn_interval = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
    int conn_latency = hci_subevent_le_connection_update_complete_get_conn_latency(packet);
    int conn_timeout = hci_subevent_le_connection_update_complete_get_supervision_timeout(packet);
    log_info("conn: interval %d latency %d timeout %d\n", conn_interval, conn_latency, conn_timeout);
}

/* ---- MIDI in: a BLE MIDI packet is a header, then (timestamp, message) pairs with running status */
static int midi_len(u8 st)                       /* data bytes of a status byte; -1 for sysex */
{
    switch (st & 0xF0) {
    case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
    case 0xC0: case 0xD0: return 1;
    case 0xF0:
        switch (st) {
        case 0xF1: case 0xF3: return 1;
        case 0xF2: return 2;
        case 0xF0: return -1;
        default: return 0;                       /* F6, F7, F8..FF */
        }
    }
    return 0;
}
static u8 cin_of(u8 st, int n)
{
    if (st >= 0xF8) return 0xF;                  /* realtime */
    if (st < 0xF0) return st >> 4;
    return n == 2 ? 3 : n == 1 ? 2 : 5;          /* system common */
}
static void midi_parse(const u8 *b, u16 len)
{
    u16 i = 1;
    u8 st = 0;
    while (i < len) {
        if (b[i] & 0x80) {                       /* a timestamp low byte, then maybe a status */
            i++;
            if (i < len && (b[i] & 0x80))
                st = b[i++];
        }
        if (!st)
            return;
        {
            int n = midi_len(st), k;
            u8 d[2] = {0, 0};
            if (n < 0) {                         /* sysex: skip to the end flag */
                while (i < len && !(b[i] & 0x80))
                    i++;
                continue;
            }
            if (st >= 0xF8) {
                ble_midi_receive(0xFu | (uint32_t)st << 8);
                continue;
            }
            for (k = 0; k < n; k++) {
                if (i >= len || (b[i] & 0x80))
                    return;
                d[k] = b[i++];
            }
            ble_midi_receive((uint32_t)cin_of(st, n) | (uint32_t)st << 8 | (uint32_t)d[0] << 16 | (uint32_t)d[1] << 24);
        }
    }
}

/* ---- MIDI out: as many messages as fit one packet (header, then timestamp + message each) */
int ble_midi_connected(void) { return con_handle && att_get_ccc_config(H_CCC); }
static u16 send_mtu = 20;                        /* ATT payload: 23 - 3 until the exchange says more */
int ble_midi_send_many(const uint32_t *pkts, int n)
{
    u8 buf[64];
    u32 t = shim_ms(), len = 0, remain = 0;
    int i;
    if (!ble_midi_connected() || n <= 0)
        return 0;
    ble_op_att_get_remain(&remain);
    if (remain < 8u)
        return 0;                                /* the stack's buffer is full: later */
    buf[len++] = (u8)(0x80 | ((t >> 7) & 0x3F));
    for (i = 0; i < n; i++) {
        u8 st = (u8)(pkts[i] >> 8), cin = pkts[i] & 0xF, m[3];
        u32 k = 0;
        m[k++] = st;
        if (cin == 0xF || cin == 5) {
        } else if (cin == 2 || cin == 0xC || cin == 0xD) {
            m[k++] = (u8)(pkts[i] >> 16);
        } else {
            m[k++] = (u8)(pkts[i] >> 16);
            m[k++] = (u8)(pkts[i] >> 24);
        }
        if (len + 1 + k > send_mtu || len + 1 + k > sizeof buf)
            break;
        buf[len++] = (u8)(0x80 | (t & 0x7F));
        memcpy(buf + len, m, k);
        len += k;
    }
    if (i == 0)
        return 0;
    if (ble_op_att_send_data(H_MIDI, buf, len, ATT_OP_AUTO_READ_CCC))
        return 0;
    return i;
}

/* ---- ATT */
static uint16_t att_read_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    uint16_t len = 0;
    switch (att_handle) {
    case H_NAME:
        len = sizeof gap_device_name - 1;
        if (offset >= len || offset + buffer_size > len)
            break;
        if (buffer) {
            memcpy(buffer, &gap_device_name[offset], buffer_size);
            len = buffer_size;
        }
        break;
    case H_MIDI:                                 /* the spec: a read returns nothing */
        len = 0;
        break;
    case H_CCC:
        if (buffer) {
            buffer[0] = att_get_ccc_config(att_handle);
            buffer[1] = 0;
        }
        len = 2;
        break;
    }
    return len;
}
static int att_write_callback(hci_con_handle_t connection_handle, uint16_t att_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size)
{
    switch (att_handle) {
    case H_CCC:
        set_ble_work_state(BLE_ST_NOTIFY_IDICATE);
        check_connetion_updata_deal();
        log_info("ccc %02x\n", buffer[0]);
        att_set_ccc_config(att_handle, buffer[0]);
        break;
    case H_MIDI:
        if (buffer_size >= 2)
            midi_parse(buffer, buffer_size);
        break;
    }
    return 0;
}

/* ---- advertising */
static int make_set_adv_data(void)
{
    static const u8 svc[16] = {MIDI_SVC_UUID};
    u8 offset = 0, *buf = adv_data;
    offset += make_eir_packet_val(&buf[offset], offset, HCI_EIR_DATATYPE_FLAGS, 0x06, 1);
    offset += make_eir_packet_data(&buf[offset], offset, HCI_EIR_DATATYPE_COMPLETE_128BIT_SERVICE_UUIDS, (void *)svc, 16);
    offset += make_eir_packet_data(&buf[offset], offset, HCI_EIR_DATATYPE_COMPLETE_LOCAL_NAME, (void *)gap_device_name, sizeof gap_device_name - 1);
    if (offset > ADV_RSP_PACKET_MAX)
        return -1;
    log_info("adv_data(%d)\n", offset);
    ble_op_set_adv_data(offset, buf);
    return 0;
}
static int make_set_rsp_data(void)
{
    u8 offset = 0, *buf = scan_rsp_data;
    offset += make_eir_packet_data(&buf[offset], offset, HCI_EIR_DATATYPE_COMPLETE_LOCAL_NAME, (void *)gap_device_name, sizeof gap_device_name - 1);
    ble_op_set_rsp_data(offset, buf);
    return 0;
}
static void advertisements_setup_init(void)
{
    ble_op_set_adv_param(ADV_INTERVAL_MIN, ADV_IND, ADV_CHANNEL_ALL);
    if (make_set_adv_data() | make_set_rsp_data())
        puts("advertisements_setup_init fail\n");
}
static int set_adv_enable(void *priv, u32 en)
{
    ble_state_e next_state = en ? BLE_ST_ADV : BLE_ST_IDLE, cur_state;
    if ((!adv_ctrl_en && en) || con_handle)
        return APP_BLE_OPERATION_ERROR;
    cur_state = get_ble_work_state();
    switch (cur_state) {
    case BLE_ST_ADV: case BLE_ST_IDLE: case BLE_ST_INIT_OK: case BLE_ST_NULL: case BLE_ST_DISCONN:
        break;
    default:
        return APP_BLE_OPERATION_ERROR;
    }
    if (cur_state == next_state)
        return APP_BLE_NO_ERROR;
    log_info("adv_en:%d\n", en);
    set_ble_work_state(next_state);
    if (en)
        advertisements_setup_init();
    ble_op_adv_enable(en);
    return APP_BLE_NO_ERROR;
}
static void bt_ble_adv_enable(u8 enable) { set_adv_enable(0, enable); }

/* ---- the stack's events */
static void server_profile_start(u16 handle)
{
    ble_op_att_send_init(handle, att_ram_buffer, ATT_RAM_BUFSIZE, ATT_LOCAL_MTU_SIZE);
    set_ble_work_state(BLE_ST_CONNECT);
}
static void cbk_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    int mtu;
    u32 tmp;
    u8 status;
    if (packet_type != HCI_EVENT_PACKET)
        return;
    switch (hci_event_packet_get_type(packet)) {
    case ATT_EVENT_CAN_SEND_NOW:
        break;
    case HCI_EVENT_LE_META:
        switch (hci_event_le_meta_get_subevent_code(packet)) {
        case HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE:
            status = hci_subevent_le_enhanced_connection_complete_get_status(packet);
            if (status) {
                log_info("connection fail %x\n", status);
                set_ble_work_state(BLE_ST_DISCONN);
                break;
            }
            con_handle = hci_subevent_le_enhanced_connection_complete_get_connection_handle(packet);
            printf("connected (enh) %x\n", con_handle);
            server_profile_start(con_handle);
            break;
        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
            con_handle = hci_subevent_le_connection_complete_get_connection_handle(packet);
            printf("connected %x\n", con_handle);
            connection_update_complete_success(packet + 8);
            server_profile_start(con_handle);
            break;
        case HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE:
            connection_update_complete_success(packet);
            break;
        }
        break;
    case HCI_EVENT_DISCONNECTION_COMPLETE:
        printf("disconnected: reason %x\n", packet[5]);
        con_handle = 0;
        ble_op_att_send_init(con_handle, 0, 0, 0);
        set_ble_work_state(BLE_ST_DISCONN);
        os_mutex_pend(&mutex, 0);
        bt_ble_adv_enable(1);
        os_mutex_post(&mutex);
        connection_update_cnt = 0;
        break;
    case ATT_EVENT_MTU_EXCHANGE_COMPLETE:
        mtu = att_event_mtu_exchange_complete_get_MTU(packet) - 3;
        log_info("ATT MTU = %u\n", mtu);
        ble_op_att_set_send_mtu(mtu);
        send_mtu = (u16)(mtu > 64 ? 64 : mtu);
        break;
    case L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_RESPONSE:
        tmp = little_endian_read_16(packet, 4);
        if (tmp) {
            connection_update_cnt++;
            check_connetion_updata_deal();
        } else {
            connection_update_cnt = CONN_PARAM_TABLE_CNT;
        }
        break;
    case HCI_EVENT_ENCRYPTION_CHANGE:
        log_info("encryption %d\n", packet[2]);
        break;
    }
}

static void cbk_sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    if (packet_type != HCI_EVENT_PACKET)
        return;
    switch (hci_event_packet_get_type(packet)) {
    case SM_EVENT_JUST_WORKS_REQUEST:
        sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
        log_info("sm: just works\n");
        break;
    case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
    case SM_EVENT_PASSKEY_DISPLAY_CANCEL:
    case SM_EVENT_PASSKEY_INPUT_NUMBER:
        break;
    }
}

void ble_profile_init(void)
{
    printf("ble profile init (MIDI)\n");
    le_device_db_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);
    sm_set_encryption_key_size_range(7, 16);
    sm_set_request_security(0);                  /* MIDI needs no pairing; a central may still ask */
    sm_event_callback_set(&cbk_sm_packet_handler);
    att_server_init(profile_data, att_read_callback, att_write_callback);
    att_server_register_packet_handler(cbk_packet_handler);
    hci_event_callback_set(&cbk_packet_handler);
    le_l2cap_register_packet_handler(&cbk_packet_handler);
    ble_vendor_set_default_att_mtu(ATT_LOCAL_MTU_SIZE);
}

void bt_ble_init(void)
{
    log_info("ble init (MIDI)\n");
    if (!os_mutex_valid(&mutex))
        os_mutex_create(&mutex);
    set_ble_work_state(BLE_ST_INIT_OK);
    os_mutex_pend(&mutex, 0);
    adv_ctrl_en = 1;
    bt_ble_adv_enable(1);
    os_mutex_post(&mutex);
}
void bt_ble_exit(void)
{
    os_mutex_pend(&mutex, 0);
    if (con_handle) {
        adv_ctrl_en = 0;
        ble_op_disconnect(con_handle);
    } else {
        bt_ble_adv_enable(0);
        adv_ctrl_en = 0;
    }
    os_mutex_post(&mutex);
}
u16 bt_ble_is_connected(void) { return con_handle; }
