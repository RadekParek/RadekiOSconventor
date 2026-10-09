/* Native runtime core: arena, dispatch, loader, entry (host + device). */
#include "rt_core.h"
#include "rt_fs.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rt_gen.h"

uint8_t *MEMBASE;
jmp_buf RT_STOP_JB;
char RT_STOP_WHY[256];
unsigned RT_MODINITS_DONE;
int RT_MAIN_REACHED;
int RT_RUN_ACTIVE;
int RT_STOP_IS_SHIM;

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (RT_RUN_ACTIVE) {
        va_list copy;
        va_copy(copy, ap);
        vsnprintf(RT_STOP_WHY, sizeof RT_STOP_WHY, fmt, copy);
        va_end(copy);
        RT_STOP_IS_SHIM = 0;
    }
    fputs("RT_FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    if (RT_RUN_ACTIVE)
        longjmp(RT_STOP_JB, 1);
    abort();
}

void trabort(void) {
    rt_fatal("jump-table arm out of range");
}

int vret_site_ok(uint32_t s) {
    uint32_t o = s - DT_CALL_LO;
    if (o >= DT_CALL_HI - DT_CALL_LO || (s & 3))
        return 0;
    return CALLSITE_MAP[o >> 2];
}

void tdispatch(CPU *cpu, uint32_t v) {
    if (v == RT_RETURN_MARKER)
        return;
    if (!(v & 1)) {
        unsigned lo = 0, hi = DT_NFUNCS;
        while (lo < hi) {
            unsigned mid = lo + (hi - lo) / 2;
            uint32_t a = DT_ADDRS[mid];
            if (a == v) {
                DT_FUNCS[mid](cpu);
                return;
            } else if (a < v) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        {
            unsigned i;
            for (i = 0; i < DT_NSTUBS; i++)
                if (DT_STUBS[i].addr == v) {
                    DT_SHIM_FUNCS[DT_STUBS[i].shim](cpu);
                    return;
                }
        }
    }
    rt_fatal("computed branch to unknown %08x (lr=%08x sp=%08x)",
             v, cpu->r[14], cpu->r[13]);
}

static uint32_t RT_HEAP_CURSOR = RT_HEAP_BASE;
#define RT_MAX_ALLOCATIONS 65536u
typedef struct { uint32_t address; uint32_t size; } RT_ALLOCATION;
static RT_ALLOCATION RT_ALLOCATIONS[RT_MAX_ALLOCATIONS];
static unsigned RT_NALLOCATIONS;

static uint32_t rt_alloc_guest(uint32_t bytes) {
    uint32_t aligned = (bytes + 7u) & ~7u;
    uint32_t address = (RT_HEAP_CURSOR + 7u) & ~7u;
    if (aligned > RT_HEAP_SIZE || address > RT_HEAP_BASE + RT_HEAP_SIZE - aligned)
        rt_fatal("translated-game heap exhausted (%u bytes)", bytes);
    if (RT_NALLOCATIONS >= RT_MAX_ALLOCATIONS)
        rt_fatal("translated-game allocation table exhausted");
    RT_HEAP_CURSOR = address + aligned;
    memset(MEMBASE + address, 0, aligned);
    RT_ALLOCATIONS[RT_NALLOCATIONS++] = (RT_ALLOCATION){address, bytes};
    return address;
}

static uint32_t rt_alloc_size(uint32_t address) {
    unsigned i;
    for (i = 0; i < RT_NALLOCATIONS; i++)
        if (RT_ALLOCATIONS[i].address == address)
            return RT_ALLOCATIONS[i].size;
    return 0;
}

typedef struct {
    uint32_t context;
    uint32_t previous;
    uint32_t call_site;
    uint32_t personality;
    uint32_t lsda;
    uint32_t dispatch;
    uint32_t saved_sp;
    unsigned active;
} RT_SJLJ_RECORD;
#define RT_MAX_SJLJ 16384u
#define RT_MAX_SJLJ_HISTORY 4096u
static RT_SJLJ_RECORD RT_SJLJ[RT_MAX_SJLJ];
static RT_SJLJ_RECORD RT_SJLJ_HISTORY[RT_MAX_SJLJ_HISTORY];
static unsigned RT_NSJLJ;
static unsigned RT_NSJLJ_HISTORY;
static unsigned RT_SJLJ_HISTORY_NEXT;
static uint32_t RT_SJLJ_TOP;
static unsigned RT_SJLJ_TRACE_EVENTS;

static int rt_trace_sjlj(void) {
    const char *value = getenv("RADEK_TRACE_SJLJ");
    return value && *value && strcmp(value, "0");
}

static int rt_trace_sjlj_full(void) {
    const char *value = getenv("RADEK_TRACE_SJLJ_FULL");
    return value && *value && strcmp(value, "0");
}

static void rt_sjlj_dump(uint32_t context) {
    unsigned i;
    if (!rt_trace_sjlj() && !rt_trace_sjlj_full()) return;
    rt_log("SJLJ context=%08x top=%08x", context, RT_SJLJ_TOP);
    for (i = 0; i < 16; i++)
        rt_log("SJLJ +%02x = %08x", i * 4u, rd32(context + i * 4u));
}

static void rt_sjlj_register(uint32_t context) {
    if (!context || context >= RT_ARENA_SIZE - 64u)
        rt_fatal("invalid SjLj context %08x", context);
    if (rt_trace_sjlj() && RT_SJLJ_TRACE_EVENTS < 40)
        rt_log("SJLJ register[%u] context=%08x sp=%08x", RT_SJLJ_TRACE_EVENTS++, context, rd32(context + 40));
    wr32(context, RT_SJLJ_TOP);
    if (RT_NSJLJ >= RT_MAX_SJLJ) {
        /* A translated frame can register more than once before the
         * corresponding ARM landing-pad cleanup is reached.  Keep the
         * newest context usable instead of turning bounded compatibility
         * bookkeeping into a fatal condition. */
        RT_SJLJ_TOP = context;
        if (rt_trace_sjlj()) {
            if (rt_trace_sjlj_full()) {
                rt_log("SJLJ record cap reached at %08x", context);
                rt_sjlj_dump(context);
            }
        }
        return;
    }
    {
        RT_SJLJ_RECORD record = {
            context, RT_SJLJ_TOP, rd32(context + 4), rd32(context + 24),
            rd32(context + 28), rd32(context + 36), rd32(context + 40), 1};
        RT_SJLJ_HISTORY[RT_SJLJ_HISTORY_NEXT] = record;
        RT_SJLJ_HISTORY_NEXT = (RT_SJLJ_HISTORY_NEXT + 1) % RT_MAX_SJLJ_HISTORY;
        if (RT_NSJLJ_HISTORY < RT_MAX_SJLJ_HISTORY) RT_NSJLJ_HISTORY++;
        if (RT_NSJLJ < RT_MAX_SJLJ) RT_SJLJ[RT_NSJLJ++] = record;
    }
    RT_SJLJ_TOP = context;
    if (rt_trace_sjlj_full()) rt_sjlj_dump(context);
}

static void rt_sjlj_unregister(uint32_t context) {
    unsigned i;
    if (!context) return;
    if (rt_trace_sjlj() && RT_SJLJ_TRACE_EVENTS < 80)
        rt_log("SJLJ unregister[%u] context=%08x", RT_SJLJ_TRACE_EVENTS++, context);
    for (i = RT_NSJLJ; i > 0; i--) {
        if (RT_SJLJ[i - 1].context == context) {
            unsigned history_step;
            if (RT_SJLJ_TOP == context)
                RT_SJLJ_TOP = RT_SJLJ[i - 1].previous;
            for (history_step = 0; history_step < RT_NSJLJ_HISTORY; history_step++) {
                unsigned history_slot = (RT_SJLJ_HISTORY_NEXT +
                    RT_MAX_SJLJ_HISTORY - 1u - history_step) % RT_MAX_SJLJ_HISTORY;
                if (RT_SJLJ_HISTORY[history_slot].active &&
                    RT_SJLJ_HISTORY[history_slot].context == context) {
                    RT_SJLJ_HISTORY[history_slot].active = 0;
                    break;
                }
            }
            if (i < RT_NSJLJ)
                memmove(&RT_SJLJ[i - 1], &RT_SJLJ[i],
                        (RT_NSJLJ - i) * sizeof RT_SJLJ[0]);
            RT_SJLJ[--RT_NSJLJ] = (RT_SJLJ_RECORD){0};
            return;
        }
    }
    if (rt_trace_sjlj()) rt_log("SJLJ unregister of unknown %08x", context);
}

static void rt_sjlj_dump_chain(void) {
    unsigned count = 0;
    uint32_t context = RT_SJLJ_TOP;
    if (!rt_trace_sjlj()) return;
    rt_log("SJLJ chain top=%08x records=%u", RT_SJLJ_TOP, RT_NSJLJ);
    while (context && context < RT_ARENA_SIZE - 64u && count < 32) {
        rt_log("SJLJ frame=%08x prev=%08x call=%08x per=%08x lsda=%08x "
               "j0=%08x j1=%08x j2=%08x j3=%08x",
               context, rd32(context), rd32(context + 4),
               rd32(context + 24), rd32(context + 28),
               rd32(context + 32), rd32(context + 36),
               rd32(context + 40), rd32(context + 44));
        context = rd32(context);
        count++;
    }
}

static const RT_LSDA_TABLE *rt_lsda_table(uint32_t address) {
    unsigned i;
    for (i = 0; i < RT_NLSDA_TABLES; i++)
        if (RT_LSDA_TABLES[i].addr == address)
            return &RT_LSDA_TABLES[i];
    return NULL;
}

static int rt_lsda_catches(const RT_LSDA_TABLE *table, unsigned action,
                           uint32_t typeinfo, unsigned *selector) {
    uint32_t record;
    unsigned step;
    if (!action) return 0;
    record = table->action_base + action - 1u;
    for (step = 0; step < table->actions + 1u; step++) {
        unsigned i;
        const RT_LSDA_ACTION *entry = NULL;
        for (i = 0; i < RT_NLSDA_ACTIONS; i++)
            if (RT_LSDA_ACTIONS[i].table == table->addr &&
                RT_LSDA_ACTIONS[i].record == record) {
                entry = &RT_LSDA_ACTIONS[i];
                break;
            }
        if (!entry) return 0;
        if (entry->filter == 0) {
            *selector = 0;
            return 1;
        }
        if (entry->filter > 0 && table->ttype_base >=
            (uint32_t)entry->filter * 4u) {
            uint32_t type_slot = table->ttype_base -
                (uint32_t)entry->filter * 4u;
            for (i = 0; i < RT_NLSDA_TYPES; i++)
                if (RT_LSDA_TYPES[i].table == table->addr &&
                    RT_LSDA_TYPES[i].slot == type_slot &&
                    RT_LSDA_TYPES[i].typeinfo == typeinfo) {
                    *selector = (unsigned)entry->filter;
                    return 1;
                }
        }
        if (!entry->next) return 0;
        record = entry->next;
    }
    return 0;
}

static int rt_sjlj_try_candidate(uint32_t typeinfo,
                                  RT_SJLJ_RECORD candidate,
                                  RT_SJLJ_RECORD *out, unsigned *landing,
                                  unsigned *action, unsigned *selector) {
    const RT_LSDA_TABLE *table;
    uint32_t call_site;
    unsigned site;
    if (!candidate.context || candidate.context >= RT_ARENA_SIZE - 64u ||
        !candidate.lsda || !(table = rt_lsda_table(candidate.lsda)))
        return 0;
    call_site = rd32(candidate.context + 4);
    if (call_site == UINT32_MAX || call_site >= table->callsites)
        return 0;
    site = table->site_first + call_site;
    if (site >= RT_NLSDA_SITES || RT_LSDA_SITES[site].table != candidate.lsda ||
        !rt_lsda_catches(table, RT_LSDA_SITES[site].action,
                         typeinfo, selector))
        return 0;
    *out = candidate;
    out->dispatch = rd32(candidate.context + 36);
    out->saved_sp = rd32(candidate.context + 40);
    *landing = RT_LSDA_SITES[site].landing;
    *action = RT_LSDA_SITES[site].action;
    if (rt_trace_sjlj())
        rt_log("SJLJ handler context=%08x call=%u landing=%u action=%u "
               "selector=%u lsda=%08x", candidate.context, call_site,
               *landing, *action, *selector, candidate.lsda);
    return 1;
}

static int rt_sjlj_handler(uint32_t typeinfo,
                           RT_SJLJ_RECORD *out, unsigned *landing,
                           unsigned *action, unsigned *selector) {
    unsigned step;
    /* Prefer the live registration chain.  The host history is a fallback
     * for translated mid-function entries whose cleanup label is emitted
     * without a matching entry label. */
    uint32_t context = RT_SJLJ_TOP;
    uint32_t seen[64];
    unsigned nseen = 0;
    while (context && context < RT_ARENA_SIZE - 64u && nseen < 64) {
        RT_SJLJ_RECORD candidate = {0};
        unsigned i;
        for (i = 0; i < RT_NSJLJ; i++)
            if (RT_SJLJ[i].context == context) {
                candidate = RT_SJLJ[i];
                break;
            }
        if (!candidate.context) {
            candidate.context = context;
            candidate.lsda = rd32(context + 28);
            candidate.call_site = rd32(context + 4);
            candidate.dispatch = rd32(context + 36);
            candidate.saved_sp = rd32(context + 40);
        }
        if (rt_sjlj_try_candidate(typeinfo, candidate, out, landing,
                                  action, selector))
            return 1;
        seen[nseen++] = context;
        context = rd32(context);
        for (i = 0; i < nseen; i++)
            if (seen[i] == context) { context = 0; break; }
    }
    for (step = 0; step < RT_NSJLJ_HISTORY; step++) {
        unsigned slot = (RT_SJLJ_HISTORY_NEXT + RT_MAX_SJLJ_HISTORY - 1u - step)
            % RT_MAX_SJLJ_HISTORY;
        if (RT_SJLJ_HISTORY[slot].active &&
            rt_sjlj_try_candidate(typeinfo, RT_SJLJ_HISTORY[slot],
                                  out, landing, action, selector))
            return 1;
    }
    return 0;
}

static void rt_stop_at_shim(CPU *cpu, const char *symbol) {
    RT_STOP_IS_SHIM = 1;
    snprintf(RT_STOP_WHY, sizeof RT_STOP_WHY,
             "shim=%s r0=%08x r1=%08x r2=%08x r3=%08x lr=%08x sp=%08x",
             symbol, cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3],
             cpu->r[14], cpu->r[13]);
    rt_log("RT_STOP %s", RT_STOP_WHY);
    longjmp(RT_STOP_JB, 1);
}

