/*
 * RISC-V Vector Extension Helpers for QEMU.
 *
 * Copyright (c) 2020 T-Head Semiconductor Co., Ltd. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/memop.h"
#include "exec/exec-all.h"
#include "exec/helper-proto.h"
#include "fpu/softfloat.h"
#include "tcg/tcg-gvec-desc.h"
#include "internals.h"
#include <math.h>

target_ulong HELPER(vsetvl)(CPURISCVState *env, target_ulong s1,
                            target_ulong s2)
{
    int vlmax, vl;
    RISCVCPU *cpu = env_archcpu(env);
    uint64_t lmul = FIELD_EX64(s2, VTYPE, VLMUL);
    uint16_t sew = 8 << FIELD_EX64(s2, VTYPE, VSEW);
    uint8_t ediv = FIELD_EX64(s2, VTYPE, VEDIV);
    bool vill = FIELD_EX64(s2, VTYPE, VILL);
    target_ulong reserved = FIELD_EX64(s2, VTYPE, RESERVED);

    if (lmul & 4) {
        /* Fractional LMUL. */
        if (lmul == 4 ||
            cpu->cfg.elen >> (8 - lmul) < sew) {
            vill = true;
        }
    }

    if ((sew > cpu->cfg.elen)
        || vill
        || (ediv != 0)
        || (reserved != 0)) {
        /* only set vill bit. */
        env->vtype = FIELD_DP64(0, VTYPE, VILL, 1);
        env->vl = 0;
        env->vstart = 0;
        return 0;
    }

    vlmax = vext_get_vlmax(cpu, s2);
    if (s1 <= vlmax) {
        vl = s1;
    } else {
        vl = vlmax;
    }
    env->vl = vl;
    env->vtype = s2;
    env->vstart = 0;
    return vl;
}

/*
 * Note that vector data is stored in host-endian 64-bit chunks,
 * so addressing units smaller than that needs a host-endian fixup.
 */
#ifdef HOST_WORDS_BIGENDIAN
#define H1(x)   ((x) ^ 7)
#define H1_2(x) ((x) ^ 6)
#define H1_4(x) ((x) ^ 4)
#define H2(x)   ((x) ^ 3)
#define H4(x)   ((x) ^ 1)
#define H8(x)   ((x))
#else
#define H1(x)   (x)
#define H1_2(x) (x)
#define H1_4(x) (x)
#define H2(x)   (x)
#define H4(x)   (x)
#define H8(x)   (x)
#endif

static inline uint32_t vext_nf(uint32_t desc)
{
    return FIELD_EX32(simd_data(desc), VDATA, NF);
}

static inline uint32_t vext_vm(uint32_t desc)
{
    return FIELD_EX32(simd_data(desc), VDATA, VM);
}

/*
 * Encode LMUL to lmul as following:
 *     LMUL    vlmul    lmul
 *      1       000       0
 *      2       001       1
 *      4       010       2
 *      8       011       3
 *      -       100       -
 *     1/8      101      -3
 *     1/4      110      -2
 *     1/2      111      -1
 */
static inline int32_t vext_lmul(uint32_t desc)
{
    return sextract32(FIELD_EX32(simd_data(desc), VDATA, LMUL), 0, 3);
}

static uint32_t vext_wd(uint32_t desc)
{
    return FIELD_EX32(simd_data(desc), VDATA, WD);
}

/*
 * Get vector group length in bytes. Its range is [64, 2048].
 *
 * As simd_desc support at most 256, the max vlen is 512 bits.
 * So vlen in bytes is encoded as maxsz.
 */
static inline uint32_t vext_maxsz(uint32_t desc)
{
    return simd_maxsz(desc) << vext_lmul(desc);
}

/*
 * This function checks watchpoint before real load operation.
 *
 * In softmmu mode, the TLB API probe_access is enough for watchpoint check.
 * In user mode, there is no watchpoint support now.
 *
 * It will trigger an exception if there is no mapping in TLB
 * and page table walk can't fill the TLB entry. Then the guest
 * software can return here after process the exception or never return.
 */
static void probe_pages(CPURISCVState *env, target_ulong addr,
                        target_ulong len, uintptr_t ra,
                        MMUAccessType access_type)
{
    target_ulong pagelen = -(addr | TARGET_PAGE_MASK);
    target_ulong curlen = MIN(pagelen, len);

    probe_access(env, addr, curlen, access_type,
                 cpu_mmu_index(env, false), ra);
    if (len > curlen) {
        addr += curlen;
        curlen = len - curlen;
        probe_access(env, addr, curlen, access_type,
                     cpu_mmu_index(env, false), ra);
    }
}

static inline void vext_set_elem_mask(void *v0, int index,
        uint8_t value)
{
    int idx = index / 64;
    int pos = index % 64;
    uint64_t old = ((uint64_t *)v0)[idx];
    ((uint64_t *)v0)[idx] = deposit64(old, pos, 1, value);
}

/*
 * Earlier designs (pre-0.9) had a varying number of bits
 * per mask value (MLEN). In the 0.9 design, MLEN=1.
 * (Section 4.5)
 */
static inline int vext_elem_mask(void *v0, int index)
{
    int idx = index / 64;
    int pos = index  % 64;
    return (((uint64_t *)v0)[idx] >> pos) & 1;
}

/* elements operations for load and store */
typedef void vext_ldst_elem_fn(CPURISCVState *env, target_ulong addr,
                               uint32_t idx, void *vd, uintptr_t retaddr);

#define GEN_VEXT_LD_ELEM(NAME, ETYPE, H, LDSUF)            \
static void NAME(CPURISCVState *env, abi_ptr addr,         \
                 uint32_t idx, void *vd, uintptr_t retaddr)\
{                                                          \
    ETYPE *cur = ((ETYPE *)vd + H(idx));                   \
    *cur = cpu_##LDSUF##_data_ra(env, addr, retaddr);      \
}                                                          \

GEN_VEXT_LD_ELEM(lde_b, int8_t,  H1, ldsb)
GEN_VEXT_LD_ELEM(lde_h, int16_t, H2, ldsw)
GEN_VEXT_LD_ELEM(lde_w, int32_t, H4, ldl)
GEN_VEXT_LD_ELEM(lde_d, int64_t, H8, ldq)

#define GEN_VEXT_ST_ELEM(NAME, ETYPE, H, STSUF)            \
static void NAME(CPURISCVState *env, abi_ptr addr,         \
                 uint32_t idx, void *vd, uintptr_t retaddr)\
{                                                          \
    ETYPE data = *((ETYPE *)vd + H(idx));                  \
    cpu_##STSUF##_data_ra(env, addr, data, retaddr);       \
}

GEN_VEXT_ST_ELEM(ste_b, int8_t,  H1, stb)
GEN_VEXT_ST_ELEM(ste_h, int16_t, H2, stw)
GEN_VEXT_ST_ELEM(ste_w, int32_t, H4, stl)
GEN_VEXT_ST_ELEM(ste_d, int64_t, H8, stq)

/*
 *** stride: access vector element from strided memory
 */
static void
vext_ldst_stride(void *vd, void *v0, target_ulong base,
                 target_ulong stride, CPURISCVState *env,
                 uint32_t desc, uint32_t vm,
                 vext_ldst_elem_fn *ldst_elem,
                 uint32_t esz, uintptr_t ra, MMUAccessType access_type)
{
    uint32_t i, k;
    uint32_t nf = vext_nf(desc);
    uint32_t vlmax = vext_maxsz(desc) / esz;

    /* probe every access*/
    for (i = 0; i < env->vl; i++) {
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        probe_pages(env, base + stride * i, nf * esz, ra, access_type);
    }
    /* do real access */
    for (i = 0; i < env->vl; i++) {
        k = 0;
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        while (k < nf) {
            target_ulong addr = base + stride * i + k * esz;
            ldst_elem(env, addr, i + k * vlmax, vd, ra);
            k++;
        }
    }
}

#define GEN_VEXT_LD_STRIDE(NAME, ETYPE, LOAD_FN)                        \
void HELPER(NAME)(void *vd, void * v0, target_ulong base,               \
                  target_ulong stride, CPURISCVState *env,              \
                  uint32_t desc)                                        \
{                                                                       \
    uint32_t vm = vext_vm(desc);                                        \
    vext_ldst_stride(vd, v0, base, stride, env, desc, vm, LOAD_FN,      \
                     sizeof(ETYPE), GETPC(), MMU_DATA_LOAD);            \
}

GEN_VEXT_LD_STRIDE(vlse8_v,  int8_t,  lde_b)
GEN_VEXT_LD_STRIDE(vlse16_v, int16_t, lde_h)
GEN_VEXT_LD_STRIDE(vlse32_v, int32_t, lde_w)
GEN_VEXT_LD_STRIDE(vlse64_v, int64_t, lde_d)

#define GEN_VEXT_ST_STRIDE(NAME, ETYPE, STORE_FN)                       \
void HELPER(NAME)(void *vd, void *v0, target_ulong base,                \
                  target_ulong stride, CPURISCVState *env,              \
                  uint32_t desc)                                        \
{                                                                       \
    uint32_t vm = vext_vm(desc);                                        \
    vext_ldst_stride(vd, v0, base, stride, env, desc, vm, STORE_FN,     \
                     sizeof(ETYPE), GETPC(), MMU_DATA_STORE);           \
}

GEN_VEXT_ST_STRIDE(vsse8_v,  int8_t,  ste_b)
GEN_VEXT_ST_STRIDE(vsse16_v, int16_t, ste_h)
GEN_VEXT_ST_STRIDE(vsse32_v, int32_t, ste_w)
GEN_VEXT_ST_STRIDE(vsse64_v, int64_t, ste_d)

/*
 *** unit-stride: access elements stored contiguously in memory
 */

/* unmasked unit-stride load and store operation*/
static void
vext_ldst_us(void *vd, target_ulong base, CPURISCVState *env, uint32_t desc,
             vext_ldst_elem_fn *ldst_elem,
             uint32_t esz, uintptr_t ra, MMUAccessType access_type)
{
    uint32_t i, k;
    uint32_t nf = vext_nf(desc);
    uint32_t vlmax = vext_maxsz(desc) / esz;

    /* probe every access */
    probe_pages(env, base, env->vl * nf * esz, ra, access_type);
    /* load bytes from guest memory */
    for (i = 0; i < env->vl; i++) {
        k = 0;
        while (k < nf) {
            target_ulong addr = base + (i * nf + k) * esz;
            ldst_elem(env, addr, i + k * vlmax, vd, ra);
            k++;
        }
    }
}

/*
 * masked unit-stride load and store operation will be a special case of stride,
 * stride = NF * sizeof (MTYPE)
 */

#define GEN_VEXT_LD_US(NAME, ETYPE, LOAD_FN)                            \
void HELPER(NAME##_mask)(void *vd, void *v0, target_ulong base,         \
                         CPURISCVState *env, uint32_t desc)             \
{                                                                       \
    uint32_t stride = vext_nf(desc) * sizeof(ETYPE);                    \
    vext_ldst_stride(vd, v0, base, stride, env, desc, false, LOAD_FN,   \
                     sizeof(ETYPE), GETPC(), MMU_DATA_LOAD);            \
}                                                                       \
                                                                        \
void HELPER(NAME)(void *vd, void *v0, target_ulong base,                \
                  CPURISCVState *env, uint32_t desc)                    \
{                                                                       \
    vext_ldst_us(vd, base, env, desc, LOAD_FN,                          \
                 sizeof(ETYPE), GETPC(), MMU_DATA_LOAD);                \
}

GEN_VEXT_LD_US(vle8_v,  int8_t,  lde_b)
GEN_VEXT_LD_US(vle16_v, int16_t, lde_h)
GEN_VEXT_LD_US(vle32_v, int32_t, lde_w)
GEN_VEXT_LD_US(vle64_v, int64_t, lde_d)

#define GEN_VEXT_ST_US(NAME, ETYPE, STORE_FN)                           \
void HELPER(NAME##_mask)(void *vd, void *v0, target_ulong base,         \
                         CPURISCVState *env, uint32_t desc)             \
{                                                                       \
    uint32_t stride = vext_nf(desc) * sizeof(ETYPE);                    \
    vext_ldst_stride(vd, v0, base, stride, env, desc, false, STORE_FN,  \
                     sizeof(ETYPE), GETPC(), MMU_DATA_STORE);           \
}                                                                       \
                                                                        \
void HELPER(NAME)(void *vd, void *v0, target_ulong base,                \
                  CPURISCVState *env, uint32_t desc)                    \
{                                                                       \
    vext_ldst_us(vd, base, env, desc, STORE_FN,                         \
                 sizeof(ETYPE), GETPC(), MMU_DATA_STORE);               \
}

GEN_VEXT_ST_US(vse8_v,  int8_t,  ste_b)
GEN_VEXT_ST_US(vse16_v, int16_t, ste_h)
GEN_VEXT_ST_US(vse32_v, int32_t, ste_w)
GEN_VEXT_ST_US(vse64_v, int64_t, ste_d)

/*
 *** index: access vector element from indexed memory
 */
typedef target_ulong vext_get_index_addr(target_ulong base,
        uint32_t idx, void *vs2);

#define GEN_VEXT_GET_INDEX_ADDR(NAME, ETYPE, H)        \
static target_ulong NAME(target_ulong base,            \
                         uint32_t idx, void *vs2)      \
{                                                      \
    return (base + *((ETYPE *)vs2 + H(idx)));          \
}

GEN_VEXT_GET_INDEX_ADDR(idx_b, int8_t,  H1)
GEN_VEXT_GET_INDEX_ADDR(idx_h, int16_t, H2)
GEN_VEXT_GET_INDEX_ADDR(idx_w, int32_t, H4)
GEN_VEXT_GET_INDEX_ADDR(idx_d, int64_t, H8)

static inline void
vext_ldst_index(void *vd, void *v0, target_ulong base,
                void *vs2, CPURISCVState *env, uint32_t desc,
                vext_get_index_addr get_index_addr,
                vext_ldst_elem_fn *ldst_elem,
                uint32_t esz, uintptr_t ra, MMUAccessType access_type)
{
    uint32_t i, k;
    uint32_t nf = vext_nf(desc);
    uint32_t vm = vext_vm(desc);
    uint32_t vlmax = vext_maxsz(desc) / esz;

    /* probe every access*/
    for (i = 0; i < env->vl; i++) {
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        probe_pages(env, get_index_addr(base, i, vs2), nf * esz, ra,
                    access_type);
    }
    /* load bytes from guest memory */
    for (i = 0; i < env->vl; i++) {
        k = 0;
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        while (k < nf) {
            abi_ptr addr = get_index_addr(base, i, vs2) + k * esz;
            ldst_elem(env, addr, i + k * vlmax, vd, ra);
            k++;
        }
    }
}

#define GEN_VEXT_LD_INDEX(NAME, ETYPE, INDEX_FN, LOAD_FN)                  \
void HELPER(NAME)(void *vd, void *v0, target_ulong base,                   \
                  void *vs2, CPURISCVState *env, uint32_t desc)            \
{                                                                          \
    vext_ldst_index(vd, v0, base, vs2, env, desc, INDEX_FN,                \
                    LOAD_FN, sizeof(ETYPE), GETPC(), MMU_DATA_LOAD);       \
}

GEN_VEXT_LD_INDEX(vlxei8_8_v,   int8_t,  idx_b, lde_b)
GEN_VEXT_LD_INDEX(vlxei8_16_v,  int16_t, idx_b, lde_h)
GEN_VEXT_LD_INDEX(vlxei8_32_v,  int32_t, idx_b, lde_w)
GEN_VEXT_LD_INDEX(vlxei8_64_v,  int64_t, idx_b, lde_d)
GEN_VEXT_LD_INDEX(vlxei16_8_v,  int8_t,  idx_h, lde_b)
GEN_VEXT_LD_INDEX(vlxei16_16_v, int16_t, idx_h, lde_h)
GEN_VEXT_LD_INDEX(vlxei16_32_v, int32_t, idx_h, lde_w)
GEN_VEXT_LD_INDEX(vlxei16_64_v, int64_t, idx_h, lde_d)
GEN_VEXT_LD_INDEX(vlxei32_8_v,  int8_t,  idx_w, lde_b)
GEN_VEXT_LD_INDEX(vlxei32_16_v, int16_t, idx_w, lde_h)
GEN_VEXT_LD_INDEX(vlxei32_32_v, int32_t, idx_w, lde_w)
GEN_VEXT_LD_INDEX(vlxei32_64_v, int64_t, idx_w, lde_d)
GEN_VEXT_LD_INDEX(vlxei64_8_v,  int8_t,  idx_d, lde_b)
GEN_VEXT_LD_INDEX(vlxei64_16_v, int16_t, idx_d, lde_h)
GEN_VEXT_LD_INDEX(vlxei64_32_v, int32_t, idx_d, lde_w)
GEN_VEXT_LD_INDEX(vlxei64_64_v, int64_t, idx_d, lde_d)

#define GEN_VEXT_ST_INDEX(NAME, ETYPE, INDEX_FN, STORE_FN)       \
void HELPER(NAME)(void *vd, void *v0, target_ulong base,         \
                  void *vs2, CPURISCVState *env, uint32_t desc)  \
{                                                                \
    vext_ldst_index(vd, v0, base, vs2, env, desc, INDEX_FN,      \
                    STORE_FN, sizeof(ETYPE),                     \
                    GETPC(), MMU_DATA_STORE);                    \
}

GEN_VEXT_ST_INDEX(vsxei8_8_v,   int8_t,  idx_b, ste_b)
GEN_VEXT_ST_INDEX(vsxei8_16_v,  int16_t, idx_b, ste_h)
GEN_VEXT_ST_INDEX(vsxei8_32_v,  int32_t, idx_b, ste_w)
GEN_VEXT_ST_INDEX(vsxei8_64_v,  int64_t, idx_b, ste_d)
GEN_VEXT_ST_INDEX(vsxei16_8_v,  int8_t,  idx_h, ste_b)
GEN_VEXT_ST_INDEX(vsxei16_16_v, int16_t, idx_h, ste_h)
GEN_VEXT_ST_INDEX(vsxei16_32_v, int32_t, idx_h, ste_w)
GEN_VEXT_ST_INDEX(vsxei16_64_v, int64_t, idx_h, ste_d)
GEN_VEXT_ST_INDEX(vsxei32_8_v,  int8_t,  idx_w, ste_b)
GEN_VEXT_ST_INDEX(vsxei32_16_v, int16_t, idx_w, ste_h)
GEN_VEXT_ST_INDEX(vsxei32_32_v, int32_t, idx_w, ste_w)
GEN_VEXT_ST_INDEX(vsxei32_64_v, int64_t, idx_w, ste_d)
GEN_VEXT_ST_INDEX(vsxei64_8_v,  int8_t,  idx_d, ste_b)
GEN_VEXT_ST_INDEX(vsxei64_16_v, int16_t, idx_d, ste_h)
GEN_VEXT_ST_INDEX(vsxei64_32_v, int32_t, idx_d, ste_w)
GEN_VEXT_ST_INDEX(vsxei64_64_v, int64_t, idx_d, ste_d)

/*
 *** unit-stride fault-only-fisrt load instructions
 */
static inline void
vext_ldff(void *vd, void *v0, target_ulong base,
          CPURISCVState *env, uint32_t desc,
          vext_ldst_elem_fn *ldst_elem,
          uint32_t esz, uint32_t msz, uintptr_t ra)
{
    void *host;
    uint32_t i, k, vl = 0;
    uint32_t nf = vext_nf(desc);
    uint32_t vm = vext_vm(desc);
    uint32_t vlmax = vext_maxsz(desc) / esz;
    target_ulong addr, offset, remain;

    /* probe every access*/
    for (i = 0; i < env->vl; i++) {
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        addr = base + nf * i * msz;
        if (i == 0) {
            probe_pages(env, addr, nf * msz, ra, MMU_DATA_LOAD);
        } else {
            /* if it triggers an exception, no need to check watchpoint */
            remain = nf * msz;
            while (remain > 0) {
                offset = -(addr | TARGET_PAGE_MASK);
                host = tlb_vaddr_to_host(env, addr, MMU_DATA_LOAD,
                                         cpu_mmu_index(env, false));
                if (host) {
#ifdef CONFIG_USER_ONLY
                    if (page_check_range(addr, nf * msz, PAGE_READ) < 0) {
                        vl = i;
                        goto ProbeSuccess;
                    }
#else
                    probe_pages(env, addr, nf * msz, ra, MMU_DATA_LOAD);
#endif
                } else {
                    vl = i;
                    goto ProbeSuccess;
                }
                if (remain <=  offset) {
                    break;
                }
                remain -= offset;
                addr += offset;
            }
        }
    }
ProbeSuccess:
    /* load bytes from guest memory */
    if (vl != 0) {
        env->vl = vl;
    }
    for (i = 0; i < env->vl; i++) {
        k = 0;
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        while (k < nf) {
            target_ulong addr = base + (i * nf + k) * msz;
            ldst_elem(env, addr, i + k * vlmax, vd, ra);
            k++;
        }
    }
}

#define GEN_VEXT_LDFF(NAME, MTYPE, ETYPE, LOAD_FN)               \
void HELPER(NAME)(void *vd, void *v0, target_ulong base,         \
                  CPURISCVState *env, uint32_t desc)             \
{                                                                \
    vext_ldff(vd, v0, base, env, desc, LOAD_FN,                  \
              sizeof(ETYPE), sizeof(MTYPE), GETPC());            \
}

GEN_VEXT_LDFF(vlbff_v_b,  int8_t,   int8_t,   ldb_b)
GEN_VEXT_LDFF(vlbff_v_h,  int8_t,   int16_t,  ldb_h)
GEN_VEXT_LDFF(vlbff_v_w,  int8_t,   int32_t,  ldb_w)
GEN_VEXT_LDFF(vlbff_v_d,  int8_t,   int64_t,  ldb_d)
GEN_VEXT_LDFF(vlhff_v_h,  int16_t,  int16_t,  ldh_h)
GEN_VEXT_LDFF(vlhff_v_w,  int16_t,  int32_t,  ldh_w)
GEN_VEXT_LDFF(vlhff_v_d,  int16_t,  int64_t,  ldh_d)
GEN_VEXT_LDFF(vlwff_v_w,  int32_t,  int32_t,  ldw_w)
GEN_VEXT_LDFF(vlwff_v_d,  int32_t,  int64_t,  ldw_d)
GEN_VEXT_LDFF(vleff_v_b,  int8_t,   int8_t,   lde_b)
GEN_VEXT_LDFF(vleff_v_h,  int16_t,  int16_t,  lde_h)
GEN_VEXT_LDFF(vleff_v_w,  int32_t,  int32_t,  lde_w)
GEN_VEXT_LDFF(vleff_v_d,  int64_t,  int64_t,  lde_d)
GEN_VEXT_LDFF(vlbuff_v_b, uint8_t,  uint8_t,  ldbu_b)
GEN_VEXT_LDFF(vlbuff_v_h, uint8_t,  uint16_t, ldbu_h)
GEN_VEXT_LDFF(vlbuff_v_w, uint8_t,  uint32_t, ldbu_w)
GEN_VEXT_LDFF(vlbuff_v_d, uint8_t,  uint64_t, ldbu_d)
GEN_VEXT_LDFF(vlhuff_v_h, uint16_t, uint16_t, ldhu_h)
GEN_VEXT_LDFF(vlhuff_v_w, uint16_t, uint32_t, ldhu_w)
GEN_VEXT_LDFF(vlhuff_v_d, uint16_t, uint64_t, ldhu_d)
GEN_VEXT_LDFF(vlwuff_v_w, uint32_t, uint32_t, ldwu_w)
GEN_VEXT_LDFF(vlwuff_v_d, uint32_t, uint64_t, ldwu_d)

/*
 *** Vector AMO Operations (Zvamo)
 */
typedef void vext_amo_noatomic_fn(void *vs3, target_ulong addr,
                                  uint32_t wd, uint32_t idx, CPURISCVState *env,
                                  uintptr_t retaddr);

/* no atomic opreation for vector atomic insructions */
#define DO_SWAP(N, M) (M)
#define DO_AND(N, M)  (N & M)
#define DO_XOR(N, M)  (N ^ M)
#define DO_OR(N, M)   (N | M)
#define DO_ADD(N, M)  (N + M)

#define GEN_VEXT_AMO_NOATOMIC_OP(NAME, ESZ, MSZ, H, DO_OP, SUF) \
static void                                                     \
vext_##NAME##_noatomic_op(void *vs3, target_ulong addr,         \
                          uint32_t wd, uint32_t idx,            \
                          CPURISCVState *env, uintptr_t retaddr)\
{                                                               \
    typedef int##ESZ##_t ETYPE;                                 \
    typedef int##MSZ##_t MTYPE;                                 \
    typedef uint##MSZ##_t UMTYPE __attribute__((unused));       \
    ETYPE *pe3 = (ETYPE *)vs3 + H(idx);                         \
    MTYPE  a = cpu_ld##SUF##_data(env, addr), b = *pe3;         \
                                                                \
    cpu_st##SUF##_data(env, addr, DO_OP(a, b));                 \
    if (wd) {                                                   \
        *pe3 = a;                                               \
    }                                                           \
}

/* Signed min/max */
#define DO_MAX(N, M)  ((N) >= (M) ? (N) : (M))
#define DO_MIN(N, M)  ((N) >= (M) ? (M) : (N))

/* Unsigned min/max */
#define DO_MAXU(N, M) DO_MAX((UMTYPE)N, (UMTYPE)M)
#define DO_MINU(N, M) DO_MIN((UMTYPE)N, (UMTYPE)M)

GEN_VEXT_AMO_NOATOMIC_OP(vamoswapw_v_w, 32, 32, H4, DO_SWAP, l)
GEN_VEXT_AMO_NOATOMIC_OP(vamoaddw_v_w,  32, 32, H4, DO_ADD,  l)
GEN_VEXT_AMO_NOATOMIC_OP(vamoxorw_v_w,  32, 32, H4, DO_XOR,  l)
GEN_VEXT_AMO_NOATOMIC_OP(vamoandw_v_w,  32, 32, H4, DO_AND,  l)
GEN_VEXT_AMO_NOATOMIC_OP(vamoorw_v_w,   32, 32, H4, DO_OR,   l)
GEN_VEXT_AMO_NOATOMIC_OP(vamominw_v_w,  32, 32, H4, DO_MIN,  l)
GEN_VEXT_AMO_NOATOMIC_OP(vamomaxw_v_w,  32, 32, H4, DO_MAX,  l)
GEN_VEXT_AMO_NOATOMIC_OP(vamominuw_v_w, 32, 32, H4, DO_MINU, l)
GEN_VEXT_AMO_NOATOMIC_OP(vamomaxuw_v_w, 32, 32, H4, DO_MAXU, l)
#ifdef TARGET_RISCV64
GEN_VEXT_AMO_NOATOMIC_OP(vamoswapw_v_d, 64, 32, H8, DO_SWAP, l)
GEN_VEXT_AMO_NOATOMIC_OP(vamoswapd_v_d, 64, 64, H8, DO_SWAP, q)
GEN_VEXT_AMO_NOATOMIC_OP(vamoaddw_v_d,  64, 32, H8, DO_ADD,  l)
GEN_VEXT_AMO_NOATOMIC_OP(vamoaddd_v_d,  64, 64, H8, DO_ADD,  q)
GEN_VEXT_AMO_NOATOMIC_OP(vamoxorw_v_d,  64, 32, H8, DO_XOR,  l)
GEN_VEXT_AMO_NOATOMIC_OP(vamoxord_v_d,  64, 64, H8, DO_XOR,  q)
GEN_VEXT_AMO_NOATOMIC_OP(vamoandw_v_d,  64, 32, H8, DO_AND,  l)
GEN_VEXT_AMO_NOATOMIC_OP(vamoandd_v_d,  64, 64, H8, DO_AND,  q)
GEN_VEXT_AMO_NOATOMIC_OP(vamoorw_v_d,   64, 32, H8, DO_OR,   l)
GEN_VEXT_AMO_NOATOMIC_OP(vamoord_v_d,   64, 64, H8, DO_OR,   q)
GEN_VEXT_AMO_NOATOMIC_OP(vamominw_v_d,  64, 32, H8, DO_MIN,  l)
GEN_VEXT_AMO_NOATOMIC_OP(vamomind_v_d,  64, 64, H8, DO_MIN,  q)
GEN_VEXT_AMO_NOATOMIC_OP(vamomaxw_v_d,  64, 32, H8, DO_MAX,  l)
GEN_VEXT_AMO_NOATOMIC_OP(vamomaxd_v_d,  64, 64, H8, DO_MAX,  q)
GEN_VEXT_AMO_NOATOMIC_OP(vamominuw_v_d, 64, 32, H8, DO_MINU, l)
GEN_VEXT_AMO_NOATOMIC_OP(vamominud_v_d, 64, 64, H8, DO_MINU, q)
GEN_VEXT_AMO_NOATOMIC_OP(vamomaxuw_v_d, 64, 32, H8, DO_MAXU, l)
GEN_VEXT_AMO_NOATOMIC_OP(vamomaxud_v_d, 64, 64, H8, DO_MAXU, q)
#endif

static inline void
vext_amo_noatomic(void *vs3, void *v0, target_ulong base,
                  void *vs2, CPURISCVState *env, uint32_t desc,
                  vext_get_index_addr get_index_addr,
                  vext_amo_noatomic_fn *noatomic_op,
                  uint32_t esz, uint32_t msz, uintptr_t ra)
{
    uint32_t i;
    target_long addr;
    uint32_t wd = vext_wd(desc);
    uint32_t vm = vext_vm(desc);

    for (i = 0; i < env->vl; i++) {
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        probe_pages(env, get_index_addr(base, i, vs2), msz, ra, MMU_DATA_LOAD);
        probe_pages(env, get_index_addr(base, i, vs2), msz, ra, MMU_DATA_STORE);
    }
    for (i = 0; i < env->vl; i++) {
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        addr = get_index_addr(base, i, vs2);
        noatomic_op(vs3, addr, wd, i, env, ra);
    }
}

#define GEN_VEXT_AMO(NAME, MTYPE, ETYPE, INDEX_FN)              \
void HELPER(NAME)(void *vs3, void *v0, target_ulong base,       \
                  void *vs2, CPURISCVState *env, uint32_t desc) \
{                                                               \
    vext_amo_noatomic(vs3, v0, base, vs2, env, desc,            \
                      INDEX_FN, vext_##NAME##_noatomic_op,      \
                      sizeof(ETYPE), sizeof(MTYPE),             \
                      GETPC());                                 \
}

#ifdef TARGET_RISCV64
GEN_VEXT_AMO(vamoswapw_v_d, int32_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamoswapd_v_d, int64_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamoaddw_v_d,  int32_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamoaddd_v_d,  int64_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamoxorw_v_d,  int32_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamoxord_v_d,  int64_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamoandw_v_d,  int32_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamoandd_v_d,  int64_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamoorw_v_d,   int32_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamoord_v_d,   int64_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamominw_v_d,  int32_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamomind_v_d,  int64_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamomaxw_v_d,  int32_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamomaxd_v_d,  int64_t,  int64_t,  idx_d)
GEN_VEXT_AMO(vamominuw_v_d, uint32_t, uint64_t, idx_d)
GEN_VEXT_AMO(vamominud_v_d, uint64_t, uint64_t, idx_d)
GEN_VEXT_AMO(vamomaxuw_v_d, uint32_t, uint64_t, idx_d)
GEN_VEXT_AMO(vamomaxud_v_d, uint64_t, uint64_t, idx_d)
#endif
GEN_VEXT_AMO(vamoswapw_v_w, int32_t,  int32_t,  idx_w)
GEN_VEXT_AMO(vamoaddw_v_w,  int32_t,  int32_t,  idx_w)
GEN_VEXT_AMO(vamoxorw_v_w,  int32_t,  int32_t,  idx_w)
GEN_VEXT_AMO(vamoandw_v_w,  int32_t,  int32_t,  idx_w)
GEN_VEXT_AMO(vamoorw_v_w,   int32_t,  int32_t,  idx_w)
GEN_VEXT_AMO(vamominw_v_w,  int32_t,  int32_t,  idx_w)
GEN_VEXT_AMO(vamomaxw_v_w,  int32_t,  int32_t,  idx_w)
GEN_VEXT_AMO(vamominuw_v_w, uint32_t, uint32_t, idx_w)
GEN_VEXT_AMO(vamomaxuw_v_w, uint32_t, uint32_t, idx_w)

