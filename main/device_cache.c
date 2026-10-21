#include "device_cache.h"
#include <string.h>

bridge_seen_device_t *bridge_cache_find(bridge_device_cache_t *cache, const uint8_t mac[6])
{
    for (size_t i = 0; i < cache->count; i++) {
        if (memcmp(cache->devices[i].mac, mac, 6) == 0) return &cache->devices[i];
    }
    return NULL;
}

void bridge_cache_observe(bridge_device_cache_t *cache, const bridge_seen_device_t *device)
{
    bridge_seen_device_t *entry = bridge_cache_find(cache, device->mac);
    if (entry) {
        entry->rssi = device->rssi;
        entry->seen_ms = device->seen_ms;
        if (device->cod) entry->cod = device->cod;
        entry->spp |= device->spp;
        if (device->name[0]) memcpy(entry->name, device->name, sizeof(entry->name));
    } else {
        size_t index = cache->count;
        if (index < BRIDGE_CACHE_CAPACITY) {
            cache->count++;
        } else {
            index = 0;
            for (size_t i = 1; i < cache->count; i++) {
                if (cache->devices[i].seen_ms < cache->devices[index].seen_ms) index = i;
            }
        }
        cache->devices[index] = *device;
    }
}
