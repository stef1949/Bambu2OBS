/* Protocol, reducer, and selection tests. Everything here is OBS-independent. */

#include "bambu-format.h"
#include "bambu-json.h"
#include "bambu-maps.h"
#include "bambu-mqtt-codec.h"
#include "bambu-preview-select.h"
#include "bambu-report.h"
#include "bambu-util.h"
#include "bambu-zip.h"
#include "printer-snapshot.h"
#include "test-framework.h"

#include <stdint.h>

int bambu_test_failures = 0;

static enum bambu_report_result apply(struct bambu_printer_snapshot *snapshot, const char *json, uint64_t now)
{
    return bambu_report_apply(snapshot, json, strlen(json), now, NULL);
}

static void test_json(void)
{
    const char *text = "{\"a\":\"caf\\u00e9\",\"b\":[1,2.5,-3e2],\"c\":true,\"d\":null,\"e\":\"\\ud83d\\ude00\"}";
    struct bambu_json *root = bambu_json_parse(text, strlen(text));
    const struct bambu_json *b;
    double value = 0;
    char out[32];

    CHECK(root != NULL);
    CHECK_STR(bambu_json_as_string(bambu_json_get(root, "a")), "caf\xC3\xA9");
    CHECK_STR(bambu_json_as_string(bambu_json_get(root, "e")), "\xF0\x9F\x98\x80");
    b = bambu_json_get(root, "b");
    CHECK(bambu_json_as_double(b->child->next, &value));
    CHECK_NEAR(value, 2.5, 1e-9);
    CHECK(bambu_json_as_double(b->child->next->next, &value));
    CHECK_NEAR(value, -300.0, 1e-9);
    bambu_json_free(root);

    CHECK(bambu_json_parse("", 0) == NULL);
    CHECK(bambu_json_parse("{", 1) == NULL);
    CHECK(bambu_json_parse("{\"a\":}", 6) == NULL);
    CHECK(bambu_json_parse("[1,]", 4) == NULL);
    CHECK(bambu_json_parse("\"unterminated", 13) == NULL);
    CHECK(bambu_json_parse("1 2", 3) == NULL);

    /* Depth bound: a hostile payload must fail cleanly rather than overflow the stack. */
    {
        static char deep[4096];
        memset(deep, '[', sizeof(deep));
        CHECK(bambu_json_parse(deep, sizeof(deep)) == NULL);
    }

    /* Trailing NUL padding is tolerated, other trailing bytes are not. */
    CHECK((root = bambu_json_parse("{}\0\0", 4)) != NULL);
    bambu_json_free(root);

    CHECK(bambu_json_escape("a\"b\\c\n", out, sizeof(out)) == 15);
    CHECK_STR(out, "\"a\\\"b\\\\c\\u000a\"" + 0);
}

static void test_json_numeric_strings(void)
{
    struct bambu_json *root = bambu_json_parse("{\"a\":\"42\",\"b\":\"4x\",\"c\":123456789012}", 37);
    double value;
    int integer;
    char text[32];

    CHECK(root != NULL);
    CHECK(bambu_json_as_int(bambu_json_get(root, "a"), &integer) && integer == 42);
    CHECK(!bambu_json_as_double(bambu_json_get(root, "b"), &value));
    CHECK(!bambu_json_as_int(bambu_json_get(root, "c"), &integer));
    CHECK(bambu_json_as_text(bambu_json_get(root, "c"), text, sizeof(text)));
    CHECK_STR(text, "123456789012");
    bambu_json_free(root);
}

static const char full_report[] =
    "{\"print\":{\"gcode_state\":\"RUNNING\",\"subtask_name\":\"Benchmark cube\",\"task_id\":\"900\","
    "\"gcode_file\":\"/data/Metadata/plate_1.gcode\",\"print_type\":\"local\",\"mc_percent\":42,"
    "\"mc_remaining_time\":83,\"layer_num\":120,\"total_layer_num\":350,\"nozzle_temper\":219.5,"
    "\"bed_temper\":60.0,\"cooling_fan_speed\":\"15\",\"spd_lvl\":2,\"stg_cur\":0,"
    "\"ams\":{\"tray_now\":\"1\",\"ams\":[{\"id\":\"0\",\"tray\":["
    "{\"id\":\"0\",\"tray_info_idx\":\"GFA00\",\"tray_type\":\"PLA\",\"tray_color\":\"FF0000FF\"},"
    "{\"id\":\"1\",\"tray_info_idx\":\"GFG99\",\"tray_type\":\"PETG\",\"tray_color\":\"00FF00FF\"},"
    "{\"id\":\"2\"},"
    "{\"id\":\"3\",\"tray_info_idx\":\"ZZZ99\",\"tray_type\":\"PA-X\",\"tray_color\":\"not-a-colour\"}"
    "]}]}}}";

static void test_full_report(void)
{
    struct bambu_printer_snapshot s;
    struct bambu_report_events events;

    bambu_snapshot_init(&s);
    CHECK(bambu_report_apply(&s, full_report, strlen(full_report), 5000, &events) == BAMBU_REPORT_APPLIED);
    CHECK(events.job_changed);
    CHECK(s.job_state == BAMBU_JOB_PRINTING);
    CHECK_STR(s.job_name, "Benchmark cube");
    CHECK_STR(s.print_profile, "Benchmark cube");
    CHECK_STR(s.stage, "Printing");
    CHECK(s.has_progress);
    CHECK_NEAR(s.progress_percent, 42.0, 1e-9);
    CHECK(s.remaining_minutes == 83);
    CHECK(s.current_layer == 120 && s.total_layers == 350);
    CHECK_NEAR(s.nozzle_temperature_c, 219.5, 1e-9);
    CHECK_NEAR(s.fan_percent, 100.0, 1e-9);
    CHECK_STR(s.speed_name, "Standard");
    CHECK(s.updated_monotonic_ms == 5000);

    CHECK(s.ams_unit_count == 1);
    CHECK(s.ams[0].unit_id == 0);
    CHECK_STR(s.ams[0].trays[0].material_name, "Bambu PLA Basic");
    CHECK(s.ams[0].trays[0].has_color && s.ams[0].trays[0].color_rgb == 0xFF0000);
    CHECK_STR(s.ams[0].trays[1].material_name, "Generic PETG");
    CHECK(s.ams[0].trays[2].present && !s.ams[0].trays[2].loaded);
    CHECK_STR(s.ams[0].trays[3].material_name, "PA-X"); /* unknown id falls back to the type */
    CHECK(!s.ams[0].trays[3].has_color);
    CHECK(s.has_active_tray && s.active_unit_id == 0 && s.active_tray_index == 1);
}