/*
 *** Vector Integer Arithmetic Instructions
 */

/* expand macro args before macro */
#define RVVCALL(macro, ...)  macro(__VA_ARGS__)

/* (TD, T1, T2, TX1, TX2) */
#define OP_SSS_B int8_t, int8_t, int8_t, int8_t, int8_t
#define OP_SSS_H int16_t, int16_t, int16_t, int16_t, int16_t
#define OP_SSS_W int32_t, int32_t, int32_t, int32_t, int32_t
#define OP_SSS_D int64_t, int64_t, int64_t, int64_t, int64_t
#define OP_UUU_B uint8_t, uint8_t, uint8_t, uint8_t, uint8_t
#define OP_UUU_H uint16_t, uint16_t, uint16_t, uint16_t, uint16_t
#define OP_UUU_W uint32_t, uint32_t, uint32_t, uint32_t, uint32_t
#define OP_UUU_D uint64_t, uint64_t, uint64_t, uint64_t, uint64_t
#define OP_SUS_B int8_t, uint8_t, int8_t, uint8_t, int8_t
#define OP_SUS_H int16_t, uint16_t, int16_t, uint16_t, int16_t
#define OP_SUS_W int32_t, uint32_t, int32_t, uint32_t, int32_t
#define OP_SUS_D int64_t, uint64_t, int64_t, uint64_t, int64_t
#define WOP_UUU_B uint16_t, uint8_t, uint8_t, uint16_t, uint16_t
#define WOP_UUU_H uint32_t, uint16_t, uint16_t, uint32_t, uint32_t
#define WOP_UUU_W uint64_t, uint32_t, uint32_t, uint64_t, uint64_t
#define WOP_SSS_B int16_t, int8_t, int8_t, int16_t, int16_t
#define WOP_SSS_H int32_t, int16_t, int16_t, int32_t, int32_t
#define WOP_SSS_W int64_t, int32_t, int32_t, int64_t, int64_t
#define WOP_SUS_B int16_t, uint8_t, int8_t, uint16_t, int16_t
#define WOP_SUS_H int32_t, uint16_t, int16_t, uint32_t, int32_t
#define WOP_SUS_W int64_t, uint32_t, int32_t, uint64_t, int64_t
#define WOP_SSU_B int16_t, int8_t, uint8_t, int16_t, uint16_t
#define WOP_SSU_H int32_t, int16_t, uint16_t, int32_t, uint32_t
#define WOP_SSU_W int64_t, int32_t, uint32_t, int64_t, uint64_t
#define NOP_SSS_B int8_t, int8_t, int16_t, int8_t, int16_t
#define NOP_SSS_H int16_t, int16_t, int32_t, int16_t, int32_t
#define NOP_SSS_W int32_t, int32_t, int64_t, int32_t, int64_t
#define NOP_UUU_B uint8_t, uint8_t, uint16_t, uint8_t, uint16_t
#define NOP_UUU_H uint16_t, uint16_t, uint32_t, uint16_t, uint32_t
#define NOP_UUU_W uint32_t, uint32_t, uint64_t, uint32_t, uint64_t

/* operation of two vector elements */
typedef void opivv2_fn(void *vd, void *vs1, void *vs2, int i);

#define OPIVV2(NAME, TD, T1, T2, TX1, TX2, HD, HS1, HS2, OP)    \
static void do_##NAME(void *vd, void *vs1, void *vs2, int i)    \
{                                                               \
    TX1 s1 = *((T1 *)vs1 + HS1(i));                             \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                             \
    *((TD *)vd + HD(i)) = OP(s2, s1);                           \
}
#define DO_SUB(N, M) (N - M)
#define DO_RSUB(N, M) (M - N)

RVVCALL(OPIVV2, vadd_vv_b, OP_SSS_B, H1, H1, H1, DO_ADD)
RVVCALL(OPIVV2, vadd_vv_h, OP_SSS_H, H2, H2, H2, DO_ADD)
RVVCALL(OPIVV2, vadd_vv_w, OP_SSS_W, H4, H4, H4, DO_ADD)
RVVCALL(OPIVV2, vadd_vv_d, OP_SSS_D, H8, H8, H8, DO_ADD)
RVVCALL(OPIVV2, vsub_vv_b, OP_SSS_B, H1, H1, H1, DO_SUB)
RVVCALL(OPIVV2, vsub_vv_h, OP_SSS_H, H2, H2, H2, DO_SUB)
RVVCALL(OPIVV2, vsub_vv_w, OP_SSS_W, H4, H4, H4, DO_SUB)
RVVCALL(OPIVV2, vsub_vv_d, OP_SSS_D, H8, H8, H8, DO_SUB)

static void do_vext_vv(void *vd, void *v0, void *vs1, void *vs2,
                       CPURISCVState *env, uint32_t desc,
                       uint32_t esz, uint32_t dsz,
                       opivv2_fn *fn)
{
    uint32_t vm = vext_vm(desc);
    uint32_t vl = env->vl;
    uint32_t i;

    for (i = 0; i < vl; i++) {
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        fn(vd, vs1, vs2, i);
    }
}

