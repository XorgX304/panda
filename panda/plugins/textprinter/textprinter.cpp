/* PANDABEGINCOMMENT
 * 
 * Authors:
 *  Tim Leek               tleek@ll.mit.edu
 *  Ryan Whelan            rwhelan@ll.mit.edu
 *  Joshua Hodosh          josh.hodosh@ll.mit.edu
 *  Michael Zhivich        mzhivich@ll.mit.edu
 *  Brendan Dolan-Gavitt   brendandg@gatech.edu
 * 
 * This work is licensed under the terms of the GNU GPL, version 2. 
 * See the COPYING file in the top-level directory. 
 * 
PANDAENDCOMMENT */
// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#define __STDC_FORMAT_MACROS

//extern "C" {

//#include "config.h"
//#include "qemu-common.h"
//#include "monitor.h"
//#include "cpu.h"
//#include "disas.h"

//#include "panda_plugin.h"

//}

#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <set>
#include <iostream>
#include <fstream>

#include "panda/plugin.h"

#include "../callstack_instr/callstack_instr.h"
#include "../callstack_instr/callstack_instr_ext.h"

// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {
bool init_plugin(void *);
void uninit_plugin(void *);
void read_mem_callback(CPUState *env, target_ulong pc, target_ulong addr, size_t size, uint8_t *buf);
void write_mem_callback(CPUState *env, target_ulong pc, target_ulong addr, size_t size, uint8_t *buf);
}

uint64_t mem_counter;

std::set<prog_point> tap_points;
gzFile read_tap_buffers;
gzFile write_tap_buffers;

void mem_callback(CPUState *env, target_ulong pc, target_ulong addr,
                  size_t size, uint8_t *buf, gzFile f) {
    prog_point p = {};
    get_prog_point(env, &p);

    if (tap_points.find(p) != tap_points.end()) {
        target_ulong callers[16] = {0};
        int nret = get_callers(callers, 16, env);
        unsigned char *buf_uc = static_cast<unsigned char *>(buf);
        for (unsigned int i = 0; i < size; i++) {
            for (int j = nret-1; j > 0; j--) {
                gzprintf(f, TARGET_FMT_lx " ", callers[j]);
            }
            gzprintf(f,
                     TARGET_FMT_lx " " TARGET_FMT_lx " %d " TARGET_FMT_lx
                                   " " TARGET_FMT_lx " %s " TARGET_FMT_lx
                                   " %ld %02x\n",
                     p.caller, p.pc, p.stackKind, p.sidFirst, p.sidSecond,
                     p.isKernelMode ? "kernel" : "user", addr + i, mem_counter,
                     buf_uc[i]);
        }
    }
    mem_counter++;

    return;
}

void mem_read_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       size_t size, uint8_t *buf) {
    mem_callback(env, pc, addr, size, buf, read_tap_buffers);
    return;
}

void mem_write_callback(CPUState *env, target_ulong pc, target_ulong addr,
                        size_t size, uint8_t *buf) {
    mem_callback(env, pc, addr, size, buf, write_tap_buffers);
    return;
}

bool init_plugin(void *self) {
    panda_cb pcb;

    printf("Initializing plugin textprinter\n");
    
    std::ifstream taps("tap_points.txt");
    if (!taps) {
        printf("Couldn't open tap_points.txt; no tap points defined. Exiting.\n");
        return false;
    }

    panda_require("callstack_instr");
    if(!init_callstack_instr_api()) return false;

    uint32_t stack_kind;
    taps >> stack_kind;
    printf("Tap points are all of stack type ");
    if (STACK_ASID == stack_kind) {
        printf("asid");
    } else if (STACK_HEURISTIC == stack_kind) {
        printf("heuristic");
    } else if (STACK_THREADED == stack_kind) {
        printf("threaded");
    } else {
        printf("UNKNOWN");
    }
    printf(" (%d)\n", stack_kind);

    prog_point p = {};
    while (taps >> std::hex >> p.caller) {
        taps >> std::hex >> p.pc;
        taps >> std::hex >> p.sidFirst;
        if (STACK_ASID == stack_kind) {
            p.sidSecond = 0;
        } else {
            taps >> std::hex >> p.sidSecond;
        }
        if (STACK_THREADED == stack_kind) {
            taps >> std::hex >> p.isKernelMode;
        }

        p.stackKind = static_cast<stack_type>(stack_kind);

        char *sid_string = get_stackid_string(p);
        printf("Adding tap point (" TARGET_FMT_lx "," TARGET_FMT_lx ", %s)\n",
               p.caller, p.pc, sid_string);
        tap_points.insert(p);
        g_free(sid_string);
    }
    taps.close();

    write_tap_buffers = gzopen("write_tap_buffers.txt.gz", "w");
    if(!write_tap_buffers) {
        printf("Couldn't open write_tap_buffers.txt for writing. Exiting.\n");
        return false;
    }
    read_tap_buffers = gzopen("read_tap_buffers.txt.gz", "w");
    if(!read_tap_buffers) {
        printf("Couldn't open read_tap_buffers.txt for writing. Exiting.\n");
        return false;
    }

    panda_enable_precise_pc();
    panda_enable_memcb();    

    pcb.virt_mem_after_read = mem_read_callback;
    panda_register_callback(self, PANDA_CB_VIRT_MEM_AFTER_READ, pcb);
    pcb.virt_mem_after_write = mem_write_callback;
    panda_register_callback(self, PANDA_CB_VIRT_MEM_AFTER_WRITE, pcb);

    return true;
}

void uninit_plugin(void *self) {
    gzclose(read_tap_buffers);
    gzclose(write_tap_buffers);
}
