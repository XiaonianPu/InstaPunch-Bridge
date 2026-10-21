#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BRIDGE_CACHE_CAPACITY 24
typedef struct {
    uint8_t mac[6];
    char name[32];
    int rssi;
    uint32_t cod;
    bool spp;
    uint64_t seen_ms;
} bridge_seen_device_t;

typedef struct {
    bridge_seen_device_t devices[BRIDGE_CACHE_CAPACITY];
    size_t count;
} bridge_device_cache_t;

// RAM only. Missing metadata in a later inquiry does not erase earlier names/services.
bridge_seen_device_t *bridge_cache_find(bridge_device_cache_t *cache, const uint8_t mac[6]);
void bridge_cache_observe(bridge_device_cache_t *cache, const bridge_seen_device_t *device);