/* generate the helpers for OPIVV */
#define GEN_VEXT_VV(NAME, ESZ, DSZ)                       \
void HELPER(NAME)(void *vd, void *v0, void *vs1,          \
                  void *vs2, CPURISCVState *env,          \
                  uint32_t desc)                          \
{                                                         \
    do_vext_vv(vd, v0, vs1, vs2, env, desc, ESZ, DSZ,     \
               do_##NAME);                                \
}

GEN_VEXT_VV(vadd_vv_b, 1, 1)
GEN_VEXT_VV(vadd_vv_h, 2, 2)
GEN_VEXT_VV(vadd_vv_w, 4, 4)
GEN_VEXT_VV(vadd_vv_d, 8, 8)
GEN_VEXT_VV(vsub_vv_b, 1, 1)
GEN_VEXT_VV(vsub_vv_h, 2, 2)
GEN_VEXT_VV(vsub_vv_w, 4, 4)
GEN_VEXT_VV(vsub_vv_d, 8, 8)

typedef void opivx2_fn(void *vd, target_long s1, void *vs2, int i);

/*
 * (T1)s1 gives the real operator type.
 * (TX1)(T1)s1 expands the operator type of widen or narrow operations.
 */
#define OPIVX2(NAME, TD, T1, T2, TX1, TX2, HD, HS2, OP)             \
static void do_##NAME(void *vd, target_long s1, void *vs2, int i)   \
{                                                                   \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                                 \
    *((TD *)vd + HD(i)) = OP(s2, (TX1)(T1)s1);                      \
}

RVVCALL(OPIVX2, vadd_vx_b, OP_SSS_B, H1, H1, DO_ADD)
RVVCALL(OPIVX2, vadd_vx_h, OP_SSS_H, H2, H2, DO_ADD)
RVVCALL(OPIVX2, vadd_vx_w, OP_SSS_W, H4, H4, DO_ADD)
RVVCALL(OPIVX2, vadd_vx_d, OP_SSS_D, H8, H8, DO_ADD)
RVVCALL(OPIVX2, vsub_vx_b, OP_SSS_B, H1, H1, DO_SUB)
RVVCALL(OPIVX2, vsub_vx_h, OP_SSS_H, H2, H2, DO_SUB)
RVVCALL(OPIVX2, vsub_vx_w, OP_SSS_W, H4, H4, DO_SUB)
RVVCALL(OPIVX2, vsub_vx_d, OP_SSS_D, H8, H8, DO_SUB)
RVVCALL(OPIVX2, vrsub_vx_b, OP_SSS_B, H1, H1, DO_RSUB)
RVVCALL(OPIVX2, vrsub_vx_h, OP_SSS_H, H2, H2, DO_RSUB)
RVVCALL(OPIVX2, vrsub_vx_w, OP_SSS_W, H4, H4, DO_RSUB)
RVVCALL(OPIVX2, vrsub_vx_d, OP_SSS_D, H8, H8, DO_RSUB)

static void do_vext_vx(void *vd, void *v0, target_long s1, void *vs2,
                       CPURISCVState *env, uint32_t desc,
                       uint32_t esz, uint32_t dsz,
                       opivx2_fn fn)
{
    uint32_t vm = vext_vm(desc);
    uint32_t vl = env->vl;
    uint32_t i;

    for (i = 0; i < vl; i++) {
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        fn(vd, s1, vs2, i);
    }
}

/* generate the helpers for OPIVX */
#define GEN_VEXT_VX(NAME, ESZ, DSZ)                       \
void HELPER(NAME)(void *vd, void *v0, target_ulong s1,    \
                  void *vs2, CPURISCVState *env,          \
                  uint32_t desc)                          \
{                                                         \
    do_vext_vx(vd, v0, s1, vs2, env, desc, ESZ, DSZ,      \
               do_##NAME);                                \
}

GEN_VEXT_VX(vadd_vx_b, 1, 1)
GEN_VEXT_VX(vadd_vx_h, 2, 2)
GEN_VEXT_VX(vadd_vx_w, 4, 4)
GEN_VEXT_VX(vadd_vx_d, 8, 8)
GEN_VEXT_VX(vsub_vx_b, 1, 1)
GEN_VEXT_VX(vsub_vx_h, 2, 2)
GEN_VEXT_VX(vsub_vx_w, 4, 4)
GEN_VEXT_VX(vsub_vx_d, 8, 8)
GEN_VEXT_VX(vrsub_vx_b, 1, 1)
GEN_VEXT_VX(vrsub_vx_h, 2, 2)
GEN_VEXT_VX(vrsub_vx_w, 4, 4)
GEN_VEXT_VX(vrsub_vx_d, 8, 8)

void HELPER(vec_rsubs8)(void *d, void *a, uint64_t b, uint32_t desc)
{
    intptr_t oprsz = simd_oprsz(desc);
    intptr_t i;

    for (i = 0; i < oprsz; i += sizeof(uint8_t)) {
        *(uint8_t *)(d + i) = (uint8_t)b - *(uint8_t *)(a + i);
    }
}

void HELPER(vec_rsubs16)(void *d, void *a, uint64_t b, uint32_t desc)
{
    intptr_t oprsz = simd_oprsz(desc);
    intptr_t i;

    for (i = 0; i < oprsz; i += sizeof(uint16_t)) {
        *(uint16_t *)(d + i) = (uint16_t)b - *(uint16_t *)(a + i);
    }
}

void HELPER(vec_rsubs32)(void *d, void *a, uint64_t b, uint32_t desc)
{
    intptr_t oprsz = simd_oprsz(desc);
    intptr_t i;

    for (i = 0; i < oprsz; i += sizeof(uint32_t)) {
        *(uint32_t *)(d + i) = (uint32_t)b - *(uint32_t *)(a + i);
    }
}

void HELPER(vec_rsubs64)(void *d, void *a, uint64_t b, uint32_t desc)
{
    intptr_t oprsz = simd_oprsz(desc);
    intptr_t i;

    for (i = 0; i < oprsz; i += sizeof(uint64_t)) {
        *(uint64_t *)(d + i) = b - *(uint64_t *)(a + i);
    }
}

/* Vector Widening Integer Add/Subtract */
#define WOP_UUU_B uint16_t, uint8_t, uint8_t, uint16_t, uint16_t
#define WOP_UUU_H uint32_t, uint16_t, uint16_t, uint32_t, uint32_t
#define WOP_UUU_W uint64_t, uint32_t, uint32_t, uint64_t, uint64_t
#define WOP_SSS_B int16_t, int8_t, int8_t, int16_t, int16_t
#define WOP_SSS_H int32_t, int16_t, int16_t, int32_t, int32_t
#define WOP_SSS_W int64_t, int32_t, int32_t, int64_t, int64_t
#define WOP_WUUU_B  uint16_t, uint8_t, uint16_t, uint16_t, uint16_t
#define WOP_WUUU_H  uint32_t, uint16_t, uint32_t, uint32_t, uint32_t
#define WOP_WUUU_W  uint64_t, uint32_t, uint64_t, uint64_t, uint64_t
#define WOP_WSSS_B  int16_t, int8_t, int16_t, int16_t, int16_t
#define WOP_WSSS_H  int32_t, int16_t, int32_t, int32_t, int32_t
#define WOP_WSSS_W  int64_t, int32_t, int64_t, int64_t, int64_t
RVVCALL(OPIVV2, vwaddu_vv_b, WOP_UUU_B, H2, H1, H1, DO_ADD)
RVVCALL(OPIVV2, vwaddu_vv_h, WOP_UUU_H, H4, H2, H2, DO_ADD)
RVVCALL(OPIVV2, vwaddu_vv_w, WOP_UUU_W, H8, H4, H4, DO_ADD)
RVVCALL(OPIVV2, vwsubu_vv_b, WOP_UUU_B, H2, H1, H1, DO_SUB)
RVVCALL(OPIVV2, vwsubu_vv_h, WOP_UUU_H, H4, H2, H2, DO_SUB)
RVVCALL(OPIVV2, vwsubu_vv_w, WOP_UUU_W, H8, H4, H4, DO_SUB)
RVVCALL(OPIVV2, vwadd_vv_b, WOP_SSS_B, H2, H1, H1, DO_ADD)
RVVCALL(OPIVV2, vwadd_vv_h, WOP_SSS_H, H4, H2, H2, DO_ADD)
RVVCALL(OPIVV2, vwadd_vv_w, WOP_SSS_W, H8, H4, H4, DO_ADD)
RVVCALL(OPIVV2, vwsub_vv_b, WOP_SSS_B, H2, H1, H1, DO_SUB)
RVVCALL(OPIVV2, vwsub_vv_h, WOP_SSS_H, H4, H2, H2, DO_SUB)
RVVCALL(OPIVV2, vwsub_vv_w, WOP_SSS_W, H8, H4, H4, DO_SUB)
RVVCALL(OPIVV2, vwaddu_wv_b, WOP_WUUU_B, H2, H1, H1, DO_ADD)
RVVCALL(OPIVV2, vwaddu_wv_h, WOP_WUUU_H, H4, H2, H2, DO_ADD)
RVVCALL(OPIVV2, vwaddu_wv_w, WOP_WUUU_W, H8, H4, H4, DO_ADD)
RVVCALL(OPIVV2, vwsubu_wv_b, WOP_WUUU_B, H2, H1, H1, DO_SUB)
RVVCALL(OPIVV2, vwsubu_wv_h, WOP_WUUU_H, H4, H2, H2, DO_SUB)
RVVCALL(OPIVV2, vwsubu_wv_w, WOP_WUUU_W, H8, H4, H4, DO_SUB)
RVVCALL(OPIVV2, vwadd_wv_b, WOP_WSSS_B, H2, H1, H1, DO_ADD)
RVVCALL(OPIVV2, vwadd_wv_h, WOP_WSSS_H, H4, H2, H2, DO_ADD)
RVVCALL(OPIVV2, vwadd_wv_w, WOP_WSSS_W, H8, H4, H4, DO_ADD)
RVVCALL(OPIVV2, vwsub_wv_b, WOP_WSSS_B, H2, H1, H1, DO_SUB)
RVVCALL(OPIVV2, vwsub_wv_h, WOP_WSSS_H, H4, H2, H2, DO_SUB)
RVVCALL(OPIVV2, vwsub_wv_w, WOP_WSSS_W, H8, H4, H4, DO_SUB)
GEN_VEXT_VV(vwaddu_vv_b, 1, 2)
GEN_VEXT_VV(vwaddu_vv_h, 2, 4)
GEN_VEXT_VV(vwaddu_vv_w, 4, 8)
GEN_VEXT_VV(vwsubu_vv_b, 1, 2)
GEN_VEXT_VV(vwsubu_vv_h, 2, 4)
GEN_VEXT_VV(vwsubu_vv_w, 4, 8)
GEN_VEXT_VV(vwadd_vv_b, 1, 2)
GEN_VEXT_VV(vwadd_vv_h, 2, 4)
GEN_VEXT_VV(vwadd_vv_w, 4, 8)
GEN_VEXT_VV(vwsub_vv_b, 1, 2)
GEN_VEXT_VV(vwsub_vv_h, 2, 4)
GEN_VEXT_VV(vwsub_vv_w, 4, 8)
GEN_VEXT_VV(vwaddu_wv_b, 1, 2)
GEN_VEXT_VV(vwaddu_wv_h, 2, 4)
GEN_VEXT_VV(vwaddu_wv_w, 4, 8)
GEN_VEXT_VV(vwsubu_wv_b, 1, 2)
GEN_VEXT_VV(vwsubu_wv_h, 2, 4)
GEN_VEXT_VV(vwsubu_wv_w, 4, 8)
GEN_VEXT_VV(vwadd_wv_b, 1, 2)
GEN_VEXT_VV(vwadd_wv_h, 2, 4)
GEN_VEXT_VV(vwadd_wv_w, 4, 8)
GEN_VEXT_VV(vwsub_wv_b, 1, 2)
GEN_VEXT_VV(vwsub_wv_h, 2, 4)
GEN_VEXT_VV(vwsub_wv_w, 4, 8)

RVVCALL(OPIVX2, vwaddu_vx_b, WOP_UUU_B, H2, H1, DO_ADD)
RVVCALL(OPIVX2, vwaddu_vx_h, WOP_UUU_H, H4, H2, DO_ADD)
RVVCALL(OPIVX2, vwaddu_vx_w, WOP_UUU_W, H8, H4, DO_ADD)
RVVCALL(OPIVX2, vwsubu_vx_b, WOP_UUU_B, H2, H1, DO_SUB)
RVVCALL(OPIVX2, vwsubu_vx_h, WOP_UUU_H, H4, H2, DO_SUB)
RVVCALL(OPIVX2, vwsubu_vx_w, WOP_UUU_W, H8, H4, DO_SUB)
RVVCALL(OPIVX2, vwadd_vx_b, WOP_SSS_B, H2, H1, DO_ADD)
RVVCALL(OPIVX2, vwadd_vx_h, WOP_SSS_H, H4, H2, DO_ADD)
RVVCALL(OPIVX2, vwadd_vx_w, WOP_SSS_W, H8, H4, DO_ADD)
RVVCALL(OPIVX2, vwsub_vx_b, WOP_SSS_B, H2, H1, DO_SUB)
RVVCALL(OPIVX2, vwsub_vx_h, WOP_SSS_H, H4, H2, DO_SUB)
RVVCALL(OPIVX2, vwsub_vx_w, WOP_SSS_W, H8, H4, DO_SUB)
RVVCALL(OPIVX2, vwaddu_wx_b, WOP_WUUU_B, H2, H1, DO_ADD)
RVVCALL(OPIVX2, vwaddu_wx_h, WOP_WUUU_H, H4, H2, DO_ADD)
RVVCALL(OPIVX2, vwaddu_wx_w, WOP_WUUU_W, H8, H4, DO_ADD)
RVVCALL(OPIVX2, vwsubu_wx_b, WOP_WUUU_B, H2, H1, DO_SUB)
RVVCALL(OPIVX2, vwsubu_wx_h, WOP_WUUU_H, H4, H2, DO_SUB)
RVVCALL(OPIVX2, vwsubu_wx_w, WOP_WUUU_W, H8, H4, DO_SUB)
RVVCALL(OPIVX2, vwadd_wx_b, WOP_WSSS_B, H2, H1, DO_ADD)
RVVCALL(OPIVX2, vwadd_wx_h, WOP_WSSS_H, H4, H2, DO_ADD)
RVVCALL(OPIVX2, vwadd_wx_w, WOP_WSSS_W, H8, H4, DO_ADD)
RVVCALL(OPIVX2, vwsub_wx_b, WOP_WSSS_B, H2, H1, DO_SUB)
RVVCALL(OPIVX2, vwsub_wx_h, WOP_WSSS_H, H4, H2, DO_SUB)
RVVCALL(OPIVX2, vwsub_wx_w, WOP_WSSS_W, H8, H4, DO_SUB)
GEN_VEXT_VX(vwaddu_vx_b, 1, 2)
GEN_VEXT_VX(vwaddu_vx_h, 2, 4)
GEN_VEXT_VX(vwaddu_vx_w, 4, 8)
GEN_VEXT_VX(vwsubu_vx_b, 1, 2)
GEN_VEXT_VX(vwsubu_vx_h, 2, 4)
GEN_VEXT_VX(vwsubu_vx_w, 4, 8)
GEN_VEXT_VX(vwadd_vx_b, 1, 2)
GEN_VEXT_VX(vwadd_vx_h, 2, 4)
GEN_VEXT_VX(vwadd_vx_w, 4, 8)
GEN_VEXT_VX(vwsub_vx_b, 1, 2)
GEN_VEXT_VX(vwsub_vx_h, 2, 4)
GEN_VEXT_VX(vwsub_vx_w, 4, 8)
GEN_VEXT_VX(vwaddu_wx_b, 1, 2)
GEN_VEXT_VX(vwaddu_wx_h, 2, 4)
GEN_VEXT_VX(vwaddu_wx_w, 4, 8)
GEN_VEXT_VX(vwsubu_wx_b, 1, 2)
GEN_VEXT_VX(vwsubu_wx_h, 2, 4)
GEN_VEXT_VX(vwsubu_wx_w, 4, 8)
GEN_VEXT_VX(vwadd_wx_b, 1, 2)
GEN_VEXT_VX(vwadd_wx_h, 2, 4)
GEN_VEXT_VX(vwadd_wx_w, 4, 8)
GEN_VEXT_VX(vwsub_wx_b, 1, 2)
GEN_VEXT_VX(vwsub_wx_h, 2, 4)
GEN_VEXT_VX(vwsub_wx_w, 4, 8)

/* Vector Integer Add-with-Carry / Subtract-with-Borrow Instructions */
#define DO_VADC(N, M, C) (N + M + C)
#define DO_VSBC(N, M, C) (N - M - C)

#define GEN_VEXT_VADC_VVM(NAME, ETYPE, H, DO_OP)              \
void HELPER(NAME)(void *vd, void *v0, void *vs1, void *vs2,   \
                  CPURISCVState *env, uint32_t desc)          \
{                                                             \
    uint32_t vl = env->vl;                                    \
    uint32_t i;                                               \
                                                              \
    for (i = 0; i < vl; i++) {                                \
        ETYPE s1 = *((ETYPE *)vs1 + H(i));                    \
        ETYPE s2 = *((ETYPE *)vs2 + H(i));                    \
        uint8_t carry = vext_elem_mask(v0, i);                \
                                                              \
        *((ETYPE *)vd + H(i)) = DO_OP(s2, s1, carry);         \
    }                                                         \
}

GEN_VEXT_VADC_VVM(vadc_vvm_b, uint8_t,  H1, DO_VADC)
GEN_VEXT_VADC_VVM(vadc_vvm_h, uint16_t, H2, DO_VADC)
GEN_VEXT_VADC_VVM(vadc_vvm_w, uint32_t, H4, DO_VADC)
GEN_VEXT_VADC_VVM(vadc_vvm_d, uint64_t, H8, DO_VADC)

GEN_VEXT_VADC_VVM(vsbc_vvm_b, uint8_t,  H1, DO_VSBC)
GEN_VEXT_VADC_VVM(vsbc_vvm_h, uint16_t, H2, DO_VSBC)
GEN_VEXT_VADC_VVM(vsbc_vvm_w, uint32_t, H4, DO_VSBC)
GEN_VEXT_VADC_VVM(vsbc_vvm_d, uint64_t, H8, DO_VSBC)

#define GEN_VEXT_VADC_VXM(NAME, ETYPE, H, DO_OP)                         \
void HELPER(NAME)(void *vd, void *v0, target_ulong s1, void *vs2,        \
                  CPURISCVState *env, uint32_t desc)                     \
{                                                                        \
    uint32_t vl = env->vl;                                               \
    uint32_t i;                                                          \
                                                                         \
    for (i = 0; i < vl; i++) {                                           \
        ETYPE s2 = *((ETYPE *)vs2 + H(i));                               \
        uint8_t carry = vext_elem_mask(v0, i);                           \
                                                                         \
        *((ETYPE *)vd + H(i)) = DO_OP(s2, (ETYPE)(target_long)s1, carry);\
    }                                                                    \
}

GEN_VEXT_VADC_VXM(vadc_vxm_b, uint8_t,  H1, DO_VADC)
GEN_VEXT_VADC_VXM(vadc_vxm_h, uint16_t, H2, DO_VADC)
GEN_VEXT_VADC_VXM(vadc_vxm_w, uint32_t, H4, DO_VADC)
GEN_VEXT_VADC_VXM(vadc_vxm_d, uint64_t, H8, DO_VADC)

GEN_VEXT_VADC_VXM(vsbc_vxm_b, uint8_t,  H1, DO_VSBC)
GEN_VEXT_VADC_VXM(vsbc_vxm_h, uint16_t, H2, DO_VSBC)
GEN_VEXT_VADC_VXM(vsbc_vxm_w, uint32_t, H4, DO_VSBC)
GEN_VEXT_VADC_VXM(vsbc_vxm_d, uint64_t, H8, DO_VSBC)

#define DO_MADC(N, M, C) (C ? (__typeof(N))(N + M + 1) <= N :           \
                          (__typeof(N))(N + M) < N)
#define DO_MSBC(N, M, C) (C ? N <= M : N < M)

#define GEN_VEXT_VMADC_VVM(NAME, ETYPE, H, DO_OP)             \
void HELPER(NAME)(void *vd, void *v0, void *vs1, void *vs2,   \
                  CPURISCVState *env, uint32_t desc)          \
{                                                             \
    uint32_t vl = env->vl;                                    \
    uint32_t vlmax = vext_maxsz(desc) / sizeof(ETYPE);        \
    uint32_t i;                                               \
                                                              \
    for (i = 0; i < vl; i++) {                                \
        ETYPE s1 = *((ETYPE *)vs1 + H(i));                    \
        ETYPE s2 = *((ETYPE *)vs2 + H(i));                    \
        uint8_t carry = vext_elem_mask(v0, i);                \
                                                              \
        vext_set_elem_mask(vd, i, DO_OP(s2, s1, carry));      \
    }                                                         \
    for (; i < vlmax; i++) {                                  \
        vext_set_elem_mask(vd, i, 0);                         \
    }                                                         \
}

GEN_VEXT_VMADC_VVM(vmadc_vvm_b, uint8_t,  H1, DO_MADC)
GEN_VEXT_VMADC_VVM(vmadc_vvm_h, uint16_t, H2, DO_MADC)
GEN_VEXT_VMADC_VVM(vmadc_vvm_w, uint32_t, H4, DO_MADC)
GEN_VEXT_VMADC_VVM(vmadc_vvm_d, uint64_t, H8, DO_MADC)

GEN_VEXT_VMADC_VVM(vmsbc_vvm_b, uint8_t,  H1, DO_MSBC)
GEN_VEXT_VMADC_VVM(vmsbc_vvm_h, uint16_t, H2, DO_MSBC)
GEN_VEXT_VMADC_VVM(vmsbc_vvm_w, uint32_t, H4, DO_MSBC)
GEN_VEXT_VMADC_VVM(vmsbc_vvm_d, uint64_t, H8, DO_MSBC)

#define GEN_VEXT_VMADC_VXM(NAME, ETYPE, H, DO_OP)               \
void HELPER(NAME)(void *vd, void *v0, target_ulong s1,          \
                  void *vs2, CPURISCVState *env, uint32_t desc) \
{                                                               \
    uint32_t vl = env->vl;                                      \
    uint32_t vlmax = vext_maxsz(desc) / sizeof(ETYPE);          \
    uint32_t i;                                                 \
                                                                \
    for (i = 0; i < vl; i++) {                                  \
        ETYPE s2 = *((ETYPE *)vs2 + H(i));                      \
        uint8_t carry = vext_elem_mask(v0, i);                  \
                                                                \
        vext_set_elem_mask(vd, i,                               \
                DO_OP(s2, (ETYPE)(target_long)s1, carry));      \
    }                                                           \
    for (; i < vlmax; i++) {                                    \
        vext_set_elem_mask(vd, i, 0);                           \
    }                                                           \
}

GEN_VEXT_VMADC_VXM(vmadc_vxm_b, uint8_t,  H1, DO_MADC)
GEN_VEXT_VMADC_VXM(vmadc_vxm_h, uint16_t, H2, DO_MADC)
GEN_VEXT_VMADC_VXM(vmadc_vxm_w, uint32_t, H4, DO_MADC)
GEN_VEXT_VMADC_VXM(vmadc_vxm_d, uint64_t, H8, DO_MADC)

GEN_VEXT_VMADC_VXM(vmsbc_vxm_b, uint8_t,  H1, DO_MSBC)
GEN_VEXT_VMADC_VXM(vmsbc_vxm_h, uint16_t, H2, DO_MSBC)
GEN_VEXT_VMADC_VXM(vmsbc_vxm_w, uint32_t, H4, DO_MSBC)
GEN_VEXT_VMADC_VXM(vmsbc_vxm_d, uint64_t, H8, DO_MSBC)

/* Vector Bitwise Logical Instructions */
RVVCALL(OPIVV2, vand_vv_b, OP_SSS_B, H1, H1, H1, DO_AND)
RVVCALL(OPIVV2, vand_vv_h, OP_SSS_H, H2, H2, H2, DO_AND)
RVVCALL(OPIVV2, vand_vv_w, OP_SSS_W, H4, H4, H4, DO_AND)
RVVCALL(OPIVV2, vand_vv_d, OP_SSS_D, H8, H8, H8, DO_AND)
RVVCALL(OPIVV2, vor_vv_b, OP_SSS_B, H1, H1, H1, DO_OR)
RVVCALL(OPIVV2, vor_vv_h, OP_SSS_H, H2, H2, H2, DO_OR)
RVVCALL(OPIVV2, vor_vv_w, OP_SSS_W, H4, H4, H4, DO_OR)
RVVCALL(OPIVV2, vor_vv_d, OP_SSS_D, H8, H8, H8, DO_OR)
RVVCALL(OPIVV2, vxor_vv_b, OP_SSS_B, H1, H1, H1, DO_XOR)
RVVCALL(OPIVV2, vxor_vv_h, OP_SSS_H, H2, H2, H2, DO_XOR)
RVVCALL(OPIVV2, vxor_vv_w, OP_SSS_W, H4, H4, H4, DO_XOR)
RVVCALL(OPIVV2, vxor_vv_d, OP_SSS_D, H8, H8, H8, DO_XOR)
GEN_VEXT_VV(vand_vv_b, 1, 1)
GEN_VEXT_VV(vand_vv_h, 2, 2)
GEN_VEXT_VV(vand_vv_w, 4, 4)
GEN_VEXT_VV(vand_vv_d, 8, 8)
GEN_VEXT_VV(vor_vv_b, 1, 1)
GEN_VEXT_VV(vor_vv_h, 2, 2)
GEN_VEXT_VV(vor_vv_w, 4, 4)
GEN_VEXT_VV(vor_vv_d, 8, 8)
GEN_VEXT_VV(vxor_vv_b, 1, 1)
GEN_VEXT_VV(vxor_vv_h, 2, 2)
GEN_VEXT_VV(vxor_vv_w, 4, 4)
GEN_VEXT_VV(vxor_vv_d, 8, 8)

RVVCALL(OPIVX2, vand_vx_b, OP_SSS_B, H1, H1, DO_AND)
RVVCALL(OPIVX2, vand_vx_h, OP_SSS_H, H2, H2, DO_AND)
RVVCALL(OPIVX2, vand_vx_w, OP_SSS_W, H4, H4, DO_AND)
RVVCALL(OPIVX2, vand_vx_d, OP_SSS_D, H8, H8, DO_AND)
RVVCALL(OPIVX2, vor_vx_b, OP_SSS_B, H1, H1, DO_OR)
RVVCALL(OPIVX2, vor_vx_h, OP_SSS_H, H2, H2, DO_OR)
RVVCALL(OPIVX2, vor_vx_w, OP_SSS_W, H4, H4, DO_OR)
RVVCALL(OPIVX2, vor_vx_d, OP_SSS_D, H8, H8, DO_OR)
RVVCALL(OPIVX2, vxor_vx_b, OP_SSS_B, H1, H1, DO_XOR)
RVVCALL(OPIVX2, vxor_vx_h, OP_SSS_H, H2, H2, DO_XOR)
RVVCALL(OPIVX2, vxor_vx_w, OP_SSS_W, H4, H4, DO_XOR)
RVVCALL(OPIVX2, vxor_vx_d, OP_SSS_D, H8, H8, DO_XOR)
GEN_VEXT_VX(vand_vx_b, 1, 1)
GEN_VEXT_VX(vand_vx_h, 2, 2)
GEN_VEXT_VX(vand_vx_w, 4, 4)
GEN_VEXT_VX(vand_vx_d, 8, 8)
GEN_VEXT_VX(vor_vx_b, 1, 1)
GEN_VEXT_VX(vor_vx_h, 2, 2)
GEN_VEXT_VX(vor_vx_w, 4, 4)
GEN_VEXT_VX(vor_vx_d, 8, 8)
GEN_VEXT_VX(vxor_vx_b, 1, 1)
GEN_VEXT_VX(vxor_vx_h, 2, 2)
GEN_VEXT_VX(vxor_vx_w, 4, 4)
GEN_VEXT_VX(vxor_vx_d, 8, 8)

/* Vector Single-Width Bit Shift Instructions */
#define DO_SLL(N, M)  (N << (M))
#define DO_SRL(N, M)  (N >> (M))

/* generate the helpers for shift instructions with two vector operators */
#define GEN_VEXT_SHIFT_VV(NAME, TS1, TS2, HS1, HS2, OP, MASK)             \
void HELPER(NAME)(void *vd, void *v0, void *vs1,                          \
                  void *vs2, CPURISCVState *env, uint32_t desc)           \
{                                                                         \
    uint32_t vm = vext_vm(desc);                                          \
    uint32_t vl = env->vl;                                                \
    uint32_t i;                                                           \
                                                                          \
    for (i = 0; i < vl; i++) {                                            \
        if (!vm && !vext_elem_mask(v0, i)) {                              \
            continue;                                                     \
        }                                                                 \
        TS1 s1 = *((TS1 *)vs1 + HS1(i));                                  \
        TS2 s2 = *((TS2 *)vs2 + HS2(i));                                  \
        *((TS1 *)vd + HS1(i)) = OP(s2, s1 & MASK);                        \
    }                                                                     \
}

GEN_VEXT_SHIFT_VV(vsll_vv_b, uint8_t,  uint8_t, H1, H1, DO_SLL, 0x7)
GEN_VEXT_SHIFT_VV(vsll_vv_h, uint16_t, uint16_t, H2, H2, DO_SLL, 0xf)
GEN_VEXT_SHIFT_VV(vsll_vv_w, uint32_t, uint32_t, H4, H4, DO_SLL, 0x1f)
GEN_VEXT_SHIFT_VV(vsll_vv_d, uint64_t, uint64_t, H8, H8, DO_SLL, 0x3f)

GEN_VEXT_SHIFT_VV(vsrl_vv_b, uint8_t, uint8_t, H1, H1, DO_SRL, 0x7)
GEN_VEXT_SHIFT_VV(vsrl_vv_h, uint16_t, uint16_t, H2, H2, DO_SRL, 0xf)
GEN_VEXT_SHIFT_VV(vsrl_vv_w, uint32_t, uint32_t, H4, H4, DO_SRL, 0x1f)
GEN_VEXT_SHIFT_VV(vsrl_vv_d, uint64_t, uint64_t, H8, H8, DO_SRL, 0x3f)

GEN_VEXT_SHIFT_VV(vsra_vv_b, uint8_t,  int8_t, H1, H1, DO_SRL, 0x7)
GEN_VEXT_SHIFT_VV(vsra_vv_h, uint16_t, int16_t, H2, H2, DO_SRL, 0xf)
GEN_VEXT_SHIFT_VV(vsra_vv_w, uint32_t, int32_t, H4, H4, DO_SRL, 0x1f)
GEN_VEXT_SHIFT_VV(vsra_vv_d, uint64_t, int64_t, H8, H8, DO_SRL, 0x3f)

/* generate the helpers for shift instructions with one vector and one scalar */
#define GEN_VEXT_SHIFT_VX(NAME, TD, TS2, HD, HS2, OP, MASK) \
void HELPER(NAME)(void *vd, void *v0, target_ulong s1,      \
        void *vs2, CPURISCVState *env, uint32_t desc)       \
{                                                           \
    uint32_t vm = vext_vm(desc);                            \
    uint32_t vl = env->vl;                                  \
    uint32_t i;                                             \
                                                            \
    for (i = 0; i < vl; i++) {                              \
        if (!vm && !vext_elem_mask(v0, i)) {                \
            continue;                                       \
        }                                                   \
        TS2 s2 = *((TS2 *)vs2 + HS2(i));                    \
        *((TD *)vd + HD(i)) = OP(s2, s1 & MASK);            \
    }                                                       \
}

GEN_VEXT_SHIFT_VX(vsll_vx_b, uint8_t, int8_t, H1, H1, DO_SLL, 0x7)
GEN_VEXT_SHIFT_VX(vsll_vx_h, uint16_t, int16_t, H2, H2, DO_SLL, 0xf)
GEN_VEXT_SHIFT_VX(vsll_vx_w, uint32_t, int32_t, H4, H4, DO_SLL, 0x1f)
GEN_VEXT_SHIFT_VX(vsll_vx_d, uint64_t, int64_t, H8, H8, DO_SLL, 0x3f)

GEN_VEXT_SHIFT_VX(vsrl_vx_b, uint8_t, uint8_t, H1, H1, DO_SRL, 0x7)
GEN_VEXT_SHIFT_VX(vsrl_vx_h, uint16_t, uint16_t, H2, H2, DO_SRL, 0xf)
GEN_VEXT_SHIFT_VX(vsrl_vx_w, uint32_t, uint32_t, H4, H4, DO_SRL, 0x1f)
GEN_VEXT_SHIFT_VX(vsrl_vx_d, uint64_t, uint64_t, H8, H8, DO_SRL, 0x3f)

GEN_VEXT_SHIFT_VX(vsra_vx_b, int8_t, int8_t, H1, H1, DO_SRL, 0x7)
GEN_VEXT_SHIFT_VX(vsra_vx_h, int16_t, int16_t, H2, H2, DO_SRL, 0xf)
GEN_VEXT_SHIFT_VX(vsra_vx_w, int32_t, int32_t, H4, H4, DO_SRL, 0x1f)
GEN_VEXT_SHIFT_VX(vsra_vx_d, int64_t, int64_t, H8, H8, DO_SRL, 0x3f)

/* Vector Narrowing Integer Right Shift Instructions */
GEN_VEXT_SHIFT_VV(vnsrl_vv_b, uint8_t,  uint16_t, H1, H2, DO_SRL, 0xf)
GEN_VEXT_SHIFT_VV(vnsrl_vv_h, uint16_t, uint32_t, H2, H4, DO_SRL, 0x1f)
GEN_VEXT_SHIFT_VV(vnsrl_vv_w, uint32_t, uint64_t, H4, H8, DO_SRL, 0x3f)
GEN_VEXT_SHIFT_VV(vnsra_vv_b, uint8_t,  int16_t, H1, H2, DO_SRL, 0xf)
GEN_VEXT_SHIFT_VV(vnsra_vv_h, uint16_t, int32_t, H2, H4, DO_SRL, 0x1f)
GEN_VEXT_SHIFT_VV(vnsra_vv_w, uint32_t, int64_t, H4, H8, DO_SRL, 0x3f)
GEN_VEXT_SHIFT_VX(vnsrl_vx_b, uint8_t, uint16_t, H1, H2, DO_SRL, 0xf)
GEN_VEXT_SHIFT_VX(vnsrl_vx_h, uint16_t, uint32_t, H2, H4, DO_SRL, 0x1f)
GEN_VEXT_SHIFT_VX(vnsrl_vx_w, uint32_t, uint64_t, H4, H8, DO_SRL, 0x3f)
GEN_VEXT_SHIFT_VX(vnsra_vx_b, int8_t, int16_t, H1, H2, DO_SRL, 0xf)
GEN_VEXT_SHIFT_VX(vnsra_vx_h, int16_t, int32_t, H2, H4, DO_SRL, 0x1f)
GEN_VEXT_SHIFT_VX(vnsra_vx_w, int32_t, int64_t, H4, H8, DO_SRL, 0x3f)

/* Vector Integer Comparison Instructions */
#define DO_MSEQ(N, M) (N == M)
#define DO_MSNE(N, M) (N != M)
#define DO_MSLT(N, M) (N < M)
#define DO_MSLE(N, M) (N <= M)
#define DO_MSGT(N, M) (N > M)

#define GEN_VEXT_CMP_VV(NAME, ETYPE, H, DO_OP)                \
void HELPER(NAME)(void *vd, void *v0, void *vs1, void *vs2,   \
                  CPURISCVState *env, uint32_t desc)          \
{                                                             \
    uint32_t vm = vext_vm(desc);                              \
    uint32_t vl = env->vl;                                    \
    uint32_t vlmax = vext_maxsz(desc) / sizeof(ETYPE);        \
    uint32_t i;                                               \
                                                              \
    for (i = 0; i < vl; i++) {                                \
        ETYPE s1 = *((ETYPE *)vs1 + H(i));                    \
        ETYPE s2 = *((ETYPE *)vs2 + H(i));                    \
        if (!vm && !vext_elem_mask(v0, i)) {                  \
            continue;                                         \
        }                                                     \
        vext_set_elem_mask(vd, i, DO_OP(s2, s1));             \
    }                                                         \
    for (; i < vlmax; i++) {                                  \
        vext_set_elem_mask(vd, i, 0);                         \
    }                                                         \
}

GEN_VEXT_CMP_VV(vmseq_vv_b, uint8_t,  H1, DO_MSEQ)
GEN_VEXT_CMP_VV(vmseq_vv_h, uint16_t, H2, DO_MSEQ)
GEN_VEXT_CMP_VV(vmseq_vv_w, uint32_t, H4, DO_MSEQ)
GEN_VEXT_CMP_VV(vmseq_vv_d, uint64_t, H8, DO_MSEQ)

GEN_VEXT_CMP_VV(vmsne_vv_b, uint8_t,  H1, DO_MSNE)
GEN_VEXT_CMP_VV(vmsne_vv_h, uint16_t, H2, DO_MSNE)
GEN_VEXT_CMP_VV(vmsne_vv_w, uint32_t, H4, DO_MSNE)
GEN_VEXT_CMP_VV(vmsne_vv_d, uint64_t, H8, DO_MSNE)

GEN_VEXT_CMP_VV(vmsltu_vv_b, uint8_t,  H1, DO_MSLT)
GEN_VEXT_CMP_VV(vmsltu_vv_h, uint16_t, H2, DO_MSLT)
GEN_VEXT_CMP_VV(vmsltu_vv_w, uint32_t, H4, DO_MSLT)
GEN_VEXT_CMP_VV(vmsltu_vv_d, uint64_t, H8, DO_MSLT)

GEN_VEXT_CMP_VV(vmslt_vv_b, int8_t,  H1, DO_MSLT)
GEN_VEXT_CMP_VV(vmslt_vv_h, int16_t, H2, DO_MSLT)
GEN_VEXT_CMP_VV(vmslt_vv_w, int32_t, H4, DO_MSLT)
GEN_VEXT_CMP_VV(vmslt_vv_d, int64_t, H8, DO_MSLT)

GEN_VEXT_CMP_VV(vmsleu_vv_b, uint8_t,  H1, DO_MSLE)
GEN_VEXT_CMP_VV(vmsleu_vv_h, uint16_t, H2, DO_MSLE)
GEN_VEXT_CMP_VV(vmsleu_vv_w, uint32_t, H4, DO_MSLE)
GEN_VEXT_CMP_VV(vmsleu_vv_d, uint64_t, H8, DO_MSLE)

GEN_VEXT_CMP_VV(vmsle_vv_b, int8_t,  H1, DO_MSLE)
GEN_VEXT_CMP_VV(vmsle_vv_h, int16_t, H2, DO_MSLE)
GEN_VEXT_CMP_VV(vmsle_vv_w, int32_t, H4, DO_MSLE)
GEN_VEXT_CMP_VV(vmsle_vv_d, int64_t, H8, DO_MSLE)

#define GEN_VEXT_CMP_VX(NAME, ETYPE, H, DO_OP)                      \
void HELPER(NAME)(void *vd, void *v0, target_ulong s1, void *vs2,   \
                  CPURISCVState *env, uint32_t desc)                \
{                                                                   \
    uint32_t vm = vext_vm(desc);                                    \
    uint32_t vl = env->vl;                                          \
    uint32_t vlmax = vext_maxsz(desc) / sizeof(ETYPE);              \
    uint32_t i;                                                     \
                                                                    \
    for (i = 0; i < vl; i++) {                                      \
        ETYPE s2 = *((ETYPE *)vs2 + H(i));                          \
        if (!vm && !vext_elem_mask(v0, i)) {                        \
            continue;                                               \
        }                                                           \
        vext_set_elem_mask(vd, i,                                   \
                DO_OP(s2, (ETYPE)(target_long)s1));                 \
    }                                                               \
    for (; i < vlmax; i++) {                                        \
        vext_set_elem_mask(vd, i, 0);                               \
    }                                                               \
}

GEN_VEXT_CMP_VX(vmseq_vx_b, uint8_t,  H1, DO_MSEQ)
GEN_VEXT_CMP_VX(vmseq_vx_h, uint16_t, H2, DO_MSEQ)
GEN_VEXT_CMP_VX(vmseq_vx_w, uint32_t, H4, DO_MSEQ)
GEN_VEXT_CMP_VX(vmseq_vx_d, uint64_t, H8, DO_MSEQ)

GEN_VEXT_CMP_VX(vmsne_vx_b, uint8_t,  H1, DO_MSNE)
GEN_VEXT_CMP_VX(vmsne_vx_h, uint16_t, H2, DO_MSNE)
GEN_VEXT_CMP_VX(vmsne_vx_w, uint32_t, H4, DO_MSNE)
GEN_VEXT_CMP_VX(vmsne_vx_d, uint64_t, H8, DO_MSNE)

GEN_VEXT_CMP_VX(vmsltu_vx_b, uint8_t,  H1, DO_MSLT)
GEN_VEXT_CMP_VX(vmsltu_vx_h, uint16_t, H2, DO_MSLT)
GEN_VEXT_CMP_VX(vmsltu_vx_w, uint32_t, H4, DO_MSLT)
GEN_VEXT_CMP_VX(vmsltu_vx_d, uint64_t, H8, DO_MSLT)

GEN_VEXT_CMP_VX(vmslt_vx_b, int8_t,  H1, DO_MSLT)
GEN_VEXT_CMP_VX(vmslt_vx_h, int16_t, H2, DO_MSLT)
GEN_VEXT_CMP_VX(vmslt_vx_w, int32_t, H4, DO_MSLT)
GEN_VEXT_CMP_VX(vmslt_vx_d, int64_t, H8, DO_MSLT)

GEN_VEXT_CMP_VX(vmsleu_vx_b, uint8_t,  H1, DO_MSLE)
GEN_VEXT_CMP_VX(vmsleu_vx_h, uint16_t, H2, DO_MSLE)
GEN_VEXT_CMP_VX(vmsleu_vx_w, uint32_t, H4, DO_MSLE)
GEN_VEXT_CMP_VX(vmsleu_vx_d, uint64_t, H8, DO_MSLE)

GEN_VEXT_CMP_VX(vmsle_vx_b, int8_t,  H1, DO_MSLE)
GEN_VEXT_CMP_VX(vmsle_vx_h, int16_t, H2, DO_MSLE)
GEN_VEXT_CMP_VX(vmsle_vx_w, int32_t, H4, DO_MSLE)
GEN_VEXT_CMP_VX(vmsle_vx_d, int64_t, H8, DO_MSLE)

GEN_VEXT_CMP_VX(vmsgtu_vx_b, uint8_t,  H1, DO_MSGT)
GEN_VEXT_CMP_VX(vmsgtu_vx_h, uint16_t, H2, DO_MSGT)
GEN_VEXT_CMP_VX(vmsgtu_vx_w, uint32_t, H4, DO_MSGT)
GEN_VEXT_CMP_VX(vmsgtu_vx_d, uint64_t, H8, DO_MSGT)

GEN_VEXT_CMP_VX(vmsgt_vx_b, int8_t,  H1, DO_MSGT)
GEN_VEXT_CMP_VX(vmsgt_vx_h, int16_t, H2, DO_MSGT)
GEN_VEXT_CMP_VX(vmsgt_vx_w, int32_t, H4, DO_MSGT)
GEN_VEXT_CMP_VX(vmsgt_vx_d, int64_t, H8, DO_MSGT)

/* Vector Integer Min/Max Instructions */
RVVCALL(OPIVV2, vminu_vv_b, OP_UUU_B, H1, H1, H1, DO_MIN)
RVVCALL(OPIVV2, vminu_vv_h, OP_UUU_H, H2, H2, H2, DO_MIN)
RVVCALL(OPIVV2, vminu_vv_w, OP_UUU_W, H4, H4, H4, DO_MIN)
RVVCALL(OPIVV2, vminu_vv_d, OP_UUU_D, H8, H8, H8, DO_MIN)
RVVCALL(OPIVV2, vmin_vv_b, OP_SSS_B, H1, H1, H1, DO_MIN)
RVVCALL(OPIVV2, vmin_vv_h, OP_SSS_H, H2, H2, H2, DO_MIN)
RVVCALL(OPIVV2, vmin_vv_w, OP_SSS_W, H4, H4, H4, DO_MIN)
RVVCALL(OPIVV2, vmin_vv_d, OP_SSS_D, H8, H8, H8, DO_MIN)
RVVCALL(OPIVV2, vmaxu_vv_b, OP_UUU_B, H1, H1, H1, DO_MAX)
RVVCALL(OPIVV2, vmaxu_vv_h, OP_UUU_H, H2, H2, H2, DO_MAX)
RVVCALL(OPIVV2, vmaxu_vv_w, OP_UUU_W, H4, H4, H4, DO_MAX)
RVVCALL(OPIVV2, vmaxu_vv_d, OP_UUU_D, H8, H8, H8, DO_MAX)
RVVCALL(OPIVV2, vmax_vv_b, OP_SSS_B, H1, H1, H1, DO_MAX)
RVVCALL(OPIVV2, vmax_vv_h, OP_SSS_H, H2, H2, H2, DO_MAX)
RVVCALL(OPIVV2, vmax_vv_w, OP_SSS_W, H4, H4, H4, DO_MAX)
RVVCALL(OPIVV2, vmax_vv_d, OP_SSS_D, H8, H8, H8, DO_MAX)
GEN_VEXT_VV(vminu_vv_b, 1, 1)
GEN_VEXT_VV(vminu_vv_h, 2, 2)
GEN_VEXT_VV(vminu_vv_w, 4, 4)
GEN_VEXT_VV(vminu_vv_d, 8, 8)
GEN_VEXT_VV(vmin_vv_b, 1, 1)
GEN_VEXT_VV(vmin_vv_h, 2, 2)
GEN_VEXT_VV(vmin_vv_w, 4, 4)
GEN_VEXT_VV(vmin_vv_d, 8, 8)
GEN_VEXT_VV(vmaxu_vv_b, 1, 1)
GEN_VEXT_VV(vmaxu_vv_h, 2, 2)
GEN_VEXT_VV(vmaxu_vv_w, 4, 4)
GEN_VEXT_VV(vmaxu_vv_d, 8, 8)
GEN_VEXT_VV(vmax_vv_b, 1, 1)
GEN_VEXT_VV(vmax_vv_h, 2, 2)
GEN_VEXT_VV(vmax_vv_w, 4, 4)
GEN_VEXT_VV(vmax_vv_d, 8, 8)

RVVCALL(OPIVX2, vminu_vx_b, OP_UUU_B, H1, H1, DO_MIN)
RVVCALL(OPIVX2, vminu_vx_h, OP_UUU_H, H2, H2, DO_MIN)
RVVCALL(OPIVX2, vminu_vx_w, OP_UUU_W, H4, H4, DO_MIN)
RVVCALL(OPIVX2, vminu_vx_d, OP_UUU_D, H8, H8, DO_MIN)
RVVCALL(OPIVX2, vmin_vx_b, OP_SSS_B, H1, H1, DO_MIN)
RVVCALL(OPIVX2, vmin_vx_h, OP_SSS_H, H2, H2, DO_MIN)
RVVCALL(OPIVX2, vmin_vx_w, OP_SSS_W, H4, H4, DO_MIN)
RVVCALL(OPIVX2, vmin_vx_d, OP_SSS_D, H8, H8, DO_MIN)
RVVCALL(OPIVX2, vmaxu_vx_b, OP_UUU_B, H1, H1, DO_MAX)
RVVCALL(OPIVX2, vmaxu_vx_h, OP_UUU_H, H2, H2, DO_MAX)
RVVCALL(OPIVX2, vmaxu_vx_w, OP_UUU_W, H4, H4, DO_MAX)
RVVCALL(OPIVX2, vmaxu_vx_d, OP_UUU_D, H8, H8, DO_MAX)
RVVCALL(OPIVX2, vmax_vx_b, OP_SSS_B, H1, H1, DO_MAX)
RVVCALL(OPIVX2, vmax_vx_h, OP_SSS_H, H2, H2, DO_MAX)
RVVCALL(OPIVX2, vmax_vx_w, OP_SSS_W, H4, H4, DO_MAX)
RVVCALL(OPIVX2, vmax_vx_d, OP_SSS_D, H8, H8, DO_MAX)
GEN_VEXT_VX(vminu_vx_b, 1, 1)
GEN_VEXT_VX(vminu_vx_h, 2, 2)
GEN_VEXT_VX(vminu_vx_w, 4, 4)
GEN_VEXT_VX(vminu_vx_d, 8, 8)
GEN_VEXT_VX(vmin_vx_b, 1, 1)
GEN_VEXT_VX(vmin_vx_h, 2, 2)
GEN_VEXT_VX(vmin_vx_w, 4, 4)
GEN_VEXT_VX(vmin_vx_d, 8, 8)
GEN_VEXT_VX(vmaxu_vx_b, 1, 1)
GEN_VEXT_VX(vmaxu_vx_h, 2, 2)
GEN_VEXT_VX(vmaxu_vx_w, 4, 4)
GEN_VEXT_VX(vmaxu_vx_d, 8, 8)
GEN_VEXT_VX(vmax_vx_b, 1, 1)
GEN_VEXT_VX(vmax_vx_h, 2, 2)
GEN_VEXT_VX(vmax_vx_w, 4, 4)
GEN_VEXT_VX(vmax_vx_d, 8, 8)

/* Vector Single-Width Integer Multiply Instructions */
#define DO_MUL(N, M) (N * M)
RVVCALL(OPIVV2, vmul_vv_b, OP_SSS_B, H1, H1, H1, DO_MUL)
RVVCALL(OPIVV2, vmul_vv_h, OP_SSS_H, H2, H2, H2, DO_MUL)
RVVCALL(OPIVV2, vmul_vv_w, OP_SSS_W, H4, H4, H4, DO_MUL)
RVVCALL(OPIVV2, vmul_vv_d, OP_SSS_D, H8, H8, H8, DO_MUL)
GEN_VEXT_VV(vmul_vv_b, 1, 1)
GEN_VEXT_VV(vmul_vv_h, 2, 2)
GEN_VEXT_VV(vmul_vv_w, 4, 4)
GEN_VEXT_VV(vmul_vv_d, 8, 8)

static int8_t do_mulh_b(int8_t s2, int8_t s1)
{
    return (int16_t)s2 * (int16_t)s1 >> 8;
}

static int16_t do_mulh_h(int16_t s2, int16_t s1)
{
    return (int32_t)s2 * (int32_t)s1 >> 16;
}

static int32_t do_mulh_w(int32_t s2, int32_t s1)
{
    return (int64_t)s2 * (int64_t)s1 >> 32;
}

static int64_t do_mulh_d(int64_t s2, int64_t s1)
{
    uint64_t hi_64, lo_64;

    muls64(&lo_64, &hi_64, s1, s2);
    return hi_64;
}

static uint8_t do_mulhu_b(uint8_t s2, uint8_t s1)
{
    return (uint16_t)s2 * (uint16_t)s1 >> 8;
}

static uint16_t do_mulhu_h(uint16_t s2, uint16_t s1)
{
    return (uint32_t)s2 * (uint32_t)s1 >> 16;
}

static uint32_t do_mulhu_w(uint32_t s2, uint32_t s1)
{
    return (uint64_t)s2 * (uint64_t)s1 >> 32;
}

static uint64_t do_mulhu_d(uint64_t s2, uint64_t s1)
{
    uint64_t hi_64, lo_64;

    mulu64(&lo_64, &hi_64, s2, s1);
    return hi_64;
}

static int8_t do_mulhsu_b(int8_t s2, uint8_t s1)
{
    return (int16_t)s2 * (uint16_t)s1 >> 8;
}

static int16_t do_mulhsu_h(int16_t s2, uint16_t s1)
{
    return (int32_t)s2 * (uint32_t)s1 >> 16;
}

static int32_t do_mulhsu_w(int32_t s2, uint32_t s1)
{
    return (int64_t)s2 * (uint64_t)s1 >> 32;
}

/*
 * Let  A = signed operand,
 *      B = unsigned operand
 *      P = mulu64(A, B), unsigned product
 *
 * LET  X = 2 ** 64  - A, 2's complement of A
 *      SP = signed product
 * THEN
 *      IF A < 0
 *          SP = -X * B
 *             = -(2 ** 64 - A) * B
 *             = A * B - 2 ** 64 * B
 *             = P - 2 ** 64 * B
 *      ELSE
 *          SP = P
 * THEN
 *      HI_P -= (A < 0 ? B : 0)
 */

static int64_t do_mulhsu_d(int64_t s2, uint64_t s1)
{
    uint64_t hi_64, lo_64;

    mulu64(&lo_64, &hi_64, s2, s1);

    hi_64 -= s2 < 0 ? s1 : 0;
    return hi_64;
}

RVVCALL(OPIVV2, vmulh_vv_b, OP_SSS_B, H1, H1, H1, do_mulh_b)
RVVCALL(OPIVV2, vmulh_vv_h, OP_SSS_H, H2, H2, H2, do_mulh_h)
RVVCALL(OPIVV2, vmulh_vv_w, OP_SSS_W, H4, H4, H4, do_mulh_w)
RVVCALL(OPIVV2, vmulh_vv_d, OP_SSS_D, H8, H8, H8, do_mulh_d)
RVVCALL(OPIVV2, vmulhu_vv_b, OP_UUU_B, H1, H1, H1, do_mulhu_b)
RVVCALL(OPIVV2, vmulhu_vv_h, OP_UUU_H, H2, H2, H2, do_mulhu_h)
RVVCALL(OPIVV2, vmulhu_vv_w, OP_UUU_W, H4, H4, H4, do_mulhu_w)
RVVCALL(OPIVV2, vmulhu_vv_d, OP_UUU_D, H8, H8, H8, do_mulhu_d)
RVVCALL(OPIVV2, vmulhsu_vv_b, OP_SUS_B, H1, H1, H1, do_mulhsu_b)
RVVCALL(OPIVV2, vmulhsu_vv_h, OP_SUS_H, H2, H2, H2, do_mulhsu_h)
RVVCALL(OPIVV2, vmulhsu_vv_w, OP_SUS_W, H4, H4, H4, do_mulhsu_w)
RVVCALL(OPIVV2, vmulhsu_vv_d, OP_SUS_D, H8, H8, H8, do_mulhsu_d)
GEN_VEXT_VV(vmulh_vv_b, 1, 1)
GEN_VEXT_VV(vmulh_vv_h, 2, 2)
GEN_VEXT_VV(vmulh_vv_w, 4, 4)
GEN_VEXT_VV(vmulh_vv_d, 8, 8)
GEN_VEXT_VV(vmulhu_vv_b, 1, 1)
GEN_VEXT_VV(vmulhu_vv_h, 2, 2)
GEN_VEXT_VV(vmulhu_vv_w, 4, 4)
GEN_VEXT_VV(vmulhu_vv_d, 8, 8)
GEN_VEXT_VV(vmulhsu_vv_b, 1, 1)
GEN_VEXT_VV(vmulhsu_vv_h, 2, 2)
GEN_VEXT_VV(vmulhsu_vv_w, 4, 4)
GEN_VEXT_VV(vmulhsu_vv_d, 8, 8)

RVVCALL(OPIVX2, vmul_vx_b, OP_SSS_B, H1, H1, DO_MUL)
RVVCALL(OPIVX2, vmul_vx_h, OP_SSS_H, H2, H2, DO_MUL)
RVVCALL(OPIVX2, vmul_vx_w, OP_SSS_W, H4, H4, DO_MUL)
RVVCALL(OPIVX2, vmul_vx_d, OP_SSS_D, H8, H8, DO_MUL)
RVVCALL(OPIVX2, vmulh_vx_b, OP_SSS_B, H1, H1, do_mulh_b)
RVVCALL(OPIVX2, vmulh_vx_h, OP_SSS_H, H2, H2, do_mulh_h)
RVVCALL(OPIVX2, vmulh_vx_w, OP_SSS_W, H4, H4, do_mulh_w)
RVVCALL(OPIVX2, vmulh_vx_d, OP_SSS_D, H8, H8, do_mulh_d)
RVVCALL(OPIVX2, vmulhu_vx_b, OP_UUU_B, H1, H1, do_mulhu_b)
RVVCALL(OPIVX2, vmulhu_vx_h, OP_UUU_H, H2, H2, do_mulhu_h)
RVVCALL(OPIVX2, vmulhu_vx_w, OP_UUU_W, H4, H4, do_mulhu_w)
RVVCALL(OPIVX2, vmulhu_vx_d, OP_UUU_D, H8, H8, do_mulhu_d)
RVVCALL(OPIVX2, vmulhsu_vx_b, OP_SUS_B, H1, H1, do_mulhsu_b)
RVVCALL(OPIVX2, vmulhsu_vx_h, OP_SUS_H, H2, H2, do_mulhsu_h)
RVVCALL(OPIVX2, vmulhsu_vx_w, OP_SUS_W, H4, H4, do_mulhsu_w)
RVVCALL(OPIVX2, vmulhsu_vx_d, OP_SUS_D, H8, H8, do_mulhsu_d)
GEN_VEXT_VX(vmul_vx_b, 1, 1)
GEN_VEXT_VX(vmul_vx_h, 2, 2)
GEN_VEXT_VX(vmul_vx_w, 4, 4)
GEN_VEXT_VX(vmul_vx_d, 8, 8)
GEN_VEXT_VX(vmulh_vx_b, 1, 1)
GEN_VEXT_VX(vmulh_vx_h, 2, 2)
GEN_VEXT_VX(vmulh_vx_w, 4, 4)
GEN_VEXT_VX(vmulh_vx_d, 8, 8)
GEN_VEXT_VX(vmulhu_vx_b, 1, 1)
GEN_VEXT_VX(vmulhu_vx_h, 2, 2)
GEN_VEXT_VX(vmulhu_vx_w, 4, 4)
GEN_VEXT_VX(vmulhu_vx_d, 8, 8)
GEN_VEXT_VX(vmulhsu_vx_b, 1, 1)
GEN_VEXT_VX(vmulhsu_vx_h, 2, 2)
GEN_VEXT_VX(vmulhsu_vx_w, 4, 4)
GEN_VEXT_VX(vmulhsu_vx_d, 8, 8)

/* Vector Integer Divide Instructions */
#define DO_DIVU(N, M) (unlikely(M == 0) ? (__typeof(N))(-1) : N / M)
#define DO_REMU(N, M) (unlikely(M == 0) ? N : N % M)
#define DO_DIV(N, M)  (unlikely(M == 0) ? (__typeof(N))(-1) :\
        unlikely((N == -N) && (M == (__typeof(N))(-1))) ? N : N / M)
