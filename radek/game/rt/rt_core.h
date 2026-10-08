/* Native runtime core: arena, dispatch, loader, entry (host + device). */
#ifndef RADEK_GAME_RT_CORE_H
#define RADEK_GAME_RT_CORE_H

#include <setjmp.h>
#include <stdint.h>
#include "cpu.h"

/* Arena layout (game addresses are arena offsets; MEMBASE is calloc'd). */
#define RT_ARENA_SIZE ((uint32_t)0x06000000u)   /* 96 MiB */
#define RT_HEAP_BASE  ((uint32_t)0x00200000u)
#define RT_HEAP_SIZE  ((uint32_t)0x03000000u)   /* 48 MiB */
#define RT_STACK_BASE ((uint32_t)0x04000000u)
#define RT_STACK_SIZE ((uint32_t)0x00800000u)   /* 8 MiB */
#define RT_STACK_TOP  ((uint32_t)0x04800000u)
#define RT_BRIDGE_BASE ((uint32_t)0x05000000u)
#define RT_BRIDGE_SIZE ((uint32_t)0x00100000u)  /* 1 MiB */
#define RT_RETURN_MARKER ((uint32_t)0x80AD0000u)
#define RT_OBJC_EXTERNAL_BASE ((uint32_t)0x05200000u)
#define RT_OBJC_OBJECT_SIZE ((uint32_t)0x00001000u)

/* Logging (stderr, unbuffered; maps to __android_log on device later). */
void rt_log(const char *fmt, ...);
void rt_fatal(const char *fmt, ...);

/* Loader + lifecycle. */
void rt_init(const char *mem_path);
void rt_run_modinits(CPU *cpu);
void rt_call_main(CPU *cpu);

/* Shim dispatch: concrete guest-state libc/ABI/ObjC and graphics adapters,
 * with named fail-closed boundaries for APIs not yet mapped to Android. */
void rt_shim(CPU *cpu, unsigned i);

/* Clean-stop support for the host harness. */
extern jmp_buf RT_STOP_JB;
extern char RT_STOP_WHY[256];
extern unsigned RT_MODINITS_DONE;
extern int RT_MAIN_REACHED;

#endif
