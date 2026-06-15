#include "esp_log.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "ble_pair_utils.h"
#include "ble_tether.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>  /* strncasecmp */


/*
* Copies advertisement name from one buffer to another
*
* @param dest      Pointer to destination buffer (should be s_adv_names[i]).
* @param dest_size Size of destination buffer (should be ADV_NAME_MAX).
* @param source    Pointer to advertisement name to copy.
* @param source_len Length of advertisement name.
*/
void copy_adv_name(char* dest, uint8_t dest_size, const uint8_t *source, uint8_t source_len)
{
    if (source == NULL || source_len == 0) {
        dest[0] = '\0';
        return;
    }
    uint8_t n = source_len < (dest_size - 1) ? source_len : (dest_size - 1);
    memcpy(dest, source, n);
    dest[n] = '\0';
}

/*
* Copies advertisement data from one buffer to another 
*
* @param dest      Pointer to destination buffer (should be s_adv_data[i]).
* @param dest_size Size of destination buffer (should be ADV_DATA_MAX).
* @param source    Pointer to advertisement data to copy.
* @param source_len Length of advertisement data.
*/
void copy_adv_data(uint8_t *dest, uint8_t dest_size,  uint8_t *data_len, const uint8_t *source, uint8_t source_len)
{
    if (source == NULL || source_len == 0) {
        dest[0] = '\0';
        *data_len = 0;
        return;
    }
    uint8_t n = source_len < ADV_DATA_MAX ? source_len : ADV_DATA_MAX;
    memcpy(dest, source, n);
    *data_len = n;
}

/*
* Determines priority of an advertisement based on its name for sorting purposes.
* @param slot  Index in s_adv_names corresponding to the advertisement.
* @return      Priority value (lower is higher priority).
*/
uint8_t adv_priority(uint8_t *device_name)
{
    const char *name = device_name != NULL ? (const char *)device_name : "";
    if (name[0] == '\0') {
        return 2;  /* unnamed */
    }
    if (strncasecmp(name, BLE_PEER_NAME, strlen(BLE_PEER_NAME)) == 0) {
        return 0;  /* priority prefix match */
    }
    return 1;  /* other named */
}

/*
* Swaps all advertisement data for two slots in the snapshot for sorting purposes.
*
* @param adv  Pointer to the advertisement structure containing the entries to swap.
* @param a         Index of the first advertisement slot to swap.
* @param b         Index of the second advertisement slot to swap.
*/
void swap_adv_slot(s_adv_t *adv, uint8_t a, uint8_t b)
{
    if (a == b) return;
    uint8_t    mac[6];
    uint16_t   handle;
    uint8_t    type;
    char       name[ADV_NAME_MAX];
    uint8_t    data[ADV_DATA_MAX];
    uint8_t    data_len;
    int8_t     rssi;
    TickType_t last_seen;

    memcpy(mac,  adv->snapshot.entries[a], 6);
    handle    = adv->snapshot.handles[a];
    type      = adv->types[a];
    memcpy(name, adv->names[a], ADV_NAME_MAX);
    memcpy(data, adv->data[a], ADV_DATA_MAX);
    data_len  = adv->data_len[a];
    rssi      = adv->rssi[a];
    last_seen = adv->last_seen[a];

    memcpy(adv->snapshot.entries[a], adv->snapshot.entries[b], 6);
    adv->snapshot.handles[a] = adv->snapshot.handles[b];
    adv->types[a]             = adv->types[b];
    memcpy(adv->names[a], adv->names[b], ADV_NAME_MAX);
    memcpy(adv->data[a], adv->data[b], ADV_DATA_MAX);
    adv->data_len[a]  = adv->data_len[b];
    adv->rssi[a]      = adv->rssi[b];
    adv->last_seen[a] = adv->last_seen[b];

    memcpy(adv->snapshot.entries[b], mac, 6);
    adv->snapshot.handles[b] = handle;
    adv->types[b]             = type;
    memcpy(adv->names[b], name, ADV_NAME_MAX);
    memcpy(adv->data[b], data, ADV_DATA_MAX);
    adv->data_len[b]  = data_len;
    adv->rssi[b]      = rssi;
    adv->last_seen[b] = last_seen;
}

void ble_pair_ensure_mtx(SemaphoreHandle_t *mtx)
{
    if (*mtx == NULL) {
        *mtx = xSemaphoreCreateMutex();
    }
}

void ble_pair_clear_adv(s_adv_t *s_adv)
{
    ble_pair_ensure_mtx(&(s_adv->mtx));
    if (s_adv->mtx == NULL) return;
    xSemaphoreTake(s_adv->mtx, portMAX_DELAY);
    memset(&s_adv->snapshot, 0, sizeof(s_adv->snapshot));
    memset(s_adv->types,    0, sizeof(s_adv->types));
    memset(s_adv->names,    0, sizeof(s_adv->names));
    memset(s_adv->data,     0, sizeof(s_adv->data));
    memset(s_adv->data_len, 0, sizeof(s_adv->data_len));
    memset(s_adv->rssi,      0, sizeof(s_adv->rssi));
    memset(s_adv->last_seen, 0, sizeof(s_adv->last_seen));
    xSemaphoreGive(s_adv->mtx);
}

const char *find_peer_name_by_addr(const uint8_t *addr, s_adv_t *s_adv)
{
    for (uint8_t i = 0; i < s_adv->snapshot.count; i++) {
        if (memcmp(s_adv->snapshot.entries[i], addr, 6) == 0) {
            return s_adv->names[i][0] != '\0' ? s_adv->names[i] : "(unknown)";
        }
    }
    return "(unknown)";
}

/* Stable insertion sort by priority. Run under s_adv.mtx. */
void sort_snapshot_by_priority(s_adv_t *s_adv)
{
    for (uint8_t i = 1; i < s_adv->snapshot.count; i++) {
        uint8_t j = i;
        while (j > 0 && adv_priority((uint8_t *)s_adv->names[j - 1]) > adv_priority((uint8_t *)s_adv->names[j])) {
            swap_adv_slot(s_adv, j - 1, j);
            j--;
        }
    }
}