#define DO_REM(N, M)  (unlikely(M == 0) ? N :\
        unlikely((N == -N) && (M == (__typeof(N))(-1))) ? 0 : N % M)

RVVCALL(OPIVV2, vdivu_vv_b, OP_UUU_B, H1, H1, H1, DO_DIVU)
RVVCALL(OPIVV2, vdivu_vv_h, OP_UUU_H, H2, H2, H2, DO_DIVU)
RVVCALL(OPIVV2, vdivu_vv_w, OP_UUU_W, H4, H4, H4, DO_DIVU)
RVVCALL(OPIVV2, vdivu_vv_d, OP_UUU_D, H8, H8, H8, DO_DIVU)
RVVCALL(OPIVV2, vdiv_vv_b, OP_SSS_B, H1, H1, H1, DO_DIV)
RVVCALL(OPIVV2, vdiv_vv_h, OP_SSS_H, H2, H2, H2, DO_DIV)
RVVCALL(OPIVV2, vdiv_vv_w, OP_SSS_W, H4, H4, H4, DO_DIV)
RVVCALL(OPIVV2, vdiv_vv_d, OP_SSS_D, H8, H8, H8, DO_DIV)
RVVCALL(OPIVV2, vremu_vv_b, OP_UUU_B, H1, H1, H1, DO_REMU)
RVVCALL(OPIVV2, vremu_vv_h, OP_UUU_H, H2, H2, H2, DO_REMU)
RVVCALL(OPIVV2, vremu_vv_w, OP_UUU_W, H4, H4, H4, DO_REMU)
RVVCALL(OPIVV2, vremu_vv_d, OP_UUU_D, H8, H8, H8, DO_REMU)
RVVCALL(OPIVV2, vrem_vv_b, OP_SSS_B, H1, H1, H1, DO_REM)
RVVCALL(OPIVV2, vrem_vv_h, OP_SSS_H, H2, H2, H2, DO_REM)
RVVCALL(OPIVV2, vrem_vv_w, OP_SSS_W, H4, H4, H4, DO_REM)
RVVCALL(OPIVV2, vrem_vv_d, OP_SSS_D, H8, H8, H8, DO_REM)
GEN_VEXT_VV(vdivu_vv_b, 1, 1)
GEN_VEXT_VV(vdivu_vv_h, 2, 2)
GEN_VEXT_VV(vdivu_vv_w, 4, 4)
GEN_VEXT_VV(vdivu_vv_d, 8, 8)
GEN_VEXT_VV(vdiv_vv_b, 1, 1)
GEN_VEXT_VV(vdiv_vv_h, 2, 2)
GEN_VEXT_VV(vdiv_vv_w, 4, 4)
GEN_VEXT_VV(vdiv_vv_d, 8, 8)
GEN_VEXT_VV(vremu_vv_b, 1, 1)
GEN_VEXT_VV(vremu_vv_h, 2, 2)
GEN_VEXT_VV(vremu_vv_w, 4, 4)
GEN_VEXT_VV(vremu_vv_d, 8, 8)
GEN_VEXT_VV(vrem_vv_b, 1, 1)
GEN_VEXT_VV(vrem_vv_h, 2, 2)
GEN_VEXT_VV(vrem_vv_w, 4, 4)
GEN_VEXT_VV(vrem_vv_d, 8, 8)

RVVCALL(OPIVX2, vdivu_vx_b, OP_UUU_B, H1, H1, DO_DIVU)
RVVCALL(OPIVX2, vdivu_vx_h, OP_UUU_H, H2, H2, DO_DIVU)
RVVCALL(OPIVX2, vdivu_vx_w, OP_UUU_W, H4, H4, DO_DIVU)
RVVCALL(OPIVX2, vdivu_vx_d, OP_UUU_D, H8, H8, DO_DIVU)
RVVCALL(OPIVX2, vdiv_vx_b, OP_SSS_B, H1, H1, DO_DIV)
RVVCALL(OPIVX2, vdiv_vx_h, OP_SSS_H, H2, H2, DO_DIV)
RVVCALL(OPIVX2, vdiv_vx_w, OP_SSS_W, H4, H4, DO_DIV)
RVVCALL(OPIVX2, vdiv_vx_d, OP_SSS_D, H8, H8, DO_DIV)
RVVCALL(OPIVX2, vremu_vx_b, OP_UUU_B, H1, H1, DO_REMU)
RVVCALL(OPIVX2, vremu_vx_h, OP_UUU_H, H2, H2, DO_REMU)
RVVCALL(OPIVX2, vremu_vx_w, OP_UUU_W, H4, H4, DO_REMU)
RVVCALL(OPIVX2, vremu_vx_d, OP_UUU_D, H8, H8, DO_REMU)
RVVCALL(OPIVX2, vrem_vx_b, OP_SSS_B, H1, H1, DO_REM)
RVVCALL(OPIVX2, vrem_vx_h, OP_SSS_H, H2, H2, DO_REM)
RVVCALL(OPIVX2, vrem_vx_w, OP_SSS_W, H4, H4, DO_REM)
RVVCALL(OPIVX2, vrem_vx_d, OP_SSS_D, H8, H8, DO_REM)
GEN_VEXT_VX(vdivu_vx_b, 1, 1)
GEN_VEXT_VX(vdivu_vx_h, 2, 2)
GEN_VEXT_VX(vdivu_vx_w, 4, 4)
GEN_VEXT_VX(vdivu_vx_d, 8, 8)
GEN_VEXT_VX(vdiv_vx_b, 1, 1)
GEN_VEXT_VX(vdiv_vx_h, 2, 2)
GEN_VEXT_VX(vdiv_vx_w, 4, 4)
GEN_VEXT_VX(vdiv_vx_d, 8, 8)
GEN_VEXT_VX(vremu_vx_b, 1, 1)
GEN_VEXT_VX(vremu_vx_h, 2, 2)
GEN_VEXT_VX(vremu_vx_w, 4, 4)
GEN_VEXT_VX(vremu_vx_d, 8, 8)
GEN_VEXT_VX(vrem_vx_b, 1, 1)
GEN_VEXT_VX(vrem_vx_h, 2, 2)
GEN_VEXT_VX(vrem_vx_w, 4, 4)
GEN_VEXT_VX(vrem_vx_d, 8, 8)

/* Vector Widening Integer Multiply Instructions */
RVVCALL(OPIVV2, vwmul_vv_b, WOP_SSS_B, H2, H1, H1, DO_MUL)
RVVCALL(OPIVV2, vwmul_vv_h, WOP_SSS_H, H4, H2, H2, DO_MUL)
RVVCALL(OPIVV2, vwmul_vv_w, WOP_SSS_W, H8, H4, H4, DO_MUL)
RVVCALL(OPIVV2, vwmulu_vv_b, WOP_UUU_B, H2, H1, H1, DO_MUL)
RVVCALL(OPIVV2, vwmulu_vv_h, WOP_UUU_H, H4, H2, H2, DO_MUL)
RVVCALL(OPIVV2, vwmulu_vv_w, WOP_UUU_W, H8, H4, H4, DO_MUL)
RVVCALL(OPIVV2, vwmulsu_vv_b, WOP_SUS_B, H2, H1, H1, DO_MUL)
RVVCALL(OPIVV2, vwmulsu_vv_h, WOP_SUS_H, H4, H2, H2, DO_MUL)
RVVCALL(OPIVV2, vwmulsu_vv_w, WOP_SUS_W, H8, H4, H4, DO_MUL)
GEN_VEXT_VV(vwmul_vv_b, 1, 2)
GEN_VEXT_VV(vwmul_vv_h, 2, 4)
GEN_VEXT_VV(vwmul_vv_w, 4, 8)
GEN_VEXT_VV(vwmulu_vv_b, 1, 2)
GEN_VEXT_VV(vwmulu_vv_h, 2, 4)
GEN_VEXT_VV(vwmulu_vv_w, 4, 8)
GEN_VEXT_VV(vwmulsu_vv_b, 1, 2)
GEN_VEXT_VV(vwmulsu_vv_h, 2, 4)
GEN_VEXT_VV(vwmulsu_vv_w, 4, 8)

RVVCALL(OPIVX2, vwmul_vx_b, WOP_SSS_B, H2, H1, DO_MUL)
RVVCALL(OPIVX2, vwmul_vx_h, WOP_SSS_H, H4, H2, DO_MUL)
RVVCALL(OPIVX2, vwmul_vx_w, WOP_SSS_W, H8, H4, DO_MUL)
RVVCALL(OPIVX2, vwmulu_vx_b, WOP_UUU_B, H2, H1, DO_MUL)
RVVCALL(OPIVX2, vwmulu_vx_h, WOP_UUU_H, H4, H2, DO_MUL)
RVVCALL(OPIVX2, vwmulu_vx_w, WOP_UUU_W, H8, H4, DO_MUL)
RVVCALL(OPIVX2, vwmulsu_vx_b, WOP_SUS_B, H2, H1, DO_MUL)
RVVCALL(OPIVX2, vwmulsu_vx_h, WOP_SUS_H, H4, H2, DO_MUL)
RVVCALL(OPIVX2, vwmulsu_vx_w, WOP_SUS_W, H8, H4, DO_MUL)
GEN_VEXT_VX(vwmul_vx_b, 1, 2)
GEN_VEXT_VX(vwmul_vx_h, 2, 4)
GEN_VEXT_VX(vwmul_vx_w, 4, 8)
GEN_VEXT_VX(vwmulu_vx_b, 1, 2)
GEN_VEXT_VX(vwmulu_vx_h, 2, 4)
GEN_VEXT_VX(vwmulu_vx_w, 4, 8)
GEN_VEXT_VX(vwmulsu_vx_b, 1, 2)
GEN_VEXT_VX(vwmulsu_vx_h, 2, 4)
GEN_VEXT_VX(vwmulsu_vx_w, 4, 8)

/* Vector Single-Width Integer Multiply-Add Instructions */
#define OPIVV3(NAME, TD, T1, T2, TX1, TX2, HD, HS1, HS2, OP)   \
static void do_##NAME(void *vd, void *vs1, void *vs2, int i)       \
{                                                                  \
    TX1 s1 = *((T1 *)vs1 + HS1(i));                                \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                                \
    TD d = *((TD *)vd + HD(i));                                    \
    *((TD *)vd + HD(i)) = OP(s2, s1, d);                           \
}

#define DO_MACC(N, M, D) (M * N + D)
#define DO_NMSAC(N, M, D) (-(M * N) + D)
#define DO_MADD(N, M, D) (M * D + N)
#define DO_NMSUB(N, M, D) (-(M * D) + N)
RVVCALL(OPIVV3, vmacc_vv_b, OP_SSS_B, H1, H1, H1, DO_MACC)
RVVCALL(OPIVV3, vmacc_vv_h, OP_SSS_H, H2, H2, H2, DO_MACC)
RVVCALL(OPIVV3, vmacc_vv_w, OP_SSS_W, H4, H4, H4, DO_MACC)
RVVCALL(OPIVV3, vmacc_vv_d, OP_SSS_D, H8, H8, H8, DO_MACC)
RVVCALL(OPIVV3, vnmsac_vv_b, OP_SSS_B, H1, H1, H1, DO_NMSAC)
RVVCALL(OPIVV3, vnmsac_vv_h, OP_SSS_H, H2, H2, H2, DO_NMSAC)
RVVCALL(OPIVV3, vnmsac_vv_w, OP_SSS_W, H4, H4, H4, DO_NMSAC)
RVVCALL(OPIVV3, vnmsac_vv_d, OP_SSS_D, H8, H8, H8, DO_NMSAC)
RVVCALL(OPIVV3, vmadd_vv_b, OP_SSS_B, H1, H1, H1, DO_MADD)
RVVCALL(OPIVV3, vmadd_vv_h, OP_SSS_H, H2, H2, H2, DO_MADD)
RVVCALL(OPIVV3, vmadd_vv_w, OP_SSS_W, H4, H4, H4, DO_MADD)
RVVCALL(OPIVV3, vmadd_vv_d, OP_SSS_D, H8, H8, H8, DO_MADD)
RVVCALL(OPIVV3, vnmsub_vv_b, OP_SSS_B, H1, H1, H1, DO_NMSUB)
RVVCALL(OPIVV3, vnmsub_vv_h, OP_SSS_H, H2, H2, H2, DO_NMSUB)
RVVCALL(OPIVV3, vnmsub_vv_w, OP_SSS_W, H4, H4, H4, DO_NMSUB)
RVVCALL(OPIVV3, vnmsub_vv_d, OP_SSS_D, H8, H8, H8, DO_NMSUB)
GEN_VEXT_VV(vmacc_vv_b, 1, 1)
GEN_VEXT_VV(vmacc_vv_h, 2, 2)
GEN_VEXT_VV(vmacc_vv_w, 4, 4)
GEN_VEXT_VV(vmacc_vv_d, 8, 8)
GEN_VEXT_VV(vnmsac_vv_b, 1, 1)
GEN_VEXT_VV(vnmsac_vv_h, 2, 2)
GEN_VEXT_VV(vnmsac_vv_w, 4, 4)
GEN_VEXT_VV(vnmsac_vv_d, 8, 8)
GEN_VEXT_VV(vmadd_vv_b, 1, 1)
GEN_VEXT_VV(vmadd_vv_h, 2, 2)
GEN_VEXT_VV(vmadd_vv_w, 4, 4)
GEN_VEXT_VV(vmadd_vv_d, 8, 8)
GEN_VEXT_VV(vnmsub_vv_b, 1, 1)
GEN_VEXT_VV(vnmsub_vv_h, 2, 2)
GEN_VEXT_VV(vnmsub_vv_w, 4, 4)
GEN_VEXT_VV(vnmsub_vv_d, 8, 8)

#define OPIVX3(NAME, TD, T1, T2, TX1, TX2, HD, HS2, OP)             \
static void do_##NAME(void *vd, target_long s1, void *vs2, int i)   \
{                                                                   \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                                 \
    TD d = *((TD *)vd + HD(i));                                     \
    *((TD *)vd + HD(i)) = OP(s2, (TX1)(T1)s1, d);                   \
}

RVVCALL(OPIVX3, vmacc_vx_b, OP_SSS_B, H1, H1, DO_MACC)
RVVCALL(OPIVX3, vmacc_vx_h, OP_SSS_H, H2, H2, DO_MACC)
RVVCALL(OPIVX3, vmacc_vx_w, OP_SSS_W, H4, H4, DO_MACC)
RVVCALL(OPIVX3, vmacc_vx_d, OP_SSS_D, H8, H8, DO_MACC)
RVVCALL(OPIVX3, vnmsac_vx_b, OP_SSS_B, H1, H1, DO_NMSAC)
RVVCALL(OPIVX3, vnmsac_vx_h, OP_SSS_H, H2, H2, DO_NMSAC)
RVVCALL(OPIVX3, vnmsac_vx_w, OP_SSS_W, H4, H4, DO_NMSAC)
RVVCALL(OPIVX3, vnmsac_vx_d, OP_SSS_D, H8, H8, DO_NMSAC)
RVVCALL(OPIVX3, vmadd_vx_b, OP_SSS_B, H1, H1, DO_MADD)
RVVCALL(OPIVX3, vmadd_vx_h, OP_SSS_H, H2, H2, DO_MADD)
RVVCALL(OPIVX3, vmadd_vx_w, OP_SSS_W, H4, H4, DO_MADD)
RVVCALL(OPIVX3, vmadd_vx_d, OP_SSS_D, H8, H8, DO_MADD)
RVVCALL(OPIVX3, vnmsub_vx_b, OP_SSS_B, H1, H1, DO_NMSUB)
RVVCALL(OPIVX3, vnmsub_vx_h, OP_SSS_H, H2, H2, DO_NMSUB)
RVVCALL(OPIVX3, vnmsub_vx_w, OP_SSS_W, H4, H4, DO_NMSUB)
RVVCALL(OPIVX3, vnmsub_vx_d, OP_SSS_D, H8, H8, DO_NMSUB)
GEN_VEXT_VX(vmacc_vx_b, 1, 1)
GEN_VEXT_VX(vmacc_vx_h, 2, 2)
GEN_VEXT_VX(vmacc_vx_w, 4, 4)
GEN_VEXT_VX(vmacc_vx_d, 8, 8)
GEN_VEXT_VX(vnmsac_vx_b, 1, 1)
GEN_VEXT_VX(vnmsac_vx_h, 2, 2)
GEN_VEXT_VX(vnmsac_vx_w, 4, 4)
GEN_VEXT_VX(vnmsac_vx_d, 8, 8)
GEN_VEXT_VX(vmadd_vx_b, 1, 1)
GEN_VEXT_VX(vmadd_vx_h, 2, 2)
GEN_VEXT_VX(vmadd_vx_w, 4, 4)
GEN_VEXT_VX(vmadd_vx_d, 8, 8)
GEN_VEXT_VX(vnmsub_vx_b, 1, 1)
GEN_VEXT_VX(vnmsub_vx_h, 2, 2)
GEN_VEXT_VX(vnmsub_vx_w, 4, 4)
GEN_VEXT_VX(vnmsub_vx_d, 8, 8)

/* Vector Widening Integer Multiply-Add Instructions */
RVVCALL(OPIVV3, vwmaccu_vv_b, WOP_UUU_B, H2, H1, H1, DO_MACC)
RVVCALL(OPIVV3, vwmaccu_vv_h, WOP_UUU_H, H4, H2, H2, DO_MACC)
RVVCALL(OPIVV3, vwmaccu_vv_w, WOP_UUU_W, H8, H4, H4, DO_MACC)
RVVCALL(OPIVV3, vwmacc_vv_b, WOP_SSS_B, H2, H1, H1, DO_MACC)
RVVCALL(OPIVV3, vwmacc_vv_h, WOP_SSS_H, H4, H2, H2, DO_MACC)
RVVCALL(OPIVV3, vwmacc_vv_w, WOP_SSS_W, H8, H4, H4, DO_MACC)
RVVCALL(OPIVV3, vwmaccsu_vv_b, WOP_SSU_B, H2, H1, H1, DO_MACC)
RVVCALL(OPIVV3, vwmaccsu_vv_h, WOP_SSU_H, H4, H2, H2, DO_MACC)
RVVCALL(OPIVV3, vwmaccsu_vv_w, WOP_SSU_W, H8, H4, H4, DO_MACC)
GEN_VEXT_VV(vwmaccu_vv_b, 1, 2)
GEN_VEXT_VV(vwmaccu_vv_h, 2, 4)
GEN_VEXT_VV(vwmaccu_vv_w, 4, 8)
GEN_VEXT_VV(vwmacc_vv_b, 1, 2)
GEN_VEXT_VV(vwmacc_vv_h, 2, 4)
GEN_VEXT_VV(vwmacc_vv_w, 4, 8)
GEN_VEXT_VV(vwmaccsu_vv_b, 1, 2)
GEN_VEXT_VV(vwmaccsu_vv_h, 2, 4)
GEN_VEXT_VV(vwmaccsu_vv_w, 4, 8)

RVVCALL(OPIVX3, vwmaccu_vx_b, WOP_UUU_B, H2, H1, DO_MACC)
RVVCALL(OPIVX3, vwmaccu_vx_h, WOP_UUU_H, H4, H2, DO_MACC)
RVVCALL(OPIVX3, vwmaccu_vx_w, WOP_UUU_W, H8, H4, DO_MACC)
RVVCALL(OPIVX3, vwmacc_vx_b, WOP_SSS_B, H2, H1, DO_MACC)
RVVCALL(OPIVX3, vwmacc_vx_h, WOP_SSS_H, H4, H2, DO_MACC)
RVVCALL(OPIVX3, vwmacc_vx_w, WOP_SSS_W, H8, H4, DO_MACC)
RVVCALL(OPIVX3, vwmaccsu_vx_b, WOP_SSU_B, H2, H1, DO_MACC)
RVVCALL(OPIVX3, vwmaccsu_vx_h, WOP_SSU_H, H4, H2, DO_MACC)
RVVCALL(OPIVX3, vwmaccsu_vx_w, WOP_SSU_W, H8, H4, DO_MACC)
RVVCALL(OPIVX3, vwmaccus_vx_b, WOP_SUS_B, H2, H1, DO_MACC)
RVVCALL(OPIVX3, vwmaccus_vx_h, WOP_SUS_H, H4, H2, DO_MACC)
RVVCALL(OPIVX3, vwmaccus_vx_w, WOP_SUS_W, H8, H4, DO_MACC)
GEN_VEXT_VX(vwmaccu_vx_b, 1, 2)
GEN_VEXT_VX(vwmaccu_vx_h, 2, 4)
GEN_VEXT_VX(vwmaccu_vx_w, 4, 8)
GEN_VEXT_VX(vwmacc_vx_b, 1, 2)
GEN_VEXT_VX(vwmacc_vx_h, 2, 4)
GEN_VEXT_VX(vwmacc_vx_w, 4, 8)
GEN_VEXT_VX(vwmaccsu_vx_b, 1, 2)
GEN_VEXT_VX(vwmaccsu_vx_h, 2, 4)
GEN_VEXT_VX(vwmaccsu_vx_w, 4, 8)
GEN_VEXT_VX(vwmaccus_vx_b, 1, 2)
GEN_VEXT_VX(vwmaccus_vx_h, 2, 4)
GEN_VEXT_VX(vwmaccus_vx_w, 4, 8)

/* Vector Integer Merge and Move Instructions */
#define GEN_VEXT_VMV_VV(NAME, ETYPE, H)                              \
void HELPER(NAME)(void *vd, void *vs1, CPURISCVState *env,           \
                  uint32_t desc)                                     \
{                                                                    \
    uint32_t vl = env->vl;                                           \
    uint32_t i;                                                      \
                                                                     \
    for (i = 0; i < vl; i++) {                                       \
        ETYPE s1 = *((ETYPE *)vs1 + H(i));                           \
        *((ETYPE *)vd + H(i)) = s1;                                  \
    }                                                                \
}

GEN_VEXT_VMV_VV(vmv_v_v_b, int8_t,  H1)
GEN_VEXT_VMV_VV(vmv_v_v_h, int16_t, H2)
GEN_VEXT_VMV_VV(vmv_v_v_w, int32_t, H4)
GEN_VEXT_VMV_VV(vmv_v_v_d, int64_t, H8)

#define GEN_VEXT_VMV_VX(NAME, ETYPE, H)                              \
void HELPER(NAME)(void *vd, uint64_t s1, CPURISCVState *env,         \
                  uint32_t desc)                                     \
{                                                                    \
    uint32_t vl = env->vl;                                           \
    uint32_t i;                                                      \
                                                                     \
    for (i = 0; i < vl; i++) {                                       \
        *((ETYPE *)vd + H(i)) = (ETYPE)s1;                           \
    }                                                                \
}

GEN_VEXT_VMV_VX(vmv_v_x_b, int8_t,  H1)
GEN_VEXT_VMV_VX(vmv_v_x_h, int16_t, H2)
GEN_VEXT_VMV_VX(vmv_v_x_w, int32_t, H4)
GEN_VEXT_VMV_VX(vmv_v_x_d, int64_t, H8)

#define GEN_VEXT_VMERGE_VV(NAME, ETYPE, H)                           \
void HELPER(NAME)(void *vd, void *v0, void *vs1, void *vs2,          \
                  CPURISCVState *env, uint32_t desc)                 \
{                                                                    \
    uint32_t vl = env->vl;                                           \
    uint32_t i;                                                      \
                                                                     \
    for (i = 0; i < vl; i++) {                                       \
        ETYPE *vt = (!vext_elem_mask(v0, i) ? vs2 : vs1);            \
        *((ETYPE *)vd + H(i)) = *(vt + H(i));                        \
    }                                                                \
}

GEN_VEXT_VMERGE_VV(vmerge_vvm_b, int8_t,  H1)
GEN_VEXT_VMERGE_VV(vmerge_vvm_h, int16_t, H2)
GEN_VEXT_VMERGE_VV(vmerge_vvm_w, int32_t, H4)
GEN_VEXT_VMERGE_VV(vmerge_vvm_d, int64_t, H8)

#define GEN_VEXT_VMERGE_VX(NAME, ETYPE, H)                           \
void HELPER(NAME)(void *vd, void *v0, target_ulong s1,               \
                  void *vs2, CPURISCVState *env, uint32_t desc)      \
{                                                                    \
    uint32_t vl = env->vl;                                           \
    uint32_t i;                                                      \
                                                                     \
    for (i = 0; i < vl; i++) {                                       \
        ETYPE s2 = *((ETYPE *)vs2 + H(i));                           \
        ETYPE d = (!vext_elem_mask(v0, i) ? s2 :                     \
                   (ETYPE)(target_long)s1);                          \
        *((ETYPE *)vd + H(i)) = d;                                   \
    }                                                                \
}

GEN_VEXT_VMERGE_VX(vmerge_vxm_b, int8_t,  H1)
GEN_VEXT_VMERGE_VX(vmerge_vxm_h, int16_t, H2)
GEN_VEXT_VMERGE_VX(vmerge_vxm_w, int32_t, H4)
GEN_VEXT_VMERGE_VX(vmerge_vxm_d, int64_t, H8)

/*
 *** Vector Fixed-Point Arithmetic Instructions
 */

/* Vector Single-Width Saturating Add and Subtract */

/*
 * As fixed point instructions probably have round mode and saturation,
 * define common macros for fixed point here.
 */
typedef void opivv2_rm_fn(void *vd, void *vs1, void *vs2, int i,
                          CPURISCVState *env, int vxrm);

#define OPIVV2_RM(NAME, TD, T1, T2, TX1, TX2, HD, HS1, HS2, OP)     \
static inline void                                                  \
do_##NAME(void *vd, void *vs1, void *vs2, int i,                    \
          CPURISCVState *env, int vxrm)                             \
{                                                                   \
    TX1 s1 = *((T1 *)vs1 + HS1(i));                                 \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                                 \
    *((TD *)vd + HD(i)) = OP(env, vxrm, s2, s1);                    \
}

static inline void
vext_vv_rm_1(void *vd, void *v0, void *vs1, void *vs2,
             CPURISCVState *env,
             uint32_t vl, uint32_t vm, int vxrm,
             opivv2_rm_fn *fn)
{
    for (uint32_t i = 0; i < vl; i++) {
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        fn(vd, vs1, vs2, i, env, vxrm);
    }
}

static inline void
vext_vv_rm_2(void *vd, void *v0, void *vs1, void *vs2,
             CPURISCVState *env,
             uint32_t desc, uint32_t esz, uint32_t dsz,
             opivv2_rm_fn *fn)
{
    uint32_t vm = vext_vm(desc);
    uint32_t vl = env->vl;

    switch (env->vxrm) {
    case 0: /* rnu */
        vext_vv_rm_1(vd, v0, vs1, vs2,
                     env, vl, vm, 0, fn);
        break;
    case 1: /* rne */
        vext_vv_rm_1(vd, v0, vs1, vs2,
                     env, vl, vm, 1, fn);
        break;
    case 2: /* rdn */
        vext_vv_rm_1(vd, v0, vs1, vs2,
                     env, vl, vm, 2, fn);
        break;
    default: /* rod */
        vext_vv_rm_1(vd, v0, vs1, vs2,
                     env, vl, vm, 3, fn);
        break;
    }
}

/* generate helpers for fixed point instructions with OPIVV format */
#define GEN_VEXT_VV_RM(NAME, ESZ, DSZ)                          \
void HELPER(NAME)(void *vd, void *v0, void *vs1, void *vs2,     \
                  CPURISCVState *env, uint32_t desc)            \
{                                                               \
    vext_vv_rm_2(vd, v0, vs1, vs2, env, desc, ESZ, DSZ,         \
                 do_##NAME);                                    \
}

static inline uint8_t saddu8(CPURISCVState *env, int vxrm, uint8_t a, uint8_t b)
{
    uint8_t res = a + b;
    if (res < a) {
        res = UINT8_MAX;
        env->vxsat = 0x1;
    }
    return res;
}

static inline uint16_t saddu16(CPURISCVState *env, int vxrm, uint16_t a,
                               uint16_t b)
{
    uint16_t res = a + b;
    if (res < a) {
        res = UINT16_MAX;
        env->vxsat = 0x1;
    }
    return res;
}

static inline uint32_t saddu32(CPURISCVState *env, int vxrm, uint32_t a,
                               uint32_t b)
{
    uint32_t res = a + b;
    if (res < a) {
        res = UINT32_MAX;
        env->vxsat = 0x1;
    }
    return res;
}

static inline uint64_t saddu64(CPURISCVState *env, int vxrm, uint64_t a,
                               uint64_t b)
{
    uint64_t res = a + b;
    if (res < a) {
        res = UINT64_MAX;
        env->vxsat = 0x1;
    }
    return res;
}

RVVCALL(OPIVV2_RM, vsaddu_vv_b, OP_UUU_B, H1, H1, H1, saddu8)
RVVCALL(OPIVV2_RM, vsaddu_vv_h, OP_UUU_H, H2, H2, H2, saddu16)
RVVCALL(OPIVV2_RM, vsaddu_vv_w, OP_UUU_W, H4, H4, H4, saddu32)
RVVCALL(OPIVV2_RM, vsaddu_vv_d, OP_UUU_D, H8, H8, H8, saddu64)
GEN_VEXT_VV_RM(vsaddu_vv_b, 1, 1)
GEN_VEXT_VV_RM(vsaddu_vv_h, 2, 2)
GEN_VEXT_VV_RM(vsaddu_vv_w, 4, 4)
GEN_VEXT_VV_RM(vsaddu_vv_d, 8, 8)

typedef void opivx2_rm_fn(void *vd, target_long s1, void *vs2, int i,
                          CPURISCVState *env, int vxrm);

#define OPIVX2_RM(NAME, TD, T1, T2, TX1, TX2, HD, HS2, OP)          \
static inline void                                                  \
do_##NAME(void *vd, target_long s1, void *vs2, int i,               \
          CPURISCVState *env, int vxrm)                             \
{                                                                   \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                                 \
    *((TD *)vd + HD(i)) = OP(env, vxrm, s2, (TX1)(T1)s1);           \
}

static inline void
vext_vx_rm_1(void *vd, void *v0, target_long s1, void *vs2,
             CPURISCVState *env,
             uint32_t vl, uint32_t vm, int vxrm,
             opivx2_rm_fn *fn)
{
    for (uint32_t i = 0; i < vl; i++) {
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        fn(vd, s1, vs2, i, env, vxrm);
    }
}

static inline void
vext_vx_rm_2(void *vd, void *v0, target_long s1, void *vs2,
             CPURISCVState *env,
             uint32_t desc, uint32_t esz, uint32_t dsz,
             opivx2_rm_fn *fn)
{
    uint32_t vm = vext_vm(desc);
    uint32_t vl = env->vl;

    switch (env->vxrm) {
    case 0: /* rnu */
        vext_vx_rm_1(vd, v0, s1, vs2,
                     env, vl, vm, 0, fn);
        break;
    case 1: /* rne */
        vext_vx_rm_1(vd, v0, s1, vs2,
                     env, vl, vm, 1, fn);
        break;
    case 2: /* rdn */
        vext_vx_rm_1(vd, v0, s1, vs2,
                     env, vl, vm, 2, fn);
        break;
    default: /* rod */
        vext_vx_rm_1(vd, v0, s1, vs2,
                     env, vl, vm, 3, fn);
        break;
    }
}