static void test_delta_retention(void)
{
    struct bambu_printer_snapshot s;
    uint64_t revision;

    bambu_snapshot_init(&s);
    apply(&s, full_report, 1000);
    revision = s.revision;

    CHECK(apply(&s, "{\"print\":{\"mc_percent\":43}}", 2000) == BAMBU_REPORT_APPLIED);
    CHECK_NEAR(s.progress_percent, 43.0, 1e-9);
    CHECK_NEAR(s.nozzle_temperature_c, 219.5, 1e-9);
    CHECK_STR(s.job_name, "Benchmark cube");
    CHECK(s.ams_unit_count == 1);
    CHECK(s.revision == revision + 1);

    /* Identical data refreshes freshness but not the revision. */
    revision = s.revision;
    CHECK(apply(&s, "{\"print\":{\"mc_percent\":43}}", 3000) == BAMBU_REPORT_APPLIED);
    CHECK(s.revision == revision);
    CHECK(s.updated_monotonic_ms == 3000);
}

static void test_malformed_values(void)
{
    struct bambu_printer_snapshot s;

    bambu_snapshot_init(&s);
    apply(&s, full_report, 1000);

    /* Bad values leave the last known good value in place. */
    CHECK(apply(&s, "{\"print\":{\"mc_percent\":\"abc\",\"nozzle_temper\":null,\"layer_num\":[]}}", 2000) ==
          BAMBU_REPORT_APPLIED);
    CHECK_NEAR(s.progress_percent, 42.0, 1e-9);
    CHECK_NEAR(s.nozzle_temperature_c, 219.5, 1e-9);
    CHECK(s.current_layer == 120);

    /* Out-of-range values are clamped. */
    apply(&s, "{\"print\":{\"mc_percent\":250,\"nozzle_temper\":9999,\"bed_temper\":-999,\"cooling_fan_speed\":99}}", 3000);
    CHECK_NEAR(s.progress_percent, 100.0, 1e-9);
    CHECK_NEAR(s.nozzle_temperature_c, 500.0, 1e-9);
    CHECK_NEAR(s.bed_temperature_c, -50.0, 1e-9);
    CHECK_NEAR(s.fan_percent, 100.0, 1e-9);

    /* Garbage is rejected whole; messages without print are ignored. */
    CHECK(apply(&s, "not json", 4000) == BAMBU_REPORT_INVALID);
    CHECK(apply(&s, "{\"info\":{\"command\":\"get_version\"}}", 4000) == BAMBU_REPORT_IGNORED);
    CHECK(s.updated_monotonic_ms == 3000);
}

static void test_unknown_ids(void)
{
    struct bambu_printer_snapshot s;
    char text[64];

    bambu_snapshot_init(&s);
    apply(&s, "{\"print\":{\"gcode_state\":\"NEW_STATE\",\"stg_cur\":777,\"spd_lvl\":9}}", 1000);
    CHECK(s.job_state == BAMBU_JOB_UNKNOWN);
    CHECK_STR(s.stage, "Stage 777");
    CHECK_STR(s.speed_name, "Speed 9");

    bambu_stage_name(-1, text, sizeof(text));
    CHECK_STR(text, "Idle");
    bambu_stage_name(255, text, sizeof(text));
    CHECK_STR(text, "Idle");
    bambu_filament_name("GFZ00", "", text, sizeof(text));
    CHECK_STR(text, "GFZ00");
    bambu_filament_name("", "", text, sizeof(text));
    CHECK_STR(text, "Unknown");
}

static void test_states(void)
{
    static const struct {
        const char *json;
        enum bambu_job_state state;
    } cases[] = {
        {"{\"print\":{\"gcode_state\":\"IDLE\"}}", BAMBU_JOB_IDLE},
        {"{\"print\":{\"gcode_state\":\"PREPARE\"}}", BAMBU_JOB_PREPARING},
        {"{\"print\":{\"gcode_state\":\"RUNNING\"}}", BAMBU_JOB_PRINTING},
        {"{\"print\":{\"gcode_state\":\"PAUSE\"}}", BAMBU_JOB_PAUSED},
        {"{\"print\":{\"gcode_state\":\"FINISH\"}}", BAMBU_JOB_FINISHED},
        {"{\"print\":{\"gcode_state\":\"FAILED\"}}", BAMBU_JOB_FAILED},
    };
    struct bambu_printer_snapshot s;

    bambu_snapshot_init(&s);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        apply(&s, cases[i].json, 1);
        CHECK(s.job_state == cases[i].state);
    }
}

