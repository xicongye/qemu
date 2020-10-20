/*
 * RISC-V translation routines for the RVB Standard Extension.
 *
 * Copyright (c) 2020 Kito Cheng, kito.cheng@sifive.com
 * Copyright (c) 2020 Frank Chang, frank.chang@sifive.com
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

static bool trans_clz(DisasContext *ctx, arg_clz *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_unary(ctx, a, &gen_clz);
}

static bool trans_ctz(DisasContext *ctx, arg_ctz *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_unary(ctx, a, &gen_ctz);
}

static bool trans_pcnt(DisasContext *ctx, arg_pcnt *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_unary(ctx, a, &tcg_gen_ctpop_tl);
}

static bool trans_andn(DisasContext *ctx, arg_andn *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_arith(ctx, a, &gen_andn);
}

static bool trans_orn(DisasContext *ctx, arg_orn *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_arith(ctx, a, &gen_orn);
}

static bool trans_xnor(DisasContext *ctx, arg_xnor *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_arith(ctx, a, &gen_xnor);
}

static bool trans_pack(DisasContext *ctx, arg_pack *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_arith(ctx, a, &gen_pack);
}

static bool trans_packu(DisasContext *ctx, arg_packu *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_arith(ctx, a, &gen_packu);
}

static bool trans_packh(DisasContext *ctx, arg_packh *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_arith(ctx, a, &gen_packh);
}

static bool trans_min(DisasContext *ctx, arg_min *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_arith(ctx, a, &tcg_gen_smin_tl);
}

static bool trans_max(DisasContext *ctx, arg_max *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_arith(ctx, a, &tcg_gen_smax_tl);
}

static bool trans_minu(DisasContext *ctx, arg_minu *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_arith(ctx, a, &tcg_gen_umin_tl);
}

static bool trans_maxu(DisasContext *ctx, arg_maxu *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_arith(ctx, a, &tcg_gen_umax_tl);
}

static bool trans_sext_b(DisasContext *ctx, arg_sext_b *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_unary(ctx, a, &tcg_gen_ext8s_tl);
}

static bool trans_sext_h(DisasContext *ctx, arg_sext_h *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_unary(ctx, a, &tcg_gen_ext16s_tl);
}

static bool trans_sbset(DisasContext *ctx, arg_sbset *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shift(ctx, a, &gen_sbset);
}

static bool trans_sbseti(DisasContext *ctx, arg_sbseti *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shifti(ctx, a, &gen_sbset);
}

static bool trans_sbclr(DisasContext *ctx, arg_sbclr *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shift(ctx, a, &gen_sbclr);
}

static bool trans_sbclri(DisasContext *ctx, arg_sbclri *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shifti(ctx, a, &gen_sbclr);
}

static bool trans_sbinv(DisasContext *ctx, arg_sbinv *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shift(ctx, a, &gen_sbinv);
}

static bool trans_sbinvi(DisasContext *ctx, arg_sbinvi *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shifti(ctx, a, &gen_sbinv);
}

static bool trans_sbext(DisasContext *ctx, arg_sbext *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shift(ctx, a, &gen_sbext);
}

static bool trans_sbexti(DisasContext *ctx, arg_sbexti *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shifti(ctx, a, &gen_sbext);
}

static bool trans_slo(DisasContext *ctx, arg_slo *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shift(ctx, a, &gen_slo);
}

static bool trans_sloi(DisasContext *ctx, arg_sloi *a)
{
    REQUIRE_EXT(ctx, RVB);

    if (a->shamt >= TARGET_LONG_BITS) {
        return false;
    }

    return gen_shifti(ctx, a, &gen_slo);
}

static bool trans_sro(DisasContext *ctx, arg_sro *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shift(ctx, a, &gen_sro);
}

static bool trans_sroi(DisasContext *ctx, arg_sroi *a)
{
    REQUIRE_EXT(ctx, RVB);

    if (a->shamt >= TARGET_LONG_BITS) {
        return false;
    }

    return gen_shifti(ctx, a, &gen_sro);
}

{
/* RV64-only instructions */
#ifdef TARGET_RISCV64

static bool trans_clzw(DisasContext *ctx, arg_clzw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_unary(ctx, a, &gen_clzw);
}

static bool trans_ctzw(DisasContext *ctx, arg_ctzw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_unary(ctx, a, &gen_ctzw);
}

static bool trans_pcntw(DisasContext *ctx, arg_pcntw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_unary(ctx, a, &gen_pcntw);
}

static bool trans_packw(DisasContext *ctx, arg_packw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_arith(ctx, a, &gen_packw);
}

static bool trans_packuw(DisasContext *ctx, arg_packuw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_arith(ctx, a, &gen_packuw);
}

static bool trans_sbsetw(DisasContext *ctx, arg_sbsetw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shiftw(ctx, a, &gen_sbset);
}

static bool trans_sbsetiw(DisasContext *ctx, arg_sbsetiw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shiftiw(ctx, a, &gen_sbset);
}

static bool trans_sbclrw(DisasContext *ctx, arg_sbclrw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shiftw(ctx, a, &gen_sbclr);
}

static bool trans_sbclriw(DisasContext *ctx, arg_sbclriw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shiftiw(ctx, a, &gen_sbclr);
}

static bool trans_sbinvw(DisasContext *ctx, arg_sbinvw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shiftw(ctx, a, &gen_sbinv);
}

static bool trans_sbinviw(DisasContext *ctx, arg_sbinviw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shiftiw(ctx, a, &gen_sbinv);
}

static bool trans_sbextw(DisasContext *ctx, arg_sbextw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shiftw(ctx, a, &gen_sbext);
}

static bool trans_slow(DisasContext *ctx, arg_slow *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shiftw(ctx, a, &gen_slo);
}

static bool trans_sloiw(DisasContext *ctx, arg_sloiw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shiftiw(ctx, a, &gen_slo);
}

static bool trans_srow(DisasContext *ctx, arg_srow *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shiftw(ctx, a, &gen_sro);
}

static bool trans_sroiw(DisasContext *ctx, arg_sroiw *a)
{
    REQUIRE_EXT(ctx, RVB);
    return gen_shiftiw(ctx, a, &gen_sro);
}

#endif