/* generate helpers for fixed point instructions with OPIVX format */
#define GEN_VEXT_VX_RM(NAME, ESZ, DSZ)                    \
void HELPER(NAME)(void *vd, void *v0, target_ulong s1,    \
        void *vs2, CPURISCVState *env, uint32_t desc)     \
{                                                         \
    vext_vx_rm_2(vd, v0, s1, vs2, env, desc, ESZ, DSZ,    \
                 do_##NAME);                              \
}

RVVCALL(OPIVX2_RM, vsaddu_vx_b, OP_UUU_B, H1, H1, saddu8)
RVVCALL(OPIVX2_RM, vsaddu_vx_h, OP_UUU_H, H2, H2, saddu16)
RVVCALL(OPIVX2_RM, vsaddu_vx_w, OP_UUU_W, H4, H4, saddu32)
RVVCALL(OPIVX2_RM, vsaddu_vx_d, OP_UUU_D, H8, H8, saddu64)
GEN_VEXT_VX_RM(vsaddu_vx_b, 1, 1)
GEN_VEXT_VX_RM(vsaddu_vx_h, 2, 2)
GEN_VEXT_VX_RM(vsaddu_vx_w, 4, 4)
GEN_VEXT_VX_RM(vsaddu_vx_d, 8, 8)

static inline int8_t sadd8(CPURISCVState *env, int vxrm, int8_t a, int8_t b)
{
    int8_t res = a + b;
    if ((res ^ a) & (res ^ b) & INT8_MIN) {
        res = a > 0 ? INT8_MAX : INT8_MIN;
        env->vxsat = 0x1;
    }
    return res;
}

static inline int16_t sadd16(CPURISCVState *env, int vxrm, int16_t a, int16_t b)
{
    int16_t res = a + b;
    if ((res ^ a) & (res ^ b) & INT16_MIN) {
        res = a > 0 ? INT16_MAX : INT16_MIN;
        env->vxsat = 0x1;
    }
    return res;
}

static inline int32_t sadd32(CPURISCVState *env, int vxrm, int32_t a, int32_t b)
{
    int32_t res = a + b;
    if ((res ^ a) & (res ^ b) & INT32_MIN) {
        res = a > 0 ? INT32_MAX : INT32_MIN;
        env->vxsat = 0x1;
    }
    return res;
}

static inline int64_t sadd64(CPURISCVState *env, int vxrm, int64_t a, int64_t b)
{
    int64_t res = a + b;
    if ((res ^ a) & (res ^ b) & INT64_MIN) {
        res = a > 0 ? INT64_MAX : INT64_MIN;
        env->vxsat = 0x1;
    }
    return res;
}

RVVCALL(OPIVV2_RM, vsadd_vv_b, OP_SSS_B, H1, H1, H1, sadd8)
RVVCALL(OPIVV2_RM, vsadd_vv_h, OP_SSS_H, H2, H2, H2, sadd16)
RVVCALL(OPIVV2_RM, vsadd_vv_w, OP_SSS_W, H4, H4, H4, sadd32)
RVVCALL(OPIVV2_RM, vsadd_vv_d, OP_SSS_D, H8, H8, H8, sadd64)
GEN_VEXT_VV_RM(vsadd_vv_b, 1, 1)
GEN_VEXT_VV_RM(vsadd_vv_h, 2, 2)
GEN_VEXT_VV_RM(vsadd_vv_w, 4, 4)
GEN_VEXT_VV_RM(vsadd_vv_d, 8, 8)

RVVCALL(OPIVX2_RM, vsadd_vx_b, OP_SSS_B, H1, H1, sadd8)
RVVCALL(OPIVX2_RM, vsadd_vx_h, OP_SSS_H, H2, H2, sadd16)
RVVCALL(OPIVX2_RM, vsadd_vx_w, OP_SSS_W, H4, H4, sadd32)
RVVCALL(OPIVX2_RM, vsadd_vx_d, OP_SSS_D, H8, H8, sadd64)
GEN_VEXT_VX_RM(vsadd_vx_b, 1, 1)
GEN_VEXT_VX_RM(vsadd_vx_h, 2, 2)
GEN_VEXT_VX_RM(vsadd_vx_w, 4, 4)
GEN_VEXT_VX_RM(vsadd_vx_d, 8, 8)

static inline uint8_t ssubu8(CPURISCVState *env, int vxrm, uint8_t a, uint8_t b)
{
    uint8_t res = a - b;
    if (res > a) {
        res = 0;
        env->vxsat = 0x1;
    }
    return res;
}

static inline uint16_t ssubu16(CPURISCVState *env, int vxrm, uint16_t a,
                               uint16_t b)
{
    uint16_t res = a - b;
    if (res > a) {
        res = 0;
        env->vxsat = 0x1;
    }
    return res;
}

static inline uint32_t ssubu32(CPURISCVState *env, int vxrm, uint32_t a,
                               uint32_t b)
{
    uint32_t res = a - b;
    if (res > a) {
        res = 0;
        env->vxsat = 0x1;
    }
    return res;
}

static inline uint64_t ssubu64(CPURISCVState *env, int vxrm, uint64_t a,
                               uint64_t b)
{
    uint64_t res = a - b;
    if (res > a) {
        res = 0;
        env->vxsat = 0x1;
    }
    return res;
}

RVVCALL(OPIVV2_RM, vssubu_vv_b, OP_UUU_B, H1, H1, H1, ssubu8)
RVVCALL(OPIVV2_RM, vssubu_vv_h, OP_UUU_H, H2, H2, H2, ssubu16)
RVVCALL(OPIVV2_RM, vssubu_vv_w, OP_UUU_W, H4, H4, H4, ssubu32)
RVVCALL(OPIVV2_RM, vssubu_vv_d, OP_UUU_D, H8, H8, H8, ssubu64)
GEN_VEXT_VV_RM(vssubu_vv_b, 1, 1)
GEN_VEXT_VV_RM(vssubu_vv_h, 2, 2)
GEN_VEXT_VV_RM(vssubu_vv_w, 4, 4)
GEN_VEXT_VV_RM(vssubu_vv_d, 8, 8)

RVVCALL(OPIVX2_RM, vssubu_vx_b, OP_UUU_B, H1, H1, ssubu8)
RVVCALL(OPIVX2_RM, vssubu_vx_h, OP_UUU_H, H2, H2, ssubu16)
RVVCALL(OPIVX2_RM, vssubu_vx_w, OP_UUU_W, H4, H4, ssubu32)
RVVCALL(OPIVX2_RM, vssubu_vx_d, OP_UUU_D, H8, H8, ssubu64)
GEN_VEXT_VX_RM(vssubu_vx_b, 1, 1)
GEN_VEXT_VX_RM(vssubu_vx_h, 2, 2)
GEN_VEXT_VX_RM(vssubu_vx_w, 4, 4)
GEN_VEXT_VX_RM(vssubu_vx_d, 8, 8)

static inline int8_t ssub8(CPURISCVState *env, int vxrm, int8_t a, int8_t b)
{
    int8_t res = a - b;
    if ((res ^ a) & (a ^ b) & INT8_MIN) {
        res = a > 0 ? INT8_MAX : INT8_MIN;
        env->vxsat = 0x1;
    }
    return res;
}

static inline int16_t ssub16(CPURISCVState *env, int vxrm, int16_t a, int16_t b)
{
    int16_t res = a - b;
    if ((res ^ a) & (a ^ b) & INT16_MIN) {
        res = a > 0 ? INT16_MAX : INT16_MIN;
        env->vxsat = 0x1;
    }
    return res;
}

static inline int32_t ssub32(CPURISCVState *env, int vxrm, int32_t a, int32_t b)
{
    int32_t res = a - b;
    if ((res ^ a) & (a ^ b) & INT32_MIN) {
        res = a > 0 ? INT32_MAX : INT32_MIN;
        env->vxsat = 0x1;
    }
    return res;
}

static inline int64_t ssub64(CPURISCVState *env, int vxrm, int64_t a, int64_t b)
{
    int64_t res = a - b;
    if ((res ^ a) & (a ^ b) & INT64_MIN) {
        res = a > 0 ? INT64_MAX : INT64_MIN;
        env->vxsat = 0x1;
    }
    return res;
}

RVVCALL(OPIVV2_RM, vssub_vv_b, OP_SSS_B, H1, H1, H1, ssub8)
RVVCALL(OPIVV2_RM, vssub_vv_h, OP_SSS_H, H2, H2, H2, ssub16)
RVVCALL(OPIVV2_RM, vssub_vv_w, OP_SSS_W, H4, H4, H4, ssub32)
RVVCALL(OPIVV2_RM, vssub_vv_d, OP_SSS_D, H8, H8, H8, ssub64)
GEN_VEXT_VV_RM(vssub_vv_b, 1, 1)
GEN_VEXT_VV_RM(vssub_vv_h, 2, 2)
GEN_VEXT_VV_RM(vssub_vv_w, 4, 4)
GEN_VEXT_VV_RM(vssub_vv_d, 8, 8)

RVVCALL(OPIVX2_RM, vssub_vx_b, OP_SSS_B, H1, H1, ssub8)
RVVCALL(OPIVX2_RM, vssub_vx_h, OP_SSS_H, H2, H2, ssub16)
RVVCALL(OPIVX2_RM, vssub_vx_w, OP_SSS_W, H4, H4, ssub32)
RVVCALL(OPIVX2_RM, vssub_vx_d, OP_SSS_D, H8, H8, ssub64)
GEN_VEXT_VX_RM(vssub_vx_b, 1, 1)
GEN_VEXT_VX_RM(vssub_vx_h, 2, 2)
GEN_VEXT_VX_RM(vssub_vx_w, 4, 4)
GEN_VEXT_VX_RM(vssub_vx_d, 8, 8)

/* Vector Single-Width Averaging Add and Subtract */
static inline uint8_t get_round(int vxrm, uint64_t v, uint8_t shift)
{
    uint8_t d = extract64(v, shift, 1);
    uint8_t d1;
    uint64_t D1, D2;

    if (shift == 0 || shift > 64) {
        return 0;
    }

    d1 = extract64(v, shift - 1, 1);
    D1 = extract64(v, 0, shift);
    if (vxrm == 0) { /* round-to-nearest-up (add +0.5 LSB) */
        return d1;
    } else if (vxrm == 1) { /* round-to-nearest-even */
        if (shift > 1) {
            D2 = extract64(v, 0, shift - 1);
            return d1 & ((D2 != 0) | d);
        } else {
            return d1 & d;
        }
    } else if (vxrm == 3) { /* round-to-odd (OR bits into LSB, aka "jam") */
        return !d & (D1 != 0);
    }
    return 0; /* round-down (truncate) */
}

static inline int32_t aadd32(CPURISCVState *env, int vxrm, int32_t a, int32_t b)
{
    int64_t res = (int64_t)a + b;
    uint8_t round = get_round(vxrm, res, 1);

    return (res >> 1) + round;
}

static inline int64_t aadd64(CPURISCVState *env, int vxrm, int64_t a, int64_t b)
{
    int64_t res = a + b;
    uint8_t round = get_round(vxrm, res, 1);
    int64_t over = (res ^ a) & (res ^ b) & INT64_MIN;

    /* With signed overflow, bit 64 is inverse of bit 63. */
    return ((res >> 1) ^ over) + round;
}

RVVCALL(OPIVV2_RM, vaadd_vv_b, OP_SSS_B, H1, H1, H1, aadd32)
RVVCALL(OPIVV2_RM, vaadd_vv_h, OP_SSS_H, H2, H2, H2, aadd32)
RVVCALL(OPIVV2_RM, vaadd_vv_w, OP_SSS_W, H4, H4, H4, aadd32)
RVVCALL(OPIVV2_RM, vaadd_vv_d, OP_SSS_D, H8, H8, H8, aadd64)
GEN_VEXT_VV_RM(vaadd_vv_b, 1, 1)
GEN_VEXT_VV_RM(vaadd_vv_h, 2, 2)
GEN_VEXT_VV_RM(vaadd_vv_w, 4, 4)
GEN_VEXT_VV_RM(vaadd_vv_d, 8, 8)

RVVCALL(OPIVX2_RM, vaadd_vx_b, OP_SSS_B, H1, H1, aadd32)
RVVCALL(OPIVX2_RM, vaadd_vx_h, OP_SSS_H, H2, H2, aadd32)
RVVCALL(OPIVX2_RM, vaadd_vx_w, OP_SSS_W, H4, H4, aadd32)
RVVCALL(OPIVX2_RM, vaadd_vx_d, OP_SSS_D, H8, H8, aadd64)
GEN_VEXT_VX_RM(vaadd_vx_b, 1, 1)
GEN_VEXT_VX_RM(vaadd_vx_h, 2, 2)
GEN_VEXT_VX_RM(vaadd_vx_w, 4, 4)
GEN_VEXT_VX_RM(vaadd_vx_d, 8, 8)

static inline int32_t asub32(CPURISCVState *env, int vxrm, int32_t a, int32_t b)
{
    int64_t res = (int64_t)a - b;
    uint8_t round = get_round(vxrm, res, 1);

    return (res >> 1) + round;
}

static inline int64_t asub64(CPURISCVState *env, int vxrm, int64_t a, int64_t b)
{
    int64_t res = (int64_t)a - b;
    uint8_t round = get_round(vxrm, res, 1);
    int64_t over = (res ^ a) & (a ^ b) & INT64_MIN;

    /* With signed overflow, bit 64 is inverse of bit 63. */
    return ((res >> 1) ^ over) + round;
}

RVVCALL(OPIVV2_RM, vasub_vv_b, OP_SSS_B, H1, H1, H1, asub32)
RVVCALL(OPIVV2_RM, vasub_vv_h, OP_SSS_H, H2, H2, H2, asub32)
RVVCALL(OPIVV2_RM, vasub_vv_w, OP_SSS_W, H4, H4, H4, asub32)
RVVCALL(OPIVV2_RM, vasub_vv_d, OP_SSS_D, H8, H8, H8, asub64)
GEN_VEXT_VV_RM(vasub_vv_b, 1, 1)
GEN_VEXT_VV_RM(vasub_vv_h, 2, 2)
GEN_VEXT_VV_RM(vasub_vv_w, 4, 4)
GEN_VEXT_VV_RM(vasub_vv_d, 8, 8)

RVVCALL(OPIVX2_RM, vasub_vx_b, OP_SSS_B, H1, H1, asub32)
RVVCALL(OPIVX2_RM, vasub_vx_h, OP_SSS_H, H2, H2, asub32)
RVVCALL(OPIVX2_RM, vasub_vx_w, OP_SSS_W, H4, H4, asub32)
RVVCALL(OPIVX2_RM, vasub_vx_d, OP_SSS_D, H8, H8, asub64)
GEN_VEXT_VX_RM(vasub_vx_b, 1, 1)
GEN_VEXT_VX_RM(vasub_vx_h, 2, 2)
GEN_VEXT_VX_RM(vasub_vx_w, 4, 4)
GEN_VEXT_VX_RM(vasub_vx_d, 8, 8)

/* Vector Single-Width Fractional Multiply with Rounding and Saturation */
static inline int8_t vsmul8(CPURISCVState *env, int vxrm, int8_t a, int8_t b)
{
    uint8_t round;
    int16_t res;

    res = (int16_t)a * (int16_t)b;
    round = get_round(vxrm, res, 7);
    res   = (res >> 7) + round;

    if (res > INT8_MAX) {
        env->vxsat = 0x1;
        return INT8_MAX;
    } else if (res < INT8_MIN) {
        env->vxsat = 0x1;
        return INT8_MIN;
    } else {
        return res;
    }
}

static int16_t vsmul16(CPURISCVState *env, int vxrm, int16_t a, int16_t b)
{
    uint8_t round;
    int32_t res;

    res = (int32_t)a * (int32_t)b;
    round = get_round(vxrm, res, 15);
    res   = (res >> 15) + round;

    if (res > INT16_MAX) {
        env->vxsat = 0x1;
        return INT16_MAX;
    } else if (res < INT16_MIN) {
        env->vxsat = 0x1;
        return INT16_MIN;
    } else {
        return res;
    }
}

static int32_t vsmul32(CPURISCVState *env, int vxrm, int32_t a, int32_t b)
{
    uint8_t round;
    int64_t res;

    res = (int64_t)a * (int64_t)b;
    round = get_round(vxrm, res, 31);
    res   = (res >> 31) + round;

    if (res > INT32_MAX) {
        env->vxsat = 0x1;
        return INT32_MAX;
    } else if (res < INT32_MIN) {
        env->vxsat = 0x1;
        return INT32_MIN;
    } else {
        return res;
    }
}

static int64_t vsmul64(CPURISCVState *env, int vxrm, int64_t a, int64_t b)
{
    uint8_t round;
    uint64_t hi_64, lo_64;
    int64_t res;

    if (a == INT64_MIN && b == INT64_MIN) {
        env->vxsat = 1;
        return INT64_MAX;
    }

    muls64(&lo_64, &hi_64, a, b);
    round = get_round(vxrm, lo_64, 63);
    /*
     * Cannot overflow, as there are always
     * 2 sign bits after multiply.
     */
    res = (hi_64 << 1) | (lo_64 >> 63);
    if (round) {
        if (res == INT64_MAX) {
            env->vxsat = 1;
        } else {
            res += 1;
        }
    }
    return res;
}

RVVCALL(OPIVV2_RM, vsmul_vv_b, OP_SSS_B, H1, H1, H1, vsmul8)
RVVCALL(OPIVV2_RM, vsmul_vv_h, OP_SSS_H, H2, H2, H2, vsmul16)
RVVCALL(OPIVV2_RM, vsmul_vv_w, OP_SSS_W, H4, H4, H4, vsmul32)
RVVCALL(OPIVV2_RM, vsmul_vv_d, OP_SSS_D, H8, H8, H8, vsmul64)
GEN_VEXT_VV_RM(vsmul_vv_b, 1, 1)
GEN_VEXT_VV_RM(vsmul_vv_h, 2, 2)
GEN_VEXT_VV_RM(vsmul_vv_w, 4, 4)
GEN_VEXT_VV_RM(vsmul_vv_d, 8, 8)

RVVCALL(OPIVX2_RM, vsmul_vx_b, OP_SSS_B, H1, H1, vsmul8)
RVVCALL(OPIVX2_RM, vsmul_vx_h, OP_SSS_H, H2, H2, vsmul16)
RVVCALL(OPIVX2_RM, vsmul_vx_w, OP_SSS_W, H4, H4, vsmul32)
RVVCALL(OPIVX2_RM, vsmul_vx_d, OP_SSS_D, H8, H8, vsmul64)
GEN_VEXT_VX_RM(vsmul_vx_b, 1, 1)
GEN_VEXT_VX_RM(vsmul_vx_h, 2, 2)
GEN_VEXT_VX_RM(vsmul_vx_w, 4, 4)
GEN_VEXT_VX_RM(vsmul_vx_d, 8, 8)

/* Vector Widening Saturating Scaled Multiply-Add */
static inline uint16_t
vwsmaccu8(CPURISCVState *env, int vxrm, uint8_t a, uint8_t b,
          uint16_t c)
{
    uint8_t round;
    uint16_t res = (uint16_t)a * b;

    round = get_round(vxrm, res, 4);
    res   = (res >> 4) + round;
    return saddu16(env, vxrm, c, res);
}

static inline uint32_t
vwsmaccu16(CPURISCVState *env, int vxrm, uint16_t a, uint16_t b,
           uint32_t c)
{
    uint8_t round;
    uint32_t res = (uint32_t)a * b;

    round = get_round(vxrm, res, 8);
    res   = (res >> 8) + round;
    return saddu32(env, vxrm, c, res);
}

static inline uint64_t
vwsmaccu32(CPURISCVState *env, int vxrm, uint32_t a, uint32_t b,
           uint64_t c)
{
    uint8_t round;
    uint64_t res = (uint64_t)a * b;

    round = get_round(vxrm, res, 16);
    res   = (res >> 16) + round;
    return saddu64(env, vxrm, c, res);
}

#define OPIVV3_RM(NAME, TD, T1, T2, TX1, TX2, HD, HS1, HS2, OP)    \
static inline void                                                 \
do_##NAME(void *vd, void *vs1, void *vs2, int i,                   \
          CPURISCVState *env, int vxrm)                            \
{                                                                  \
    TX1 s1 = *((T1 *)vs1 + HS1(i));                                \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                                \
    TD d = *((TD *)vd + HD(i));                                    \
    *((TD *)vd + HD(i)) = OP(env, vxrm, s2, s1, d);                \
}

RVVCALL(OPIVV3_RM, vwsmaccu_vv_b, WOP_UUU_B, H2, H1, H1, vwsmaccu8)
RVVCALL(OPIVV3_RM, vwsmaccu_vv_h, WOP_UUU_H, H4, H2, H2, vwsmaccu16)
RVVCALL(OPIVV3_RM, vwsmaccu_vv_w, WOP_UUU_W, H8, H4, H4, vwsmaccu32)
GEN_VEXT_VV_RM(vwsmaccu_vv_b, 1, 2)
GEN_VEXT_VV_RM(vwsmaccu_vv_h, 2, 4)
GEN_VEXT_VV_RM(vwsmaccu_vv_w, 4, 8)

#define OPIVX3_RM(NAME, TD, T1, T2, TX1, TX2, HD, HS2, OP)         \
static inline void                                                 \
do_##NAME(void *vd, target_long s1, void *vs2, int i,              \
          CPURISCVState *env, int vxrm)                            \
{                                                                  \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                                \
    TD d = *((TD *)vd + HD(i));                                    \
    *((TD *)vd + HD(i)) = OP(env, vxrm, s2, (TX1)(T1)s1, d);       \
}

RVVCALL(OPIVX3_RM, vwsmaccu_vx_b, WOP_UUU_B, H2, H1, vwsmaccu8)
RVVCALL(OPIVX3_RM, vwsmaccu_vx_h, WOP_UUU_H, H4, H2, vwsmaccu16)
RVVCALL(OPIVX3_RM, vwsmaccu_vx_w, WOP_UUU_W, H8, H4, vwsmaccu32)
GEN_VEXT_VX_RM(vwsmaccu_vx_b, 1, 2)
GEN_VEXT_VX_RM(vwsmaccu_vx_h, 2, 4)
GEN_VEXT_VX_RM(vwsmaccu_vx_w, 4, 8)

static inline int16_t
vwsmacc8(CPURISCVState *env, int vxrm, int8_t a, int8_t b, int16_t c)
{
    uint8_t round;
    int16_t res = (int16_t)a * b;

    round = get_round(vxrm, res, 4);
    res   = (res >> 4) + round;
    return sadd16(env, vxrm, c, res);
}

static inline int32_t
vwsmacc16(CPURISCVState *env, int vxrm, int16_t a, int16_t b, int32_t c)
{
    uint8_t round;
    int32_t res = (int32_t)a * b;

    round = get_round(vxrm, res, 8);
    res   = (res >> 8) + round;
    return sadd32(env, vxrm, c, res);

}

static inline int64_t
vwsmacc32(CPURISCVState *env, int vxrm, int32_t a, int32_t b, int64_t c)
{
    uint8_t round;
    int64_t res = (int64_t)a * b;

    round = get_round(vxrm, res, 16);
    res   = (res >> 16) + round;
    return sadd64(env, vxrm, c, res);
}

RVVCALL(OPIVV3_RM, vwsmacc_vv_b, WOP_SSS_B, H2, H1, H1, vwsmacc8)
RVVCALL(OPIVV3_RM, vwsmacc_vv_h, WOP_SSS_H, H4, H2, H2, vwsmacc16)
RVVCALL(OPIVV3_RM, vwsmacc_vv_w, WOP_SSS_W, H8, H4, H4, vwsmacc32)
GEN_VEXT_VV_RM(vwsmacc_vv_b, 1, 2)
GEN_VEXT_VV_RM(vwsmacc_vv_h, 2, 4)
GEN_VEXT_VV_RM(vwsmacc_vv_w, 4, 8)
RVVCALL(OPIVX3_RM, vwsmacc_vx_b, WOP_SSS_B, H2, H1, vwsmacc8)
RVVCALL(OPIVX3_RM, vwsmacc_vx_h, WOP_SSS_H, H4, H2, vwsmacc16)
RVVCALL(OPIVX3_RM, vwsmacc_vx_w, WOP_SSS_W, H8, H4, vwsmacc32)
GEN_VEXT_VX_RM(vwsmacc_vx_b, 1, 2)
GEN_VEXT_VX_RM(vwsmacc_vx_h, 2, 4)
GEN_VEXT_VX_RM(vwsmacc_vx_w, 4, 8)

static inline int16_t
vwsmaccsu8(CPURISCVState *env, int vxrm, uint8_t a, int8_t b, int16_t c)
{
    uint8_t round;
    int16_t res = a * (int16_t)b;

    round = get_round(vxrm, res, 4);
    res   = (res >> 4) + round;
    return ssub16(env, vxrm, c, res);
}

static inline int32_t
vwsmaccsu16(CPURISCVState *env, int vxrm, uint16_t a, int16_t b, uint32_t c)
{
    uint8_t round;
    int32_t res = a * (int32_t)b;

    round = get_round(vxrm, res, 8);
    res   = (res >> 8) + round;
    return ssub32(env, vxrm, c, res);
}

static inline int64_t
vwsmaccsu32(CPURISCVState *env, int vxrm, uint32_t a, int32_t b, int64_t c)
{
    uint8_t round;
    int64_t res = a * (int64_t)b;

    round = get_round(vxrm, res, 16);
    res   = (res >> 16) + round;
    return ssub64(env, vxrm, c, res);
}

RVVCALL(OPIVV3_RM, vwsmaccsu_vv_b, WOP_SSU_B, H2, H1, H1, vwsmaccsu8)
RVVCALL(OPIVV3_RM, vwsmaccsu_vv_h, WOP_SSU_H, H4, H2, H2, vwsmaccsu16)
RVVCALL(OPIVV3_RM, vwsmaccsu_vv_w, WOP_SSU_W, H8, H4, H4, vwsmaccsu32)
GEN_VEXT_VV_RM(vwsmaccsu_vv_b, 1, 2)
GEN_VEXT_VV_RM(vwsmaccsu_vv_h, 2, 4)
GEN_VEXT_VV_RM(vwsmaccsu_vv_w, 4, 8)
RVVCALL(OPIVX3_RM, vwsmaccsu_vx_b, WOP_SSU_B, H2, H1, vwsmaccsu8)
RVVCALL(OPIVX3_RM, vwsmaccsu_vx_h, WOP_SSU_H, H4, H2, vwsmaccsu16)
RVVCALL(OPIVX3_RM, vwsmaccsu_vx_w, WOP_SSU_W, H8, H4, vwsmaccsu32)
GEN_VEXT_VX_RM(vwsmaccsu_vx_b, 1, 2)
GEN_VEXT_VX_RM(vwsmaccsu_vx_h, 2, 4)
GEN_VEXT_VX_RM(vwsmaccsu_vx_w, 4, 8)

static inline int16_t
vwsmaccus8(CPURISCVState *env, int vxrm, int8_t a, uint8_t b, int16_t c)
{
    uint8_t round;
    int16_t res = (int16_t)a * b;

    round = get_round(vxrm, res, 4);
    res   = (res >> 4) + round;
    return ssub16(env, vxrm, c, res);
}

static inline int32_t
vwsmaccus16(CPURISCVState *env, int vxrm, int16_t a, uint16_t b, int32_t c)
{
    uint8_t round;
    int32_t res = (int32_t)a * b;

    round = get_round(vxrm, res, 8);
    res   = (res >> 8) + round;
    return ssub32(env, vxrm, c, res);
}

static inline int64_t
vwsmaccus32(CPURISCVState *env, int vxrm, int32_t a, uint32_t b, int64_t c)
{
    uint8_t round;
    int64_t res = (int64_t)a * b;

    round = get_round(vxrm, res, 16);
    res   = (res >> 16) + round;
    return ssub64(env, vxrm, c, res);
}

RVVCALL(OPIVX3_RM, vwsmaccus_vx_b, WOP_SUS_B, H2, H1, vwsmaccus8)
RVVCALL(OPIVX3_RM, vwsmaccus_vx_h, WOP_SUS_H, H4, H2, vwsmaccus16)
RVVCALL(OPIVX3_RM, vwsmaccus_vx_w, WOP_SUS_W, H8, H4, vwsmaccus32)
GEN_VEXT_VX_RM(vwsmaccus_vx_b, 1, 2)
GEN_VEXT_VX_RM(vwsmaccus_vx_h, 2, 4)
GEN_VEXT_VX_RM(vwsmaccus_vx_w, 4, 8)

/* Vector Single-Width Scaling Shift Instructions */
static inline uint8_t
vssrl8(CPURISCVState *env, int vxrm, uint8_t a, uint8_t b)
{
    uint8_t round, shift = b & 0x7;
    uint8_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    return res;
}
static inline uint16_t
vssrl16(CPURISCVState *env, int vxrm, uint16_t a, uint16_t b)
{
    uint8_t round, shift = b & 0xf;
    uint16_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    return res;
}
static inline uint32_t
vssrl32(CPURISCVState *env, int vxrm, uint32_t a, uint32_t b)
{
    uint8_t round, shift = b & 0x1f;
    uint32_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    return res;
}
static inline uint64_t
vssrl64(CPURISCVState *env, int vxrm, uint64_t a, uint64_t b)
{
    uint8_t round, shift = b & 0x3f;
    uint64_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    return res;
}
RVVCALL(OPIVV2_RM, vssrl_vv_b, OP_UUU_B, H1, H1, H1, vssrl8)
RVVCALL(OPIVV2_RM, vssrl_vv_h, OP_UUU_H, H2, H2, H2, vssrl16)
RVVCALL(OPIVV2_RM, vssrl_vv_w, OP_UUU_W, H4, H4, H4, vssrl32)
RVVCALL(OPIVV2_RM, vssrl_vv_d, OP_UUU_D, H8, H8, H8, vssrl64)
GEN_VEXT_VV_RM(vssrl_vv_b, 1, 1)
GEN_VEXT_VV_RM(vssrl_vv_h, 2, 2)
GEN_VEXT_VV_RM(vssrl_vv_w, 4, 4)
GEN_VEXT_VV_RM(vssrl_vv_d, 8, 8)

RVVCALL(OPIVX2_RM, vssrl_vx_b, OP_UUU_B, H1, H1, vssrl8)
RVVCALL(OPIVX2_RM, vssrl_vx_h, OP_UUU_H, H2, H2, vssrl16)
RVVCALL(OPIVX2_RM, vssrl_vx_w, OP_UUU_W, H4, H4, vssrl32)
RVVCALL(OPIVX2_RM, vssrl_vx_d, OP_UUU_D, H8, H8, vssrl64)
GEN_VEXT_VX_RM(vssrl_vx_b, 1, 1)
GEN_VEXT_VX_RM(vssrl_vx_h, 2, 2)
GEN_VEXT_VX_RM(vssrl_vx_w, 4, 4)
GEN_VEXT_VX_RM(vssrl_vx_d, 8, 8)

static inline int8_t
vssra8(CPURISCVState *env, int vxrm, int8_t a, int8_t b)
{
    uint8_t round, shift = b & 0x7;
    int8_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    return res;
}
static inline int16_t
vssra16(CPURISCVState *env, int vxrm, int16_t a, int16_t b)
{
    uint8_t round, shift = b & 0xf;
    int16_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    return res;
}
static inline int32_t
vssra32(CPURISCVState *env, int vxrm, int32_t a, int32_t b)
{
    uint8_t round, shift = b & 0x1f;
    int32_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    return res;
}
static inline int64_t
vssra64(CPURISCVState *env, int vxrm, int64_t a, int64_t b)
{
    uint8_t round, shift = b & 0x3f;
    int64_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    return res;
}

