#ifndef TETHER_SDIO_PROTO_H
#define TETHER_SDIO_PROTO_H

/*
 * Wire format for P4 <-> C6 BLE commands over SDIO.
 * Keep opcode and payload changes in sync with the C6 copy.
 *
 * Frame layout:
 *
 *   offset  size  field
 *     0      1B   op
 *     1      1B   status
 *     2      1B   flags
 *     3      2B   len
 *     5      N    payload
 *
 * Larger messages use FLAG_MORE across multiple frames.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


#define TETHER_FRAME_HDR_SIZE       5

#define TETHER_MAX_FRAME_SIZE       128
#define TETHER_MAX_PAYLOAD_SIZE     (TETHER_MAX_FRAME_SIZE - TETHER_FRAME_HDR_SIZE)

typedef struct __attribute__((packed)) {
    uint8_t  op;
    uint8_t  status;
    uint8_t  flags;
    uint16_t len;
} tether_frame_hdr_t;

_Static_assert(sizeof(tether_frame_hdr_t) == TETHER_FRAME_HDR_SIZE,
               "tether_frame_hdr_t must be 5 bytes packed");


#define TETHER_FLAG_MORE            0x01

#define TETHER_OP_RESP_MASK         0x80
#define TETHER_OP_EVT_MASK          0xC0

#define TETHER_OP_SHOW              0x01    /* req: empty                  resp: slim per-device list */
#define TETHER_OP_PAIR              0x02    /* req: u8 index               resp: empty (ACK only)     */
#define TETHER_OP_READ_ADV          0x03    /* req: u8 index               resp: full detail          */
#define TETHER_OP_CONNECTED         0x04    /* req: empty                  resp: connected list       */
#define TETHER_OP_DISCONNECT        0x05    /* req: u8 index               resp: empty                */
#define TETHER_OP_REMOVE            0x06    /* req: u8 index               resp: empty                */
#define TETHER_OP_DETECT            0x07    /* req: empty                  resp: tether_scan_report_t */
#define TETHER_OP_PAIR_MAC          0x08    /* req: u8 mac[6]              resp: empty (ACK only)     */

#define TETHER_OP_SHOW_RESP         (TETHER_OP_RESP_MASK | TETHER_OP_SHOW)
#define TETHER_OP_PAIR_RESP         (TETHER_OP_RESP_MASK | TETHER_OP_PAIR)
#define TETHER_OP_READ_ADV_RESP     (TETHER_OP_RESP_MASK | TETHER_OP_READ_ADV)
#define TETHER_OP_CONNECTED_RESP    (TETHER_OP_RESP_MASK | TETHER_OP_CONNECTED)
#define TETHER_OP_DISCONNECT_RESP   (TETHER_OP_RESP_MASK | TETHER_OP_DISCONNECT)
#define TETHER_OP_REMOVE_RESP       (TETHER_OP_RESP_MASK | TETHER_OP_REMOVE)
#define TETHER_OP_DETECT_RESP       (TETHER_OP_RESP_MASK | TETHER_OP_DETECT)
#define TETHER_OP_PAIR_MAC_RESP     (TETHER_OP_RESP_MASK | TETHER_OP_PAIR_MAC)

#define TETHER_EVT_PAIR_COMPLETE    (TETHER_OP_EVT_MASK | 0x02)  /* payload: u8 mac[6], u8 success, u8 connection_type (1 = tether->peripheral, 2 = peripheral->tether) */
#define TETHER_EVT_PEER_DISCONN     (TETHER_OP_EVT_MASK | 0x03)  /* payload: u8 mac[6], u16 reason, u8 intentional (1 = C6 dropped it for OP_DISCONNECT/OP_REMOVE) */
#define TETHER_EVT_PAIR_REMOVE      (TETHER_OP_EVT_MASK | 0x04)  /* payload: u8 mac[6] */


#define TETHER_ST_OK                0x00
#define TETHER_ST_BAD_OPCODE        0x01
#define TETHER_ST_BAD_LEN           0x02
#define TETHER_ST_BAD_INDEX         0x03
#define TETHER_ST_NO_SNAPSHOT       0x04
#define TETHER_ST_BLE_ERR           0x05
#define TETHER_ST_BUSY              0x06
#define TETHER_ST_INTERNAL          0xFF


#define TETHER_MAC_LEN              6

#define TETHER_SHOW_NAME_MAX        20
#define TETHER_SHOW_ENTRY_MAX       (1 + TETHER_MAC_LEN + 1 + 1 + TETHER_SHOW_NAME_MAX)

/* OP_CONNECTED entry: u8 mac[6], i8 rssi, u8 name_len, char name[name_len]. Count prefixed;
 * variable width like OP_SHOW. Name is the cached display name, truncated to TETHER_CONN_NAME_MAX. */
#define TETHER_CONN_NAME_MAX        20
#define TETHER_CONN_ENTRY_MAX       (TETHER_MAC_LEN + 1 + 1 + TETHER_CONN_NAME_MAX)

#define TETHER_SCAN_MAX             30
typedef struct __attribute__((packed)) {
    uint8_t  count;
    uint8_t  entries[TETHER_SCAN_MAX][6];
} tether_scan_report_t;

_Static_assert(sizeof(tether_scan_report_t) == 1 + TETHER_SCAN_MAX * 6,
               "tether_scan_report_t wire layout drifted");

#define TETHER_READ_ADV_NAME_MAX    31
#define TETHER_READ_ADV_DATA_MAX    31


#ifdef __cplusplus
}
#endif

#endif /* TETHER_SDIO_PROTO_H */