static uint32_t rt_f32_unary(CPU *cpu, float (*fn)(float)) {
    return f2u(fn(u2f(cpu->r[0])));
}

static uint32_t rt_f32_binary(CPU *cpu, float (*fn)(float, float)) {
    return f2u(fn(u2f(cpu->r[0]), u2f(cpu->r[1])));
}

static uint64_t rt_abi_get_d(CPU *cpu, unsigned lo) {
    return (uint64_t)cpu->r[lo] | ((uint64_t)cpu->r[lo + 1] << 32);
}

static void rt_abi_put_d(CPU *cpu, unsigned lo, uint64_t value) {
    cpu->r[lo] = (uint32_t)value;
    cpu->r[lo + 1] = (uint32_t)(value >> 32);
}

static uint32_t rt_guest_string_length(uint32_t address) {
    uint32_t n = 0;
    if (address >= RT_ARENA_SIZE)
        rt_fatal("guest string pointer out of arena: %08x", address);
    while (address + n < RT_ARENA_SIZE && MEMBASE[address + n]) {
        if (++n == RT_ARENA_SIZE)
            rt_fatal("unterminated guest string at %08x", address);
    }
    if (address + n >= RT_ARENA_SIZE)
        rt_fatal("unterminated guest string at %08x", address);
    return n;
}

static void rt_check_guest_range(uint32_t address, uint32_t length) {
    if (address > RT_ARENA_SIZE || length > RT_ARENA_SIZE - address)
        rt_fatal("guest memory range out of arena: %08x+%08x", address, length);
}

static uint32_t rt_guest_strlen(CPU *cpu) {
    return rt_guest_string_length(cpu->r[0]);
}

static int rt_guest_strcmp(uint32_t a, uint32_t b, uint32_t limit, int fold_case) {
    uint32_t n = 0;
    rt_check_guest_range(a, 1);
    rt_check_guest_range(b, 1);
    while (n < limit) {
        unsigned ca = MEMBASE[a + n], cb = MEMBASE[b + n];
        if (fold_case) {
            if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
            if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        }
        if (ca != cb || ca == 0)
            return ca < cb ? -1 : (ca > cb ? 1 : 0);
        n++;
    }
    return 0;
}

static uint32_t rt_guest_copy_string(CPU *cpu, int append, int bounded) {
    uint32_t dst = cpu->r[0], src = cpu->r[1];
    uint32_t src_len = rt_guest_string_length(src);
    uint32_t old_len = append ? rt_guest_string_length(dst) : 0;
    uint32_t take = bounded && cpu->r[2] < src_len ? cpu->r[2] : src_len;
    rt_check_guest_range(dst, old_len + take + 1);
    memmove(MEMBASE + dst + old_len, MEMBASE + src, take);
    MEMBASE[dst + old_len + take] = 0;
    return dst;
}

static uint32_t rt_guest_memcpy(CPU *cpu, int move) {
    rt_check_guest_range(cpu->r[0], cpu->r[2]);
    rt_check_guest_range(cpu->r[1], cpu->r[2]);
    if (move)
        memmove(MEMBASE + cpu->r[0], MEMBASE + cpu->r[1], cpu->r[2]);
    else
        memcpy(MEMBASE + cpu->r[0], MEMBASE + cpu->r[1], cpu->r[2]);
    return cpu->r[0];
}

static uint32_t rt_guest_strchr(CPU *cpu, int reverse) {
    uint32_t p = cpu->r[0], n = rt_guest_string_length(p), i;
    unsigned needle = cpu->r[1] & 0xffu;
    if (reverse) {
        for (i = n + 1; i-- > 0;)
            if (MEMBASE[p + i] == needle)
                return p + i;
    } else {
        for (i = 0; i <= n; i++)
            if (MEMBASE[p + i] == needle)
                return p + i;
    }
    return 0;
}

typedef struct {
    uint32_t handle;
    const char *name;
} RT_OBJC_EXTERNAL;

typedef struct {
    uint32_t address;
    uint32_t class_handle;
    uint32_t association;
    uint32_t value;
    uint32_t value2;
    uint32_t elements[4];
    uint32_t dict_keys[8];
    uint32_t dict_values[8];
    unsigned count;
    unsigned pairs;
    unsigned kind;
    unsigned retain_count;
} RT_OBJC_OBJECT;

enum {
    RT_OBJC_KIND_GENERIC = 0,
    RT_OBJC_KIND_STRING = 1,
    RT_OBJC_KIND_ARRAY = 2,
    RT_OBJC_KIND_NUMBER = 3,
    RT_OBJC_KIND_APPLICATION = 4,
    RT_OBJC_KIND_DICTIONARY = 5,
};

#define RT_MAX_OBJC_EXTERNALS 64u
#define RT_MAX_OBJC_OBJECTS 2048u
static RT_OBJC_EXTERNAL RT_OBJC_EXTERNALS[RT_MAX_OBJC_EXTERNALS];
static unsigned RT_NOBJC_EXTERNALS;
static RT_OBJC_OBJECT RT_OBJC_OBJECTS[RT_MAX_OBJC_OBJECTS];
static unsigned RT_NOBJC_OBJECTS;

#define RT_MAX_FILES 128u
typedef struct {
    uint32_t token;
    FILE *host;
} RT_FILE_HANDLE;
static RT_FILE_HANDLE RT_FILES[RT_MAX_FILES];
static unsigned RT_NFILES;

static RT_FILE_HANDLE *rt_file_handle(uint32_t token) {
    unsigned i;
    for (i = 0; i < RT_NFILES; i++)
        if (RT_FILES[i].token == token)
            return &RT_FILES[i];
    return NULL;
}


static uint32_t rt_find_guest_cstring(uint32_t address) {
    unsigned offset;
    if (!address || address >= RT_ARENA_SIZE)
        return 0;
    if (MEMBASE[address] >= 32 && MEMBASE[address] <= 126) {
        rt_guest_string_length(address);
        return address;
    }
    /* A String value passed through an ARM C varargs call contributes its
     * first word, which points at the runtime's string-data record.  Older
     * records keep the bytes inline after three header words; prefer that
     * representation before treating later object words as pointers. */
    if (address <= RT_ARENA_SIZE - 16 && !rd32(address) &&
        MEMBASE[address + 12] >= 32 && MEMBASE[address + 12] <= 126) {
        rt_guest_string_length(address + 12);
        return address + 12;
    }
    for (offset = 0; offset <= 32; offset += 4) {
        uint32_t candidate;
        if (address > RT_ARENA_SIZE - offset - 4)
            break;
        candidate = rd32(address + offset);
        if (candidate && candidate < RT_ARENA_SIZE &&
            MEMBASE[candidate] >= 32 && MEMBASE[candidate] <= 126) {
            rt_guest_string_length(candidate);
            return candidate;
        }
    }
    return 0;
}

typedef struct {
    uint32_t values[32];
    unsigned count;
    unsigned next;
} RT_FORMAT_ARGS;

static uint32_t rt_format_arg(RT_FORMAT_ARGS *args) {
    return args->next < args->count ? args->values[args->next++] : 0;
}

static void rt_format_put(char *out, uint32_t limit, uint32_t *used,
                          const char *text) {
    while (*text && *used + 1 < limit)
        out[(*used)++] = *text++;
    if (limit)
        out[*used < limit ? *used : limit - 1] = 0;
}

static uint32_t rt_guest_format(uint32_t destination, uint32_t limit,
                                uint32_t format_address, RT_FORMAT_ARGS *args) {
    uint32_t used = 0, i = 0;
    char temp[128];
    const char *format;
    rt_check_guest_range(destination, limit ? limit : 1);
    format = (const char *)(MEMBASE + format_address);
    if (!limit) return 0;
    while (format[i] && used + 1 < limit) {
        if (format[i] != '%') {
            ((char *)(MEMBASE + destination))[used++] = format[i++];
            continue;
        }
        i++;
        if (format[i] == '%') {
            ((char *)(MEMBASE + destination))[used++] = format[i++];
            continue;
        }
        while (format[i] == '-' || format[i] == '+' || format[i] == ' ' ||
               format[i] == '#' || format[i] == '0') i++;
        while ((format[i] >= '0' && format[i] <= '9') || format[i] == '.') i++;
        if (format[i] == 'l' || format[i] == 'h' || format[i] == 'z') i++;
        if (format[i] == 'l' && format[i + 1] == 'l') i += 2;
        switch (format[i++]) {
        case 's': {
            uint32_t value = rt_format_arg(args), address = rt_find_guest_cstring(value);
            rt_format_put((char *)(MEMBASE + destination), limit, &used,
                          address ? (const char *)(MEMBASE + address) : "(null)");
            break;
        }
        case 'c':
            if (used + 1 < limit) ((char *)(MEMBASE + destination))[used++] = (char)rt_format_arg(args);
            break;
        case 'd': case 'i':
            snprintf(temp, sizeof temp, "%d", (int32_t)rt_format_arg(args));
            rt_format_put((char *)(MEMBASE + destination), limit, &used, temp);
            break;
        case 'u':
            snprintf(temp, sizeof temp, "%u", rt_format_arg(args));
            rt_format_put((char *)(MEMBASE + destination), limit, &used, temp);
            break;
        case 'x': case 'X':
            snprintf(temp, sizeof temp, format[i - 1] == 'x' ? "%x" : "%X", rt_format_arg(args));
            rt_format_put((char *)(MEMBASE + destination), limit, &used, temp);
            break;
        case 'p':
            snprintf(temp, sizeof temp, "0x%08x", rt_format_arg(args));
            rt_format_put((char *)(MEMBASE + destination), limit, &used, temp);
            break;
        case 'f': case 'g': case 'e':
            snprintf(temp, sizeof temp, "%g", (double)u2f(rt_format_arg(args)));
            rt_format_put((char *)(MEMBASE + destination), limit, &used, temp);
            break;
        default:
            if (used + 1 < limit)
                ((char *)(MEMBASE + destination))[used++] = '?';
            break;
        }
    }
    ((char *)(MEMBASE + destination))[used < limit ? used : limit - 1] = 0;
    return used;
}