RVVCALL(OPIVV2_RM, vssra_vv_b, OP_SSS_B, H1, H1, H1, vssra8)
RVVCALL(OPIVV2_RM, vssra_vv_h, OP_SSS_H, H2, H2, H2, vssra16)
RVVCALL(OPIVV2_RM, vssra_vv_w, OP_SSS_W, H4, H4, H4, vssra32)
RVVCALL(OPIVV2_RM, vssra_vv_d, OP_SSS_D, H8, H8, H8, vssra64)
GEN_VEXT_VV_RM(vssra_vv_b, 1, 1)
GEN_VEXT_VV_RM(vssra_vv_h, 2, 2)
GEN_VEXT_VV_RM(vssra_vv_w, 4, 4)
GEN_VEXT_VV_RM(vssra_vv_d, 8, 8)

RVVCALL(OPIVX2_RM, vssra_vx_b, OP_SSS_B, H1, H1, vssra8)
RVVCALL(OPIVX2_RM, vssra_vx_h, OP_SSS_H, H2, H2, vssra16)
RVVCALL(OPIVX2_RM, vssra_vx_w, OP_SSS_W, H4, H4, vssra32)
RVVCALL(OPIVX2_RM, vssra_vx_d, OP_SSS_D, H8, H8, vssra64)
GEN_VEXT_VX_RM(vssra_vx_b, 1, 1)
GEN_VEXT_VX_RM(vssra_vx_h, 2, 2)
GEN_VEXT_VX_RM(vssra_vx_w, 4, 4)
GEN_VEXT_VX_RM(vssra_vx_d, 8, 8)

/* Vector Narrowing Fixed-Point Clip Instructions */
static inline int8_t
vnclip8(CPURISCVState *env, int vxrm, int16_t a, int8_t b)
{
    uint8_t round, shift = b & 0xf;
    int16_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    if (res > INT8_MAX) {
        env->vxsat = 0x1;
        return INT8_MAX;
    } else if (res < INT8_MIN) {
        env->vxsat = 0x1;
        return INT8_MIN;
    } else {
        return res;
    }
}

static inline int16_t
vnclip16(CPURISCVState *env, int vxrm, int32_t a, int16_t b)
{
    uint8_t round, shift = b & 0x1f;
    int32_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    if (res > INT16_MAX) {
        env->vxsat = 0x1;
        return INT16_MAX;
    } else if (res < INT16_MIN) {
        env->vxsat = 0x1;
        return INT16_MIN;
    } else {
        return res;
    }
}

static inline int32_t
vnclip32(CPURISCVState *env, int vxrm, int64_t a, int32_t b)
{
    uint8_t round, shift = b & 0x3f;
    int64_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    if (res > INT32_MAX) {
        env->vxsat = 0x1;
        return INT32_MAX;
    } else if (res < INT32_MIN) {
        env->vxsat = 0x1;
        return INT32_MIN;
    } else {
        return res;
    }
}

RVVCALL(OPIVV2_RM, vnclip_vv_b, NOP_SSS_B, H1, H2, H1, vnclip8)
RVVCALL(OPIVV2_RM, vnclip_vv_h, NOP_SSS_H, H2, H4, H2, vnclip16)
RVVCALL(OPIVV2_RM, vnclip_vv_w, NOP_SSS_W, H4, H8, H4, vnclip32)
GEN_VEXT_VV_RM(vnclip_vv_b, 1, 1)
GEN_VEXT_VV_RM(vnclip_vv_h, 2, 2)
GEN_VEXT_VV_RM(vnclip_vv_w, 4, 4)

RVVCALL(OPIVX2_RM, vnclip_vx_b, NOP_SSS_B, H1, H2, vnclip8)
RVVCALL(OPIVX2_RM, vnclip_vx_h, NOP_SSS_H, H2, H4, vnclip16)
RVVCALL(OPIVX2_RM, vnclip_vx_w, NOP_SSS_W, H4, H8, vnclip32)
GEN_VEXT_VX_RM(vnclip_vx_b, 1, 1)
GEN_VEXT_VX_RM(vnclip_vx_h, 2, 2)
GEN_VEXT_VX_RM(vnclip_vx_w, 4, 4)

static inline uint8_t
vnclipu8(CPURISCVState *env, int vxrm, uint16_t a, uint8_t b)
{
    uint8_t round, shift = b & 0xf;
    uint16_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    if (res > UINT8_MAX) {
        env->vxsat = 0x1;
        return UINT8_MAX;
    } else {
        return res;
    }
}

static inline uint16_t
vnclipu16(CPURISCVState *env, int vxrm, uint32_t a, uint16_t b)
{
    uint8_t round, shift = b & 0x1f;
    uint32_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    if (res > UINT16_MAX) {
        env->vxsat = 0x1;
        return UINT16_MAX;
    } else {
        return res;
    }
}

static inline uint32_t
vnclipu32(CPURISCVState *env, int vxrm, uint64_t a, uint32_t b)
{
    uint8_t round, shift = b & 0x3f;
    int64_t res;

    round = get_round(vxrm, a, shift);
    res   = (a >> shift)  + round;
    if (res > UINT32_MAX) {
        env->vxsat = 0x1;
        return UINT32_MAX;
    } else {
        return res;
    }
}

RVVCALL(OPIVV2_RM, vnclipu_vv_b, NOP_UUU_B, H1, H2, H1, vnclipu8)
RVVCALL(OPIVV2_RM, vnclipu_vv_h, NOP_UUU_H, H2, H4, H2, vnclipu16)
RVVCALL(OPIVV2_RM, vnclipu_vv_w, NOP_UUU_W, H4, H8, H4, vnclipu32)
GEN_VEXT_VV_RM(vnclipu_vv_b, 1, 1)
GEN_VEXT_VV_RM(vnclipu_vv_h, 2, 2)
GEN_VEXT_VV_RM(vnclipu_vv_w, 4, 4)

RVVCALL(OPIVX2_RM, vnclipu_vx_b, NOP_UUU_B, H1, H2, vnclipu8)
RVVCALL(OPIVX2_RM, vnclipu_vx_h, NOP_UUU_H, H2, H4, vnclipu16)
RVVCALL(OPIVX2_RM, vnclipu_vx_w, NOP_UUU_W, H4, H8, vnclipu32)
GEN_VEXT_VX_RM(vnclipu_vx_b, 1, 1)
GEN_VEXT_VX_RM(vnclipu_vx_h, 2, 2)
GEN_VEXT_VX_RM(vnclipu_vx_w, 4, 4)

/*
 *** Vector Float Point Arithmetic Instructions
 */
/* Vector Single-Width Floating-Point Add/Subtract Instructions */
#define OPFVV2(NAME, TD, T1, T2, TX1, TX2, HD, HS1, HS2, OP)   \
static void do_##NAME(void *vd, void *vs1, void *vs2, int i,   \
                      CPURISCVState *env)                      \
{                                                              \
    TX1 s1 = *((T1 *)vs1 + HS1(i));                            \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                            \
    *((TD *)vd + HD(i)) = OP(s2, s1, &env->fp_status);         \
}

#define GEN_VEXT_VV_ENV(NAME, ESZ, DSZ)                   \
void HELPER(NAME)(void *vd, void *v0, void *vs1,          \
                  void *vs2, CPURISCVState *env,          \
                  uint32_t desc)                          \
{                                                         \
    uint32_t vm = vext_vm(desc);                          \
    uint32_t vl = env->vl;                                \
    uint32_t i;                                           \
                                                          \
    for (i = 0; i < vl; i++) {                            \
        if (!vm && !vext_elem_mask(v0, i)) {              \
            continue;                                     \
        }                                                 \
        do_##NAME(vd, vs1, vs2, i, env);                  \
    }                                                     \
}

RVVCALL(OPFVV2, vfadd_vv_h, OP_UUU_H, H2, H2, H2, float16_add)
RVVCALL(OPFVV2, vfadd_vv_w, OP_UUU_W, H4, H4, H4, float32_add)
RVVCALL(OPFVV2, vfadd_vv_d, OP_UUU_D, H8, H8, H8, float64_add)
GEN_VEXT_VV_ENV(vfadd_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfadd_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfadd_vv_d, 8, 8)

#define OPFVF2(NAME, TD, T1, T2, TX1, TX2, HD, HS2, OP)        \
static void do_##NAME(void *vd, uint64_t s1, void *vs2, int i, \
                      CPURISCVState *env)                      \
{                                                              \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                            \
    *((TD *)vd + HD(i)) = OP(s2, (TX1)(T1)s1, &env->fp_status);\
}

#define GEN_VEXT_VF(NAME, ESZ, DSZ)                       \
void HELPER(NAME)(void *vd, void *v0, uint64_t s1,        \
                  void *vs2, CPURISCVState *env,          \
                  uint32_t desc)                          \
{                                                         \
    uint32_t vm = vext_vm(desc);                          \
    uint32_t vl = env->vl;                                \
    uint32_t i;                                           \
                                                          \
    for (i = 0; i < vl; i++) {                            \
        if (!vm && !vext_elem_mask(v0, i)) {              \
            continue;                                     \
        }                                                 \
        do_##NAME(vd, s1, vs2, i, env);                   \
    }                                                     \
}

RVVCALL(OPFVF2, vfadd_vf_h, OP_UUU_H, H2, H2, float16_add)
RVVCALL(OPFVF2, vfadd_vf_w, OP_UUU_W, H4, H4, float32_add)
RVVCALL(OPFVF2, vfadd_vf_d, OP_UUU_D, H8, H8, float64_add)
GEN_VEXT_VF(vfadd_vf_h, 2, 2)
GEN_VEXT_VF(vfadd_vf_w, 4, 4)
GEN_VEXT_VF(vfadd_vf_d, 8, 8)

RVVCALL(OPFVV2, vfsub_vv_h, OP_UUU_H, H2, H2, H2, float16_sub)
RVVCALL(OPFVV2, vfsub_vv_w, OP_UUU_W, H4, H4, H4, float32_sub)
RVVCALL(OPFVV2, vfsub_vv_d, OP_UUU_D, H8, H8, H8, float64_sub)
GEN_VEXT_VV_ENV(vfsub_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfsub_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfsub_vv_d, 8, 8)
RVVCALL(OPFVF2, vfsub_vf_h, OP_UUU_H, H2, H2, float16_sub)
RVVCALL(OPFVF2, vfsub_vf_w, OP_UUU_W, H4, H4, float32_sub)
RVVCALL(OPFVF2, vfsub_vf_d, OP_UUU_D, H8, H8, float64_sub)
GEN_VEXT_VF(vfsub_vf_h, 2, 2)
GEN_VEXT_VF(vfsub_vf_w, 4, 4)
GEN_VEXT_VF(vfsub_vf_d, 8, 8)

static uint16_t float16_rsub(uint16_t a, uint16_t b, float_status *s)
{
    return float16_sub(b, a, s);
}

static uint32_t float32_rsub(uint32_t a, uint32_t b, float_status *s)
{
    return float32_sub(b, a, s);
}

static uint64_t float64_rsub(uint64_t a, uint64_t b, float_status *s)
{
    return float64_sub(b, a, s);
}

RVVCALL(OPFVF2, vfrsub_vf_h, OP_UUU_H, H2, H2, float16_rsub)
RVVCALL(OPFVF2, vfrsub_vf_w, OP_UUU_W, H4, H4, float32_rsub)
RVVCALL(OPFVF2, vfrsub_vf_d, OP_UUU_D, H8, H8, float64_rsub)
GEN_VEXT_VF(vfrsub_vf_h, 2, 2)
GEN_VEXT_VF(vfrsub_vf_w, 4, 4)
GEN_VEXT_VF(vfrsub_vf_d, 8, 8)

/* Vector Widening Floating-Point Add/Subtract Instructions */
static uint32_t vfwadd16(uint16_t a, uint16_t b, float_status *s)
{
    return float32_add(float16_to_float32(a, true, s),
            float16_to_float32(b, true, s), s);
}

static uint64_t vfwadd32(uint32_t a, uint32_t b, float_status *s)
{
    return float64_add(float32_to_float64(a, s),
            float32_to_float64(b, s), s);

}

RVVCALL(OPFVV2, vfwadd_vv_h, WOP_UUU_H, H4, H2, H2, vfwadd16)
RVVCALL(OPFVV2, vfwadd_vv_w, WOP_UUU_W, H8, H4, H4, vfwadd32)
GEN_VEXT_VV_ENV(vfwadd_vv_h, 2, 4)
GEN_VEXT_VV_ENV(vfwadd_vv_w, 4, 8)
RVVCALL(OPFVF2, vfwadd_vf_h, WOP_UUU_H, H4, H2, vfwadd16)
RVVCALL(OPFVF2, vfwadd_vf_w, WOP_UUU_W, H8, H4, vfwadd32)
GEN_VEXT_VF(vfwadd_vf_h, 2, 4)
GEN_VEXT_VF(vfwadd_vf_w, 4, 8)

static uint32_t vfwsub16(uint16_t a, uint16_t b, float_status *s)
{
    return float32_sub(float16_to_float32(a, true, s),
            float16_to_float32(b, true, s), s);
}

static uint64_t vfwsub32(uint32_t a, uint32_t b, float_status *s)
{
    return float64_sub(float32_to_float64(a, s),
            float32_to_float64(b, s), s);

}

RVVCALL(OPFVV2, vfwsub_vv_h, WOP_UUU_H, H4, H2, H2, vfwsub16)
RVVCALL(OPFVV2, vfwsub_vv_w, WOP_UUU_W, H8, H4, H4, vfwsub32)
GEN_VEXT_VV_ENV(vfwsub_vv_h, 2, 4)
GEN_VEXT_VV_ENV(vfwsub_vv_w, 4, 8)
RVVCALL(OPFVF2, vfwsub_vf_h, WOP_UUU_H, H4, H2, vfwsub16)
RVVCALL(OPFVF2, vfwsub_vf_w, WOP_UUU_W, H8, H4, vfwsub32)
GEN_VEXT_VF(vfwsub_vf_h, 2, 4)
GEN_VEXT_VF(vfwsub_vf_w, 4, 8)

static uint32_t vfwaddw16(uint32_t a, uint16_t b, float_status *s)
{
    return float32_add(a, float16_to_float32(b, true, s), s);
}

static uint64_t vfwaddw32(uint64_t a, uint32_t b, float_status *s)
{
    return float64_add(a, float32_to_float64(b, s), s);
}

RVVCALL(OPFVV2, vfwadd_wv_h, WOP_WUUU_H, H4, H2, H2, vfwaddw16)
RVVCALL(OPFVV2, vfwadd_wv_w, WOP_WUUU_W, H8, H4, H4, vfwaddw32)
GEN_VEXT_VV_ENV(vfwadd_wv_h, 2, 4)
GEN_VEXT_VV_ENV(vfwadd_wv_w, 4, 8)
RVVCALL(OPFVF2, vfwadd_wf_h, WOP_WUUU_H, H4, H2, vfwaddw16)
RVVCALL(OPFVF2, vfwadd_wf_w, WOP_WUUU_W, H8, H4, vfwaddw32)
GEN_VEXT_VF(vfwadd_wf_h, 2, 4)
GEN_VEXT_VF(vfwadd_wf_w, 4, 8)

static uint32_t vfwsubw16(uint32_t a, uint16_t b, float_status *s)
{
    return float32_sub(a, float16_to_float32(b, true, s), s);
}

static uint64_t vfwsubw32(uint64_t a, uint32_t b, float_status *s)
{
    return float64_sub(a, float32_to_float64(b, s), s);
}

RVVCALL(OPFVV2, vfwsub_wv_h, WOP_WUUU_H, H4, H2, H2, vfwsubw16)
RVVCALL(OPFVV2, vfwsub_wv_w, WOP_WUUU_W, H8, H4, H4, vfwsubw32)
GEN_VEXT_VV_ENV(vfwsub_wv_h, 2, 4)
GEN_VEXT_VV_ENV(vfwsub_wv_w, 4, 8)
RVVCALL(OPFVF2, vfwsub_wf_h, WOP_WUUU_H, H4, H2, vfwsubw16)
RVVCALL(OPFVF2, vfwsub_wf_w, WOP_WUUU_W, H8, H4, vfwsubw32)
GEN_VEXT_VF(vfwsub_wf_h, 2, 4)
GEN_VEXT_VF(vfwsub_wf_w, 4, 8)

/* Vector Single-Width Floating-Point Multiply/Divide Instructions */
RVVCALL(OPFVV2, vfmul_vv_h, OP_UUU_H, H2, H2, H2, float16_mul)
RVVCALL(OPFVV2, vfmul_vv_w, OP_UUU_W, H4, H4, H4, float32_mul)
RVVCALL(OPFVV2, vfmul_vv_d, OP_UUU_D, H8, H8, H8, float64_mul)
GEN_VEXT_VV_ENV(vfmul_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfmul_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfmul_vv_d, 8, 8)
RVVCALL(OPFVF2, vfmul_vf_h, OP_UUU_H, H2, H2, float16_mul)
RVVCALL(OPFVF2, vfmul_vf_w, OP_UUU_W, H4, H4, float32_mul)
RVVCALL(OPFVF2, vfmul_vf_d, OP_UUU_D, H8, H8, float64_mul)
GEN_VEXT_VF(vfmul_vf_h, 2, 2)
GEN_VEXT_VF(vfmul_vf_w, 4, 4)
GEN_VEXT_VF(vfmul_vf_d, 8, 8)

RVVCALL(OPFVV2, vfdiv_vv_h, OP_UUU_H, H2, H2, H2, float16_div)
RVVCALL(OPFVV2, vfdiv_vv_w, OP_UUU_W, H4, H4, H4, float32_div)
RVVCALL(OPFVV2, vfdiv_vv_d, OP_UUU_D, H8, H8, H8, float64_div)
GEN_VEXT_VV_ENV(vfdiv_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfdiv_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfdiv_vv_d, 8, 8)
RVVCALL(OPFVF2, vfdiv_vf_h, OP_UUU_H, H2, H2, float16_div)
RVVCALL(OPFVF2, vfdiv_vf_w, OP_UUU_W, H4, H4, float32_div)
RVVCALL(OPFVF2, vfdiv_vf_d, OP_UUU_D, H8, H8, float64_div)
GEN_VEXT_VF(vfdiv_vf_h, 2, 2)
GEN_VEXT_VF(vfdiv_vf_w, 4, 4)
GEN_VEXT_VF(vfdiv_vf_d, 8, 8)

static uint16_t float16_rdiv(uint16_t a, uint16_t b, float_status *s)
{
    return float16_div(b, a, s);
}

static uint32_t float32_rdiv(uint32_t a, uint32_t b, float_status *s)
{
    return float32_div(b, a, s);
}

static uint64_t float64_rdiv(uint64_t a, uint64_t b, float_status *s)
{
    return float64_div(b, a, s);
}

RVVCALL(OPFVF2, vfrdiv_vf_h, OP_UUU_H, H2, H2, float16_rdiv)
RVVCALL(OPFVF2, vfrdiv_vf_w, OP_UUU_W, H4, H4, float32_rdiv)
RVVCALL(OPFVF2, vfrdiv_vf_d, OP_UUU_D, H8, H8, float64_rdiv)
GEN_VEXT_VF(vfrdiv_vf_h, 2, 2)
GEN_VEXT_VF(vfrdiv_vf_w, 4, 4)
GEN_VEXT_VF(vfrdiv_vf_d, 8, 8)

/* Vector Widening Floating-Point Multiply */
static uint32_t vfwmul16(uint16_t a, uint16_t b, float_status *s)
{
    return float32_mul(float16_to_float32(a, true, s),
            float16_to_float32(b, true, s), s);
}

static uint64_t vfwmul32(uint32_t a, uint32_t b, float_status *s)
{
    return float64_mul(float32_to_float64(a, s),
            float32_to_float64(b, s), s);

}
RVVCALL(OPFVV2, vfwmul_vv_h, WOP_UUU_H, H4, H2, H2, vfwmul16)
RVVCALL(OPFVV2, vfwmul_vv_w, WOP_UUU_W, H8, H4, H4, vfwmul32)
GEN_VEXT_VV_ENV(vfwmul_vv_h, 2, 4)
GEN_VEXT_VV_ENV(vfwmul_vv_w, 4, 8)
RVVCALL(OPFVF2, vfwmul_vf_h, WOP_UUU_H, H4, H2, vfwmul16)
RVVCALL(OPFVF2, vfwmul_vf_w, WOP_UUU_W, H8, H4, vfwmul32)
GEN_VEXT_VF(vfwmul_vf_h, 2, 4)
GEN_VEXT_VF(vfwmul_vf_w, 4, 8)

/* Vector Single-Width Floating-Point Fused Multiply-Add Instructions */
#define OPFVV3(NAME, TD, T1, T2, TX1, TX2, HD, HS1, HS2, OP)       \
static void do_##NAME(void *vd, void *vs1, void *vs2, int i,       \
        CPURISCVState *env)                                        \
{                                                                  \
    TX1 s1 = *((T1 *)vs1 + HS1(i));                                \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                                \
    TD d = *((TD *)vd + HD(i));                                    \
    *((TD *)vd + HD(i)) = OP(s2, s1, d, &env->fp_status);          \
}

static uint16_t fmacc16(uint16_t a, uint16_t b, uint16_t d, float_status *s)
{
    return float16_muladd(a, b, d, 0, s);
}

static uint32_t fmacc32(uint32_t a, uint32_t b, uint32_t d, float_status *s)
{
    return float32_muladd(a, b, d, 0, s);
}

static uint64_t fmacc64(uint64_t a, uint64_t b, uint64_t d, float_status *s)
{
    return float64_muladd(a, b, d, 0, s);
}

RVVCALL(OPFVV3, vfmacc_vv_h, OP_UUU_H, H2, H2, H2, fmacc16)
RVVCALL(OPFVV3, vfmacc_vv_w, OP_UUU_W, H4, H4, H4, fmacc32)
RVVCALL(OPFVV3, vfmacc_vv_d, OP_UUU_D, H8, H8, H8, fmacc64)
GEN_VEXT_VV_ENV(vfmacc_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfmacc_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfmacc_vv_d, 8, 8)

#define OPFVF3(NAME, TD, T1, T2, TX1, TX2, HD, HS2, OP)           \
static void do_##NAME(void *vd, uint64_t s1, void *vs2, int i,    \
        CPURISCVState *env)                                       \
{                                                                 \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                               \
    TD d = *((TD *)vd + HD(i));                                   \
    *((TD *)vd + HD(i)) = OP(s2, (TX1)(T1)s1, d, &env->fp_status);\
}

RVVCALL(OPFVF3, vfmacc_vf_h, OP_UUU_H, H2, H2, fmacc16)
RVVCALL(OPFVF3, vfmacc_vf_w, OP_UUU_W, H4, H4, fmacc32)
RVVCALL(OPFVF3, vfmacc_vf_d, OP_UUU_D, H8, H8, fmacc64)
GEN_VEXT_VF(vfmacc_vf_h, 2, 2)
GEN_VEXT_VF(vfmacc_vf_w, 4, 4)
GEN_VEXT_VF(vfmacc_vf_d, 8, 8)

static uint16_t fnmacc16(uint16_t a, uint16_t b, uint16_t d, float_status *s)
{
    return float16_muladd(a, b, d,
            float_muladd_negate_c | float_muladd_negate_product, s);
}

static uint32_t fnmacc32(uint32_t a, uint32_t b, uint32_t d, float_status *s)
{
    return float32_muladd(a, b, d,
            float_muladd_negate_c | float_muladd_negate_product, s);
}

static uint64_t fnmacc64(uint64_t a, uint64_t b, uint64_t d, float_status *s)
{
    return float64_muladd(a, b, d,
            float_muladd_negate_c | float_muladd_negate_product, s);
}

RVVCALL(OPFVV3, vfnmacc_vv_h, OP_UUU_H, H2, H2, H2, fnmacc16)
RVVCALL(OPFVV3, vfnmacc_vv_w, OP_UUU_W, H4, H4, H4, fnmacc32)
RVVCALL(OPFVV3, vfnmacc_vv_d, OP_UUU_D, H8, H8, H8, fnmacc64)
GEN_VEXT_VV_ENV(vfnmacc_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfnmacc_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfnmacc_vv_d, 8, 8)
RVVCALL(OPFVF3, vfnmacc_vf_h, OP_UUU_H, H2, H2, fnmacc16)
RVVCALL(OPFVF3, vfnmacc_vf_w, OP_UUU_W, H4, H4, fnmacc32)
RVVCALL(OPFVF3, vfnmacc_vf_d, OP_UUU_D, H8, H8, fnmacc64)
GEN_VEXT_VF(vfnmacc_vf_h, 2, 2)
GEN_VEXT_VF(vfnmacc_vf_w, 4, 4)
GEN_VEXT_VF(vfnmacc_vf_d, 8, 8)

static uint16_t fmsac16(uint16_t a, uint16_t b, uint16_t d, float_status *s)
{
    return float16_muladd(a, b, d, float_muladd_negate_c, s);
}

static uint32_t fmsac32(uint32_t a, uint32_t b, uint32_t d, float_status *s)
{
    return float32_muladd(a, b, d, float_muladd_negate_c, s);
}

static uint64_t fmsac64(uint64_t a, uint64_t b, uint64_t d, float_status *s)
{
    return float64_muladd(a, b, d, float_muladd_negate_c, s);
}

RVVCALL(OPFVV3, vfmsac_vv_h, OP_UUU_H, H2, H2, H2, fmsac16)
RVVCALL(OPFVV3, vfmsac_vv_w, OP_UUU_W, H4, H4, H4, fmsac32)
RVVCALL(OPFVV3, vfmsac_vv_d, OP_UUU_D, H8, H8, H8, fmsac64)
GEN_VEXT_VV_ENV(vfmsac_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfmsac_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfmsac_vv_d, 8, 8)
RVVCALL(OPFVF3, vfmsac_vf_h, OP_UUU_H, H2, H2, fmsac16)
RVVCALL(OPFVF3, vfmsac_vf_w, OP_UUU_W, H4, H4, fmsac32)
RVVCALL(OPFVF3, vfmsac_vf_d, OP_UUU_D, H8, H8, fmsac64)
GEN_VEXT_VF(vfmsac_vf_h, 2, 2)
GEN_VEXT_VF(vfmsac_vf_w, 4, 4)
GEN_VEXT_VF(vfmsac_vf_d, 8, 8)

static uint16_t fnmsac16(uint16_t a, uint16_t b, uint16_t d, float_status *s)
{
    return float16_muladd(a, b, d, float_muladd_negate_product, s);
}

static uint32_t fnmsac32(uint32_t a, uint32_t b, uint32_t d, float_status *s)
{
    return float32_muladd(a, b, d, float_muladd_negate_product, s);
}

static uint64_t fnmsac64(uint64_t a, uint64_t b, uint64_t d, float_status *s)
{
    return float64_muladd(a, b, d, float_muladd_negate_product, s);
}

RVVCALL(OPFVV3, vfnmsac_vv_h, OP_UUU_H, H2, H2, H2, fnmsac16)
RVVCALL(OPFVV3, vfnmsac_vv_w, OP_UUU_W, H4, H4, H4, fnmsac32)
RVVCALL(OPFVV3, vfnmsac_vv_d, OP_UUU_D, H8, H8, H8, fnmsac64)
GEN_VEXT_VV_ENV(vfnmsac_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfnmsac_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfnmsac_vv_d, 8, 8)
RVVCALL(OPFVF3, vfnmsac_vf_h, OP_UUU_H, H2, H2, fnmsac16)
RVVCALL(OPFVF3, vfnmsac_vf_w, OP_UUU_W, H4, H4, fnmsac32)
RVVCALL(OPFVF3, vfnmsac_vf_d, OP_UUU_D, H8, H8, fnmsac64)
GEN_VEXT_VF(vfnmsac_vf_h, 2, 2)
GEN_VEXT_VF(vfnmsac_vf_w, 4, 4)
GEN_VEXT_VF(vfnmsac_vf_d, 8, 8)

static uint16_t fmadd16(uint16_t a, uint16_t b, uint16_t d, float_status *s)
{
    return float16_muladd(d, b, a, 0, s);
}

static uint32_t fmadd32(uint32_t a, uint32_t b, uint32_t d, float_status *s)
{
    return float32_muladd(d, b, a, 0, s);
}

static uint64_t fmadd64(uint64_t a, uint64_t b, uint64_t d, float_status *s)
{
    return float64_muladd(d, b, a, 0, s);
}

RVVCALL(OPFVV3, vfmadd_vv_h, OP_UUU_H, H2, H2, H2, fmadd16)
RVVCALL(OPFVV3, vfmadd_vv_w, OP_UUU_W, H4, H4, H4, fmadd32)
RVVCALL(OPFVV3, vfmadd_vv_d, OP_UUU_D, H8, H8, H8, fmadd64)
GEN_VEXT_VV_ENV(vfmadd_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfmadd_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfmadd_vv_d, 8, 8)
RVVCALL(OPFVF3, vfmadd_vf_h, OP_UUU_H, H2, H2, fmadd16)
RVVCALL(OPFVF3, vfmadd_vf_w, OP_UUU_W, H4, H4, fmadd32)
RVVCALL(OPFVF3, vfmadd_vf_d, OP_UUU_D, H8, H8, fmadd64)
GEN_VEXT_VF(vfmadd_vf_h, 2, 2)
GEN_VEXT_VF(vfmadd_vf_w, 4, 4)
GEN_VEXT_VF(vfmadd_vf_d, 8, 8)

static uint16_t fnmadd16(uint16_t a, uint16_t b, uint16_t d, float_status *s)
{
    return float16_muladd(d, b, a,
            float_muladd_negate_c | float_muladd_negate_product, s);
}

static uint32_t fnmadd32(uint32_t a, uint32_t b, uint32_t d, float_status *s)
{
    return float32_muladd(d, b, a,
            float_muladd_negate_c | float_muladd_negate_product, s);
}

static uint64_t fnmadd64(uint64_t a, uint64_t b, uint64_t d, float_status *s)
{
    return float64_muladd(d, b, a,
            float_muladd_negate_c | float_muladd_negate_product, s);
}

RVVCALL(OPFVV3, vfnmadd_vv_h, OP_UUU_H, H2, H2, H2, fnmadd16)
RVVCALL(OPFVV3, vfnmadd_vv_w, OP_UUU_W, H4, H4, H4, fnmadd32)
RVVCALL(OPFVV3, vfnmadd_vv_d, OP_UUU_D, H8, H8, H8, fnmadd64)
GEN_VEXT_VV_ENV(vfnmadd_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfnmadd_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfnmadd_vv_d, 8, 8)
RVVCALL(OPFVF3, vfnmadd_vf_h, OP_UUU_H, H2, H2, fnmadd16)
RVVCALL(OPFVF3, vfnmadd_vf_w, OP_UUU_W, H4, H4, fnmadd32)
RVVCALL(OPFVF3, vfnmadd_vf_d, OP_UUU_D, H8, H8, fnmadd64)
GEN_VEXT_VF(vfnmadd_vf_h, 2, 2)
GEN_VEXT_VF(vfnmadd_vf_w, 4, 4)
GEN_VEXT_VF(vfnmadd_vf_d, 8, 8)

static uint16_t fmsub16(uint16_t a, uint16_t b, uint16_t d, float_status *s)
{
    return float16_muladd(d, b, a, float_muladd_negate_c, s);
}

static uint32_t fmsub32(uint32_t a, uint32_t b, uint32_t d, float_status *s)
{
    return float32_muladd(d, b, a, float_muladd_negate_c, s);
}

static uint64_t fmsub64(uint64_t a, uint64_t b, uint64_t d, float_status *s)
{
    return float64_muladd(d, b, a, float_muladd_negate_c, s);
}

