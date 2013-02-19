// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#define __STDC_FORMAT_MACROS

extern "C" {

#include "config.h"
#include "qemu-common.h"
#include "monitor.h"
#include "cpu.h"
#include "disas.h"

#include "panda_plugin.h"

}

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <set>
#include <iostream>
#include <fstream>

// These need to be extern "C" so that the ABI is compatible with
// QEMU/PANDA, which is written in C
extern "C" {

bool init_plugin(void *);
void uninit_plugin(void *);
int mem_callback(CPUState *env, target_ulong pc, target_ulong addr, target_ulong size, void *buf);

}

uint64_t mem_counter;

struct prog_point {
    target_ulong caller;
    target_ulong pc;
    target_ulong cr3;
    bool operator <(const prog_point &p) const {
        return (this->pc < p.pc) || \
               (this->pc == p.pc && this->caller < p.caller) || \
               (this->pc == p.pc && this->caller == p.caller && this->cr3 < p.cr3);
    }
};


std::set<prog_point> tap_points;
FILE *tap_buffers;

#ifdef TARGET_ARM
// ARM: stolen from target-arm/helper.c
static uint32_t arm_get_vaddr_table(CPUState *env, uint32_t address)
{   
    uint32_t table;

    if (address & env->cp15.c2_mask)
        table = env->cp15.c2_base1 & 0xffffc000;
    else
        table = env->cp15.c2_base0 & env->cp15.c2_base_mask;

    return table;
}
#endif

int mem_callback(CPUState *env, target_ulong pc, target_ulong addr,
                       target_ulong size, void *buf) {
    prog_point p = {};

    // Caller
#ifdef TARGET_I386
    panda_virtual_memory_rw(env, env->regs[R_EBP]+4, (uint8_t *)&p.caller, 4, 0);
#endif

    // ASID
#if defined(TARGET_I386)
    if((env->hflags & HF_CPL_MASK) != 0) // Lump all kernel-mode CR3s together
        p.cr3 = env->cr[3];
#elif defined(TARGET_ARM)
    p.cr3 = arm_get_vaddr_table(env, addr);
#endif

    p.pc = pc;

    // XXX disable CR3 check
    //p.cr3 = 0;
    //p.caller = 0;

    if (tap_points.find(p) != tap_points.end()) {
        for (unsigned int i = 0; i < size; i++) {
            fprintf(tap_buffers, TARGET_FMT_lx " " TARGET_FMT_lx " " TARGET_FMT_lx " " TARGET_FMT_lx " %ld %02x\n",
                    p.caller, p.pc, p.cr3, addr+i, mem_counter, ((unsigned char *)buf)[i]);
        }
    }
    mem_counter++;

    return 1;
}

bool init_plugin(void *self) {
    panda_cb pcb;

    printf("Initializing plugin textprinter\n");
    
    panda_enable_precise_pc();
    panda_enable_memcb();    
    pcb.virt_mem_write = mem_callback;
    panda_register_callback(self, PANDA_CB_VIRT_MEM_WRITE, pcb);

    std::ifstream taps("tap_points.txt");
    if (!taps) {
        printf("Couldn't open tap_points.txt; no tap points defined. Exiting.\n");
        return false;
    }

    prog_point p = {};
    while (taps >> std::hex >> p.caller) {
        taps >> std::hex >> p.pc;
        taps >> std::hex >> p.cr3;

        // XXX disable CR3 check
        //p.cr3 = 0;
        //p.caller = 0;

        printf("Adding tap point (" TARGET_FMT_lx "," TARGET_FMT_lx "," TARGET_FMT_lx ")\n",
               p.caller, p.pc, p.cr3);
        tap_points.insert(p);
    }
    taps.close();

    tap_buffers = fopen("tap_buffers.txt", "w");
    if(!tap_buffers) {
        printf("Couldn't open tap_buffers.txt for writing. Exiting.\n");
        return false;
    }
    setbuf(tap_buffers, NULL);

    return true;
}

void uninit_plugin(void *self) {
    fclose(tap_buffers);
}