static void rt_format_args_from_regs(CPU *cpu, uint32_t first,
                                      uint32_t second, RT_FORMAT_ARGS *args) {
    unsigned i;
    args->count = args->next = 0;
    args->values[args->count++] = first;
    args->values[args->count++] = second;
    for (i = 0; i < 28 && args->count < 32; i++) {
        uint32_t address = cpu->r[13] + i * 4u;
        if (address >= RT_ARENA_SIZE - 4) break;
        args->values[args->count++] = rd32(address);
    }
}

static void rt_format_args_from_va(uint32_t va, RT_FORMAT_ARGS *args) {
    unsigned i;
    args->count = args->next = 0;
    for (i = 0; i < 32; i++) {
        rt_check_guest_range(va + i * 4u, 4);
        args->values[args->count++] = rd32(va + i * 4u);
    }
}

static const char *rt_objc_external_name(const char *symbol) {
    const char *marker = strstr(symbol, "$_");
    return marker ? marker + 2 : NULL;
}

static uint32_t rt_objc_class_handle(const char *name) {
    unsigned i;
    for (i = 0; i < RT_NOBJC_CLASSES; i++)
        if (!strcmp(RT_OBJC_CLASSES[i].name, name))
            return RT_OBJC_CLASSES[i].addr;
    for (i = 0; i < RT_NOBJC_EXTERNALS; i++)
        if (!strcmp(RT_OBJC_EXTERNALS[i].name, name))
            return RT_OBJC_EXTERNALS[i].handle;
    if (RT_NOBJC_EXTERNALS >= RT_MAX_OBJC_EXTERNALS)
        rt_fatal("too many external Objective-C classes");
    RT_OBJC_EXTERNALS[RT_NOBJC_EXTERNALS].handle =
        RT_OBJC_EXTERNAL_BASE + RT_NOBJC_EXTERNALS * 0x100u;
    RT_OBJC_EXTERNALS[RT_NOBJC_EXTERNALS].name = name;
    return RT_OBJC_EXTERNALS[RT_NOBJC_EXTERNALS++].handle;
}

static RT_OBJC_OBJECT *rt_objc_object(uint32_t address) {
    unsigned i;
    for (i = 0; i < RT_NOBJC_OBJECTS; i++)
        if (RT_OBJC_OBJECTS[i].address == address)
            return &RT_OBJC_OBJECTS[i];
    return NULL;
}

static const char *rt_objc_class_name(uint32_t handle) {
    unsigned i;
    RT_OBJC_OBJECT *object = rt_objc_object(handle);
    if (object)
        handle = object->class_handle;
    for (i = 0; i < RT_NOBJC_CLASSES; i++)
        if (RT_OBJC_CLASSES[i].addr == handle)
            return RT_OBJC_CLASSES[i].name;
    for (i = 0; i < RT_NOBJC_EXTERNALS; i++)
        if (RT_OBJC_EXTERNALS[i].handle == handle)
            return RT_OBJC_EXTERNALS[i].name;
    return NULL;
}

static uint32_t rt_objc_new(uint32_t class_handle) {
    uint32_t address;
    if (RT_NOBJC_OBJECTS >= RT_MAX_OBJC_OBJECTS)
        rt_fatal("translated Objective-C object table exhausted");
    address = rt_alloc_guest(RT_OBJC_OBJECT_SIZE);
    wr32(address, class_handle);
    RT_OBJC_OBJECTS[RT_NOBJC_OBJECTS++] = (RT_OBJC_OBJECT){
        address, class_handle, 0, 0, 0, {0, 0, 0, 0},
        {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0},
        0, 0, RT_OBJC_KIND_GENERIC, 1
    };
    return address;
}

static RT_OBJC_OBJECT *rt_objc_new_kind(uint32_t class_handle, unsigned kind) {
    uint32_t address = rt_objc_new(class_handle);
    RT_OBJC_OBJECT *object = rt_objc_object(address);
    object->kind = kind;
    return object;
}

static RT_OBJC_OBJECT *rt_objc_new_string(const char *text) {
    RT_OBJC_OBJECT *object = rt_objc_new_kind(
        rt_objc_class_handle("NSString"), RT_OBJC_KIND_STRING);
    uint32_t length = (uint32_t)strlen(text);
    object->value = rt_alloc_guest(length + 1);
    memcpy(MEMBASE + object->value, text, length + 1);
    return object;
}

static RT_OBJC_OBJECT *rt_objc_new_array(uint32_t element) {
    RT_OBJC_OBJECT *object = rt_objc_new_kind(
        rt_objc_class_handle("NSArray"), RT_OBJC_KIND_ARRAY);
    object->elements[0] = element;
    object->count = 1;
    return object;
}

static RT_OBJC_OBJECT *rt_objc_new_number(uint32_t value) {
    RT_OBJC_OBJECT *object = rt_objc_new_kind(
        rt_objc_class_handle("NSNumber"), RT_OBJC_KIND_NUMBER);
    object->value = value;
    return object;
}

static RT_OBJC_OBJECT *rt_objc_new_dictionary(uint32_t value, uint32_t key) {
    RT_OBJC_OBJECT *object = rt_objc_new_kind(
        rt_objc_class_handle("NSDictionary"), RT_OBJC_KIND_DICTIONARY);
    if (value && key) {
        object->dict_values[0] = value;
        object->dict_keys[0] = key;
        object->pairs = 1;
    }
    return object;
}

static int rt_objc_has_method(const char *owner, const char *selector) {
    unsigned i;
    for (i = 0; i < RT_NOBJC_METHODS; i++)
        if (!strcmp(RT_OBJC_METHODS[i].owner, owner) &&
            !strcmp(RT_OBJC_METHODS[i].selector, selector))
            return 1;
    return 0;
}

static uint32_t rt_objc_method_imp(const char *owner, const char *selector) {
    unsigned i;
    for (i = 0; i < RT_NOBJC_METHODS; i++)
        if (!strcmp(RT_OBJC_METHODS[i].owner, owner) &&
            !strcmp(RT_OBJC_METHODS[i].selector, selector))
            return RT_OBJC_METHODS[i].imp;
    return 0;
}

static uint32_t rt_objc_cfstring_cstr(uint32_t address) {
    uint32_t candidate, length;
    if (!address || address >= RT_ARENA_SIZE)
        return 0;
    if (MEMBASE[address] >= 32 && MEMBASE[address] <= 126) {
        rt_guest_string_length(address);
        return address;
    }
    if (address > RT_ARENA_SIZE - 16)
        return 0;
    candidate = rd32(address + 8);
    length = rd32(address + 12);
    if (!candidate || candidate >= RT_ARENA_SIZE || length > 1024)
        return 0;
    rt_check_guest_range(candidate, length + 1);
    return candidate;
}

static uint32_t rt_objc_selector_literal(const char *selector) {
    uint32_t length = (uint32_t)strlen(selector);
    uint32_t address = rt_alloc_guest(length + 1);
    memcpy(MEMBASE + address, selector, length + 1);
    return address;
}

static void rt_objc_dispatch_imp(CPU *cpu, uint32_t imp) {
    uint32_t caller_lr = cpu->r[14];
    cpu->r[14] = RT_RETURN_MARKER;
    tdispatch(cpu, imp);
    cpu->r[14] = caller_lr;
}