static void test_job_change(void)
{
    struct bambu_printer_snapshot s;
    struct bambu_report_events events;
    const char *next = "{\"print\":{\"task_id\":\"901\",\"subtask_name\":\"Second job\"}}";

    bambu_snapshot_init(&s);
    apply(&s, full_report, 1000);
    s.has_total_time = true;
    s.has_total_weight = true;

    /* The same job repeated is not a change. */
    bambu_report_apply(&s, full_report, strlen(full_report), 1500, &events);
    CHECK(!events.job_changed);

    bambu_report_apply(&s, next, strlen(next), 2000, &events);
    CHECK(events.job_changed);
    CHECK_STR(s.job_name, "Second job");
    CHECK(!s.has_layer);
    CHECK(!s.has_total_time && !s.has_total_weight);

    /* LAN prints report task_id 0, so identity must come from the name. */
    bambu_snapshot_init(&s);
    apply(&s, "{\"print\":{\"task_id\":\"0\",\"subtask_name\":\"LAN one\"}}", 1000);
    {
        const char *second = "{\"print\":{\"task_id\":\"0\",\"subtask_name\":\"LAN two\"}}";
        bambu_report_apply(&s, second, strlen(second), 2000, &events);
    }
    CHECK(events.job_changed);
    CHECK_STR(s.job_name, "LAN two");

    /* A file name alone gives a readable title. */
    bambu_snapshot_init(&s);
    apply(&s, "{\"print\":{\"gcode_file\":\"/sdcard/Fancy Vase.gcode.3mf\"}}", 1000);
    CHECK_STR(s.job_name, "Fancy Vase");

    /* Cloud titles are not overwritten by later MQTT deltas. */
    bambu_snapshot_init(&s);
    apply(&s, full_report, 1000);
    bambu_copy_text(s.job_name, sizeof(s.job_name), "Cloud title");
    s.cloud_job_title = true;
    apply(&s, full_report, 2000);
    CHECK_STR(s.job_name, "Cloud title");
}

static void test_multi_ams(void)
{
    struct bambu_printer_snapshot s;
    const char *two_units =
        "{\"print\":{\"ams\":{\"tray_now\":\"6\",\"ams\":["
        "{\"id\":\"1\",\"tray\":[{\"id\":\"2\",\"tray_info_idx\":\"GFA01\",\"tray_type\":\"PLA\",\"tray_color\":\"0000FFFF\"}]},"
        "{\"id\":\"0\",\"tray\":[{\"id\":\"0\",\"tray_info_idx\":\"GFA00\",\"tray_type\":\"PLA\",\"tray_color\":\"FFFFFFFF\"}]}"
        "]}}}";
    const char *one_unit = "{\"print\":{\"ams\":{\"ams\":[{\"id\":\"1\",\"tray\":[]}]}}}";

    bambu_snapshot_init(&s);
    apply(&s, two_units, 1000);
    CHECK(s.ams_unit_count == 2);
    CHECK(s.ams[0].unit_id == 0 && s.ams[1].unit_id == 1); /* sorted, not payload order */
    CHECK_STR(s.ams[1].trays[2].material_name, "Bambu PLA Matte");
    CHECK(s.ams[1].trays[2].color_rgb == 0x0000FF);
    CHECK(s.has_active_tray && s.active_unit_id == 1 && s.active_tray_index == 2);

    /* An empty tray list keeps known trays; a unit missing from the list is removed. */
    apply(&s, one_unit, 2000);
    CHECK(s.ams_unit_count == 1 && s.ams[0].unit_id == 1);
    CHECK_STR(s.ams[0].trays[2].material_name, "Bambu PLA Matte");

    /* External spool and "none" clear the AMS selection without inventing a tray. */
    apply(&s, "{\"print\":{\"ams\":{\"tray_now\":\"254\"}}}", 3000);
    CHECK(!s.has_active_tray && s.external_spool_active);
    apply(&s, "{\"print\":{\"ams\":{\"tray_now\":\"255\"}}}", 4000);
    CHECK(!s.has_active_tray && !s.external_spool_active);

    /* A hostile tray index must not write outside the unit. */
    apply(&s, "{\"print\":{\"ams\":{\"ams\":[{\"id\":\"0\",\"tray\":[{\"id\":\"99\",\"tray_type\":\"X\"}]}]}}}", 5000);
    CHECK(s.ams_unit_count == 1);
    CHECK(!s.ams[0].trays[0].present);
}

static void test_ams_unit_overflow(void)
{
    struct bambu_printer_snapshot s;
    char json[2048];
    size_t used = (size_t)snprintf(json, sizeof(json), "{\"print\":{\"ams\":{\"ams\":[");

    for (int i = 0; i < 20; i++)
        used += (size_t)snprintf(json + used, sizeof(json) - used, "%s{\"id\":\"%d\",\"tray\":[]}", i ? "," : "", i);
    snprintf(json + used, sizeof(json) - used, "]}}}");

    bambu_snapshot_init(&s);
    CHECK(apply(&s, json, 1) == BAMBU_REPORT_APPLIED);
    CHECK(s.ams_unit_count == BAMBU_MAX_AMS_UNITS);
}

static void test_format(void)
{
    char text[64];

    bambu_format_remaining(45, text, sizeof(text));
    CHECK_STR(text, "45m");
    bambu_format_remaining(83, text, sizeof(text));
    CHECK_STR(text, "1h 23m");
    bambu_format_remaining(60 * 27, text, sizeof(text));
    CHECK_STR(text, "1d 3h");
    bambu_format_remaining(-5, text, sizeof(text));
    CHECK_STR(text, "0m");

    bambu_format_duration_seconds(3725, text, sizeof(text));
    CHECK_STR(text, "1h 02m 05s");

    bambu_format_eta(13, 29, 83, true, text, sizeof(text));
    CHECK_STR(text, "14:52");
    bambu_format_eta(13, 29, 83, false, text, sizeof(text));
    CHECK_STR(text, "2:52 PM");
    bambu_format_eta(23, 30, 60, true, text, sizeof(text));
    CHECK_STR(text, "00:30 +1d");
    bambu_format_eta(0, 5, 0, false, text, sizeof(text));
    CHECK_STR(text, "12:05 AM");

    bambu_format_temperature(219.5, BAMBU_UNIT_CELSIUS, 1, text, sizeof(text));
    CHECK_STR(text, "219.5\xC2\xB0" "C");
    bambu_format_temperature(100.0, BAMBU_UNIT_FAHRENHEIT, 0, text, sizeof(text));
    CHECK_STR(text, "212\xC2\xB0" "F");

    bambu_format_percent(42.4, text, sizeof(text));
    CHECK_STR(text, "42%");
    bambu_format_percent(140.0, text, sizeof(text));
    CHECK_STR(text, "100%");

    bambu_format_layers(120, 350, text, sizeof(text));
    CHECK_STR(text, "120 / 350");
    bambu_format_layers(400, 350, text, sizeof(text));
    CHECK_STR(text, "350 / 350");
    bambu_format_layers(12, 0, text, sizeof(text));
    CHECK_STR(text, "12 / ?");

    bambu_format_weight(123.45, text, sizeof(text));
    CHECK_STR(text, "123.5 g");
    bambu_format_weight(1250.0, text, sizeof(text));
    CHECK_STR(text, "1.25 kg");
}

