#ifdef WIN32
#else
#include <unistd.h>
#endif

#include <sys/types.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>

#include "mutex_manager.h"
#include "handler_sse.h"
#include "handler_rtnl.h"
#include "settings.h"
#include "stats.h"
#include "mqtt.h"
#include "fs_ext.h"
#include "cloud_request.h"
#include "server_helpers.h"
#include "toniesJson.h"
#include "server_helpers.h"

#include "proto/toniebox.pb.rtnl.pb-c.h"

static void escapeString(const char_t *input, size_t size, char_t *output);
static void escapeString(const char_t *input, size_t size, char_t *output)
{
    // Replacement sequences for special characters
    const char_t *replacements[] = {
        "\"", "\"\"", // Double quote (")
        "\n", "\\n",  // Newline
        "\r", "\\r"   // Carriage return
    };
    const size_t num_replacements = sizeof(replacements) / sizeof(replacements[0]);

    size_t input_length = size;
    size_t escaped_length = 0;

    // First pass to count the number of additional characters required for escaping
    for (size_t i = 0; i < input_length; i++)
    {
        for (size_t j = 0; j < num_replacements; j++)
        {
            if (input[i] == replacements[j][0])
            {
                escaped_length += osStrlen(replacements[j]) - 1;
                break;
            }
        }
    }

    size_t j = 0;
    // Second pass to actually escape the characters
    for (size_t i = 0; i < input_length; i++)
    {
        bool_t replaced = false;
        for (size_t k = 0; k < num_replacements; k++)
        {
            if (input[i] == replacements[k][0])
            {
                size_t len = osStrlen(replacements[k]);
                osStrcpy(&output[j], replacements[k]);
                j += len;
                replaced = true;
                break;
            }
        }

        if (!replaced)
        {
            if (isalnum(input[i]))
            {
                output[j++] = input[i];
            }
            else
            {
                output[j++] = '.';
            }
        }
    }

    // Null-terminate the escaped string
    output[j] = '\0';
}

error_t handleRtnl(HttpConnection *connection, const char_t *uri, const char_t *queryString, client_ctx_t *client_ctx)
{
    char_t *buffer = connection->buffer;
    size_t size = connection->response.contentLength;

    char current_time[64];
    time_format_current(current_time);
    mqtt_sendBoxEvent("LastSeen", current_time, client_ctx);

    size_t pos = 0;
    do
    {
        /* check for enough data for the length header */
        if (pos + 4 > size)
        {
            break;
        }
        uint32_t protoLength = (uint32_t)((buffer[pos] << 24) | (buffer[pos + 1] << 16) | (buffer[pos + 2] << 8) | buffer[pos + 3]);

        if (pos + 4 + protoLength > size)
        {
            break;
        }

        /* check if we have enough data but not too much for the packet we seem to have there.
           do some safety checks */
        if (protoLength == 0 || buffer[pos] != 0 || buffer[pos + 1] != 0)
        {
            TRACE_WARNING("Invalid protoLen=%" PRIu32 ", pos=%" PRIuSIZE "\r\n", protoLength, pos);
            return ERROR_FAILURE;
        }

        /* there is enough bytes for that packet */
        if (client_ctx->settings->rtnl.logRaw)
        {
            mutex_lock(MUTEX_RTNL_FILE);
            FsFile *file = fsOpenFileEx(client_ctx->settings->rtnl.logRawFile, "ab");
            if (file)
            {
                fsWriteFile(file, &buffer[pos], 4 + protoLength);
                fsCloseFile(file);
            }
            mutex_unlock(MUTEX_RTNL_FILE);
        }

        pos += 4;
        TonieRtnlRPC *rpc = tonie_rtnl_rpc__unpack(NULL, protoLength, (const uint8_t *)&buffer[pos]);

        pos += protoLength;
        if (rpc && (rpc->log2 || rpc->log3))
        {
            rtnlEvent(connection, rpc, client_ctx);
            rtnlEventLog(connection, rpc);
            rtnlEventDump(connection, rpc, client_ctx->settings);
        }
        tonie_rtnl_rpc__free_unpacked(rpc, NULL);
    } while (true);

    /* move left-over data to the start of the buffer */
    connection->response.byteCount = size - pos;
    osMemmove(buffer, &buffer[pos], connection->response.byteCount);

    return NO_ERROR;
}