RVVCALL(OPFVV3, vfmsub_vv_h, OP_UUU_H, H2, H2, H2, fmsub16)
RVVCALL(OPFVV3, vfmsub_vv_w, OP_UUU_W, H4, H4, H4, fmsub32)
RVVCALL(OPFVV3, vfmsub_vv_d, OP_UUU_D, H8, H8, H8, fmsub64)
GEN_VEXT_VV_ENV(vfmsub_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfmsub_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfmsub_vv_d, 8, 8)
RVVCALL(OPFVF3, vfmsub_vf_h, OP_UUU_H, H2, H2, fmsub16)
RVVCALL(OPFVF3, vfmsub_vf_w, OP_UUU_W, H4, H4, fmsub32)
RVVCALL(OPFVF3, vfmsub_vf_d, OP_UUU_D, H8, H8, fmsub64)
GEN_VEXT_VF(vfmsub_vf_h, 2, 2)
GEN_VEXT_VF(vfmsub_vf_w, 4, 4)
GEN_VEXT_VF(vfmsub_vf_d, 8, 8)

static uint16_t fnmsub16(uint16_t a, uint16_t b, uint16_t d, float_status *s)
{
    return float16_muladd(d, b, a, float_muladd_negate_product, s);
}

static uint32_t fnmsub32(uint32_t a, uint32_t b, uint32_t d, float_status *s)
{
    return float32_muladd(d, b, a, float_muladd_negate_product, s);
}

static uint64_t fnmsub64(uint64_t a, uint64_t b, uint64_t d, float_status *s)
{
    return float64_muladd(d, b, a, float_muladd_negate_product, s);
}

RVVCALL(OPFVV3, vfnmsub_vv_h, OP_UUU_H, H2, H2, H2, fnmsub16)
RVVCALL(OPFVV3, vfnmsub_vv_w, OP_UUU_W, H4, H4, H4, fnmsub32)
RVVCALL(OPFVV3, vfnmsub_vv_d, OP_UUU_D, H8, H8, H8, fnmsub64)
GEN_VEXT_VV_ENV(vfnmsub_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfnmsub_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfnmsub_vv_d, 8, 8)
RVVCALL(OPFVF3, vfnmsub_vf_h, OP_UUU_H, H2, H2, fnmsub16)
RVVCALL(OPFVF3, vfnmsub_vf_w, OP_UUU_W, H4, H4, fnmsub32)
RVVCALL(OPFVF3, vfnmsub_vf_d, OP_UUU_D, H8, H8, fnmsub64)
GEN_VEXT_VF(vfnmsub_vf_h, 2, 2)
GEN_VEXT_VF(vfnmsub_vf_w, 4, 4)
GEN_VEXT_VF(vfnmsub_vf_d, 8, 8)

/* Vector Widening Floating-Point Fused Multiply-Add Instructions */
static uint32_t fwmacc16(uint16_t a, uint16_t b, uint32_t d, float_status *s)
{
    return float32_muladd(float16_to_float32(a, true, s),
                        float16_to_float32(b, true, s), d, 0, s);
}

static uint64_t fwmacc32(uint32_t a, uint32_t b, uint64_t d, float_status *s)
{
    return float64_muladd(float32_to_float64(a, s),
                        float32_to_float64(b, s), d, 0, s);
}

RVVCALL(OPFVV3, vfwmacc_vv_h, WOP_UUU_H, H4, H2, H2, fwmacc16)
RVVCALL(OPFVV3, vfwmacc_vv_w, WOP_UUU_W, H8, H4, H4, fwmacc32)
GEN_VEXT_VV_ENV(vfwmacc_vv_h, 2, 4)
GEN_VEXT_VV_ENV(vfwmacc_vv_w, 4, 8)
RVVCALL(OPFVF3, vfwmacc_vf_h, WOP_UUU_H, H4, H2, fwmacc16)
RVVCALL(OPFVF3, vfwmacc_vf_w, WOP_UUU_W, H8, H4, fwmacc32)
GEN_VEXT_VF(vfwmacc_vf_h, 2, 4)
GEN_VEXT_VF(vfwmacc_vf_w, 4, 8)

static uint32_t fwnmacc16(uint16_t a, uint16_t b, uint32_t d, float_status *s)
{
    return float32_muladd(float16_to_float32(a, true, s),
                        float16_to_float32(b, true, s), d,
                        float_muladd_negate_c | float_muladd_negate_product, s);
}

static uint64_t fwnmacc32(uint32_t a, uint32_t b, uint64_t d, float_status *s)
{
    return float64_muladd(float32_to_float64(a, s),
                        float32_to_float64(b, s), d,
                        float_muladd_negate_c | float_muladd_negate_product, s);
}

RVVCALL(OPFVV3, vfwnmacc_vv_h, WOP_UUU_H, H4, H2, H2, fwnmacc16)
RVVCALL(OPFVV3, vfwnmacc_vv_w, WOP_UUU_W, H8, H4, H4, fwnmacc32)
GEN_VEXT_VV_ENV(vfwnmacc_vv_h, 2, 4)
GEN_VEXT_VV_ENV(vfwnmacc_vv_w, 4, 8)
RVVCALL(OPFVF3, vfwnmacc_vf_h, WOP_UUU_H, H4, H2, fwnmacc16)
RVVCALL(OPFVF3, vfwnmacc_vf_w, WOP_UUU_W, H8, H4, fwnmacc32)
GEN_VEXT_VF(vfwnmacc_vf_h, 2, 4)
GEN_VEXT_VF(vfwnmacc_vf_w, 4, 8)

static uint32_t fwmsac16(uint16_t a, uint16_t b, uint32_t d, float_status *s)
{
    return float32_muladd(float16_to_float32(a, true, s),
                        float16_to_float32(b, true, s), d,
                        float_muladd_negate_c, s);
}

static uint64_t fwmsac32(uint32_t a, uint32_t b, uint64_t d, float_status *s)
{
    return float64_muladd(float32_to_float64(a, s),
                        float32_to_float64(b, s), d,
                        float_muladd_negate_c, s);
}

RVVCALL(OPFVV3, vfwmsac_vv_h, WOP_UUU_H, H4, H2, H2, fwmsac16)
RVVCALL(OPFVV3, vfwmsac_vv_w, WOP_UUU_W, H8, H4, H4, fwmsac32)
GEN_VEXT_VV_ENV(vfwmsac_vv_h, 2, 4)
GEN_VEXT_VV_ENV(vfwmsac_vv_w, 4, 8)
RVVCALL(OPFVF3, vfwmsac_vf_h, WOP_UUU_H, H4, H2, fwmsac16)
RVVCALL(OPFVF3, vfwmsac_vf_w, WOP_UUU_W, H8, H4, fwmsac32)
GEN_VEXT_VF(vfwmsac_vf_h, 2, 4)
GEN_VEXT_VF(vfwmsac_vf_w, 4, 8)

static uint32_t fwnmsac16(uint16_t a, uint16_t b, uint32_t d, float_status *s)
{
    return float32_muladd(float16_to_float32(a, true, s),
                        float16_to_float32(b, true, s), d,
                        float_muladd_negate_product, s);
}

static uint64_t fwnmsac32(uint32_t a, uint32_t b, uint64_t d, float_status *s)
{
    return float64_muladd(float32_to_float64(a, s),
                        float32_to_float64(b, s), d,
                        float_muladd_negate_product, s);
}

RVVCALL(OPFVV3, vfwnmsac_vv_h, WOP_UUU_H, H4, H2, H2, fwnmsac16)
RVVCALL(OPFVV3, vfwnmsac_vv_w, WOP_UUU_W, H8, H4, H4, fwnmsac32)
GEN_VEXT_VV_ENV(vfwnmsac_vv_h, 2, 4)
GEN_VEXT_VV_ENV(vfwnmsac_vv_w, 4, 8)
RVVCALL(OPFVF3, vfwnmsac_vf_h, WOP_UUU_H, H4, H2, fwnmsac16)
RVVCALL(OPFVF3, vfwnmsac_vf_w, WOP_UUU_W, H8, H4, fwnmsac32)
GEN_VEXT_VF(vfwnmsac_vf_h, 2, 4)
GEN_VEXT_VF(vfwnmsac_vf_w, 4, 8)

/* Vector Floating-Point Square-Root Instruction */
/* (TD, T2, TX2) */
#define OP_UU_H uint16_t, uint16_t, uint16_t
#define OP_UU_W uint32_t, uint32_t, uint32_t
#define OP_UU_D uint64_t, uint64_t, uint64_t

#define OPFVV1(NAME, TD, T2, TX2, HD, HS2, OP)        \
static void do_##NAME(void *vd, void *vs2, int i,      \
        CPURISCVState *env)                            \
{                                                      \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                    \
    *((TD *)vd + HD(i)) = OP(s2, &env->fp_status);     \
}

#define GEN_VEXT_V_ENV(NAME, ESZ, DSZ)                 \
void HELPER(NAME)(void *vd, void *v0, void *vs2,       \
        CPURISCVState *env, uint32_t desc)             \
{                                                      \
    uint32_t vm = vext_vm(desc);                       \
    uint32_t vl = env->vl;                             \
    uint32_t i;                                        \
                                                       \
    if (vl == 0) {                                     \
        return;                                        \
    }                                                  \
    for (i = 0; i < vl; i++) {                         \
        if (!vm && !vext_elem_mask(v0, i)) {           \
            continue;                                  \
        }                                              \
        do_##NAME(vd, vs2, i, env);                    \
    }                                                  \
}

RVVCALL(OPFVV1, vfsqrt_v_h, OP_UU_H, H2, H2, float16_sqrt)
RVVCALL(OPFVV1, vfsqrt_v_w, OP_UU_W, H4, H4, float32_sqrt)
RVVCALL(OPFVV1, vfsqrt_v_d, OP_UU_D, H8, H8, float64_sqrt)
GEN_VEXT_V_ENV(vfsqrt_v_h, 2, 2)
GEN_VEXT_V_ENV(vfsqrt_v_w, 4, 4)
GEN_VEXT_V_ENV(vfsqrt_v_d, 8, 8)

/* Vector Floating-Point MIN/MAX Instructions */
RVVCALL(OPFVV2, vfmin_vv_h, OP_UUU_H, H2, H2, H2, float16_minnum)
RVVCALL(OPFVV2, vfmin_vv_w, OP_UUU_W, H4, H4, H4, float32_minnum)
RVVCALL(OPFVV2, vfmin_vv_d, OP_UUU_D, H8, H8, H8, float64_minnum)
GEN_VEXT_VV_ENV(vfmin_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfmin_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfmin_vv_d, 8, 8)
RVVCALL(OPFVF2, vfmin_vf_h, OP_UUU_H, H2, H2, float16_minnum)
RVVCALL(OPFVF2, vfmin_vf_w, OP_UUU_W, H4, H4, float32_minnum)
RVVCALL(OPFVF2, vfmin_vf_d, OP_UUU_D, H8, H8, float64_minnum)
GEN_VEXT_VF(vfmin_vf_h, 2, 2)
GEN_VEXT_VF(vfmin_vf_w, 4, 4)
GEN_VEXT_VF(vfmin_vf_d, 8, 8)

RVVCALL(OPFVV2, vfmax_vv_h, OP_UUU_H, H2, H2, H2, float16_maxnum)
RVVCALL(OPFVV2, vfmax_vv_w, OP_UUU_W, H4, H4, H4, float32_maxnum)
RVVCALL(OPFVV2, vfmax_vv_d, OP_UUU_D, H8, H8, H8, float64_maxnum)
GEN_VEXT_VV_ENV(vfmax_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfmax_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfmax_vv_d, 8, 8)
RVVCALL(OPFVF2, vfmax_vf_h, OP_UUU_H, H2, H2, float16_maxnum)
RVVCALL(OPFVF2, vfmax_vf_w, OP_UUU_W, H4, H4, float32_maxnum)
RVVCALL(OPFVF2, vfmax_vf_d, OP_UUU_D, H8, H8, float64_maxnum)
GEN_VEXT_VF(vfmax_vf_h, 2, 2)
GEN_VEXT_VF(vfmax_vf_w, 4, 4)
GEN_VEXT_VF(vfmax_vf_d, 8, 8)

/* Vector Floating-Point Sign-Injection Instructions */
static uint16_t fsgnj16(uint16_t a, uint16_t b, float_status *s)
{
    return deposit64(b, 0, 15, a);
}

static uint32_t fsgnj32(uint32_t a, uint32_t b, float_status *s)
{
    return deposit64(b, 0, 31, a);
}

static uint64_t fsgnj64(uint64_t a, uint64_t b, float_status *s)
{
    return deposit64(b, 0, 63, a);
}

RVVCALL(OPFVV2, vfsgnj_vv_h, OP_UUU_H, H2, H2, H2, fsgnj16)
RVVCALL(OPFVV2, vfsgnj_vv_w, OP_UUU_W, H4, H4, H4, fsgnj32)
RVVCALL(OPFVV2, vfsgnj_vv_d, OP_UUU_D, H8, H8, H8, fsgnj64)
GEN_VEXT_VV_ENV(vfsgnj_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfsgnj_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfsgnj_vv_d, 8, 8)
RVVCALL(OPFVF2, vfsgnj_vf_h, OP_UUU_H, H2, H2, fsgnj16)
RVVCALL(OPFVF2, vfsgnj_vf_w, OP_UUU_W, H4, H4, fsgnj32)
RVVCALL(OPFVF2, vfsgnj_vf_d, OP_UUU_D, H8, H8, fsgnj64)
GEN_VEXT_VF(vfsgnj_vf_h, 2, 2)
GEN_VEXT_VF(vfsgnj_vf_w, 4, 4)
GEN_VEXT_VF(vfsgnj_vf_d, 8, 8)

static uint16_t fsgnjn16(uint16_t a, uint16_t b, float_status *s)
{
    return deposit64(~b, 0, 15, a);
}

static uint32_t fsgnjn32(uint32_t a, uint32_t b, float_status *s)
{
    return deposit64(~b, 0, 31, a);
}

static uint64_t fsgnjn64(uint64_t a, uint64_t b, float_status *s)
{
    return deposit64(~b, 0, 63, a);
}

RVVCALL(OPFVV2, vfsgnjn_vv_h, OP_UUU_H, H2, H2, H2, fsgnjn16)
RVVCALL(OPFVV2, vfsgnjn_vv_w, OP_UUU_W, H4, H4, H4, fsgnjn32)
RVVCALL(OPFVV2, vfsgnjn_vv_d, OP_UUU_D, H8, H8, H8, fsgnjn64)
GEN_VEXT_VV_ENV(vfsgnjn_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfsgnjn_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfsgnjn_vv_d, 8, 8)
RVVCALL(OPFVF2, vfsgnjn_vf_h, OP_UUU_H, H2, H2, fsgnjn16)
RVVCALL(OPFVF2, vfsgnjn_vf_w, OP_UUU_W, H4, H4, fsgnjn32)
RVVCALL(OPFVF2, vfsgnjn_vf_d, OP_UUU_D, H8, H8, fsgnjn64)
GEN_VEXT_VF(vfsgnjn_vf_h, 2, 2)
GEN_VEXT_VF(vfsgnjn_vf_w, 4, 4)
GEN_VEXT_VF(vfsgnjn_vf_d, 8, 8)

static uint16_t fsgnjx16(uint16_t a, uint16_t b, float_status *s)
{
    return deposit64(b ^ a, 0, 15, a);
}

static uint32_t fsgnjx32(uint32_t a, uint32_t b, float_status *s)
{
    return deposit64(b ^ a, 0, 31, a);
}

static uint64_t fsgnjx64(uint64_t a, uint64_t b, float_status *s)
{
    return deposit64(b ^ a, 0, 63, a);
}

RVVCALL(OPFVV2, vfsgnjx_vv_h, OP_UUU_H, H2, H2, H2, fsgnjx16)
RVVCALL(OPFVV2, vfsgnjx_vv_w, OP_UUU_W, H4, H4, H4, fsgnjx32)
RVVCALL(OPFVV2, vfsgnjx_vv_d, OP_UUU_D, H8, H8, H8, fsgnjx64)
GEN_VEXT_VV_ENV(vfsgnjx_vv_h, 2, 2)
GEN_VEXT_VV_ENV(vfsgnjx_vv_w, 4, 4)
GEN_VEXT_VV_ENV(vfsgnjx_vv_d, 8, 8)
RVVCALL(OPFVF2, vfsgnjx_vf_h, OP_UUU_H, H2, H2, fsgnjx16)
RVVCALL(OPFVF2, vfsgnjx_vf_w, OP_UUU_W, H4, H4, fsgnjx32)
RVVCALL(OPFVF2, vfsgnjx_vf_d, OP_UUU_D, H8, H8, fsgnjx64)
GEN_VEXT_VF(vfsgnjx_vf_h, 2, 2)
GEN_VEXT_VF(vfsgnjx_vf_w, 4, 4)
GEN_VEXT_VF(vfsgnjx_vf_d, 8, 8)

/* Vector Floating-Point Compare Instructions */
#define GEN_VEXT_CMP_VV_ENV(NAME, ETYPE, H, DO_OP)            \
void HELPER(NAME)(void *vd, void *v0, void *vs1, void *vs2,   \
                  CPURISCVState *env, uint32_t desc)          \
{                                                             \
    uint32_t vm = vext_vm(desc);                              \
    uint32_t vl = env->vl;                                    \
    uint32_t vlmax = vext_maxsz(desc) / sizeof(ETYPE);        \
    uint32_t i;                                               \
                                                              \
    for (i = 0; i < vl; i++) {                                \
        ETYPE s1 = *((ETYPE *)vs1 + H(i));                    \
        ETYPE s2 = *((ETYPE *)vs2 + H(i));                    \
        if (!vm && !vext_elem_mask(v0, i)) {                  \
            continue;                                         \
        }                                                     \
        vext_set_elem_mask(vd, i,                             \
                           DO_OP(s2, s1, &env->fp_status));   \
    }                                                         \
    for (; i < vlmax; i++) {                                  \
        vext_set_elem_mask(vd, i, 0);                         \
    }                                                         \
}

GEN_VEXT_CMP_VV_ENV(vmfeq_vv_h, uint16_t, H2, float16_eq_quiet)
GEN_VEXT_CMP_VV_ENV(vmfeq_vv_w, uint32_t, H4, float32_eq_quiet)
GEN_VEXT_CMP_VV_ENV(vmfeq_vv_d, uint64_t, H8, float64_eq_quiet)

#define GEN_VEXT_CMP_VF(NAME, ETYPE, H, DO_OP)                      \
void HELPER(NAME)(void *vd, void *v0, uint64_t s1, void *vs2,       \
                  CPURISCVState *env, uint32_t desc)                \
{                                                                   \
    uint32_t vm = vext_vm(desc);                                    \
    uint32_t vl = env->vl;                                          \
    uint32_t vlmax = vext_maxsz(desc) / sizeof(ETYPE);              \
    uint32_t i;                                                     \
                                                                    \
    for (i = 0; i < vl; i++) {                                      \
        ETYPE s2 = *((ETYPE *)vs2 + H(i));                          \
        if (!vm && !vext_elem_mask(v0, i)) {                        \
            continue;                                               \
        }                                                           \
        vext_set_elem_mask(vd, i,                                   \
                           DO_OP(s2, (ETYPE)s1, &env->fp_status));  \
    }                                                               \
    for (; i < vlmax; i++) {                                        \
        vext_set_elem_mask(vd, i, 0);                               \
    }                                                               \
}

GEN_VEXT_CMP_VF(vmfeq_vf_h, uint16_t, H2, float16_eq_quiet)
GEN_VEXT_CMP_VF(vmfeq_vf_w, uint32_t, H4, float32_eq_quiet)
GEN_VEXT_CMP_VF(vmfeq_vf_d, uint64_t, H8, float64_eq_quiet)

static bool vmfne16(uint16_t a, uint16_t b, float_status *s)
{
    FloatRelation compare = float16_compare_quiet(a, b, s);
    return compare != float_relation_equal;
}

static bool vmfne32(uint32_t a, uint32_t b, float_status *s)
{
    FloatRelation compare = float32_compare_quiet(a, b, s);
    return compare != float_relation_equal;
}

static bool vmfne64(uint64_t a, uint64_t b, float_status *s)
{
    FloatRelation compare = float64_compare_quiet(a, b, s);
    return compare != float_relation_equal;
}

GEN_VEXT_CMP_VV_ENV(vmfne_vv_h, uint16_t, H2, vmfne16)
GEN_VEXT_CMP_VV_ENV(vmfne_vv_w, uint32_t, H4, vmfne32)
GEN_VEXT_CMP_VV_ENV(vmfne_vv_d, uint64_t, H8, vmfne64)
GEN_VEXT_CMP_VF(vmfne_vf_h, uint16_t, H2, vmfne16)
GEN_VEXT_CMP_VF(vmfne_vf_w, uint32_t, H4, vmfne32)
GEN_VEXT_CMP_VF(vmfne_vf_d, uint64_t, H8, vmfne64)

GEN_VEXT_CMP_VV_ENV(vmflt_vv_h, uint16_t, H2, float16_lt)
GEN_VEXT_CMP_VV_ENV(vmflt_vv_w, uint32_t, H4, float32_lt)
GEN_VEXT_CMP_VV_ENV(vmflt_vv_d, uint64_t, H8, float64_lt)
GEN_VEXT_CMP_VF(vmflt_vf_h, uint16_t, H2, float16_lt)
GEN_VEXT_CMP_VF(vmflt_vf_w, uint32_t, H4, float32_lt)
GEN_VEXT_CMP_VF(vmflt_vf_d, uint64_t, H8, float64_lt)

GEN_VEXT_CMP_VV_ENV(vmfle_vv_h, uint16_t, H2, float16_le)
GEN_VEXT_CMP_VV_ENV(vmfle_vv_w, uint32_t, H4, float32_le)
GEN_VEXT_CMP_VV_ENV(vmfle_vv_d, uint64_t, H8, float64_le)
GEN_VEXT_CMP_VF(vmfle_vf_h, uint16_t, H2, float16_le)
GEN_VEXT_CMP_VF(vmfle_vf_w, uint32_t, H4, float32_le)
GEN_VEXT_CMP_VF(vmfle_vf_d, uint64_t, H8, float64_le)

static bool vmfgt16(uint16_t a, uint16_t b, float_status *s)
{
    FloatRelation compare = float16_compare(a, b, s);
    return compare == float_relation_greater;
}

static bool vmfgt32(uint32_t a, uint32_t b, float_status *s)
{
    FloatRelation compare = float32_compare(a, b, s);
    return compare == float_relation_greater;
}

static bool vmfgt64(uint64_t a, uint64_t b, float_status *s)
{
    FloatRelation compare = float64_compare(a, b, s);
    return compare == float_relation_greater;
}

GEN_VEXT_CMP_VF(vmfgt_vf_h, uint16_t, H2, vmfgt16)
GEN_VEXT_CMP_VF(vmfgt_vf_w, uint32_t, H4, vmfgt32)
GEN_VEXT_CMP_VF(vmfgt_vf_d, uint64_t, H8, vmfgt64)

static bool vmfge16(uint16_t a, uint16_t b, float_status *s)
{
    FloatRelation compare = float16_compare(a, b, s);
    return compare == float_relation_greater ||
           compare == float_relation_equal;
}

static bool vmfge32(uint32_t a, uint32_t b, float_status *s)
{
    FloatRelation compare = float32_compare(a, b, s);
    return compare == float_relation_greater ||
           compare == float_relation_equal;
}

static bool vmfge64(uint64_t a, uint64_t b, float_status *s)
{
    FloatRelation compare = float64_compare(a, b, s);
    return compare == float_relation_greater ||
           compare == float_relation_equal;
}

GEN_VEXT_CMP_VF(vmfge_vf_h, uint16_t, H2, vmfge16)
GEN_VEXT_CMP_VF(vmfge_vf_w, uint32_t, H4, vmfge32)
GEN_VEXT_CMP_VF(vmfge_vf_d, uint64_t, H8, vmfge64)

GEN_VEXT_CMP_VV_ENV(vmford_vv_h, uint16_t, H2, !float16_unordered_quiet)
GEN_VEXT_CMP_VV_ENV(vmford_vv_w, uint32_t, H4, !float32_unordered_quiet)
GEN_VEXT_CMP_VV_ENV(vmford_vv_d, uint64_t, H8, !float64_unordered_quiet)
GEN_VEXT_CMP_VF(vmford_vf_h, uint16_t, H2, !float16_unordered_quiet)
GEN_VEXT_CMP_VF(vmford_vf_w, uint32_t, H4, !float32_unordered_quiet)
GEN_VEXT_CMP_VF(vmford_vf_d, uint64_t, H8, !float64_unordered_quiet)

/* Vector Floating-Point Classify Instruction */
#define OPIVV1(NAME, TD, T2, TX2, HD, HS2, OP)         \
static void do_##NAME(void *vd, void *vs2, int i)      \
{                                                      \
    TX2 s2 = *((T2 *)vs2 + HS2(i));                    \
    *((TD *)vd + HD(i)) = OP(s2);                      \
}

#define GEN_VEXT_V(NAME, ESZ, DSZ)                     \
void HELPER(NAME)(void *vd, void *v0, void *vs2,       \
                  CPURISCVState *env, uint32_t desc)   \
{                                                      \
    uint32_t vm = vext_vm(desc);                       \
    uint32_t vl = env->vl;                             \
    uint32_t i;                                        \
                                                       \
    for (i = 0; i < vl; i++) {                         \
        if (!vm && !vext_elem_mask(v0, i)) {           \
            continue;                                  \
        }                                              \
        do_##NAME(vd, vs2, i);                         \
    }                                                  \
}

target_ulong fclass_h(uint64_t frs1)
{
    float16 f = frs1;
    bool sign = float16_is_neg(f);

    if (float16_is_infinity(f)) {
        return sign ? 1 << 0 : 1 << 7;
    } else if (float16_is_zero(f)) {
        return sign ? 1 << 3 : 1 << 4;
    } else if (float16_is_zero_or_denormal(f)) {
        return sign ? 1 << 2 : 1 << 5;
    } else if (float16_is_any_nan(f)) {
        float_status s = { }; /* for snan_bit_is_one */
        return float16_is_quiet_nan(f, &s) ? 1 << 9 : 1 << 8;
    } else {
        return sign ? 1 << 1 : 1 << 6;
    }
}

target_ulong fclass_s(uint64_t frs1)
{
    float32 f = frs1;
    bool sign = float32_is_neg(f);

    if (float32_is_infinity(f)) {
        return sign ? 1 << 0 : 1 << 7;
    } else if (float32_is_zero(f)) {
        return sign ? 1 << 3 : 1 << 4;
    } else if (float32_is_zero_or_denormal(f)) {
        return sign ? 1 << 2 : 1 << 5;
    } else if (float32_is_any_nan(f)) {
        float_status s = { }; /* for snan_bit_is_one */
        return float32_is_quiet_nan(f, &s) ? 1 << 9 : 1 << 8;
    } else {
        return sign ? 1 << 1 : 1 << 6;
    }
}

target_ulong fclass_d(uint64_t frs1)
{
    float64 f = frs1;
    bool sign = float64_is_neg(f);

    if (float64_is_infinity(f)) {
        return sign ? 1 << 0 : 1 << 7;
    } else if (float64_is_zero(f)) {
        return sign ? 1 << 3 : 1 << 4;
    } else if (float64_is_zero_or_denormal(f)) {
        return sign ? 1 << 2 : 1 << 5;
    } else if (float64_is_any_nan(f)) {
        float_status s = { }; /* for snan_bit_is_one */
        return float64_is_quiet_nan(f, &s) ? 1 << 9 : 1 << 8;
    } else {
        return sign ? 1 << 1 : 1 << 6;
    }
}

RVVCALL(OPIVV1, vfclass_v_h, OP_UU_H, H2, H2, fclass_h)
RVVCALL(OPIVV1, vfclass_v_w, OP_UU_W, H4, H4, fclass_s)
RVVCALL(OPIVV1, vfclass_v_d, OP_UU_D, H8, H8, fclass_d)
GEN_VEXT_V(vfclass_v_h, 2, 2)
GEN_VEXT_V(vfclass_v_w, 4, 4)
GEN_VEXT_V(vfclass_v_d, 8, 8)

/* Vector Floating-Point Merge Instruction */
#define GEN_VFMERGE_VF(NAME, ETYPE, H)                        \
void HELPER(NAME)(void *vd, void *v0, uint64_t s1, void *vs2, \
                  CPURISCVState *env, uint32_t desc)          \
{                                                             \
    uint32_t vm = vext_vm(desc);                              \
    uint32_t vl = env->vl;                                    \
    uint32_t i;                                               \
                                                              \
    for (i = 0; i < vl; i++) {                                \
        ETYPE s2 = *((ETYPE *)vs2 + H(i));                    \
        *((ETYPE *)vd + H(i))                                 \
          = (!vm && !vext_elem_mask(v0, i) ? s2 : s1);        \
    }                                                         \
}

GEN_VFMERGE_VF(vfmerge_vfm_h, int16_t, H2)
GEN_VFMERGE_VF(vfmerge_vfm_w, int32_t, H4)
GEN_VFMERGE_VF(vfmerge_vfm_d, int64_t, H8)

/* Single-Width Floating-Point/Integer Type-Convert Instructions */
/* vfcvt.xu.f.v vd, vs2, vm # Convert float to unsigned integer. */
RVVCALL(OPFVV1, vfcvt_xu_f_v_h, OP_UU_H, H2, H2, float16_to_uint16)
RVVCALL(OPFVV1, vfcvt_xu_f_v_w, OP_UU_W, H4, H4, float32_to_uint32)
RVVCALL(OPFVV1, vfcvt_xu_f_v_d, OP_UU_D, H8, H8, float64_to_uint64)
GEN_VEXT_V_ENV(vfcvt_xu_f_v_h, 2, 2)
GEN_VEXT_V_ENV(vfcvt_xu_f_v_w, 4, 4)
GEN_VEXT_V_ENV(vfcvt_xu_f_v_d, 8, 8)

/* vfcvt.x.f.v vd, vs2, vm # Convert float to signed integer. */
RVVCALL(OPFVV1, vfcvt_x_f_v_h, OP_UU_H, H2, H2, float16_to_int16)
RVVCALL(OPFVV1, vfcvt_x_f_v_w, OP_UU_W, H4, H4, float32_to_int32)
RVVCALL(OPFVV1, vfcvt_x_f_v_d, OP_UU_D, H8, H8, float64_to_int64)
GEN_VEXT_V_ENV(vfcvt_x_f_v_h, 2, 2)
GEN_VEXT_V_ENV(vfcvt_x_f_v_w, 4, 4)
GEN_VEXT_V_ENV(vfcvt_x_f_v_d, 8, 8)

/* vfcvt.f.xu.v vd, vs2, vm # Convert unsigned integer to float. */
RVVCALL(OPFVV1, vfcvt_f_xu_v_h, OP_UU_H, H2, H2, uint16_to_float16)
RVVCALL(OPFVV1, vfcvt_f_xu_v_w, OP_UU_W, H4, H4, uint32_to_float32)
RVVCALL(OPFVV1, vfcvt_f_xu_v_d, OP_UU_D, H8, H8, uint64_to_float64)
GEN_VEXT_V_ENV(vfcvt_f_xu_v_h, 2, 2)
GEN_VEXT_V_ENV(vfcvt_f_xu_v_w, 4, 4)
GEN_VEXT_V_ENV(vfcvt_f_xu_v_d, 8, 8)

/* vfcvt.f.x.v vd, vs2, vm # Convert integer to float. */
RVVCALL(OPFVV1, vfcvt_f_x_v_h, OP_UU_H, H2, H2, int16_to_float16)
RVVCALL(OPFVV1, vfcvt_f_x_v_w, OP_UU_W, H4, H4, int32_to_float32)
RVVCALL(OPFVV1, vfcvt_f_x_v_d, OP_UU_D, H8, H8, int64_to_float64)
GEN_VEXT_V_ENV(vfcvt_f_x_v_h, 2, 2)
GEN_VEXT_V_ENV(vfcvt_f_x_v_w, 4, 4)
GEN_VEXT_V_ENV(vfcvt_f_x_v_d, 8, 8)