static void test_backoff(void)
{
    /* Bounds hold at both ends of the injected random range, for every attempt. */
    for (uint32_t attempt = 0; attempt < 40; attempt++) {
        uint64_t ceiling = 1000;
        uint32_t low = bambu_backoff_delay_ms(attempt, 1000, 120000, 0.0);
        uint32_t high = bambu_backoff_delay_ms(attempt, 1000, 120000, 1.0);

        for (uint32_t i = 0; i < attempt && ceiling < 120000; i++)
            ceiling *= 2;
        if (ceiling > 120000)
            ceiling = 120000;
        CHECK(low == ceiling / 2);
        CHECK(high == ceiling);
        CHECK(high <= 120000);
    }
    CHECK(bambu_backoff_delay_ms(0, 1000, 120000, -4.0) == 500);
    CHECK(bambu_backoff_delay_ms(0, 1000, 120000, 9.0) == 1000);
    CHECK(bambu_backoff_delay_ms(4000000000u, 1000, 120000, 1.0) == 120000);
}

static void test_redaction(void)
{
    const char *secrets[] = {"12345678", "tok-abc"};
    char out[512];

    bambu_redact_text("login failed for code 12345678 and tok-abc again 12345678", secrets, 2, out, sizeof(out));
    CHECK(strstr(out, "12345678") == NULL);
    CHECK(strstr(out, "tok-abc") == NULL);

    bambu_redact_text("Authorization: Bearer abc.def-123 sent", NULL, 0, out, sizeof(out));
    CHECK(strstr(out, "abc.def") == NULL);
    CHECK(strstr(out, "sent") != NULL);

    bambu_redact_text("user jane.doe+x@example.co.uk failed", NULL, 0, out, sizeof(out));
    CHECK(strstr(out, "example") == NULL);
    CHECK(strstr(out, "failed") != NULL);

    bambu_redact_text("GET https://cdn.example.net/a.png?sig=SECRET&x=1 done", NULL, 0, out, sizeof(out));
    CHECK(strstr(out, "SECRET") == NULL);
    CHECK(strstr(out, "a.png") != NULL);
    CHECK(strstr(out, "done") != NULL);

    /* A secret longer than the marker, near capacity, must never leave a fragment. */
    {
        char small[24];
        const char *long_secret[] = {"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"};
        bambu_redact_text("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", long_secret, 1, small, sizeof(small));
        CHECK(strstr(small, "AAAA") == NULL);
    }

    bambu_redact_url("https://user:pw@cdn.example.net/x/y.png?token=abc#frag", out, sizeof(out));
    CHECK_STR(out, "https://cdn.example.net/x/y.png");
    bambu_redact_url("nonsense", out, sizeof(out));
    CHECK_STR(out, "<invalid-url>");

    bambu_redact_serial("01S00C123456789", out, sizeof(out));
    CHECK_STR(out, "****6789");
    bambu_redact_serial("abc", out, sizeof(out));
    CHECK_STR(out, "****");
}

static void test_fingerprint_and_color(void)
{
    char out[BAMBU_FINGERPRINT_CAPACITY];
    const char *colons =
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";
    uint32_t rgb;

    CHECK(bambu_normalize_fingerprint(colons, out));
    CHECK_STR(out, "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899");
    CHECK(!bambu_normalize_fingerprint("aabb", out));
    CHECK(!bambu_normalize_fingerprint("zz", out));

    CHECK(bambu_parse_hex_color("FF8000FF", &rgb) && rgb == 0xFF8000);
    CHECK(bambu_parse_hex_color("00ae42", &rgb) && rgb == 0x00AE42);
    CHECK(!bambu_parse_hex_color("FF80", &rgb));
    CHECK(!bambu_parse_hex_color("GG8000FF", &rgb));
    CHECK(!bambu_parse_hex_color(NULL, &rgb));
}