static void rt_objc_msg_send(CPU *cpu, int stret, int super_send) {
    uint32_t return_buffer = 0;
    uint32_t receiver;
    uint32_t selector_address;
    const char *selector;
    const char *class_name;
    RT_OBJC_OBJECT *object;
    uint32_t class_handle;
    uint32_t imp;

    if (stret) {
        return_buffer = cpu->r[0];
        receiver = cpu->r[1];
        selector_address = cpu->r[2];
        cpu->r[0] = receiver;
        cpu->r[1] = selector_address;
        cpu->r[2] = cpu->r[3];
        cpu->r[3] = 0;
    } else if (super_send) {
        uint32_t super = cpu->r[0];
        receiver = super ? rd32(super) : 0;
        selector_address = cpu->r[1];
        cpu->r[0] = receiver;
    } else {
        receiver = cpu->r[0];
        selector_address = cpu->r[1];
    }
    if (!selector_address || selector_address >= RT_ARENA_SIZE) {
        cpu->r[0] = 0;
        return;
    }
    rt_check_guest_range(selector_address, 1);
    rt_guest_string_length(selector_address);
    selector = (const char *)(MEMBASE + selector_address);
    object = rt_objc_object(receiver);
    class_handle = object ? object->class_handle : receiver;
    class_name = rt_objc_class_name(class_handle);

    /* Objective-C nil messaging is a defined zero-return operation. */
    if (!receiver) {
        cpu->r[0] = 0;
        if (stret && return_buffer) {
            rt_check_guest_range(return_buffer, 32);
            memset(MEMBASE + return_buffer, 0, 32);
        }
        return;
    }

    if (class_name && !strcmp(class_name, "NSNumber") &&
        (!strcmp(selector, "numberWithBool:") ||
         !strcmp(selector, "numberWithInt:") ||
         !strcmp(selector, "numberWithUnsignedInt:") ||
         !strcmp(selector, "numberWithFloat:") ||
         !strcmp(selector, "numberWithDouble:"))) {
        cpu->r[0] = rt_objc_new_number(cpu->r[2])->address;
        return;
    }
    if (class_name && !strcmp(class_name, "NSDictionary") &&
        !strcmp(selector, "dictionaryWithObjectsAndKeys:")) {
        cpu->r[0] = rt_objc_new_dictionary(cpu->r[2], cpu->r[3])->address;
        return;
    }
    if (!strcmp(selector, "new") || !strcmp(selector, "alloc")) {
        if (!class_name)
            rt_stop_at_shim(cpu, "objc_msgSend unknown class");
        cpu->r[0] = rt_objc_new(class_handle);
        if (!strcmp(selector, "new")) {
            uint32_t construct = rt_objc_method_imp(class_name, ".cxx_construct");
            if (construct)
                rt_objc_dispatch_imp(cpu, construct);
        }
        return;
    }
    if (!strcmp(selector, "init") || !strcmp(selector, "self") ||
        !strcmp(selector, "retain") || !strcmp(selector, "autorelease")) {
        cpu->r[0] = receiver;
        return;
    }
    if (!strcmp(selector, "release") || !strcmp(selector, "dealloc")) {
        cpu->r[0] = 0;
        return;
    }
    if (!strcmp(selector, "class")) {
        cpu->r[0] = class_handle;
        return;
    }
    if (object && object->kind == RT_OBJC_KIND_STRING) {
        if (!strcmp(selector, "UTF8String") || !strcmp(selector, "cString")) {
            cpu->r[0] = object->value;
            return;
        }
        if (!strcmp(selector, "length")) {
            cpu->r[0] = rt_guest_string_length(object->value);
            return;
        }
        if (!strcmp(selector, "description") || !strcmp(selector, "copy") ||
            !strcmp(selector, "retain") || !strcmp(selector, "autorelease")) {
            cpu->r[0] = receiver;
            return;
        }
    }
    if (object && object->kind == RT_OBJC_KIND_ARRAY) {
        if (!strcmp(selector, "count")) {
            cpu->r[0] = object->count;
            return;
        }
        if (!strcmp(selector, "objectAtIndex:") || !strcmp(selector, "objectAtIndexedSubscript:")) {
            cpu->r[0] = cpu->r[2] < object->count ? object->elements[cpu->r[2]] : 0;
            return;
        }
        if (!strcmp(selector, "lastObject")) {
            cpu->r[0] = object->count ? object->elements[object->count - 1] : 0;
            return;
        }
    }
    if (object && object->kind == RT_OBJC_KIND_DICTIONARY) {
        if (!strcmp(selector, "count")) {
            cpu->r[0] = object->pairs;
            return;
        }
        if (!strcmp(selector, "objectForKey:") || !strcmp(selector, "valueForKey:")) {
            unsigned j;
            cpu->r[0] = 0;
            for (j = 0; j < object->pairs; j++)
                if (object->dict_keys[j] == cpu->r[2]) {
                    cpu->r[0] = object->dict_values[j];
                    break;
                }
            return;
        }
        if (!strcmp(selector, "setObject:forKey:")) {
            if (object->pairs < 8) {
                object->dict_values[object->pairs] = cpu->r[2];
                object->dict_keys[object->pairs++] = cpu->r[3];
            }
            cpu->r[0] = receiver;
            return;
        }
    }
    if (object && object->kind == RT_OBJC_KIND_NUMBER) {
        if (!strcmp(selector, "intValue") || !strcmp(selector, "integerValue") ||
            !strcmp(selector, "boolValue")) {
            cpu->r[0] = object->value;
            return;
        }
        if (!strcmp(selector, "floatValue")) {
            cpu->r[0] = object->value;
            return;
        }
    }
    if (!strcmp(selector, "respondsToSelector:")) {
        const char *requested = rt_objc_class_name(cpu->r[2]);
        (void)requested;
        cpu->r[0] = class_name && rt_objc_has_method(class_name,
            (cpu->r[2] && cpu->r[2] < RT_ARENA_SIZE) ?
            (const char *)(MEMBASE + cpu->r[2]) : "") ? 1u : 0u;
        return;
    }
    if (!strcmp(selector, "isKindOfClass:") || !strcmp(selector, "isMemberOfClass:")) {
        cpu->r[0] = (cpu->r[2] == class_handle) ? 1u : 0u;
        return;
    }
    if (!strcmp(selector, "setDelegate:")) {
        object = rt_objc_object(receiver);
        if (object)
            object->association = cpu->r[2];
        cpu->r[0] = receiver;
        return;
    }
    if (!strcmp(selector, "delegate")) {
        cpu->r[0] = object ? object->association : 0;
        return;
    }
    if (!strcmp(selector, "mainBundle")) {
        static uint32_t bundle;
        if (!bundle)
            bundle = rt_objc_new(rt_objc_class_handle("NSBundle"));
        cpu->r[0] = bundle;
        return;
    }
    if (!strcmp(selector, "layer")) {
        cpu->r[0] = rt_objc_new(rt_objc_class_handle("CAEAGLLayer"));
        return;
    }
    if (object && class_name && !strcmp(class_name, "NSBundle") &&
        (!strcmp(selector, "resourcePath") || !strcmp(selector, "bundlePath") ||
         !strcmp(selector, "pathForResource:ofType:"))) {
        cpu->r[0] = rt_objc_new_string("/radek-bundle/App.app")->address;
        return;
    }
    if (!strcmp(selector, "sharedApplication") ||
        !strcmp(selector, "mainScreen") || !strcmp(selector, "currentThread") ||
        !strcmp(selector, "sharedAccelerometer")) {
        static uint32_t singletons[4];
        unsigned slot = !strcmp(selector, "sharedApplication") ? 0 :
            (!strcmp(selector, "mainScreen") ? 1 :
             (!strcmp(selector, "currentThread") ? 2 : 3));
        if (!singletons[slot]) {
            uint32_t singleton_class = !strcmp(selector, "sharedApplication")
                ? rt_objc_class_handle("UIApplication")
                : (!strcmp(selector, "mainScreen")
                   ? rt_objc_class_handle("UIScreen")
                   : (!strcmp(selector, "currentThread")
                      ? rt_objc_class_handle("NSThread")
                      : rt_objc_class_handle("UIAccelerometer")));
            singletons[slot] = rt_objc_new(singleton_class);
            if (slot == 0) {
                RT_OBJC_OBJECT *application_object = rt_objc_object(singletons[slot]);
                application_object->kind = RT_OBJC_KIND_APPLICATION;
            }
        }
        cpu->r[0] = singletons[slot];
        return;
    }

    imp = class_name ? rt_objc_method_imp(class_name, selector) : 0;
    if (imp) {
        rt_objc_dispatch_imp(cpu, imp);
        return;
    }
    if (!strncmp(selector, "init", 4)) {
        cpu->r[0] = receiver;
        return;
    }
    if (class_name && !strcmp(class_name, "EAGLContext") &&
        !strcmp(selector, "currentContext")) {
        static uint32_t current_context;
        if (!current_context)
            current_context = rt_objc_new(class_handle);
        cpu->r[0] = current_context;
        return;
    }
    if (object && selector[0] == 's' && !strncmp(selector, "set", 3)) {
        object->association = cpu->r[2];
        cpu->r[0] = receiver;
        return;
    }
    if (!strcmp(selector, "addSubview:") || !strcmp(selector, "insertSubview:atIndex:") ||
        !strcmp(selector, "removeFromSuperview") || !strcmp(selector, "setNeedsDisplay") ||
        !strcmp(selector, "layoutIfNeeded") || !strcmp(selector, "makeKeyAndVisible") ||
        !strcmp(selector, "start") || !strcmp(selector, "stop") ||
        !strcmp(selector, "cancel")) {
        cpu->r[0] = receiver;
        return;
    }
    if (selector[0] == 's' && !strncmp(selector, "set", 3)) {
        cpu->r[0] = receiver;
        return;
    }
    if (stret && return_buffer) {
        rt_check_guest_range(return_buffer, 32);
        memset(MEMBASE + return_buffer, 0, 32);
        cpu->r[0] = return_buffer;
        return;
    }
    rt_stop_at_shim(cpu, selector);
}

static void rt_objc_application_main(CPU *cpu) {
    uint32_t class_text = rt_objc_cfstring_cstr(cpu->r[3]);
    const char *class_name = class_text ? (const char *)(MEMBASE + class_text) : "AppController";
    uint32_t class_handle = rt_objc_class_handle(class_name);
    uint32_t delegate = rt_objc_new(class_handle);
    uint32_t application = rt_objc_new(rt_objc_class_handle("UIApplication"));
    uint32_t selector = rt_objc_selector_literal("applicationDidFinishLaunching:");
    uint32_t caller_lr = cpu->r[14];
    cpu->r[0] = delegate;
    cpu->r[1] = selector;
    cpu->r[2] = application;
    cpu->r[14] = RT_RETURN_MARKER;
    {
        uint32_t imp = rt_objc_method_imp(class_name, "applicationDidFinishLaunching:");
        if (imp)
            tdispatch(cpu, imp);
    }
    cpu->r[14] = caller_lr;
    cpu->r[0] = 0;
}

static uint32_t rt_stub_address(const char *symbol) {
    unsigned i;
    while (*symbol == '_') symbol++;
    for (i = 0; i < DT_NSTUBS; i++)
        if (!strcmp(rt_shim_symbol(DT_STUBS[i].shim), symbol))
            return DT_STUBS[i].addr;
    return 0;
}

/* Materialize the small, locale-independent part of Darwin's
 * _DefaultRuneLocale that is used by the ARM binary's inlined ctype code.
 *
 * The imported symbol is data, not a callable shim.  The old generic loader
 * therefore left its GOT/NLSYM slot at zero, which made code equivalent to
 *
 *     _DefaultRuneLocale.__runetype['0'] & _CTYPE_D
 *
 * read from address zero and report that every digit was invalid.  Keep the
 * object in guest memory: translated code must never receive a host pointer.
 * The layout and offsets below are the 32-bit Darwin _RuneLocale layout:
 * magic[8], encoding[32], two function pointers, invalid_rune, then the
 * 256-entry runetype/lower/upper arrays. */