int32_t read_little_endian(const uint8_t *buf)
{
    return (int32_t)(buf[0] | buf[1] << 8 | buf[2] << 16 | buf[3] << 24);
}

void rtnlEvent(HttpConnection *connection, TonieRtnlRPC *rpc, client_ctx_t *client_ctx)
{
    char_t buffer[4096];

    if (rpc->log2)
    {
        sse_startEventRaw("rtnl-raw-log2");
        osSprintf(buffer, "{\"uptime\": %" PRIu64 ", "
                          "\"sequence\": %" PRIu32 ", "
                          "\"field3\": %" PRIu32 ", "
                          "\"function_group\": %" PRIu32 ", "
                          "\"function\": %" PRIu32 ", "
                          "\"field6\": \"",
                  rpc->log2->uptime,
                  rpc->log2->sequence,
                  rpc->log2->field3,
                  rpc->log2->function_group,
                  rpc->log2->function);
        sse_rawData(buffer);

        if (rpc->log2->field6.len > 0)
        {
            for (size_t i = 0; i < rpc->log2->field6.len; i++)
            {
                osSprintf(&buffer[i * 2], "%02X", rpc->log2->field6.data[i]);
            }
            sse_rawData(buffer);
        }

        osSprintf(buffer, "\","
                          "\"field8\": %" PRIu32 ", "
                          "\"field9\": \"",
                  rpc->log2->field8);
        sse_rawData(buffer);

        if (rpc->log2->field9.len > 0)
        {
            for (size_t i = 0; i < rpc->log2->field9.len; i++)
            {
                osSprintf(&buffer[i * 2], "%02X", rpc->log2->field9.data[i]);
            }
            sse_rawData(buffer);
        }
        sse_rawData("\"}");

        sse_endEventRaw();
    }

    if (rpc->log3)
    {
        sse_startEventRaw("rtnl-raw-log3");
        osSprintf(buffer, "{\"datetime\": %" PRIu32 ", "
                          "\"field2\": %" PRIu32 "}",
                  rpc->log3->datetime,
                  rpc->log3->field2);
        sse_rawData(buffer);
        sse_endEventRaw();
    }

    if (rpc->log3)
    {
        settings_internal_rtnl_t *rtnl_setting = &client_ctx->settings->internal.rtnl;
        switch (rpc->log3->field2)
        {
        case RTNL3_TYPE_EAR_BIG:
            if (rtnl_setting->lastEarId == EAR_BIG && rtnl_setting->wasDoubleEarpress)
            {
                rtnl_setting->lastEarId = EAR_NONE;
                rtnl_setting->wasDoubleEarpress = false;
                sse_sendEvent("pressed", "ear-big-double", true);
                mqtt_sendBoxEvent("VolUp", "{\"event_type\": \"double-pressed\"}", client_ctx);
            }
            else
            {
                rtnl_setting->lastEarId = EAR_BIG;
                sse_sendEvent("pressed", "ear-big", true);
                mqtt_sendBoxEvent("VolUp", "{\"event_type\": \"pressed\"}", client_ctx);
            }
            break;
        case RTNL3_TYPE_EAR_SMALL:
            if (rtnl_setting->lastEarId == EAR_SMALL && rtnl_setting->wasDoubleEarpress)
            {
                rtnl_setting->lastEarId = EAR_NONE;
                rtnl_setting->wasDoubleEarpress = false;
                sse_sendEvent("pressed", "ear-small-double", true);
                mqtt_sendBoxEvent("VolDown", "{\"event_type\": \"double-pressed\"}", client_ctx);
            }
            else
            {
                rtnl_setting->lastEarId = EAR_SMALL;
                sse_sendEvent("pressed", "ear-small", true);
                mqtt_sendBoxEvent("VolDown", "{\"event_type\": \"pressed\"}", client_ctx);
            }
            break;
        case RTNL3_TYPE_KNOCK_FORWARD:
            sse_sendEvent("knock", "forward", true);
            mqtt_sendBoxEvent("KnockForward", "{\"event_type\": \"triggered\"}", client_ctx);
            break;
        case RTNL3_TYPE_KNOCK_BACKWARD:
            sse_sendEvent("knock", "backward", true);
            mqtt_sendBoxEvent("KnockBackward", "{\"event_type\": \"triggered\"}", client_ctx);
            break;
        case RTNL3_TYPE_TILT_FORWARD:
            sse_sendEvent("tilt", "forward", true);
            mqtt_sendBoxEvent("TiltForward", "{\"event_type\": \"triggered\"}", client_ctx);
            break;
        case RTNL3_TYPE_TILT_BACKWARD:
            sse_sendEvent("tilt", "backward", true);
            mqtt_sendBoxEvent("TiltBackward", "{\"event_type\": \"triggered\"}", client_ctx);
            break;
        case RTNL3_TYPE_CHARGER_ON:
            sse_sendEvent("charger", "on", true);
            mqtt_sendBoxEvent("Charger", "ON", client_ctx);
            break;
        case RTNL3_TYPE_CHARGER_OFF:
            sse_sendEvent("charger", "off", true);
            mqtt_sendBoxEvent("Charger", "OFF", client_ctx);
            break;
        case RTNL3_TYPE_PLAYBACK_STARTING:
            sse_sendEvent("playback", "starting", true);
            mqtt_sendBoxEvent("Playback", "ON", client_ctx);
            mqtt_sendBoxEvent("TagInvalid", "", client_ctx);
            break;
        case RTNL3_TYPE_PLAYBACK_STARTED:
            sse_sendEvent("playback", "started", true);
            mqtt_sendBoxEvent("Playback", "ON", client_ctx);
            mqtt_sendBoxEvent("TagInvalid", "", client_ctx);
            break;
        case RTNL3_TYPE_PLAYBACK_STOPPED:
            sse_sendEvent("playback", "stopped", true);
            mqtt_sendBoxEvent("Playback", "OFF", client_ctx);
            mqtt_sendBoxEvent("TagValid", "", client_ctx);
            mqtt_sendBoxEvent("TagInvalid", "", client_ctx);
            mqtt_sendBoxEvent("AudioId", "", client_ctx);
            mqtt_sendBoxEvent("ContentTitle", "", client_ctx);
            char *url = custom_asprintf("%s/img_empty.png", settings_get_string("core.host_url"));
            mqtt_sendBoxEvent("ContentPicture", url, client_ctx);
            osFreeMem(url);
            break;
        default:
            TRACE_WARNING("Not-yet-known log3 type: %d\r\n", rpc->log3->field2);
            break;
        }
    }

    if (rpc->log2)
    {
        char buffer[33];

        if (rpc->log2->function_group == RTNL2_FUGR_TAG && (rpc->log2->function == RTNL2_FUNC_TAG_INVALID_CC3200 || rpc->log2->function == RTNL2_FUNC_TAG_INVALID_ESP32))
        {
            if (rpc->log2->field6.len == 8)
            {
                for (size_t i = 0; i < rpc->log2->field6.len; i++)
                {
                    osSprintf(&buffer[i * 2], "%02X", rpc->log2->field6.data[(i + 4) % 8]);
                }
            }
            sse_sendEvent("TagInvalid", buffer, true);
            mqtt_sendBoxEvent("TagInvalid", buffer, client_ctx);
            mqtt_sendBoxEvent("TagValid", "", client_ctx);
        }
        else if (rpc->log2->function_group == RTNL2_FUGR_TAG && (rpc->log2->function == RTNL2_FUNC_TAG_VALID_CC3200 || rpc->log2->function == RTNL2_FUNC_TAG_VALID_ESP32))
        {
            if (rpc->log2->field6.len == 8)
            {
                for (size_t i = 0; i < rpc->log2->field6.len; i++)
                {
                    osSprintf(&buffer[i * 2], "%02X", rpc->log2->field6.data[(i + 4) % 8]);
                }
            }
            sse_sendEvent("TagValid", buffer, true);
            mqtt_sendBoxEvent("TagValid", buffer, client_ctx);
            mqtt_sendBoxEvent("TagInvalid", "", client_ctx);
        }
        else if ((rpc->log2->function_group == RTNL2_FUGR_AUDIO_A && (rpc->log2->function == RTNL2_FUNC_AUDIO_ID_CC3200 || rpc->log2->function == RTNL2_FUNC_AUDIO_ID_ESP32)) || (rpc->log2->function_group == RTNL2_FUGR_AUDIO_B && rpc->log2->function == RTNL2_FUNC_AUDIO_ID))
        {
            uint32_t audioId = read_little_endian(rpc->log2->field6.data);
            osSprintf(buffer, "%d", audioId);
            toniesJson_item_t *item = tonies_byAudioId(audioId);
            sse_sendEvent("AudioId", buffer, true);
            mqtt_sendBoxEvent("AudioId", buffer, client_ctx);
            if (item == NULL)
            {
                sse_sendEvent("ContentTitle", "Unknown", true);
                mqtt_sendBoxEvent("ContentTitle", "Unknown", client_ctx);
                if (audioId < 0x50000000)
                {
                    /* custom tonie */
                    char *url = custom_asprintf("%s/img_custom.png", settings_get_string("core.host_url"));
                    mqtt_sendBoxEvent("ContentPicture", url, client_ctx);
                    osFreeMem(url);
                }
                else
                {
                    /* no image in the json file */
                    char *url = custom_asprintf("%s/img_unknown.png", settings_get_string("core.host_url"));
                    mqtt_sendBoxEvent("ContentPicture", url, client_ctx);
                    osFreeMem(url);
                }
            }
            else
            {
                sse_sendEvent("ContentTitle", item->title, true);
                mqtt_sendBoxEvent("ContentTitle", item->title, client_ctx);
                sse_sendEvent("ContentPicture", item->picture, true);
                mqtt_sendBoxEvent("ContentPicture", item->picture, client_ctx);
            }
        }
        else if (rpc->log2->function_group == RTNL2_FUGR_TILT && rpc->log2->function == RTNL2_FUNC_TILT_A_ESP32)
        {
            int32_t angle = read_little_endian(rpc->log2->field6.data);
            osSprintf(buffer, "%d", angle);
            sse_sendEvent("BoxTilt-A", buffer, true);
            mqtt_sendBoxEvent("BoxTilt", buffer, client_ctx);
        }
        else if (rpc->log2->function_group == RTNL2_FUGR_TILT && rpc->log2->function == RTNL2_FUNC_TILT_B_ESP32)
        {
            int32_t angle = read_little_endian(rpc->log2->field6.data);
            osSprintf(buffer, "%d", angle);
            sse_sendEvent("BoxTilt-B", buffer, true);
            mqtt_sendBoxEvent("BoxTilt", buffer, client_ctx);
        }
        else if (rpc->log2->function_group == RTNL2_FUGR_VOLUME && (rpc->log2->function == RTNL2_FUNC_VOLUME_CHANGE_CC3200 || rpc->log2->function == RTNL2_FUNC_VOLUME_CHANGE_ESP32))
        {
            /* DE210000 DBFFFFFF 01000000 */ /* 963C0000 D8FFFFFF 00000000 */
            int32_t volumedB = read_little_endian(&rpc->log2->field6.data[4]);
            int32_t volumeLevel = read_little_endian(&rpc->log2->field6.data[8]);
            osSprintf(buffer, "%d", volumeLevel);
            sse_sendEvent("VolumeLevel", buffer, true);
            mqtt_sendBoxEvent("VolumeLevel", buffer, client_ctx);
            osSprintf(buffer, "%d", volumedB);
            sse_sendEvent("VolumedB", buffer, true);
            mqtt_sendBoxEvent("VolumedB", buffer, client_ctx);

            settings_internal_rtnl_t *rtnl_setting = &client_ctx->settings->internal.rtnl;
            if (rpc->log2->uptime - rtnl_setting->lastEarpress < rtnl_setting->multipressTime)
            {
                rtnl_setting->wasDoubleEarpress = true;
            }
            rtnl_setting->lastEarpress = rpc->log2->uptime;
        }
        else if (rpc->log2->function_group == RTNL2_FUGR_FIRMWARE && rpc->log2->function == RTNL2_FUNC_FIRMWARE_VERSION)
        {
            const char *rtnlVersion = (const char *)rpc->log2->field6.data;
            settings_set_string_id("internal.toniebox_firmware.rtnlVersion", rtnlVersion, client_ctx->settings->internal.overlayNumber);
        }
        else if (rpc->log2->function_group == RTNL2_FUGR_FIRMWARE && rpc->log2->function == RTNL2_FUNC_FIRMWARE_FULL_VERSION)
        {
            const char *rtnlFullVersion = (const char *)rpc->log2->field6.data;
            settings_set_string_id("internal.toniebox_firmware.rtnlFullVersion", rtnlFullVersion, client_ctx->settings->internal.overlayNumber);
        }
        else if (rpc->log2->function_group == RTNL2_FUGR_FIRMWARE && rpc->log2->function == RTNL2_FUNC_FIRMWARE_INFOS)
        {
            // Raw2 | #158 Uptime: 13505 Func:  8-7146 Payload: 'A93394604657000032363430633166003036204D61792032303A32310009000000030000000100000000000000' ASCII: '.3.`FW..2640c1f.06 May 20:21.................'
            // TODO
            settings_set_string_id("internal.toniebox_firmware.rtnlDetail", (const char *)&rpc->log2->field6.data[8], client_ctx->settings->internal.overlayNumber);
        }
        else if (rpc->log2->function_group == RTNL2_FUGR_NETWORK_HTTP && rpc->log2->function == RTNL2_FUNC_NETWORK_REGION)
        {
            // Raw2 | #102 Uptime: 7606 Func:  6-791 Payload: '45550030010000' ASCII: 'EU.0...'
            // TODO
            settings_set_string_id("internal.toniebox_firmware.rtnlRegion", (const char *)rpc->log2->field6.data, client_ctx->settings->internal.overlayNumber);
        }
    }
}