static void test_mqtt_codec(void)
{
    uint8_t buffer[512];
    struct bambu_mqtt_packet packet;
    size_t consumed = 0;
    size_t length;

    length = bambu_mqtt_encode_connect(buffer, sizeof(buffer), "cid", "bblp", "secret", 30);
    CHECK(length == 2 + 10 + 2 + 3 + 2 + 4 + 2 + 6);
    CHECK(buffer[0] == 0x10);
    CHECK(memcmp(buffer + 4, "MQTT", 4) == 0);
    CHECK(buffer[9] == 0xC2);
    CHECK(buffer[11] == 30);

    CHECK(bambu_mqtt_encode_connect(buffer, 8, "cid", "bblp", "secret", 30) == 0);

    length = bambu_mqtt_encode_subscribe(buffer, sizeof(buffer), 7, "device/SN/report");
    CHECK(buffer[0] == 0x82);
    CHECK(buffer[3] == 7);
    CHECK(length == 2 + 2 + 2 + 16 + 1);

    length = bambu_mqtt_encode_publish(buffer, sizeof(buffer), "t/x", "hello", 5);
    CHECK(bambu_mqtt_decode(buffer, length, &packet, &consumed) == BAMBU_MQTT_PACKET);
    CHECK(consumed == length);
    CHECK(packet.type == BAMBU_MQTT_PUBLISH);
    CHECK(packet.topic_length == 3 && memcmp(packet.topic, "t/x", 3) == 0);
    CHECK(packet.payload_length == 5 && memcmp(packet.payload, "hello", 5) == 0);

    /* Every prefix of a packet is incomplete, never malformed or a false packet. */
    for (size_t i = 0; i < length; i++)
        CHECK(bambu_mqtt_decode(buffer, i, &packet, &consumed) == BAMBU_MQTT_NEED_MORE);

    /* Multi-byte remaining length round trip. */
    {
        static uint8_t big[70000];
        static char payload[60000];
        memset(payload, 'x', sizeof(payload));
        length = bambu_mqtt_encode_publish(big, sizeof(big), "t", payload, sizeof(payload));
        CHECK(length > sizeof(payload));
        CHECK(bambu_mqtt_decode(big, length, &packet, &consumed) == BAMBU_MQTT_PACKET);
        CHECK(packet.payload_length == sizeof(payload));
    }

    {
        const uint8_t connack_ok[] = {0x20, 2, 0, 0};
        const uint8_t connack_auth[] = {0x20, 2, 0, 5};
        const uint8_t suback[] = {0x90, 3, 0, 1, 0};
        const uint8_t ping[] = {0xD0, 0};
        const uint8_t oversized[] = {0x30, 0xFF, 0xFF, 0xFF, 0x7F};
        const uint8_t runaway[] = {0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        const uint8_t bad_topic[] = {0x30, 3, 0, 9, 'x'};
        const uint8_t bad_connack[] = {0x20, 1, 0};

        CHECK(bambu_mqtt_decode(connack_ok, 4, &packet, &consumed) == BAMBU_MQTT_PACKET);
        CHECK(packet.type == BAMBU_MQTT_CONNACK && packet.connack_code == 0);
        CHECK(bambu_mqtt_decode(connack_auth, 4, &packet, &consumed) == BAMBU_MQTT_PACKET);
        CHECK(packet.connack_code == 5);
        CHECK(bambu_mqtt_decode(suback, 5, &packet, &consumed) == BAMBU_MQTT_PACKET);
        CHECK(packet.type == BAMBU_MQTT_SUBACK);
        CHECK(bambu_mqtt_decode(ping, 2, &packet, &consumed) == BAMBU_MQTT_PACKET);
        CHECK(packet.type == BAMBU_MQTT_PINGRESP);
        CHECK(bambu_mqtt_decode(oversized, 5, &packet, &consumed) == BAMBU_MQTT_MALFORMED);
        CHECK(bambu_mqtt_decode(runaway, 6, &packet, &consumed) == BAMBU_MQTT_MALFORMED);
        CHECK(bambu_mqtt_decode(bad_topic, 5, &packet, &consumed) == BAMBU_MQTT_MALFORMED);
        CHECK(bambu_mqtt_decode(bad_connack, 3, &packet, &consumed) == BAMBU_MQTT_MALFORMED);
    }
}

static void test_names(void)
{
    char out[128];

    bambu_strip_print_suffix("/sdcard/models/Fancy Vase.gcode.3mf", out, sizeof(out));
    CHECK_STR(out, "Fancy Vase");
    bambu_strip_print_suffix("C:\\tmp\\Part.3MF", out, sizeof(out));
    CHECK_STR(out, "Part");
    bambu_strip_print_suffix("plain.gcode", out, sizeof(out));
    CHECK_STR(out, "plain");
    bambu_normalize_preview_name("Fancy Vase_v2!", out, sizeof(out));
    CHECK_STR(out, "fancyvasev2");
}

static void test_ftp_listing(void)
{
    struct bambu_ftp_entry dated;
    struct bambu_ftp_entry entry;

    CHECK(bambu_ftp_parse_list_line("-rw-r--r-- 1 root root 12345 Mar 14 09:30 My Print.gcode.3mf", 2026, &entry));
    CHECK_STR(entry.name, "My Print.gcode.3mf");
    CHECK(!entry.is_directory);
    CHECK(entry.size == 12345);
    CHECK(entry.sort_time != 0);

    CHECK(bambu_ftp_parse_list_line("drwxr-xr-x 2 root root 4096 Jan  2  2024 cache", 2026, &dated));
    CHECK(dated.is_directory);
    CHECK_STR(dated.name, "cache");
    CHECK(dated.sort_time < entry.sort_time);

    CHECK(!bambu_ftp_parse_list_line("total 8", 2026, &entry));
    CHECK(!bambu_ftp_parse_list_line("", 2026, &entry));

    /* A nonsense date still yields a usable entry, without a sort time. */
    CHECK(bambu_ftp_parse_list_line("-rw-r--r-- 1 u g 9 ??? 99 xx:yy file.3mf", 2026, &entry));
    CHECK(entry.sort_time == 0);
}

static void test_archive_selection(void)
{
    struct bambu_ftp_entry entries[3];
    const char *candidates[] = {"fancyvase"};
    bool matched = false;

    memset(entries, 0, sizeof(entries));
    snprintf(entries[0].name, sizeof(entries[0].name), "other.gcode.3mf");
    entries[0].sort_time = 900;
    snprintf(entries[1].name, sizeof(entries[1].name), "Fancy Vase.gcode.3mf");
    entries[1].sort_time = 100;
    snprintf(entries[2].name, sizeof(entries[2].name), "Fancy Vase v2.3mf");
    entries[2].sort_time = 500;

    CHECK(bambu_score_archive_name("Fancy Vase.3mf", candidates, 1) == 100);
    CHECK(bambu_score_archive_name("Fancy Vase.gcode.3mf", candidates, 1) == 105);
    CHECK(bambu_score_archive_name("Fancy Vase v2.3mf", candidates, 1) == 80);
    CHECK(bambu_score_archive_name("my fancyvase old.3mf", candidates, 1) == 60);
    CHECK(bambu_score_archive_name("nothing.3mf", candidates, 1) == 0);

    CHECK(bambu_select_archive(entries, 3, candidates, 1, &matched) == 1);
    CHECK(matched);

    /* Without a name match the newest archive wins, flagged as a guess. */
    {
        const char *none[] = {"unrelated"};
        CHECK(bambu_select_archive(entries, 3, none, 1, &matched) == 0);
        CHECK(!matched);
        CHECK(bambu_select_archive(entries, 3, NULL, 0, &matched) == 0);
        CHECK(!matched);
    }
    CHECK(bambu_select_archive(entries, 0, candidates, 1, &matched) == -1);
}

static void test_candidates_and_keys(void)
{
    struct bambu_printer_snapshot s;
    char candidates[BAMBU_MAX_NAME_CANDIDATES][BAMBU_FTP_NAME_CAPACITY];
    char first_key[128];
    char second_key[128];

    bambu_snapshot_init(&s);
    bambu_copy_text(s.job_name, sizeof(s.job_name), "Fancy Vase");
    bambu_copy_text(s.subtask_name, sizeof(s.subtask_name), "Fancy Vase");
    bambu_copy_text(s.gcode_file, sizeof(s.gcode_file), "/data/Metadata/plate_1.gcode");
    CHECK(bambu_preview_candidates(&s, candidates) == 1); /* de-duplicated; plate_1 ignored */
    CHECK_STR(candidates[0], "fancyvase");

    bambu_preview_job_key(&s, first_key, sizeof(first_key));
    bambu_copy_text(s.task_id, sizeof(s.task_id), "42");
    bambu_preview_job_key(&s, second_key, sizeof(second_key));
    CHECK(strcmp(first_key, second_key) != 0);

    bambu_snapshot_init(&s);
    bambu_preview_job_key(&s, first_key, sizeof(first_key));
    CHECK_STR(first_key, "unknown_job");

    CHECK(!bambu_is_active_local_print(&s));
    bambu_copy_text(s.print_type, sizeof(s.print_type), "local");
    s.job_state = BAMBU_JOB_PRINTING;
    CHECK(bambu_is_active_local_print(&s));
    s.job_state = BAMBU_JOB_FINISHED;
    CHECK(!bambu_is_active_local_print(&s));
    bambu_copy_text(s.print_type, sizeof(s.print_type), "cloud");
    s.job_state = BAMBU_JOB_PRINTING;
    CHECK(!bambu_is_active_local_print(&s));
}

static void test_url_validation(void)
{
    char host[64];

    CHECK(bambu_validate_remote_url("https://public-cdn.bblmw.com/x/cover.png?sig=1"));
    CHECK(bambu_validate_remote_url("https://8.8.8.8/x.png"));
    CHECK(!bambu_validate_remote_url("http://public-cdn.bblmw.com/x.png"));
    CHECK(!bambu_validate_remote_url("ftp://a.example/x"));
    CHECK(!bambu_validate_remote_url("https://user:pw@a.example/x"));
    CHECK(!bambu_validate_remote_url("https://localhost/x"));
    CHECK(!bambu_validate_remote_url("https://foo.localhost/x"));
    CHECK(!bambu_validate_remote_url("https://127.0.0.1/x"));
    CHECK(!bambu_validate_remote_url("https://10.1.2.3/x"));
    CHECK(!bambu_validate_remote_url("https://172.16.0.1/x"));
    CHECK(!bambu_validate_remote_url("https://192.168.1.1/x"));
    CHECK(!bambu_validate_remote_url("https://169.254.169.254/latest"));
    CHECK(!bambu_validate_remote_url("https://[::1]/x"));
    CHECK(!bambu_validate_remote_url("https://2130706433/x"));
    CHECK(!bambu_validate_remote_url("https://0x7f000001/x"));
    CHECK(!bambu_validate_remote_url("https://a b.example/x"));
    CHECK(!bambu_validate_remote_url(NULL));

    CHECK(bambu_url_host("https://API.BambuLab.com:443/v1", host, sizeof(host)));
    CHECK_STR(host, "api.bambulab.com");
}

static void test_cloud_tasks(void)
{
    struct bambu_cloud_task task;
    char device[64];
    const char *hits =
        "{\"hits\":{\"hits\":["
        "{\"id\":1,\"deviceId\":\"A\",\"endTime\":\"2026-01-01T00:00:00Z\",\"designTitle\":\"Old\"},"
        "{\"id\":2,\"deviceId\":\"A\",\"endTime\":\"2026-03-01T00:00:00Z\",\"designTitle\":\"New\","
        "\"title\":\"0.2mm\",\"weight\":12.5,\"costTime\":3600,\"cover\":\"https://c.example/a.png\","
        "\"taskMessage\":{\"cover\":\"https://c.example/a.png\",\"thumbnail\":\"https://c.example/b.png\"}},"
        "{\"id\":3,\"deviceId\":\"B\",\"endTime\":\"2026-06-01T00:00:00Z\",\"designTitle\":\"Other printer\"}"
        "]}}";
    const char *nested = "{\"data\":{\"list\":[{\"id\":\"9\",\"designTitle\":\"Listed\"}]}}";
    const char *devices = "{\"data\":{\"devices\":[{\"sn\":\"S1\",\"deviceId\":\"D1\"},{\"dev_id\":\"S2\"}]}}";

    CHECK(bambu_cloud_parse_tasks(hits, strlen(hits), "A", &task));
    CHECK_STR(task.design_title, "New");
    CHECK_STR(task.title, "0.2mm");
    CHECK_STR(task.id, "2");
    CHECK(task.has_weight);
    CHECK_NEAR(task.weight_grams, 12.5, 1e-9);
    CHECK(task.cost_time_seconds == 3600);
    CHECK(task.preview_url_count == 2); /* de-duplicated across task and taskMessage */
    CHECK_STR(task.preview_urls[0], "https://c.example/a.png");
    CHECK_STR(task.preview_urls[1], "https://c.example/b.png");

    /* An unmatched identifier falls back to the newest task overall. */
    CHECK(bambu_cloud_parse_tasks(hits, strlen(hits), "Z", &task));
    CHECK_STR(task.design_title, "Other printer");

    CHECK(bambu_cloud_parse_tasks(nested, strlen(nested), "", &task));
    CHECK_STR(task.design_title, "Listed");
    CHECK(!bambu_cloud_parse_tasks("{}", 2, "A", &task));
    CHECK(!bambu_cloud_parse_tasks("junk", 4, "A", &task));

    CHECK(bambu_cloud_parse_device_id(devices, strlen(devices), "S1", device, sizeof(device)));
    CHECK_STR(device, "D1");
    CHECK(bambu_cloud_parse_device_id(devices, strlen(devices), "S2", device, sizeof(device)));
    CHECK_STR(device, "S2");
    CHECK(!bambu_cloud_parse_device_id(devices, strlen(devices), "S3", device, sizeof(device)));
}

/* A 1x1 PNG signature is all the reader validates; WIC performs the real decode. */
static const uint8_t png_bytes[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3, 4};

static size_t put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    return 2;
}

