/* Android-callable entry for one NDK-linked portable-C translation. */
#define _POSIX_C_SOURCE 200809L
#include "rt_entry.h"

#include "rt_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void append_json_string(char *output, size_t capacity, const char *value) {
    size_t used;
    const unsigned char *cursor = (const unsigned char *)(value ? value : "");
    if (capacity == 0)
        return;
    used = strlen(output);
    while (*cursor && used + 1 < capacity) {
        unsigned char character = *cursor++;
        if (character == '"' || character == '\\') {
            if (used + 2 >= capacity)
                break;
            output[used++] = '\\';
            output[used++] = (char)character;
        } else if (character < 0x20) {
            int written = snprintf(output + used, capacity - used,
                                   "\\u%04x", (unsigned)character);
            if (written < 0 || (size_t)written >= capacity - used)
                break;
            used += (size_t)written;
        } else {
            output[used++] = (char)character;
        }
    }
    output[used] = '\0';
}

static void set_report(char *output, size_t capacity, const char *runner_status,
                       const char *execution_status, int trapped,
                       const char *reason) {
    char trap_name[128] = "";
    const char *cursor;
    int written;

    if (capacity == 0)
        return;
    if (trapped && strncmp(reason, "shim=", 5) == 0) {
        cursor = reason + 5;
        size_t count = 0;
        while (cursor[count] && cursor[count] != ' ' && count + 1 < sizeof trap_name) {
            trap_name[count] = cursor[count];
            count++;
        }
        trap_name[count] = '\0';
    }
    written = snprintf(output, capacity,
        "{\"translatedPortableC\":{\"status\":\"%s\","
        "\"modinitsCompleted\":%u,\"mainReached\":%s},"
        "\"execution\":{\"status\":\"%s\"},"
        "\"renderer\":{\"status\":\"NOT_CONNECTED_TO_EGL\","
        "\"pixelsVerified\":false,\"gameplayVerified\":false},"
        "\"trappedImport\":%s,\"reason\":\"",
        runner_status, RT_MODINITS_DONE,
        RT_MAIN_REACHED ? "true" : "false", execution_status,
        trap_name[0] ? "\"" : "null");
    if (written < 0 || (size_t)written >= capacity) {
        output[capacity - 1] = '\0';
        return;
    }
    size_t used = (size_t)written;
    if (trap_name[0]) {
        if (used + strlen(trap_name) + 2 >= capacity) {
            output[capacity - 1] = '\0';
            return;
        }
        memcpy(output + used, trap_name, strlen(trap_name));
        used += strlen(trap_name);
        output[used++] = '"';
        output[used] = '\0';
    }
    append_json_string(output, capacity, reason);
    used = strlen(output);
    if (used + 3 < capacity) {
        output[used++] = '"';
        output[used++] = '}';
        output[used] = '\0';
    } else {
        output[capacity - 1] = '\0';
    }
}

int radek_translated_game_run(const char *memory_image_path,
                              const char *guest_data_root,
                              const char *guest_bundle_root,
                              const char *guest_obb_root,
                              char *report_json,
                              size_t report_capacity) {
    CPU cpu;
    const char *runner_status = "SETUP_FAILED";
    const char *execution_status = "EXECUTION_FAULT";
    int trapped = 0;

    if (report_json == NULL || report_capacity == 0)
        return -1;
    report_json[0] = '\0';
    RT_MODINITS_DONE = 0;
    RT_MAIN_REACHED = 0;
    RT_STOP_IS_SHIM = 0;
    RT_STOP_WHY[0] = '\0';

    if (memory_image_path == NULL || memory_image_path[0] == '\0' ||
        guest_data_root == NULL || guest_data_root[0] == '\0') {
        snprintf(RT_STOP_WHY, sizeof RT_STOP_WHY,
                 "translated memory image path or app-private guest data root is empty");
        set_report(report_json, report_capacity, runner_status, execution_status, 0,
                   RT_STOP_WHY);
        return -1;
    }
    if (MEMBASE != NULL) {
        snprintf(RT_STOP_WHY, sizeof RT_STOP_WHY,
                 "translated runtime is single-use and already has guest memory");
        set_report(report_json, report_capacity, runner_status, execution_status, 0,
                   RT_STOP_WHY);
        return -1;
    }

    if (setjmp(RT_STOP_JB) == 0) {
        RT_RUN_ACTIVE = 1;
        if (setenv("RADEK_GAME_DATA", guest_data_root, 1) != 0)
            rt_fatal("could not set the app-private guest data root");
        if (guest_bundle_root != NULL && guest_bundle_root[0] != '\0') {
            if (setenv("RADEK_GAME_BUNDLE", guest_bundle_root, 1) != 0)
                rt_fatal("could not set the read-only guest bundle root");
        } else {
            unsetenv("RADEK_GAME_BUNDLE");
        }
        if (guest_obb_root != NULL && guest_obb_root[0] != '\0') {
            if (setenv("RADEK_GAME_OBB", guest_obb_root, 1) != 0)
                rt_fatal("could not set the optional read-only OBB root");
        } else {
            unsetenv("RADEK_GAME_OBB");
        }
        memset(&cpu, 0, sizeof cpu);
        cpu.r[13] = RT_STACK_TOP;
        cpu.cpsr = 0x10;
        rt_init(memory_image_path);
        rt_run_modinits(&cpu);
        rt_call_main(&cpu);
        RT_RUN_ACTIVE = 0;
        runner_status = "RETURNED";
        execution_status = "RETURNED";
    } else {
        RT_RUN_ACTIVE = 0;
        trapped = RT_STOP_IS_SHIM;
        runner_status = trapped ? "STOPPED_AT_RUNTIME_SHIM" : "RUNTIME_FAULT";
        execution_status = trapped ? "STOPPED_AT_TRAP" : "EXECUTION_FAULT";
    }

    set_report(report_json, report_capacity, runner_status, execution_status,
               trapped, RT_STOP_WHY);
    return trapped ? 1 : (strcmp(runner_status, "RETURNED") == 0 ? 0 : 2);
}