/* Widening Floating-Point/Integer Type-Convert Instructions */
/* (TD, T2, TX2) */
#define WOP_UU_H uint32_t, uint16_t, uint16_t
#define WOP_UU_W uint64_t, uint32_t, uint32_t
/* vfwcvt.xu.f.v vd, vs2, vm # Convert float to double-width unsigned integer.*/
RVVCALL(OPFVV1, vfwcvt_xu_f_v_h, WOP_UU_H, H4, H2, float16_to_uint32)
RVVCALL(OPFVV1, vfwcvt_xu_f_v_w, WOP_UU_W, H8, H4, float32_to_uint64)
GEN_VEXT_V_ENV(vfwcvt_xu_f_v_h, 2, 4)
GEN_VEXT_V_ENV(vfwcvt_xu_f_v_w, 4, 8)

/* vfwcvt.x.f.v vd, vs2, vm # Convert float to double-width signed integer. */
RVVCALL(OPFVV1, vfwcvt_x_f_v_h, WOP_UU_H, H4, H2, float16_to_int32)
RVVCALL(OPFVV1, vfwcvt_x_f_v_w, WOP_UU_W, H8, H4, float32_to_int64)
GEN_VEXT_V_ENV(vfwcvt_x_f_v_h, 2, 4)
GEN_VEXT_V_ENV(vfwcvt_x_f_v_w, 4, 8)

/* vfwcvt.f.xu.v vd, vs2, vm # Convert unsigned integer to double-width float */
RVVCALL(OPFVV1, vfwcvt_f_xu_v_h, WOP_UU_H, H4, H2, uint16_to_float32)
RVVCALL(OPFVV1, vfwcvt_f_xu_v_w, WOP_UU_W, H8, H4, uint32_to_float64)
GEN_VEXT_V_ENV(vfwcvt_f_xu_v_h, 2, 4)
GEN_VEXT_V_ENV(vfwcvt_f_xu_v_w, 4, 8)

/* vfwcvt.f.x.v vd, vs2, vm # Convert integer to double-width float. */
RVVCALL(OPFVV1, vfwcvt_f_x_v_h, WOP_UU_H, H4, H2, int16_to_float32)
RVVCALL(OPFVV1, vfwcvt_f_x_v_w, WOP_UU_W, H8, H4, int32_to_float64)
GEN_VEXT_V_ENV(vfwcvt_f_x_v_h, 2, 4)
GEN_VEXT_V_ENV(vfwcvt_f_x_v_w, 4, 8)

/*
 * vfwcvt.f.f.v vd, vs2, vm #
 * Convert single-width float to double-width float.
 */
static uint32_t vfwcvtffv16(uint16_t a, float_status *s)
{
    return float16_to_float32(a, true, s);
}

RVVCALL(OPFVV1, vfwcvt_f_f_v_h, WOP_UU_H, H4, H2, vfwcvtffv16)
RVVCALL(OPFVV1, vfwcvt_f_f_v_w, WOP_UU_W, H8, H4, float32_to_float64)
GEN_VEXT_V_ENV(vfwcvt_f_f_v_h, 2, 4)
GEN_VEXT_V_ENV(vfwcvt_f_f_v_w, 4, 8)

/* Narrowing Floating-Point/Integer Type-Convert Instructions */
/* (TD, T2, TX2) */
#define NOP_UU_H uint16_t, uint32_t, uint32_t
#define NOP_UU_W uint32_t, uint64_t, uint64_t
/* vfncvt.xu.f.v vd, vs2, vm # Convert float to unsigned integer. */
RVVCALL(OPFVV1, vfncvt_xu_f_v_h, NOP_UU_H, H2, H4, float32_to_uint16)
RVVCALL(OPFVV1, vfncvt_xu_f_v_w, NOP_UU_W, H4, H8, float64_to_uint32)
GEN_VEXT_V_ENV(vfncvt_xu_f_v_h, 2, 2)
GEN_VEXT_V_ENV(vfncvt_xu_f_v_w, 4, 4)

/* vfncvt.x.f.v vd, vs2, vm # Convert double-width float to signed integer. */
RVVCALL(OPFVV1, vfncvt_x_f_v_h, NOP_UU_H, H2, H4, float32_to_int16)
RVVCALL(OPFVV1, vfncvt_x_f_v_w, NOP_UU_W, H4, H8, float64_to_int32)
GEN_VEXT_V_ENV(vfncvt_x_f_v_h, 2, 2)
GEN_VEXT_V_ENV(vfncvt_x_f_v_w, 4, 4)

/* vfncvt.f.xu.v vd, vs2, vm # Convert double-width unsigned integer to float */
RVVCALL(OPFVV1, vfncvt_f_xu_v_h, NOP_UU_H, H2, H4, uint32_to_float16)
RVVCALL(OPFVV1, vfncvt_f_xu_v_w, NOP_UU_W, H4, H8, uint64_to_float32)
GEN_VEXT_V_ENV(vfncvt_f_xu_v_h, 2, 2)
GEN_VEXT_V_ENV(vfncvt_f_xu_v_w, 4, 4)

/* vfncvt.f.x.v vd, vs2, vm # Convert double-width integer to float. */
RVVCALL(OPFVV1, vfncvt_f_x_v_h, NOP_UU_H, H2, H4, int32_to_float16)
RVVCALL(OPFVV1, vfncvt_f_x_v_w, NOP_UU_W, H4, H8, int64_to_float32)
GEN_VEXT_V_ENV(vfncvt_f_x_v_h, 2, 2)
GEN_VEXT_V_ENV(vfncvt_f_x_v_w, 4, 4)

/* vfncvt.f.f.v vd, vs2, vm # Convert double float to single-width float. */
static uint16_t vfncvtffv16(uint32_t a, float_status *s)
{
    return float32_to_float16(a, true, s);
}

RVVCALL(OPFVV1, vfncvt_f_f_v_h, NOP_UU_H, H2, H4, vfncvtffv16)
RVVCALL(OPFVV1, vfncvt_f_f_v_w, NOP_UU_W, H4, H8, float64_to_float32)
GEN_VEXT_V_ENV(vfncvt_f_f_v_h, 2, 2)
GEN_VEXT_V_ENV(vfncvt_f_f_v_w, 4, 4)

/*
 *** Vector Reduction Operations
 */
/* Vector Single-Width Integer Reduction Instructions */
#define GEN_VEXT_RED(NAME, TD, TS2, HD, HS2, OP)          \
void HELPER(NAME)(void *vd, void *v0, void *vs1,          \
        void *vs2, CPURISCVState *env, uint32_t desc)     \
{                                                         \
    uint32_t vm = vext_vm(desc);                          \
    uint32_t vl = env->vl;                                \
    uint32_t i;                                           \
    TD s1 =  *((TD *)vs1 + HD(0));                        \
                                                          \
    for (i = 0; i < vl; i++) {                            \
        TS2 s2 = *((TS2 *)vs2 + HS2(i));                  \
        if (!vm && !vext_elem_mask(v0, i)) {              \
            continue;                                     \
        }                                                 \
        s1 = OP(s1, (TD)s2);                              \
    }                                                     \
    *((TD *)vd + HD(0)) = s1;                             \
}

/* vd[0] = sum(vs1[0], vs2[*]) */
GEN_VEXT_RED(vredsum_vs_b, int8_t,  int8_t,  H1, H1, DO_ADD)
GEN_VEXT_RED(vredsum_vs_h, int16_t, int16_t, H2, H2, DO_ADD)
GEN_VEXT_RED(vredsum_vs_w, int32_t, int32_t, H4, H4, DO_ADD)
GEN_VEXT_RED(vredsum_vs_d, int64_t, int64_t, H8, H8, DO_ADD)

/* vd[0] = maxu(vs1[0], vs2[*]) */
GEN_VEXT_RED(vredmaxu_vs_b, uint8_t,  uint8_t,  H1, H1, DO_MAX)
GEN_VEXT_RED(vredmaxu_vs_h, uint16_t, uint16_t, H2, H2, DO_MAX)
GEN_VEXT_RED(vredmaxu_vs_w, uint32_t, uint32_t, H4, H4, DO_MAX)
GEN_VEXT_RED(vredmaxu_vs_d, uint64_t, uint64_t, H8, H8, DO_MAX)

/* vd[0] = max(vs1[0], vs2[*]) */
GEN_VEXT_RED(vredmax_vs_b, int8_t,  int8_t,  H1, H1, DO_MAX)
GEN_VEXT_RED(vredmax_vs_h, int16_t, int16_t, H2, H2, DO_MAX)
GEN_VEXT_RED(vredmax_vs_w, int32_t, int32_t, H4, H4, DO_MAX)
GEN_VEXT_RED(vredmax_vs_d, int64_t, int64_t, H8, H8, DO_MAX)

/* vd[0] = minu(vs1[0], vs2[*]) */
GEN_VEXT_RED(vredminu_vs_b, uint8_t,  uint8_t,  H1, H1, DO_MIN)
GEN_VEXT_RED(vredminu_vs_h, uint16_t, uint16_t, H2, H2, DO_MIN)
GEN_VEXT_RED(vredminu_vs_w, uint32_t, uint32_t, H4, H4, DO_MIN)
GEN_VEXT_RED(vredminu_vs_d, uint64_t, uint64_t, H8, H8, DO_MIN)

/* vd[0] = min(vs1[0], vs2[*]) */
GEN_VEXT_RED(vredmin_vs_b, int8_t,  int8_t,  H1, H1, DO_MIN)
GEN_VEXT_RED(vredmin_vs_h, int16_t, int16_t, H2, H2, DO_MIN)
GEN_VEXT_RED(vredmin_vs_w, int32_t, int32_t, H4, H4, DO_MIN)
GEN_VEXT_RED(vredmin_vs_d, int64_t, int64_t, H8, H8, DO_MIN)

/* vd[0] = and(vs1[0], vs2[*]) */
GEN_VEXT_RED(vredand_vs_b, int8_t,  int8_t,  H1, H1, DO_AND)
GEN_VEXT_RED(vredand_vs_h, int16_t, int16_t, H2, H2, DO_AND)
GEN_VEXT_RED(vredand_vs_w, int32_t, int32_t, H4, H4, DO_AND)
GEN_VEXT_RED(vredand_vs_d, int64_t, int64_t, H8, H8, DO_AND)

/* vd[0] = or(vs1[0], vs2[*]) */
GEN_VEXT_RED(vredor_vs_b, int8_t,  int8_t,  H1, H1, DO_OR)
GEN_VEXT_RED(vredor_vs_h, int16_t, int16_t, H2, H2, DO_OR)
GEN_VEXT_RED(vredor_vs_w, int32_t, int32_t, H4, H4, DO_OR)
GEN_VEXT_RED(vredor_vs_d, int64_t, int64_t, H8, H8, DO_OR)

/* vd[0] = xor(vs1[0], vs2[*]) */
GEN_VEXT_RED(vredxor_vs_b, int8_t,  int8_t,  H1, H1, DO_XOR)
GEN_VEXT_RED(vredxor_vs_h, int16_t, int16_t, H2, H2, DO_XOR)
GEN_VEXT_RED(vredxor_vs_w, int32_t, int32_t, H4, H4, DO_XOR)
GEN_VEXT_RED(vredxor_vs_d, int64_t, int64_t, H8, H8, DO_XOR)

/* Vector Widening Integer Reduction Instructions */
/* signed sum reduction into double-width accumulator */
GEN_VEXT_RED(vwredsum_vs_b, int16_t, int8_t,  H2, H1, DO_ADD)
GEN_VEXT_RED(vwredsum_vs_h, int32_t, int16_t, H4, H2, DO_ADD)
GEN_VEXT_RED(vwredsum_vs_w, int64_t, int32_t, H8, H4, DO_ADD)

/* Unsigned sum reduction into double-width accumulator */
GEN_VEXT_RED(vwredsumu_vs_b, uint16_t, uint8_t,  H2, H1, DO_ADD)
GEN_VEXT_RED(vwredsumu_vs_h, uint32_t, uint16_t, H4, H2, DO_ADD)
GEN_VEXT_RED(vwredsumu_vs_w, uint64_t, uint32_t, H8, H4, DO_ADD)

/* Vector Single-Width Floating-Point Reduction Instructions */
#define GEN_VEXT_FRED(NAME, TD, TS2, HD, HS2, OP)          \
void HELPER(NAME)(void *vd, void *v0, void *vs1,           \
                  void *vs2, CPURISCVState *env,           \
                  uint32_t desc)                           \
{                                                          \
    uint32_t vm = vext_vm(desc);                           \
    uint32_t vl = env->vl;                                 \
    uint32_t i;                                            \
    TD s1 =  *((TD *)vs1 + HD(0));                         \
                                                           \
    for (i = 0; i < vl; i++) {                             \
        TS2 s2 = *((TS2 *)vs2 + HS2(i));                   \
        if (!vm && !vext_elem_mask(v0, i)) {               \
            continue;                                      \
        }                                                  \
        s1 = OP(s1, (TD)s2, &env->fp_status);              \
    }                                                      \
    *((TD *)vd + HD(0)) = s1;                              \
}

/* Unordered sum */
GEN_VEXT_FRED(vfredsum_vs_h, uint16_t, uint16_t, H2, H2, float16_add)
GEN_VEXT_FRED(vfredsum_vs_w, uint32_t, uint32_t, H4, H4, float32_add)
GEN_VEXT_FRED(vfredsum_vs_d, uint64_t, uint64_t, H8, H8, float64_add)

/* Maximum value */
GEN_VEXT_FRED(vfredmax_vs_h, uint16_t, uint16_t, H2, H2, float16_maxnum)
GEN_VEXT_FRED(vfredmax_vs_w, uint32_t, uint32_t, H4, H4, float32_maxnum)
GEN_VEXT_FRED(vfredmax_vs_d, uint64_t, uint64_t, H8, H8, float64_maxnum)

/* Minimum value */
GEN_VEXT_FRED(vfredmin_vs_h, uint16_t, uint16_t, H2, H2, float16_minnum)
GEN_VEXT_FRED(vfredmin_vs_w, uint32_t, uint32_t, H4, H4, float32_minnum)
GEN_VEXT_FRED(vfredmin_vs_d, uint64_t, uint64_t, H8, H8, float64_minnum)

/* Vector Widening Floating-Point Reduction Instructions */
/* Unordered reduce 2*SEW = 2*SEW + sum(promote(SEW)) */
void HELPER(vfwredsum_vs_h)(void *vd, void *v0, void *vs1,
                            void *vs2, CPURISCVState *env, uint32_t desc)
{
    uint32_t vm = vext_vm(desc);
    uint32_t vl = env->vl;
    uint32_t i;
    uint32_t s1 =  *((uint32_t *)vs1 + H4(0));

    for (i = 0; i < vl; i++) {
        uint16_t s2 = *((uint16_t *)vs2 + H2(i));
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        s1 = float32_add(s1, float16_to_float32(s2, true, &env->fp_status),
                         &env->fp_status);
    }
    *((uint32_t *)vd + H4(0)) = s1;
}

void HELPER(vfwredsum_vs_w)(void *vd, void *v0, void *vs1,
                            void *vs2, CPURISCVState *env, uint32_t desc)
{
    uint32_t vm = vext_vm(desc);
    uint32_t vl = env->vl;
    uint32_t i;
    uint64_t s1 =  *((uint64_t *)vs1);

    for (i = 0; i < vl; i++) {
        uint32_t s2 = *((uint32_t *)vs2 + H4(i));
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        s1 = float64_add(s1, float32_to_float64(s2, &env->fp_status),
                         &env->fp_status);
    }
    *((uint64_t *)vd) = s1;
}

/*
 *** Vector Mask Operations
 */
/* Vector Mask-Register Logical Instructions */
#define GEN_VEXT_MASK_VV(NAME, OP)                        \
void HELPER(NAME)(void *vd, void *v0, void *vs1,          \
                  void *vs2, CPURISCVState *env,          \
                  uint32_t desc)                          \
{                                                         \
    uint32_t vlmax = env_archcpu(env)->cfg.vlen;          \
    uint32_t vl = env->vl;                                \
    uint32_t i;                                           \
    int a, b;                                             \
                                                          \
    for (i = 0; i < vl; i++) {                            \
        a = vext_elem_mask(vs1, i);                       \
        b = vext_elem_mask(vs2, i);                       \
        vext_set_elem_mask(vd, i, OP(b, a));              \
    }                                                     \
    for (; i < vlmax; i++) {                              \
        vext_set_elem_mask(vd, i, 0);                     \
    }                                                     \
}

#define DO_NAND(N, M)  (!(N & M))
#define DO_ANDNOT(N, M)  (N & !M)
#define DO_NOR(N, M)  (!(N | M))
#define DO_ORNOT(N, M)  (N | !M)
#define DO_XNOR(N, M)  (!(N ^ M))

GEN_VEXT_MASK_VV(vmand_mm, DO_AND)
GEN_VEXT_MASK_VV(vmnand_mm, DO_NAND)
GEN_VEXT_MASK_VV(vmandnot_mm, DO_ANDNOT)
GEN_VEXT_MASK_VV(vmxor_mm, DO_XOR)
GEN_VEXT_MASK_VV(vmor_mm, DO_OR)
GEN_VEXT_MASK_VV(vmnor_mm, DO_NOR)
GEN_VEXT_MASK_VV(vmornot_mm, DO_ORNOT)
GEN_VEXT_MASK_VV(vmxnor_mm, DO_XNOR)

/* Vector mask population count vmpopc */
target_ulong HELPER(vmpopc_m)(void *v0, void *vs2, CPURISCVState *env,
                              uint32_t desc)
{
    target_ulong cnt = 0;
    uint32_t vm = vext_vm(desc);
    uint32_t vl = env->vl;
    int i;

    for (i = 0; i < vl; i++) {
        if (vm || vext_elem_mask(v0, i)) {
            if (vext_elem_mask(vs2, i)) {
                cnt++;
            }
        }
    }
    return cnt;
}

/* vmfirst find-first-set mask bit*/
target_ulong HELPER(vmfirst_m)(void *v0, void *vs2, CPURISCVState *env,
                               uint32_t desc)
{
    uint32_t vm = vext_vm(desc);
    uint32_t vl = env->vl;
    int i;

    for (i = 0; i < vl; i++) {
        if (vm || vext_elem_mask(v0, i)) {
            if (vext_elem_mask(vs2, i)) {
                return i;
            }
        }
    }
    return -1LL;
}

enum set_mask_type {
    ONLY_FIRST = 1,
    INCLUDE_FIRST,
    BEFORE_FIRST,
};

static void vmsetm(void *vd, void *v0, void *vs2, CPURISCVState *env,
                   uint32_t desc, enum set_mask_type type)
{
    uint32_t vlmax = env_archcpu(env)->cfg.vlen;
    uint32_t vm = vext_vm(desc);
    uint32_t vl = env->vl;
    int i;
    bool first_mask_bit = false;

    for (i = 0; i < vl; i++) {
        if (!vm && !vext_elem_mask(v0, i)) {
            continue;
        }
        /* write a zero to all following active elements */
        if (first_mask_bit) {
            vext_set_elem_mask(vd, i, 0);
            continue;
        }
        if (vext_elem_mask(vs2, i)) {
            first_mask_bit = true;
            if (type == BEFORE_FIRST) {
                vext_set_elem_mask(vd, i, 0);
            } else {
                vext_set_elem_mask(vd, i, 1);
            }
        } else {
            if (type == ONLY_FIRST) {
                vext_set_elem_mask(vd, i, 0);
            } else {
                vext_set_elem_mask(vd, i, 1);
            }
        }
    }
    for (; i < vlmax; i++) {
        vext_set_elem_mask(vd, i, 0);
    }
}

void HELPER(vmsbf_m)(void *vd, void *v0, void *vs2, CPURISCVState *env,
                     uint32_t desc)
{
    vmsetm(vd, v0, vs2, env, desc, BEFORE_FIRST);
}

void HELPER(vmsif_m)(void *vd, void *v0, void *vs2, CPURISCVState *env,
                     uint32_t desc)
{
    vmsetm(vd, v0, vs2, env, desc, INCLUDE_FIRST);
}

void HELPER(vmsof_m)(void *vd, void *v0, void *vs2, CPURISCVState *env,
                     uint32_t desc)
{
    vmsetm(vd, v0, vs2, env, desc, ONLY_FIRST);
}

/* Vector Iota Instruction */
#define GEN_VEXT_VIOTA_M(NAME, ETYPE, H)                                  \
void HELPER(NAME)(void *vd, void *v0, void *vs2, CPURISCVState *env,      \
                  uint32_t desc)                                          \
{                                                                         \
    uint32_t vm = vext_vm(desc);                                          \
    uint32_t vl = env->vl;                                                \
    uint32_t sum = 0;                                                     \
    int i;                                                                \
                                                                          \
    for (i = 0; i < vl; i++) {                                            \
        if (!vm && !vext_elem_mask(v0, i)) {                              \
            continue;                                                     \
        }                                                                 \
        *((ETYPE *)vd + H(i)) = sum;                                      \
        if (vext_elem_mask(vs2, i)) {                                     \
            sum++;                                                        \
        }                                                                 \
    }                                                                     \
}

GEN_VEXT_VIOTA_M(viota_m_b, uint8_t,  H1)
GEN_VEXT_VIOTA_M(viota_m_h, uint16_t, H2)
GEN_VEXT_VIOTA_M(viota_m_w, uint32_t, H4)
GEN_VEXT_VIOTA_M(viota_m_d, uint64_t, H8)

/* Vector Element Index Instruction */
#define GEN_VEXT_VID_V(NAME, ETYPE, H)                                    \
void HELPER(NAME)(void *vd, void *v0, CPURISCVState *env, uint32_t desc)  \
{                                                                         \
    uint32_t vm = vext_vm(desc);                                          \
    uint32_t vl = env->vl;                                                \
    int i;                                                                \
                                                                          \
    for (i = 0; i < vl; i++) {                                            \
        if (!vm && !vext_elem_mask(v0, i)) {                              \
            continue;                                                     \
        }                                                                 \
        *((ETYPE *)vd + H(i)) = i;                                        \
    }                                                                     \
}

GEN_VEXT_VID_V(vid_v_b, uint8_t,  H1)
GEN_VEXT_VID_V(vid_v_h, uint16_t, H2)
GEN_VEXT_VID_V(vid_v_w, uint32_t, H4)
GEN_VEXT_VID_V(vid_v_d, uint64_t, H8)

/*
 *** Vector Permutation Instructions
 */

/* Vector Slide Instructions */
#define GEN_VEXT_VSLIDEUP_VX(NAME, ETYPE, H)                              \
void HELPER(NAME)(void *vd, void *v0, target_ulong s1, void *vs2,         \
                  CPURISCVState *env, uint32_t desc)                      \
{                                                                         \
    uint32_t vm = vext_vm(desc);                                          \
    uint32_t vl = env->vl;                                                \
    target_ulong offset = s1, i;                                          \
                                                                          \
    for (i = offset; i < vl; i++) {                                       \
        if (!vm && !vext_elem_mask(v0, i)) {                              \
            continue;                                                     \
        }                                                                 \
        *((ETYPE *)vd + H(i)) = *((ETYPE *)vs2 + H(i - offset));          \
    }                                                                     \
}

/* vslideup.vx vd, vs2, rs1, vm # vd[i+rs1] = vs2[i] */
GEN_VEXT_VSLIDEUP_VX(vslideup_vx_b, uint8_t,  H1)
GEN_VEXT_VSLIDEUP_VX(vslideup_vx_h, uint16_t, H2)
GEN_VEXT_VSLIDEUP_VX(vslideup_vx_w, uint32_t, H4)
GEN_VEXT_VSLIDEUP_VX(vslideup_vx_d, uint64_t, H8)

#define GEN_VEXT_VSLIDEDOWN_VX(NAME, ETYPE, H)                            \
void HELPER(NAME)(void *vd, void *v0, target_ulong s1, void *vs2,         \
                  CPURISCVState *env, uint32_t desc)                      \
{                                                                         \
    uint32_t vlmax = env_archcpu(env)->cfg.vlen;                          \
    uint32_t vm = vext_vm(desc);                                          \
    uint32_t vl = env->vl;                                                \
    target_ulong offset = s1, i;                                          \
                                                                          \
    for (i = 0; i < vl; ++i) {                                            \
        target_ulong j = i + offset;                                      \
        if (!vm && !vext_elem_mask(v0, i)) {                              \
            continue;                                                     \
        }                                                                 \
        *((ETYPE *)vd + H(i)) = j >= vlmax ? 0 : *((ETYPE *)vs2 + H(j));  \
    }                                                                     \
}

/* vslidedown.vx vd, vs2, rs1, vm # vd[i] = vs2[i+rs1] */
GEN_VEXT_VSLIDEDOWN_VX(vslidedown_vx_b, uint8_t,  H1)
GEN_VEXT_VSLIDEDOWN_VX(vslidedown_vx_h, uint16_t, H2)
GEN_VEXT_VSLIDEDOWN_VX(vslidedown_vx_w, uint32_t, H4)
GEN_VEXT_VSLIDEDOWN_VX(vslidedown_vx_d, uint64_t, H8)

#define GEN_VEXT_VSLIDE1UP_VX(NAME, ETYPE, H)                             \
void HELPER(NAME)(void *vd, void *v0, target_ulong s1, void *vs2,         \
                  CPURISCVState *env, uint32_t desc)                      \
{                                                                         \
    uint32_t vm = vext_vm(desc);                                          \
    uint32_t vl = env->vl;                                                \
    uint32_t i;                                                           \
                                                                          \
    for (i = 0; i < vl; i++) {                                            \
        if (!vm && !vext_elem_mask(v0, i)) {                              \
            continue;                                                     \
        }                                                                 \
        if (i == 0) {                                                     \
            *((ETYPE *)vd + H(i)) = s1;                                   \
        } else {                                                          \
            *((ETYPE *)vd + H(i)) = *((ETYPE *)vs2 + H(i - 1));           \
        }                                                                 \
    }                                                                     \
}

/* vslide1up.vx vd, vs2, rs1, vm # vd[0]=x[rs1], vd[i+1] = vs2[i] */
GEN_VEXT_VSLIDE1UP_VX(vslide1up_vx_b, uint8_t,  H1)
GEN_VEXT_VSLIDE1UP_VX(vslide1up_vx_h, uint16_t, H2)
GEN_VEXT_VSLIDE1UP_VX(vslide1up_vx_w, uint32_t, H4)
GEN_VEXT_VSLIDE1UP_VX(vslide1up_vx_d, uint64_t, H8)

#define GEN_VEXT_VSLIDE1DOWN_VX(NAME, ETYPE, H)                           \
void HELPER(NAME)(void *vd, void *v0, target_ulong s1, void *vs2,         \
                  CPURISCVState *env, uint32_t desc)                      \
{                                                                         \
    uint32_t vm = vext_vm(desc);                                          \
    uint32_t vl = env->vl;                                                \
    uint32_t i;                                                           \
                                                                          \
    for (i = 0; i < vl; i++) {                                            \
        if (!vm && !vext_elem_mask(v0, i)) {                              \
            continue;                                                     \
        }                                                                 \
        if (i == vl - 1) {                                                \
            *((ETYPE *)vd + H(i)) = s1;                                   \
        } else {                                                          \
            *((ETYPE *)vd + H(i)) = *((ETYPE *)vs2 + H(i + 1));           \
        }                                                                 \
    }                                                                     \
}

/* vslide1down.vx vd, vs2, rs1, vm # vd[i] = vs2[i+1], vd[vl-1]=x[rs1] */
GEN_VEXT_VSLIDE1DOWN_VX(vslide1down_vx_b, uint8_t,  H1)
GEN_VEXT_VSLIDE1DOWN_VX(vslide1down_vx_h, uint16_t, H2)
GEN_VEXT_VSLIDE1DOWN_VX(vslide1down_vx_w, uint32_t, H4)
GEN_VEXT_VSLIDE1DOWN_VX(vslide1down_vx_d, uint64_t, H8)

/* Vector Register Gather Instruction */
#define GEN_VEXT_VRGATHER_VV(NAME, ETYPE, H)                              \
void HELPER(NAME)(void *vd, void *v0, void *vs1, void *vs2,               \
                  CPURISCVState *env, uint32_t desc)                      \
{                                                                         \
    uint32_t vlmax = env_archcpu(env)->cfg.vlen;                          \
    uint32_t vm = vext_vm(desc);                                          \
    uint32_t vl = env->vl;                                                \
    uint32_t index, i;                                                    \
                                                                          \
    for (i = 0; i < vl; i++) {                                            \
        if (!vm && !vext_elem_mask(v0, i)) {                              \
            continue;                                                     \
        }                                                                 \
        index = *((ETYPE *)vs1 + H(i));                                   \
        if (index >= vlmax) {                                             \
            *((ETYPE *)vd + H(i)) = 0;                                    \
        } else {                                                          \
            *((ETYPE *)vd + H(i)) = *((ETYPE *)vs2 + H(index));           \
        }                                                                 \
    }                                                                     \
}

/* vd[i] = (vs1[i] >= VLMAX) ? 0 : vs2[vs1[i]]; */
GEN_VEXT_VRGATHER_VV(vrgather_vv_b, uint8_t,  H1)
GEN_VEXT_VRGATHER_VV(vrgather_vv_h, uint16_t, H2)
GEN_VEXT_VRGATHER_VV(vrgather_vv_w, uint32_t, H4)
GEN_VEXT_VRGATHER_VV(vrgather_vv_d, uint64_t, H8)

#define GEN_VEXT_VRGATHER_VX(NAME, ETYPE, H)                              \
void HELPER(NAME)(void *vd, void *v0, target_ulong s1, void *vs2,         \
                  CPURISCVState *env, uint32_t desc)                      \
{                                                                         \
    uint32_t vlmax = env_archcpu(env)->cfg.vlen;                          \
    uint32_t vm = vext_vm(desc);                                          \
    uint32_t vl = env->vl;                                                \
    uint32_t index = s1, i;                                               \
                                                                          \
    for (i = 0; i < vl; i++) {                                            \
        if (!vm && !vext_elem_mask(v0, i)) {                              \
            continue;                                                     \
        }                                                                 \
        if (index >= vlmax) {                                             \
            *((ETYPE *)vd + H(i)) = 0;                                    \
        } else {                                                          \
            *((ETYPE *)vd + H(i)) = *((ETYPE *)vs2 + H(index));           \
        }                                                                 \
    }                                                                     \
}

/* vd[i] = (x[rs1] >= VLMAX) ? 0 : vs2[rs1] */
GEN_VEXT_VRGATHER_VX(vrgather_vx_b, uint8_t,  H1)
GEN_VEXT_VRGATHER_VX(vrgather_vx_h, uint16_t, H2)
GEN_VEXT_VRGATHER_VX(vrgather_vx_w, uint32_t, H4)
GEN_VEXT_VRGATHER_VX(vrgather_vx_d, uint64_t, H8)

/* Vector Compress Instruction */
#define GEN_VEXT_VCOMPRESS_VM(NAME, ETYPE, H)                             \
void HELPER(NAME)(void *vd, void *v0, void *vs1, void *vs2,               \
                  CPURISCVState *env, uint32_t desc)                      \
{                                                                         \
    uint32_t vl = env->vl;                                                \
    uint32_t num = 0, i;                                                  \
                                                                          \
    for (i = 0; i < vl; i++) {                                            \
        if (!vext_elem_mask(vs1, i)) {                                    \
            continue;                                                     \
        }                                                                 \
        *((ETYPE *)vd + H(num)) = *((ETYPE *)vs2 + H(i));                 \
        num++;                                                            \
    }                                                                     \
}

/* Compress into vd elements of vs2 where vs1 is enabled */
GEN_VEXT_VCOMPRESS_VM(vcompress_vm_b, uint8_t,  H1)
GEN_VEXT_VCOMPRESS_VM(vcompress_vm_h, uint16_t, H2)
GEN_VEXT_VCOMPRESS_VM(vcompress_vm_w, uint32_t, H4)
GEN_VEXT_VCOMPRESS_VM(vcompress_vm_d, uint64_t, H8)