static size_t put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xFFFF);
    put16(p + 2, v >> 16);
    return 4;
}

struct member_spec {
    const char *name;
    const uint8_t *data;
    size_t size;
    uint32_t declared_size; /* 0 means use size */
};

/* Builds a stored (method 0) zip in out and returns its length. */
static size_t build_zip(uint8_t *out, const struct member_spec *members, size_t count)
{
    uint32_t offsets[16];
    size_t used = 0;
    size_t directory_start;
    size_t directory_size;

    for (size_t i = 0; i < count; i++) {
        size_t name_length = strlen(members[i].name);
        offsets[i] = (uint32_t)used;
        used += put32(out + used, 0x04034b50u);
        used += put16(out + used, 20);
        used += put16(out + used, 0);
        used += put16(out + used, 0);
        used += put32(out + used, 0);
        used += put32(out + used, bambu_crc32(members[i].data, members[i].size));
        used += put32(out + used, (uint32_t)members[i].size);
        used += put32(out + used, (uint32_t)members[i].size);
        used += put16(out + used, (unsigned)name_length);
        used += put16(out + used, 0);
        memcpy(out + used, members[i].name, name_length);
        used += name_length;
        memcpy(out + used, members[i].data, members[i].size);
        used += members[i].size;
    }
    directory_start = used;
    for (size_t i = 0; i < count; i++) {
        size_t name_length = strlen(members[i].name);
        uint32_t declared = members[i].declared_size ? members[i].declared_size : (uint32_t)members[i].size;
        used += put32(out + used, 0x02014b50u);
        used += put16(out + used, 20);
        used += put16(out + used, 20);
        used += put16(out + used, 0);
        used += put16(out + used, 0);
        used += put32(out + used, 0);
        used += put32(out + used, bambu_crc32(members[i].data, members[i].size));
        used += put32(out + used, (uint32_t)members[i].size);
        used += put32(out + used, declared);
        used += put16(out + used, (unsigned)name_length);
        used += put16(out + used, 0);
        used += put16(out + used, 0);
        used += put16(out + used, 0);
        used += put16(out + used, 0);
        used += put32(out + used, 0);
        used += put32(out + used, offsets[i]);
        memcpy(out + used, members[i].name, name_length);
        used += name_length;
    }
    directory_size = used - directory_start;
    used += put32(out + used, 0x06054b50u);
    used += put16(out + used, 0);
    used += put16(out + used, 0);
    used += put16(out + used, (unsigned)count);
    used += put16(out + used, (unsigned)count);
    used += put32(out + used, (uint32_t)directory_size);
    used += put32(out + used, (uint32_t)directory_start);
    used += put16(out + used, 0);
    return used;
}