void rtnlEventLog(HttpConnection *connection, TonieRtnlRPC *rpc)
{
    TRACE_DEBUG("RTNL: \r\n");
    if (rpc->log2)
    {
        TRACE_DEBUG(" LOG2:\r\n");
        TRACE_DEBUG("  uptime=%" PRIu64 "\r\n", rpc->log2->uptime);
        TRACE_DEBUG("  sequence=%" PRIu32 "\r\n", rpc->log2->sequence);
        TRACE_DEBUG("  3=%" PRIu32 "\r\n", rpc->log2->field3);
        TRACE_DEBUG("  group=%" PRIu32 "\r\n", rpc->log2->function_group);
        TRACE_DEBUG("  function=%" PRIu32 "\r\n", rpc->log2->function);
        TRACE_DEBUG("  6=len(data)=%" PRIuSIZE ", data=", rpc->log2->field6.len);
        for (size_t i = 0; i < rpc->log2->field6.len; i++)
        {
            TRACE_DEBUG_RESUME("%02X", rpc->log2->field6.data[i]);
        }
        TRACE_DEBUG_RESUME(", txt=%.*s\r\n", (int)rpc->log2->field6.len, rpc->log2->field6.data);
        if (rpc->log2->has_field8)
            TRACE_DEBUG("  8=%" PRIu32 "\r\n", rpc->log2->field8);
        if (rpc->log2->has_field9)
        {
            TRACE_DEBUG("  9=len(data)=%" PRIuSIZE ", data=", rpc->log2->field9.len);
            for (size_t i = 0; i < rpc->log2->field9.len; i++)
            {
                TRACE_DEBUG_RESUME("%02X", rpc->log2->field9.data[i]);
            }
            TRACE_DEBUG_RESUME(", txt=%.*s\r\n", (int)rpc->log2->field9.len, rpc->log2->field9.data);
        }
    }
    if (rpc->log3)
    {
        TRACE_DEBUG(" LOG3:\r\n");
        TRACE_DEBUG("  datetime=%" PRIu32 "\r\n", rpc->log3->datetime);
        TRACE_DEBUG("  2=%" PRIu32 "\r\n", rpc->log3->field2);
    }
}