static uint32_t rt_materialize_default_rune_locale(void) {
    enum {
        RT_RUNE_MAGIC = 0,
        RT_RUNE_ENCODING = 8,
        RT_RUNE_INVALID = 48,
        RT_RUNE_TYPE = 52,
        RT_RUNE_LOWER = RT_RUNE_TYPE + 256 * 4,
        RT_RUNE_UPPER = RT_RUNE_LOWER + 256 * 4,
        RT_RUNE_BYTES = RT_RUNE_UPPER + 256 * 4
    };
    static const char magic[] = "RuneMagA";
    static const char encoding[] = "US-ASCII";
    uint32_t locale = rt_alloc_guest((RT_RUNE_BYTES + 7u) & ~7u);
    unsigned c;

    memcpy(MEMBASE + locale + RT_RUNE_MAGIC, magic, sizeof magic - 1u);
    memcpy(MEMBASE + locale + RT_RUNE_ENCODING, encoding, sizeof encoding - 1u);
    wr32(locale + RT_RUNE_INVALID, 0xfffdu);
    for (c = 0; c < 256u; c++) {
        uint32_t mask = 0;
        uint32_t lower = c;
        uint32_t upper = c;
        int alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        int digit = c >= '0' && c <= '9';
        int control = c < 0x20u || c == 0x7fu;
        int space = c == ' ' || (c >= '\t' && c <= '\r');

        if (alpha) mask |= 0x00000100u; /* _CTYPE_A */
        if (control) mask |= 0x00000200u; /* _CTYPE_C */
        if (digit) mask |= 0x00000400u | (c - '0'); /* _CTYPE_D + value */
        if (c >= 0x21u && c <= 0x7eu) mask |= 0x00000800u; /* _CTYPE_G */
        if (c >= 'a' && c <= 'z') mask |= 0x00001000u; /* _CTYPE_L */
        if (c >= 0x21u && c <= 0x7eu && !alpha && !digit)
            mask |= 0x00002000u; /* _CTYPE_P */
        if (space) mask |= 0x00004000u; /* _CTYPE_S */
        if (c >= 'A' && c <= 'Z') mask |= 0x00008000u; /* _CTYPE_U */
        if (digit || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
            mask |= 0x00010000u; /* _CTYPE_X */
        if (c == ' ' || c == '\t') mask |= 0x00020000u; /* _CTYPE_B */
        if (c >= 0x20u && c <= 0x7eu) mask |= 0x00040000u; /* _CTYPE_R */
        if (c >= 'A' && c <= 'Z') lower = c + ('a' - 'A');
        if (c >= 'a' && c <= 'z') upper = c - ('a' - 'A');
        wr32(locale + RT_RUNE_TYPE + c * 4u, mask);
        wr32(locale + RT_RUNE_LOWER + c * 4u, lower);
        wr32(locale + RT_RUNE_UPPER + c * 4u, upper);
    }
    return locale;
}

static void rt_bind_loader(void) {
    unsigned i;
    for (i = 0; i < RT_NNLSYM; i++)
        if (!strcmp(RT_NLSYM[i].sym, "__DefaultRuneLocale")) {
            wr32(RT_NLSYM[i].addr, rt_materialize_default_rune_locale());
            break;
        }
    for (i = 0; i < RT_NEXTREL; i++) {
        const char *symbol = RT_EXTREL[i].sym;
        const char *class_name = rt_objc_external_name(symbol);
        uint32_t value = class_name &&
            (strstr(symbol, "_OBJC_CLASS_$_") || strstr(symbol, "_OBJC_METACLASS_$_"))
            ? rt_objc_class_handle(class_name) : rt_stub_address(symbol);
        if (value)
            wr32(RT_EXTREL[i].addr, value);
    }
    for (i = 0; i < RT_NNLSYM; i++) {
        uint32_t value = rt_stub_address(RT_NLSYM[i].sym);
        if (value)
            wr32(RT_NLSYM[i].addr, value);
    }
    for (i = 0; i < RT_NLASYM; i++) {
        uint32_t value = rt_stub_address(RT_LASYM[i].sym);
        if (value)
            wr32(RT_LASYM[i].addr, value);
    }
}

typedef struct {
    uint32_t token;
    uint32_t result;
    unsigned done;
} RT_GUEST_THREAD;
static RT_GUEST_THREAD RT_GUEST_THREADS[128];
static unsigned RT_NGUEST_THREADS;

static RT_GUEST_THREAD *rt_guest_thread(uint32_t token) {
    unsigned i;
    for (i = 0; i < RT_NGUEST_THREADS; i++)
        if (RT_GUEST_THREADS[i].token == token)
            return &RT_GUEST_THREADS[i];
    return NULL;
}

/* Bounded C ABI support used by translated static initializers.  These are
 * guest-state operations, not host pointer calls: all pointers remain offsets
 * in MEMBASE.  Unknown or framework-dependent imports still stop at a named
 * boundary instead of receiving an unsafe fake return. */
/* Explicit guest path resolution lives in rt_fs.c so its mount and traversal
 * rules can be exercised independently of the generated game image. */
static int rt_mode_writes(const char *mode) {
    return mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
}


void rt_shim(CPU *cpu, unsigned i) {
    const char *symbol = rt_shim_symbol(i);

    if (!strcmp(symbol, "objc_msgSend")) {
        rt_objc_msg_send(cpu, 0, 0);
        return;
    }
    if (!strcmp(symbol, "objc_msgSendSuper2")) {
        rt_objc_msg_send(cpu, 0, 1);
        return;
    }
    if (!strcmp(symbol, "objc_msgSend_stret")) {
        rt_objc_msg_send(cpu, 1, 0);
        return;
    }
    if (!strcmp(symbol, "objc_setProperty")) {
        RT_OBJC_OBJECT *object = rt_objc_object(cpu->r[0]);
        if (object)
            object->association = cpu->r[2];
        cpu->r[0] = cpu->r[2];
        return;
    }
    if (!strcmp(symbol, "objc_enumerationMutation")) {
        rt_stop_at_shim(cpu, symbol);
        return;
    }
    if (!strcmp(symbol, "UIApplicationMain")) {
        rt_objc_application_main(cpu);
        return;
    }
    if (!strcmp(symbol, "NSHomeDirectory") || !strcmp(symbol, "_NSHomeDirectory")) {
        cpu->r[0] = rt_objc_new_string("/")->address;
        return;
    }
    if (!strcmp(symbol, "NSTemporaryDirectory") || !strcmp(symbol, "_NSTemporaryDirectory")) {
        cpu->r[0] = rt_objc_new_string("/tmp")->address;
        return;
    }
    if (!strcmp(symbol, "NSSearchPathForDirectoriesInDomains")) {
        static const char *const guest_paths[] = {
            "/", "/Documents", "/Library", "/Library/Caches",
            "/Library/Application Support", "/tmp"
        };
        static uint32_t path_sets[sizeof guest_paths / sizeof guest_paths[0]];
        static uint32_t path_strings[sizeof guest_paths / sizeof guest_paths[0]];
        unsigned index = 0;
        switch (cpu->r[0]) {
        case 9: index = 1; break;  /* NSDocumentDirectory */
        case 5: index = 2; break;  /* NSLibraryDirectory */
        case 13: index = 3; break; /* NSCachesDirectory */
        case 14: index = 4; break; /* NSApplicationSupportDirectory */
        default: index = 0; break; /* app sandbox home */
        }
        if (!path_sets[index]) {
            path_strings[index] = rt_objc_new_string(guest_paths[index])->address;
            path_sets[index] = rt_objc_new_array(path_strings[index])->address;
        }
        cpu->r[0] = path_sets[index];
        return;
    }
    if (!strcmp(symbol, "AudioSessionInitialize") ||
        !strcmp(symbol, "AudioSessionSetActive")) {
        cpu->r[0] = 0;
        return;
    }
    if (!strncmp(symbol, "gl", 2)) {
        static uint32_t next_name = 1;
        if (!strncmp(symbol, "glGen", 5)) {
            uint32_t count = cpu->r[0], output = cpu->r[1], j;
            rt_check_guest_range(output, count * 4u);
            for (j = 0; j < count; j++)
                wr32(output + j * 4u, next_name++);
            cpu->r[0] = 0;
            return;
        }
        if (!strcmp(symbol, "glCheckFramebufferStatusOES")) {
            cpu->r[0] = 0x8CD5u; /* GL_FRAMEBUFFER_COMPLETE_OES */
            return;
        }
        if (!strcmp(symbol, "glGetIntegerv")) {
            /* The loader queries these limits before accepting a texture.
             * Returning the generic stub value zero makes every asset fail
             * with "Texture is too large" even though no host GLES context
             * has been needed yet.  Keep the values conservative and
             * deterministic; they describe the software GLES contract used by
             * the translated game, not a host pointer or a guessed texture. */
            uint32_t pname = cpu->r[0];
            uint32_t value = 0;
            unsigned count = 1;
            switch (pname) {
            case 0x0d33u: /* GL_MAX_TEXTURE_SIZE */
            case 0x8513u: /* GL_MAX_CUBE_MAP_TEXTURE_SIZE */
            case 0x84e8u: /* GL_MAX_RENDERBUFFER_SIZE_OES */
                value = 4096u;
                break;
            case 0x0d3au: /* GL_MAX_VIEWPORT_DIMS */
                value = 4096u;
                count = 2;
                break;
            case 0x84e2u: /* GL_MAX_TEXTURE_UNITS */
            case 0x8872u: /* GL_MAX_TEXTURE_IMAGE_UNITS */
            case 0x8b4du: /* GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */
                value = 8u;
                break;
            case 0x8869u: /* GL_MAX_VERTEX_ATTRIBS */
                value = 8u;
                break;
            case 0x8dfbu: /* GL_MAX_VERTEX_UNIFORM_VECTORS */
                value = 128u;
                break;
            case 0x8dfdu: /* GL_MAX_FRAGMENT_UNIFORM_VECTORS */
                value = 64u;
                break;
            case 0x0d56u: /* GL_DEPTH_BITS */
                value = 24u;
                break;
            case 0x0d57u: /* GL_STENCIL_BITS */
                value = 8u;
                break;
            default:
                break;
            }
            rt_check_guest_range(cpu->r[1], count * 4u);
            wr32(cpu->r[1], value);
            if (count > 1) wr32(cpu->r[1] + 4u, value);
            if (getenv("RADEK_TRACE_GL"))
                rt_log("glGetIntegerv pname=%04x value=%u count=%u",
                       pname, value, count);
            cpu->r[0] = 0;
            return;
        }
        if (!strcmp(symbol, "glGetRenderbufferParameterivOES")) {
            rt_check_guest_range(cpu->r[2], 4);
            wr32(cpu->r[2], 0);
            cpu->r[0] = 0;
            return;
        }
        cpu->r[0] = 0;
        return;
    }
    if (!strncmp(symbol, "al", 2) || !strncmp(symbol, "alc", 3)) {
        static uint32_t next_name = 1;
        if (!strcmp(symbol, "alcOpenDevice") ||
            !strcmp(symbol, "alcCreateContext")) {
            cpu->r[0] = rt_alloc_guest(4);
            return;
        }
        if (!strcmp(symbol, "alGenBuffers") || !strcmp(symbol, "alGenSources")) {
            uint32_t count = cpu->r[0], output = cpu->r[1], j;
            rt_check_guest_range(output, count * 4u);
            for (j = 0; j < count; j++)
                wr32(output + j * 4u, next_name++);
            cpu->r[0] = 0;
            return;
        }
        if (!strcmp(symbol, "alGetSourcei")) {
            rt_check_guest_range(cpu->r[2], 4);
            wr32(cpu->r[2], 0);
        }
        cpu->r[0] = 0;
        return;
    }
    if (!strcmp(symbol, "fopen") || !strcmp(symbol, "freopen")) {
        uint32_t path_address = rt_find_guest_cstring(cpu->r[0]);
        uint32_t mode_address = rt_find_guest_cstring(cpu->r[1]);
        const char *root = getenv("RADEK_GAME_DATA");
        char path[1024];
        FILE *host = NULL;
        if (path_address && mode_address) {
            const char *guest_path = (const char *)(MEMBASE + path_address);
            const char *mode = (const char *)(MEMBASE + mode_address);
            if (rt_guest_host_path(guest_path, mode, path, sizeof path))
                host = fopen(path, mode);
            else
                errno = EACCES;
            rt_log("guest fopen path=%s guest=%s mode=%s dataRoot=%s", host ? path : "<refused>",
                   guest_path, mode, root ? root : "");
            if (!host && !rt_mode_writes(mode) &&
                (!strcmp(guest_path, "data/bundleIndex.idx") ||
                 !strcmp(guest_path, "./data/bundleIndex.idx") ||
                 strstr(guest_path, "/data/bundleIndex.idx") != NULL) &&
                (!getenv("RADEK_DISABLE_SYNTHETIC_BUNDLE_INDEX") ||
                 strcmp(getenv("RADEK_DISABLE_SYNTHETIC_BUNDLE_INDEX"), "0"))) {
                /* The IPA fixture carries loose data files but no generated
                 * packed bundle index.  An empty seekable index makes the
                 * FileBundle parser represent "no packed entries" and lets
                 * its direct-file fallback continue without inventing a
                 * binary index format.  A host override is useful for
                 * validating a recovered index encoding without changing the
                 * guest path. */
                {
                    const char *override = getenv("RADEK_BUNDLE_INDEX_FILE");
                    host = override && *override ? fopen(override, mode) : tmpfile();
                    if (host && (!override || !*override)) {
                        static const char empty_index[] = "__loose_files__,0,0,0\n";
                        fwrite(empty_index, 1, sizeof empty_index - 1, host);
                        rewind(host);
                    }
                }
                if (host)
                    rt_log("guest fopen: using %s bundle index",
                           getenv("RADEK_BUNDLE_INDEX_FILE") ? "host override" : "loose-file synthetic");
            }
            if (!host && !rt_mode_writes(mode) &&
                (strstr(guest_path, "/highscores.lua") != NULL ||
                          !strcmp(guest_path, "highscores.lua") ||
                          !strcmp(guest_path, "./highscores.lua") ||
                          strstr(guest_path, "/settings.lua") != NULL ||
                          !strcmp(guest_path, "settings.lua") ||
                          !strcmp(guest_path, "./settings.lua")) &&
                (!getenv("RADEK_DISABLE_SYNTHETIC_USER_STATE") ||
                 strcmp(getenv("RADEK_DISABLE_SYNTHETIC_USER_STATE"), "0"))) {
                /* These are writable per-user files, not bundle assets.  A
                 * first launch has no file to read; model that state as an
                 * empty seekable file so the game's normal defaults path can
                 * create/populate it later. */
                host = tmpfile();
                if (host)
                    rt_log("guest fopen: using empty synthetic user state for %s", guest_path);
            }
        }
        if (!host) {
            cpu->r[0] = 0;
            return;
        }
        if (RT_NFILES >= RT_MAX_FILES)
            rt_fatal("translated FILE table exhausted");
        RT_FILES[RT_NFILES].token = rt_alloc_guest(4);
        RT_FILES[RT_NFILES].host = host;
        cpu->r[0] = RT_FILES[RT_NFILES++].token;
        return;
    }
    if (!strcmp(symbol, "tmpfile")) {
        if (RT_NFILES >= RT_MAX_FILES)
            rt_fatal("translated FILE table exhausted");
        RT_FILES[RT_NFILES].token = rt_alloc_guest(4);
        RT_FILES[RT_NFILES].host = tmpfile();
        cpu->r[0] = RT_FILES[RT_NFILES++].token;
        return;
    }
    if (!strcmp(symbol, "fclose") || !strcmp(symbol, "fflush") ||
        !strcmp(symbol, "feof") || !strcmp(symbol, "ferror") ||
        !strcmp(symbol, "fseek") || !strcmp(symbol, "ftell") ||
        !strcmp(symbol, "fread") || !strcmp(symbol, "fwrite") ||
        !strcmp(symbol, "fgets") || !strcmp(symbol, "fputs") ||
        !strcmp(symbol, "fputc") || !strcmp(symbol, "getc") ||
        !strcmp(symbol, "ungetc") || !strcmp(symbol, "clearerr")) {
        uint32_t file_token = cpu->r[0];
        if (!strcmp(symbol, "fread") || !strcmp(symbol, "fwrite"))
            file_token = cpu->r[3];
        else if (!strcmp(symbol, "fgets"))
            file_token = cpu->r[2];
        else if (!strcmp(symbol, "fputs") || !strcmp(symbol, "fputc") ||
                 !strcmp(symbol, "ungetc"))
            file_token = cpu->r[1];
        RT_FILE_HANDLE *file = rt_file_handle(file_token);
        if (!file || !file->host) {
            cpu->r[0] = 0;
            return;
        }
        if (!strcmp(symbol, "fclose")) cpu->r[0] = (uint32_t)fclose(file->host);
        else if (!strcmp(symbol, "fflush")) cpu->r[0] = (uint32_t)fflush(file->host);
        else if (!strcmp(symbol, "feof")) cpu->r[0] = (uint32_t)feof(file->host);
        else if (!strcmp(symbol, "ferror")) cpu->r[0] = (uint32_t)ferror(file->host);
        else if (!strcmp(symbol, "clearerr")) { clearerr(file->host); cpu->r[0] = 0; }
        else if (!strcmp(symbol, "fseek")) cpu->r[0] = (uint32_t)fseek(file->host, (long)(int32_t)cpu->r[1], (int)cpu->r[2]);
        else if (!strcmp(symbol, "ftell")) cpu->r[0] = (uint32_t)ftell(file->host);
        else if (!strcmp(symbol, "fread")) {
            uint32_t size = cpu->r[1], count = cpu->r[2];
            rt_check_guest_range(cpu->r[0], size > UINT32_MAX / (count ? count : 1) ? 0 : size * count);
            cpu->r[0] = (uint32_t)fread(MEMBASE + cpu->r[0], size, count, file->host);
        } else if (!strcmp(symbol, "fwrite")) {
            uint32_t size = cpu->r[1], count = cpu->r[2];
            rt_check_guest_range(cpu->r[0], size > UINT32_MAX / (count ? count : 1) ? 0 : size * count);
            cpu->r[0] = (uint32_t)fwrite(MEMBASE + cpu->r[0], size, count, file->host);
        } else if (!strcmp(symbol, "fgets")) {
            rt_check_guest_range(cpu->r[0], cpu->r[1] ? cpu->r[1] : 1);
            cpu->r[0] = fgets((char *)(MEMBASE + cpu->r[0]), (int)cpu->r[1], file->host)
                ? cpu->r[0] : 0;
        } else if (!strcmp(symbol, "fputs")) {
            cpu->r[0] = (uint32_t)fputs((const char *)(MEMBASE + cpu->r[0]), file->host);
        } else if (!strcmp(symbol, "fputc")) cpu->r[0] = (uint32_t)fputc((int)cpu->r[0], file->host);
        else if (!strcmp(symbol, "getc")) cpu->r[0] = (uint32_t)getc(file->host);
        else if (!strcmp(symbol, "ungetc")) cpu->r[0] = (uint32_t)ungetc((int)cpu->r[0], file->host);
        if (getenv("RADEK_TRACE_IO")) {
            if (!strcmp(symbol, "fread") || !strcmp(symbol, "fwrite"))
                rt_log("stdio %s token=%08x size=%u count=%u result=%08x",
                       symbol, file_token, cpu->r[1], cpu->r[2], cpu->r[0]);
            else if (!strcmp(symbol, "fseek"))
                rt_log("stdio fseek token=%08x off=%d whence=%u result=%08x",
                       file_token, (int32_t)cpu->r[1], cpu->r[2], cpu->r[0]);
            else if (!strcmp(symbol, "ftell"))
                rt_log("stdio ftell token=%08x result=%08x", file_token, cpu->r[0]);
        }
        return;
    }

    if (!strcmp(symbol, "vsprintf")) {
        RT_FORMAT_ARGS args;
        rt_format_args_from_va(cpu->r[2], &args);
        if (getenv("RADEK_TRACE_FORMAT")) {
            rt_log("vsprintf dst=%08x format=%08x va=%08x text=%s args=%08x,%08x,%08x,%08x,%08x,%08x arg0bytes=%02x%02x%02x%02x cstr=%08x",
                   cpu->r[0], cpu->r[1], cpu->r[2],
                   cpu->r[1] < RT_ARENA_SIZE ? (char *)(MEMBASE + cpu->r[1]) : "<bad>",
                   args.values[0], args.values[1], args.values[2], args.values[3],
                   args.values[4], args.values[5],
                   args.values[0] < RT_ARENA_SIZE ? MEMBASE[args.values[0]] : 0,
                   args.values[0] + 1 < RT_ARENA_SIZE ? MEMBASE[args.values[0] + 1] : 0,
                   args.values[0] + 2 < RT_ARENA_SIZE ? MEMBASE[args.values[0] + 2] : 0,
                   args.values[0] + 3 < RT_ARENA_SIZE ? MEMBASE[args.values[0] + 3] : 0,
                   rt_find_guest_cstring(args.values[0]));
            if (args.values[0] && args.values[0] + 32 < RT_ARENA_SIZE)
                rt_log("vsprintf arg0 words=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x ptr8=%08x ptr8text=%s",
                       rd32(args.values[0]), rd32(args.values[0] + 4),
                       rd32(args.values[0] + 8), rd32(args.values[0] + 12),
                       rd32(args.values[0] + 16), rd32(args.values[0] + 20),
                       rd32(args.values[0] + 24), rd32(args.values[0] + 28),
                       rd32(args.values[0] + 8),
                       rd32(args.values[0] + 8) < RT_ARENA_SIZE
                           ? (char *)(MEMBASE + rd32(args.values[0] + 8)) : "<bad>");
        }
        {
            uint32_t destination = cpu->r[0];
            cpu->r[0] = rt_guest_format(destination, RT_ARENA_SIZE - destination,
                                        cpu->r[1], &args);
            if (getenv("RADEK_TRACE_FORMAT"))
                rt_log("vsprintf result=%s", (char *)(MEMBASE + destination));
        }
        return;
    }
    if (!strcmp(symbol, "snprintf")) {
        RT_FORMAT_ARGS args;
        rt_format_args_from_regs(cpu, cpu->r[3], rd32(cpu->r[13]), &args);
        cpu->r[0] = rt_guest_format(cpu->r[0], cpu->r[1], cpu->r[2], &args);
        return;
    }
    if (!strcmp(symbol, "sprintf")) {
        RT_FORMAT_ARGS args;
        rt_format_args_from_regs(cpu, cpu->r[2], cpu->r[3], &args);
        cpu->r[0] = rt_guest_format(cpu->r[0], RT_ARENA_SIZE - cpu->r[0], cpu->r[1], &args);
        return;
    }
    if (!strcmp(symbol, "printf") || !strcmp(symbol, "fprintf")) {
        RT_FORMAT_ARGS args;
        uint32_t format = !strcmp(symbol, "printf") ? cpu->r[0] : cpu->r[1];
        uint32_t out = rt_alloc_guest(4096);
        args.count = args.next = 0;
        if (!strcmp(symbol, "printf")) {
            args.values[args.count++] = cpu->r[1];
            args.values[args.count++] = cpu->r[2];
            args.values[args.count++] = cpu->r[3];
        } else {
            args.values[args.count++] = cpu->r[2];
            args.values[args.count++] = cpu->r[3];
        }
        for (unsigned j = 0; j < 29 && args.count < 32; j++)
            args.values[args.count++] = rd32(cpu->r[13] + j * 4u);
        rt_guest_format(out, 4096, format, &args);
        if (!strcmp(symbol, "printf")) {
            if (getenv("RADEK_TRACE_FORMAT"))
                rt_log("printf format=%08x text=%s result=%s args=%08x,%08x,%08x",
                       format, format < RT_ARENA_SIZE ? (char *)(MEMBASE + format) : "<bad>",
                       (char *)(MEMBASE + out), args.values[0], args.values[1], args.values[2]);
            rt_log("guest printf: %s", (const char *)(MEMBASE + out));
        }
        else {
            RT_FILE_HANDLE *file = rt_file_handle(cpu->r[0]);
            if (file && file->host)
                fputs((const char *)(MEMBASE + out), file->host);
        }
        cpu->r[0] = (uint32_t)rt_guest_string_length(out);
        return;
    }
    if (!strcmp(symbol, "remove") || !strcmp(symbol, "rename")) {
        uint32_t old_address = rt_find_guest_cstring(cpu->r[0]);
        uint32_t new_address = !strcmp(symbol, "rename") ? rt_find_guest_cstring(cpu->r[1]) : 0;
        char old_path[1024], new_path[1024];
        const char *old_guest = old_address ? (const char *)(MEMBASE + old_address) : NULL;
        const char *new_guest = new_address ? (const char *)(MEMBASE + new_address) : NULL;
        if (!old_address || !rt_guest_host_path(old_guest, "w", old_path, sizeof old_path)) {
            cpu->r[0] = (uint32_t)-1;
            return;
        }
        if (!strcmp(symbol, "rename")) {
            if (!new_address || !rt_guest_host_path(new_guest, "w", new_path, sizeof new_path)) {
                cpu->r[0] = (uint32_t)-1;
                return;
            }
            cpu->r[0] = (uint32_t)rename(old_path, new_path);
        } else {
            cpu->r[0] = (uint32_t)remove(old_path);
        }
        return;
    }
    if (!strcmp(symbol, "strlen")) { cpu->r[0] = rt_guest_strlen(cpu); return; }
    if (!strcmp(symbol, "strcmp") || !strcmp(symbol, "strcoll")) {
        cpu->r[0] = (uint32_t)rt_guest_strcmp(cpu->r[0], cpu->r[1],
                                               UINT32_MAX, 0);
        return;
    }
    if (!strcmp(symbol, "strcasecmp")) {
        cpu->r[0] = (uint32_t)rt_guest_strcmp(cpu->r[0], cpu->r[1],
                                               UINT32_MAX, 1);
        return;
    }
    if (!strcmp(symbol, "strncmp")) {
        cpu->r[0] = (uint32_t)rt_guest_strcmp(cpu->r[0], cpu->r[1],
                                               cpu->r[2], 0);
        return;
    }
    if (!strcmp(symbol, "memcpy") || !strcmp(symbol, "memmove")) {
        cpu->r[0] = rt_guest_memcpy(cpu, !strcmp(symbol, "memmove"));
        return;
    }
    if (!strcmp(symbol, "memset")) {
        rt_check_guest_range(cpu->r[0], cpu->r[2]);
        memset(MEMBASE + cpu->r[0], cpu->r[1] & 0xffu, cpu->r[2]);
        return;
    }
    if (!strcmp(symbol, "memcmp")) {
        rt_check_guest_range(cpu->r[0], cpu->r[2]);
        rt_check_guest_range(cpu->r[1], cpu->r[2]);
        cpu->r[0] = (uint32_t)memcmp(MEMBASE + cpu->r[0], MEMBASE + cpu->r[1], cpu->r[2]);
        return;
    }
    if (!strcmp(symbol, "memchr")) {
        uint32_t base = cpu->r[0], n = cpu->r[2], j;
        rt_check_guest_range(base, n);
        cpu->r[0] = 0;
        for (j = 0; j < n; j++)
            if (MEMBASE[base + j] == (cpu->r[1] & 0xffu)) {
                cpu->r[0] = base + j;
                break;
            }
        return;
    }
    if (!strcmp(symbol, "strcpy")) {
        cpu->r[0] = rt_guest_copy_string(cpu, 0, 0); return;
    }
    if (!strcmp(symbol, "strcat")) {
        cpu->r[0] = rt_guest_copy_string(cpu, 1, 0); return;
    }
    if (!strcmp(symbol, "strncpy")) {
        cpu->r[0] = rt_guest_copy_string(cpu, 0, 1); return;
    }
    if (!strcmp(symbol, "strncat")) {
        cpu->r[0] = rt_guest_copy_string(cpu, 1, 1); return;
    }
    if (!strcmp(symbol, "strchr")) { cpu->r[0] = rt_guest_strchr(cpu, 0); return; }
    if (!strcmp(symbol, "strrchr")) { cpu->r[0] = rt_guest_strchr(cpu, 1); return; }
    if (!strcmp(symbol, "strcspn")) {
        uint32_t n = rt_guest_string_length(cpu->r[0]), j, k;
        uint32_t reject = cpu->r[1];
        uint32_t rn = rt_guest_string_length(reject);
        for (j = 0; j < n; j++) {
            for (k = 0; k < rn; k++)
                if (MEMBASE[cpu->r[0] + j] == MEMBASE[reject + k]) break;
            if (k < rn) break;
        }
        cpu->r[0] = j;
        return;
    }
    if (!strcmp(symbol, "strtok")) {
        static uint32_t cursor;
        uint32_t input = cpu->r[0] ? cpu->r[0] : cursor;
        uint32_t delimiters = cpu->r[1];
        uint32_t dn = rt_guest_string_length(delimiters);
        uint32_t start, j, k;
        if (!input) { cpu->r[0] = 0; return; }
        start = input;
        while (MEMBASE[start]) {
            for (k = 0; k < dn; k++)
                if (MEMBASE[start] == MEMBASE[delimiters + k]) break;
            if (k == dn) break;
            start++;
        }
        if (!MEMBASE[start]) { cursor = 0; cpu->r[0] = 0; return; }
        for (j = start; MEMBASE[j]; j++) {
            for (k = 0; k < dn; k++)
                if (MEMBASE[start + (j - start)] == MEMBASE[delimiters + k]) break;
            if (k < dn) {
                MEMBASE[j] = 0;
                cursor = j + 1;
                cpu->r[0] = start;
                if (getenv("RADEK_TRACE_INDEX"))
                    rt_log("guest strtok input=%08x delim=%s result=%08x text=%s next=%08x",
                           input, (char *)(MEMBASE + delimiters), start,
                           (char *)(MEMBASE + start), cursor);
                return;
            }
        }
        cursor = 0;
        cpu->r[0] = start;
        if (getenv("RADEK_TRACE_INDEX"))
            rt_log("guest strtok input=%08x delim=%s result=%08x text=%s next=0",
                   input, (char *)(MEMBASE + delimiters), start,
                   (char *)(MEMBASE + start));
        return;
    }
    if (!strcmp(symbol, "strpbrk")) {
        uint32_t base = cpu->r[0], reject = cpu->r[1];
        uint32_t n = rt_guest_string_length(base), rn = rt_guest_string_length(reject);
        uint32_t j, k;
        cpu->r[0] = 0;
        for (j = 0; j < n; j++) {
            for (k = 0; k < rn; k++)
                if (MEMBASE[base + j] == MEMBASE[reject + k]) break;
            if (k < rn) { cpu->r[0] = base + j; break; }
        }
        return;
    }
    if (!strcmp(symbol, "strtod")) {
        char *end;
        uint32_t input = cpu->r[0], end_slot = cpu->r[1];
        double value = strtod((const char *)(MEMBASE + input), &end);
        rt_abi_put_d(cpu, 0, d2u(value));
        if (end_slot) wr32(end_slot, input + (uint32_t)(end - (char *)(MEMBASE + input)));
        return;
    }
    if (!strcmp(symbol, "strtol") || !strcmp(symbol, "strtoul")) {
        char *end;
        uint32_t input = cpu->r[0], end_slot = cpu->r[1], base = cpu->r[2];
        unsigned long value = !strcmp(symbol, "strtol")
            ? (unsigned long)strtol((const char *)(MEMBASE + input), &end, (int)base)
            : strtoul((const char *)(MEMBASE + input), &end, (int)base);
        cpu->r[0] = (uint32_t)value;
        if (end_slot) wr32(end_slot, input + (uint32_t)(end - (char *)(MEMBASE + input)));
        return;
    }
    if (!strcmp(symbol, "getenv")) {
        char *name = (char *)(MEMBASE + cpu->r[0]);
        const char *value = getenv(name);
        if (!value) { cpu->r[0] = 0; return; }
        uint32_t n = (uint32_t)strlen(value), out = rt_alloc_guest(n + 1);
        memcpy(MEMBASE + out, value, n + 1);
        cpu->r[0] = out;
        return;
    }
    if (!strcmp(symbol, "strerror")) {
        const char *value = strerror((int)cpu->r[0]);
        uint32_t n = (uint32_t)strlen(value), out = rt_alloc_guest(n + 1);
        memcpy(MEMBASE + out, value, n + 1);
        cpu->r[0] = out;
        return;
    }
    if (!strcmp(symbol, "rand") || !strcmp(symbol, "srand")) {
        static uint32_t random_state = 1;
        if (!strcmp(symbol, "srand")) random_state = cpu->r[0];
        else {
            random_state = random_state * 1103515245u + 12345u;
            cpu->r[0] = (random_state >> 1) & 0x7fffffffu;
        }
        return;
    }
    if (!strcmp(symbol, "error")) {
        static uint32_t errno_cell;
        if (!errno_cell) errno_cell = rt_alloc_guest(4);
        cpu->r[0] = errno_cell;
        return;
    }
    if (!strcmp(symbol, "gettimeofday")) {
        struct timeval { long tv_sec; long tv_usec; } now;
        time_t seconds = time(NULL);
        now.tv_sec = (long)seconds;
        now.tv_usec = 0;
        if (cpu->r[0]) {
            rt_check_guest_range(cpu->r[0], 8);
            wr32(cpu->r[0], (uint32_t)now.tv_sec);
            wr32(cpu->r[0] + 4, (uint32_t)now.tv_usec);
        }
        if (cpu->r[1]) { rt_check_guest_range(cpu->r[1], 8); memset(MEMBASE + cpu->r[1], 0, 8); }
        cpu->r[0] = 0;
        return;
    }
    if (!strcmp(symbol, "time")) {
        time_t value = time(NULL);
        if (cpu->r[0]) { rt_check_guest_range(cpu->r[0], 4); wr32(cpu->r[0], (uint32_t)value); }
        cpu->r[0] = (uint32_t)value;
        return;
    }
    if (!strcmp(symbol, "clock")) { cpu->r[0] = (uint32_t)clock(); return; }
    if (!strcmp(symbol, "usleep") || !strcmp(symbol, "sched_yield")) { cpu->r[0] = 0; return; }
    if (!strcmp(symbol, "setlocale")) {
        static uint32_t locale;
        if (!locale) { locale = rt_alloc_guest(2); MEMBASE[locale] = 'C'; MEMBASE[locale + 1] = 0; }
        cpu->r[0] = locale;
        return;
    }
    if (!strcmp(symbol, "localeconv")) {
        static uint32_t locale_info;
        if (!locale_info) locale_info = rt_alloc_guest(64);
        cpu->r[0] = locale_info;
        return;
    }
    if (!strcmp(symbol, "pthread_create")) {
        /* The portable CPU model has one guest address space and its SJLJ
         * bookkeeping is deliberately process-local.  Run this bounded
         * worker synchronously instead of creating a host thread that could
         * race the translated stack/exception state; pthread_join then
         * observes the completed result.  This is enough for the game's
         * loader/decode workers and preserves the guest-callable ABI. */
        uint32_t output = cpu->r[0];
        uint32_t entry = cpu->r[2];
        unsigned slot;
        if (!output || !entry || RT_NGUEST_THREADS >= 128u) {
            cpu->r[0] = 22u; /* EINVAL */
            return;
        }
        rt_check_guest_range(output, 4);
        slot = RT_NGUEST_THREADS++;
        RT_GUEST_THREADS[slot].token = rt_alloc_guest(4);
        RT_GUEST_THREADS[slot].result = 0;
        RT_GUEST_THREADS[slot].done = 0;
        wr32(output, RT_GUEST_THREADS[slot].token);
        {
            CPU worker;
            uint32_t stack = rt_alloc_guest(0x20000u);
            memset(&worker, 0, sizeof worker);
            worker.r[0] = cpu->r[3]; /* start_routine(arg) */
            worker.r[13] = stack + 0x20000u;
            worker.r[14] = RT_RETURN_MARKER;
            worker.cpsr = 0x10;
            tdispatch(&worker, entry);
            RT_GUEST_THREADS[slot].result = worker.r[0];
            RT_GUEST_THREADS[slot].done = 1;
        }
        cpu->r[0] = 0;
        return;
    }
    if (!strcmp(symbol, "pthread_join")) {
        /* See pthread_create above: all guest workers are already complete. */
        RT_GUEST_THREAD *thread = rt_guest_thread(cpu->r[0]);
        if (!thread)
            cpu->r[0] = 3u; /* ESRCH */
        else {
            if (cpu->r[1]) {
                rt_check_guest_range(cpu->r[1], 4);
                wr32(cpu->r[1], thread->result);
            }
            cpu->r[0] = thread->done ? 0u : 16u; /* EBUSY if incomplete */
        }
        return;
    }
    if (!strcmp(symbol, "pthread_exit") ||
        !strcmp(symbol, "pthread_getschedparam") ||
        !strcmp(symbol, "pthread_setschedparam")) {
        if (!strcmp(symbol, "pthread_getschedparam")) {
            if (cpu->r[1]) { rt_check_guest_range(cpu->r[1], 4); wr32(cpu->r[1], 0); }
            if (cpu->r[2]) { rt_check_guest_range(cpu->r[2], 4); wr32(cpu->r[2], 0); }
        }
        cpu->r[0] = 0;
        return;
    }
    if (!strcmp(symbol, "pthread_mutex_init") || !strcmp(symbol, "pthread_mutex_destroy") ||
        !strcmp(symbol, "pthread_mutex_lock") || !strcmp(symbol, "pthread_mutex_trylock") ||
        !strcmp(symbol, "pthread_mutex_unlock") || !strcmp(symbol, "pthread_mutexattr_init") ||
        !strcmp(symbol, "pthread_mutexattr_destroy") || !strcmp(symbol, "pthread_mutexattr_settype")) {
        if (!strcmp(symbol, "pthread_mutex_init") || !strcmp(symbol, "pthread_mutexattr_init")) {
            rt_check_guest_range(cpu->r[0], 4); wr32(cpu->r[0], 0);
        }
        cpu->r[0] = 0;
        return;
    }
    if (!strcmp(symbol, "tolower") || !strcmp(symbol, "toupper") ||
        !strcmp(symbol, "maskrune")) {
        unsigned char c = (unsigned char)cpu->r[0];
        if (!strcmp(symbol, "tolower") && c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (!strcmp(symbol, "toupper") && c >= 'a' && c <= 'z') c -= 'a' - 'A';
        cpu->r[0] = c;
        return;
    }

    if (!strcmp(symbol, "acosf")) { cpu->r[0] = rt_f32_unary(cpu, acosf); return; }
    if (!strcmp(symbol, "asinf")) { cpu->r[0] = rt_f32_unary(cpu, asinf); return; }
    if (!strcmp(symbol, "atanf")) { cpu->r[0] = rt_f32_unary(cpu, atanf); return; }
    if (!strcmp(symbol, "ceilf")) { cpu->r[0] = rt_f32_unary(cpu, ceilf); return; }
    if (!strcmp(symbol, "cosf")) { cpu->r[0] = rt_f32_unary(cpu, cosf); return; }
    if (!strcmp(symbol, "expf")) { cpu->r[0] = rt_f32_unary(cpu, expf); return; }
    if (!strcmp(symbol, "floorf")) { cpu->r[0] = rt_f32_unary(cpu, floorf); return; }
    if (!strcmp(symbol, "log10f")) { cpu->r[0] = rt_f32_unary(cpu, log10f); return; }
    if (!strcmp(symbol, "logf")) { cpu->r[0] = rt_f32_unary(cpu, logf); return; }
    if (!strcmp(symbol, "sinf")) { cpu->r[0] = rt_f32_unary(cpu, sinf); return; }
    if (!strcmp(symbol, "sinhf")) { cpu->r[0] = rt_f32_unary(cpu, sinhf); return; }
    if (!strcmp(symbol, "tanf")) { cpu->r[0] = rt_f32_unary(cpu, tanf); return; }
    if (!strcmp(symbol, "tanhf")) { cpu->r[0] = rt_f32_unary(cpu, tanhf); return; }
    if (!strcmp(symbol, "atan2f")) { cpu->r[0] = rt_f32_binary(cpu, atan2f); return; }
    if (!strcmp(symbol, "cos") || !strcmp(symbol, "sin") ||
        !strcmp(symbol, "atan2") || !strcmp(symbol, "fmod") ||
        !strcmp(symbol, "pow")) {
        double a = u2d(rt_abi_get_d(cpu, 0));
        double result;
        if (!strcmp(symbol, "cos")) result = cos(a);
        else if (!strcmp(symbol, "sin")) result = sin(a);
        else if (!strcmp(symbol, "atan2")) result = atan2(a, u2d(rt_abi_get_d(cpu, 2)));
        else if (!strcmp(symbol, "fmod")) result = fmod(a, u2d(rt_abi_get_d(cpu, 2)));
        else result = pow(a, u2d(rt_abi_get_d(cpu, 2)));
        rt_abi_put_d(cpu, 0, d2u(result));
        return;
    }
    if (!strcmp(symbol, "ldexp")) {
        rt_abi_put_d(cpu, 0, d2u(ldexp(u2d(rt_abi_get_d(cpu, 0)), (int32_t)cpu->r[2])));
        return;
    }
    if (!strcmp(symbol, "frexp") || !strcmp(symbol, "modf")) {
        double value = u2d(rt_abi_get_d(cpu, 0));
        uint32_t out = cpu->r[2];
        double result;
        rt_check_guest_range(out, sizeof(double));
        if (!strcmp(symbol, "frexp")) {
            int exponent = 0;
            result = frexp(value, &exponent);
            wr32(out, (uint32_t)exponent);
        } else {
            double integer_part;
            result = modf(value, &integer_part);
            wr64(out, d2u(integer_part));
        }
        rt_abi_put_d(cpu, 0, d2u(result));
        return;
    }
    if (!strcmp(symbol, "floatdidf")) {
        int64_t value = (int64_t)rt_abi_get_d(cpu, 0);
        rt_abi_put_d(cpu, 0, d2u((double)value));
        return;
    }
    if (!strcmp(symbol, "floatdisf")) {
        int64_t value = (int64_t)rt_abi_get_d(cpu, 0);
        cpu->r[0] = f2u((float)value);
        return;
    }
    if (!strcmp(symbol, "fixdfdi")) {
        double value = u2d(rt_abi_get_d(cpu, 0));
        int64_t result = isnan(value) ? 0 : (int64_t)value;
        rt_abi_put_d(cpu, 0, (uint64_t)result);
        return;
    }

    if (!strcmp(symbol, "cxa_atexit") || !strcmp(symbol, "cxa_finalize") ||
        !strcmp(symbol, "cxa_end_catch") || !strcmp(symbol, "cxa_free_exception")) {
        cpu->r[0] = 0;
        return;
    }
    if (!strcmp(symbol, "cxa_begin_catch"))
        return;
    if (!strcmp(symbol, "cxa_allocate_exception")) {
        cpu->r[0] = rt_alloc_guest(cpu->r[0]);
        return;
    }
    if (!strcmp(symbol, "cxa_guard_acquire")) {
        uint32_t value = rd32(cpu->r[0]);
        if (value == 0) {
            wr32(cpu->r[0], 1);
            cpu->r[0] = 1;
        } else {
            cpu->r[0] = 0;
        }
        return;
    }
    if (!strcmp(symbol, "cxa_guard_release")) {
        wr32(cpu->r[0], 2);
        cpu->r[0] = 0;
        return;
    }
    if (!strcmp(symbol, "cxa_guard_abort")) {
        wr32(cpu->r[0], 0);
        cpu->r[0] = 0;
        return;
    }
    if (!strcmp(symbol, "cxa_demangle")) {
        cpu->r[0] = 0;
        return;
    }
    if (!strcmp(symbol, "Unwind_SjLj_Register")) {
        rt_sjlj_register(cpu->r[0]);
        cpu->r[0] = 0;
        return;
    }
    if (!strcmp(symbol, "Unwind_SjLj_Unregister")) {
        rt_sjlj_unregister(cpu->r[0]);
        cpu->r[0] = 0;
        return;
    }
    if (!strcmp(symbol, "cxa_throw")) {
        RT_SJLJ_RECORD handler;
        unsigned landing, action, selector;
        if (rt_sjlj_handler(cpu->r[1], &handler, &landing, &action,
                            &selector) && handler.dispatch &&
            handler.saved_sp < RT_STACK_TOP) {
            (void)action;
            /* The SJLJ personality returns the exception and selector in
             * data[0:1], replaces call_site with the compact landing-pad
             * index, and resumes through the setjmp dispatch label. */
            wr32(handler.context + 4, landing);
            wr32(handler.context + 8, cpu->r[0]);
            wr32(handler.context + 12, selector);
            /* GCC's ARM SJLJ setjmp buffer is the three-word tuple
             * {hard-frame-pointer, dispatch-label, saved-stack-pointer}.
             * Restoring only SP enters the landing pad with a stale frame
             * pointer; nested String/Format handlers then walk unrelated
             * guest stack objects and eventually corrupt SJLJ contexts. */
            cpu->r[7] = rd32(handler.context + 32);
            cpu->r[13] = handler.saved_sp;
            cpu->r[14] = RT_RETURN_MARKER;
            if (rt_trace_sjlj())
                rt_log("SJLJ resume dispatch=%08x sp=%08x", handler.dispatch,
                       handler.saved_sp);
            tdispatch(cpu, handler.dispatch);
            return;
        }
        if (rt_trace_sjlj()) {
            if (RT_SJLJ_TOP) rt_sjlj_dump(RT_SJLJ_TOP);
            rt_sjlj_dump_chain();
        }
        rt_stop_at_shim(cpu, symbol);
        return;
    }
    if (!strcmp(symbol, "setjmp") || !strcmp(symbol, "_setjmp")) {
        /* Valid libpng paths only test the zero return.  Keep the guest
         * buffer initialized for diagnostics; a non-local transfer from a
         * malformed asset remains a bounded stop rather than jumping into
         * an arbitrary translated C label. */
        /* The translated error paths are bounded below; do not guess at
         * the platform-specific opaque jmp_buf layout or overwrite the
         * surrounding libpng state. */
        cpu->r[0] = getenv("RADEK_FORCE_SETJMP_ERROR") ? 1u : 0u;
        return;
    }
    if (!strcmp(symbol, "longjmp") || !strcmp(symbol, "_longjmp")) {
        rt_stop_at_shim(cpu, symbol);
        return;
    }
    if (!strcmp(symbol, "cxa_rethrow") || !strcmp(symbol, "cxa_pure_virtual") ||
        !strcmp(symbol, "Unwind_SjLj_Resume") ||
        !strcmp(symbol, "ZSt9terminatev")) {
        rt_stop_at_shim(cpu, symbol);
        return;
    }

    if (!strcmp(symbol, "modsi3") || !strcmp(symbol, "divsi3") ||
        !strcmp(symbol, "umodsi3") || !strcmp(symbol, "udivsi3")) {
        uint32_t a = cpu->r[0], b = cpu->r[1];
        if (b == 0)
            rt_stop_at_shim(cpu, symbol);
        else if (!strcmp(symbol, "modsi3"))
            cpu->r[0] = (uint32_t)((int32_t)a % (int32_t)b);
        else if (!strcmp(symbol, "divsi3"))
            cpu->r[0] = (uint32_t)((int32_t)a / (int32_t)b);
        else if (!strcmp(symbol, "umodsi3"))
            cpu->r[0] = a % b;
        else
            cpu->r[0] = a / b;
        return;
    }
    if (!strcmp(symbol, "moddi3") || !strcmp(symbol, "divdi3")) {
        uint64_t a = (uint64_t)cpu->r[0] | ((uint64_t)cpu->r[1] << 32);
        uint64_t b = (uint64_t)cpu->r[2] | ((uint64_t)cpu->r[3] << 32);
        if (b == 0)
            rt_stop_at_shim(cpu, symbol);
        else {
            uint64_t result = !strcmp(symbol, "moddi3")
                ? (uint64_t)((int64_t)a % (int64_t)b)
                : (uint64_t)((int64_t)a / (int64_t)b);
            cpu->r[0] = (uint32_t)result;
            cpu->r[1] = (uint32_t)(result >> 32);
        }
        return;
    }
    if (!strcmp(symbol, "Znwm") || !strcmp(symbol, "Znam") ||
        !strcmp(symbol, "malloc") || !strcmp(symbol, "calloc")) {
        uint32_t bytes = cpu->r[0];
        if (!strcmp(symbol, "calloc"))
            bytes = bytes > UINT32_MAX / cpu->r[1] ? UINT32_MAX : bytes * cpu->r[1];
        cpu->r[0] = rt_alloc_guest(bytes);
        return;
    }
    if (!strcmp(symbol, "realloc")) {
        uint32_t old = cpu->r[0], bytes = cpu->r[1];
        uint32_t replacement = rt_alloc_guest(bytes);
        uint32_t old_size = rt_alloc_size(old);
        uint32_t copy = old_size < bytes ? old_size : bytes;
        if (old && copy)
            memcpy(MEMBASE + replacement, MEMBASE + old, copy);
        cpu->r[0] = replacement;
        return;
    }
    if (!strcmp(symbol, "ZdlPv") || !strcmp(symbol, "ZdaPv") ||
        !strcmp(symbol, "free")) {
        cpu->r[0] = 0;
        return;
    }

    rt_stop_at_shim(cpu, symbol);
}

void rt_init(const char *mem_path) {
    MEMBASE = (uint8_t *)calloc(1, RT_ARENA_SIZE);
    if (!MEMBASE)
        rt_fatal("arena calloc(%u) failed", RT_ARENA_SIZE);
    FILE *f = fopen(mem_path, "rb");
    if (!f)
        rt_fatal("cannot open mem blob %s", mem_path);
    fseek(f, 0, SEEK_END);
    long blob_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *blob = (uint8_t *)malloc(blob_len > 0 ? (size_t)blob_len : 1);
    if (blob_len > 0 && fread(blob, 1, (size_t)blob_len, f) != (size_t)blob_len)
        rt_fatal("short read on %s", mem_path);
    fclose(f);
    for (unsigned i = 0; i < RT_NREGIONS; i++) {
        const RT_REGION *r = &RT_REGIONS[i];
        if (r->addr + r->len < r->addr || r->addr + r->len > RT_ARENA_SIZE)
            rt_fatal("region %u out of arena (%08x+%08x)", i, r->addr, r->len);
        if (r->zero)
            continue; /* calloc already zeroed bss */
        if (r->blob + r->len > (uint32_t)blob_len)
            rt_fatal("region %u outside blob", i);
        memcpy(MEMBASE + r->addr, blob + r->blob, r->len);
    }
    free(blob);
    rt_bind_loader();
    rt_log("rt_init: %u regions loaded, %u extrel processed, %u nlsym, %u lasym, %u LSDA tables",
           RT_NREGIONS, RT_NEXTREL, RT_NNLSYM, RT_NLASYM, RT_NLSDA_TABLES);
}

void rt_run_modinits(CPU *cpu) {
    for (unsigned i = 0; i < RT_NMODINITS; i++) {
        rt_log("modinit %u/%u @%08x ...", i, RT_NMODINITS, RT_MODINIT_ADDRS[i]);
        cpu->r[14] = RT_RETURN_MARKER;
        RT_MODINITS[i](cpu);
        RT_MODINITS_DONE = i + 1;
        rt_log("modinit %u done (r0=%08x sp=%08x)", i, cpu->r[0], cpu->r[13]);
    }
}

static uint32_t rt_bridge_alloc(uint32_t n) {
    static uint32_t brk = RT_BRIDGE_BASE;
    uint32_t a = (brk + 3) & ~3u;
    if (a + n < a || a + n > RT_BRIDGE_BASE + RT_BRIDGE_SIZE)
        rt_fatal("bridge carve exhausted");
    brk = a + n;
    return a;
}

void rt_call_main(CPU *cpu) {
    static const char argv0[] = "AngryBirds";
    uint32_t s = rt_bridge_alloc((uint32_t)sizeof argv0);
    memcpy(MEMBASE + s, argv0, sizeof argv0);
    uint32_t av = rt_bridge_alloc(8);
    wr32(av, s);
    wr32(av + 4, 0);
    memset(cpu, 0, sizeof *cpu);
    cpu->r[0] = 1;
    cpu->r[1] = av;
    cpu->r[13] = RT_STACK_TOP;
    cpu->r[14] = RT_RETURN_MARKER;
    cpu->cpsr = 0x10;
    RT_MAIN_REACHED = 1;
    rt_log("calling main @%08x ...", RT_MAIN_ADDR);
    RT_MAIN(cpu);
    rt_log("main returned r0=%08x", cpu->r[0]);
}