static void test_zip_selection(void)
{
    static uint8_t zip[4096];
    uint8_t *image = NULL;
    size_t image_length = 0;
    char name[64];
    const uint8_t text[] = "not an image";
    struct member_spec priority[] = {
        {"Metadata/pick_1.png", png_bytes, sizeof(png_bytes), 0},
        {"Metadata/plate_1_small.png", png_bytes, sizeof(png_bytes), 0},
        {"Metadata/plate_1.png", png_bytes, sizeof(png_bytes), 0},
        {"3D/3dmodel.model", text, sizeof(text), 0},
    };
    struct member_spec fallback[] = {
        {"Metadata/zzz_long_name_thumbnail.png", png_bytes, sizeof(png_bytes), 0},
        {"Metadata/thumb.jpg", png_bytes, sizeof(png_bytes), 0},
    };
    struct member_spec none[] = {{"3D/3dmodel.model", text, sizeof(text), 0}};
    struct member_spec wrong_type[] = {{"Metadata/plate_1.png", text, sizeof(text), 0}};
    struct member_spec traversal[] = {{"../../evil.png", png_bytes, sizeof(png_bytes), 0}};
    struct member_spec bomb[] = {{"Metadata/plate_1.png", png_bytes, sizeof(png_bytes), 0x7FFFFFFF}};
    size_t length;

    length = build_zip(zip, priority, 4);
    CHECK(bambu_3mf_extract_preview(zip, length, &image, &image_length, name, sizeof(name)) == BAMBU_ZIP_OK);
    CHECK_STR(name, "Metadata/plate_1.png");
    CHECK(image_length == sizeof(png_bytes) && memcmp(image, png_bytes, image_length) == 0);
    free(image);

    length = build_zip(zip, fallback, 2);
    CHECK(bambu_3mf_extract_preview(zip, length, &image, &image_length, name, sizeof(name)) == BAMBU_ZIP_OK);
    CHECK_STR(name, "Metadata/thumb.jpg"); /* shortest name wins among non-priority images */
    free(image);

    length = build_zip(zip, none, 1);
    CHECK(bambu_3mf_extract_preview(zip, length, &image, &image_length, NULL, 0) == BAMBU_ZIP_NO_PREVIEW);

    length = build_zip(zip, wrong_type, 1);
    CHECK(bambu_3mf_extract_preview(zip, length, &image, &image_length, NULL, 0) == BAMBU_ZIP_BAD_IMAGE);

    /* A traversal name is simply not a metadata image, and nothing is extracted to disk. */
    length = build_zip(zip, traversal, 1);
    CHECK(bambu_3mf_extract_preview(zip, length, &image, &image_length, NULL, 0) == BAMBU_ZIP_NO_PREVIEW);

    /* A declared size over the bound is refused before any allocation. */
    length = build_zip(zip, bomb, 1);
    CHECK(bambu_3mf_extract_preview(zip, length, &image, &image_length, NULL, 0) == BAMBU_ZIP_TOO_LARGE);

    /* Corrupt CRC. */
    length = build_zip(zip, priority, 1);
    zip[30 + strlen(priority[0].name)] ^= 0xFF;
    CHECK(bambu_3mf_extract_preview(zip, length, &image, &image_length, NULL, 0) != BAMBU_ZIP_OK);

    /* Truncation anywhere must fail cleanly. */
    length = build_zip(zip, priority, 4);
    for (size_t cut = 0; cut < length; cut += 7)
        CHECK(bambu_3mf_extract_preview(zip, cut, &image, &image_length, NULL, 0) != BAMBU_ZIP_OK);

    CHECK(bambu_3mf_extract_preview(zip, BAMBU_ZIP_MAX_ARCHIVE_BYTES + 1u, &image, &image_length, NULL, 0) ==
          BAMBU_ZIP_TOO_LARGE);
}