void rtnlEventDump(HttpConnection *connection, TonieRtnlRPC *rpc, settings_t *settings)
{
    if (settings->rtnl.logHuman)
    {
        bool_t addHeader = !fsFileExists(settings->rtnl.logHumanFile);
        FsFile *file = fsOpenFileEx(settings->rtnl.logHumanFile, "ab");
        if (addHeader)
        {
            char_t *header = "timestamp;log2;uptime;sequence;3;group;function;6(len);6(bytes);6(string);8;9(len);9(bytes);9(string);log3;datetime;2\r\n";
            fsWriteFile(file, header, osStrlen(header));
        }
        char_t buffer[4096];
        osSprintf(buffer, "%" PRIuTIME ";", time(NULL));
        fsWriteFile(file, buffer, osStrlen(buffer));

        if (rpc->log2)
        {
            osSprintf(buffer, "x;%" PRIu64 ";%" PRIu32 ";%" PRIu32 ";%" PRIu32 ";%" PRIu32 ";%" PRIuSIZE ";",
                      rpc->log2->uptime,
                      rpc->log2->sequence,
                      rpc->log2->field3,
                      rpc->log2->function_group,
                      rpc->log2->function,
                      rpc->log2->field6.len);
            fsWriteFile(file, buffer, osStrlen(buffer));

            if (rpc->log2->field6.len > 0)
            {
                for (size_t i = 0; i < rpc->log2->field6.len; i++)
                {
                    osSprintf(&buffer[i * 2], "%02X", rpc->log2->field6.data[i]);
                }
                fsWriteFile(file, buffer, osStrlen(buffer));
            }

            osSprintf(buffer, ";\"");
            escapeString((char_t *)rpc->log2->field6.data, rpc->log2->field6.len, &buffer[2]);
            fsWriteFile(file, buffer, osStrlen(buffer));

            osSprintf(buffer, "\";%" PRIu32 ";%" PRIuSIZE ";",
                      rpc->log2->field8, // TODO hasfield
                      rpc->log2->field9.len);
            fsWriteFile(file, buffer, osStrlen(buffer));

            if (rpc->log2->has_field9)
            {
                if (rpc->log2->field9.len > 0)
                {
                    for (size_t i = 0; i < rpc->log2->field9.len; i++)
                    {
                        osSprintf(&buffer[i * 2], "%02X", rpc->log2->field9.data[i]);
                    }
                    fsWriteFile(file, buffer, osStrlen(buffer));
                }
                osSprintf(buffer, ";\"");
                escapeString((char_t *)rpc->log2->field9.data, rpc->log2->field9.len, &buffer[2]);
                fsWriteFile(file, buffer, osStrlen(buffer));
                char_t *output = "\";";
                fsWriteFile(file, output, osStrlen(output));
            }
            else
            {
                char_t *output = ";;";
                fsWriteFile(file, output, osStrlen(output));
            }
        }
        else
        {
            char_t *output = ";;;;;;;;;;;;;";
            fsWriteFile(file, output, osStrlen(output));
        }

        if (rpc->log3)
        {
            osSprintf(buffer, "x;%" PRIu32 ";%" PRIu32 "\r\n",
                      rpc->log3->datetime,
                      rpc->log3->field2);
            fsWriteFile(file, buffer, osStrlen(buffer));
        }
        else
        {
            char_t *output = ";;\r\n";
            fsWriteFile(file, output, osStrlen(output));
        }
        fsCloseFile(file);
    }
}