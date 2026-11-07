#pragma once

/* Minimal MQTT 3.1.1 packet encoder and incremental decoder. This covers exactly
 * what the printer link needs: CONNECT, SUBSCRIBE, QoS 0 PUBLISH, and PINGREQ. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bambu full-status reports are tens of kilobytes; anything near this is hostile. */
#define BAMBU_MQTT_MAX_PACKET (1024u * 1024u)

enum bambu_mqtt_packet_type {
    BAMBU_MQTT_CONNACK = 2,
    BAMBU_MQTT_PUBLISH = 3,
    BAMBU_MQTT_PUBACK = 4,
    BAMBU_MQTT_SUBACK = 9,
    BAMBU_MQTT_PINGRESP = 13,
};

/* Return the encoded length, or 0 if the buffer is too small or the fields invalid. */
size_t bambu_mqtt_encode_connect(
    uint8_t *out,
    size_t capacity,
    const char *client_id,
    const char *username,
    const char *password,
    uint16_t keepalive_seconds
);
size_t bambu_mqtt_encode_subscribe(uint8_t *out, size_t capacity, uint16_t packet_id, const char *topic);
size_t bambu_mqtt_encode_publish(
    uint8_t *out,
    size_t capacity,
    const char *topic,
    const char *payload,
    size_t payload_length
);
size_t bambu_mqtt_encode_pingreq(uint8_t *out, size_t capacity);
size_t bambu_mqtt_encode_disconnect(uint8_t *out, size_t capacity);

struct bambu_mqtt_packet {
    int type;
    uint8_t flags;
    /* CONNACK */
    uint8_t connack_code;
    /* SUBACK */
    uint8_t suback_code;
    /* PUBLISH: pointers into the decoder buffer, valid until the next feed. */
    const char *topic;
    size_t topic_length;
    const char *payload;
    size_t payload_length;
};

enum bambu_mqtt_decode_result {
    BAMBU_MQTT_NEED_MORE = 0,
    BAMBU_MQTT_PACKET,
    BAMBU_MQTT_MALFORMED,
};

/* Parses the first packet in buffer. On BAMBU_MQTT_PACKET, *consumed is its
 * total size. A declared length over the bound is reported as malformed
 * immediately, before any of the body has been buffered. */
enum bambu_mqtt_decode_result bambu_mqtt_decode(
    const uint8_t *buffer,
    size_t length,
    struct bambu_mqtt_packet *packet,
    size_t *consumed
);