static void test_zip_member_limit(void)
{
    static uint8_t zip[64];
    uint8_t *image = NULL;
    size_t image_length = 0;
    size_t used = 0;

    used += put32(zip + used, 0x06054b50u);
    used += put16(zip + used, 0);
    used += put16(zip + used, 0);
    used += put16(zip + used, 5000);
    used += put16(zip + used, 5000);
    used += put32(zip + used, 0);
    used += put32(zip + used, 0);
    used += put16(zip + used, 0);
    CHECK(bambu_3mf_extract_preview(zip, used, &image, &image_length, NULL, 0) == BAMBU_ZIP_TOO_MANY_MEMBERS);
}

static void test_inflate(void)
{
    /* Vectors produced by Python zlib.compressobj(9, zlib.DEFLATED, -15). */
    static const uint8_t fixed[] = {0xCB, 0x48, 0xCD, 0xC9, 0xC9, 0x57, 0xC8, 0x40, 0x27, 0x01};
    static const uint8_t stored[] = {0x01, 0x03, 0x00, 0xFC, 0xFF, 'a', 'b', 'c'};
    static const uint8_t bad_stored[] = {0x01, 0x03, 0x00, 0x00, 0x00, 'a', 'b', 'c'};
    static const uint8_t reserved[] = {0x07};
    static const uint8_t dynamic[] = {
#include "inflate-vector.inc"
    };
    static uint8_t large[2048];
    uint8_t out[64];
    size_t written = 0;

    CHECK(bambu_inflate(fixed, sizeof(fixed), out, sizeof(out), &written));
    CHECK(written == 23);
    CHECK(memcmp(out, "hello hello hello hello", 23) == 0);

    /* Output capacity is enforced mid-stream, which is the zip-bomb defence. */
    CHECK(!bambu_inflate(fixed, sizeof(fixed), out, 10, &written));

    CHECK(bambu_inflate(stored, sizeof(stored), out, sizeof(out), &written));
    CHECK(written == 3 && memcmp(out, "abc", 3) == 0);
    CHECK(!bambu_inflate(bad_stored, sizeof(bad_stored), out, sizeof(out), &written));
    CHECK(!bambu_inflate(reserved, sizeof(reserved), out, sizeof(out), &written));
    CHECK(!bambu_inflate(fixed, 3, out, sizeof(out), &written));

    /* Dynamic Huffman block, checked against the CRC of the original text. */
    CHECK((dynamic[0] & 6) == 4);
    CHECK(bambu_inflate(dynamic, sizeof(dynamic), large, sizeof(large), &written));
    CHECK(written == 1670);
    CHECK(bambu_crc32(large, written) == 3193750693u);

    /* Corrupting a dynamic stream may fail or produce wrong bytes, but never overrun. */
    for (size_t i = 0; i < sizeof(dynamic); i++) {
        static uint8_t mutated[sizeof(dynamic)];
        memcpy(mutated, dynamic, sizeof(dynamic));
        mutated[i] ^= 0x55;
        bambu_inflate(mutated, sizeof(mutated), large, sizeof(large), &written);
    }

    CHECK(bambu_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(bambu_image_signature_ok(png_bytes, sizeof(png_bytes)));
    CHECK(!bambu_image_signature_ok((const uint8_t *)"hello world!", 12));
}

static void test_text_copy(void)
{
    char small[6] = "";

    CHECK(bambu_copy_text(small, sizeof(small), "abc"));
    CHECK(!bambu_copy_text(small, sizeof(small), "abc"));
    CHECK(bambu_copy_text(small, sizeof(small), "abcdefgh"));
    CHECK_STR(small, "abcde");
    /* A two-byte character straddling the limit is dropped whole. */
    CHECK(bambu_copy_text(small, sizeof(small), "abcd\xC3\xA9"));
    CHECK_STR(small, "abcd");
}

int main(void)
{
    test_json();
    test_json_numeric_strings();
    test_full_report();
    test_delta_retention();
    test_malformed_values();
    test_unknown_ids();
    test_states();
    test_job_change();
    test_multi_ams();
    test_ams_unit_overflow();
    test_format();
    test_backoff();
    test_redaction();
    test_fingerprint_and_color();
    test_mqtt_codec();
    test_names();
    test_ftp_listing();
    test_archive_selection();
    test_candidates_and_keys();
    test_url_validation();
    test_cloud_tasks();
    test_zip_selection();
    test_zip_member_limit();
    test_inflate();
    test_text_copy();

    if (bambu_test_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", bambu_test_failures);
        return 1;
    }
    puts("protocol tests passed");
    return 0;
}
