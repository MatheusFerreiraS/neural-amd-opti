// The key-block loop of vit_attn.comp's NR_VTRANS path, included twice.
// NR_VT_GUARD 1: a chunk that may run past `pc.tokens` (tiles beyond it read
// zero K and contribute no probability to P.V). NR_VT_GUARD 0: a full chunk,
// where both tests are constant and the zero-or-load phi they created - which
// ACO copied with four byte-inserting `v_perm_b32` a dword - does not exist.
        for (uint kb = 0u; kb < uint(NR_KC); kb += 16u) {
            // K as the A operand: the same addresses its ColumnMajor B load read.
            NR_FRAG_A kfr[2];
            for (uint d = 0u; d < 2u; ++d) {
#if NR_VKMAN && NR_VT_GUARD == 0
                {
                    const uint nrk = kc4 + kb * (X3 >> 2) + d * 64u;
                    const fe4m3vec4 r0 = nr_act4(nrk), r1 = nr_act4(nrk + 1u);
                    for (int v = 0; v < 4; ++v) { kfr[d][v] = r0[v]; kfr[d][v + 4] = r1[v]; }
                }
#elif NR_VADDR && NR_VT_GUARD == 0
                NR_LOAD_A_ACT(kfr[d], kcb + kb * X3 + d * 256u, 16u);
#else
                if (NR_VT_GUARD == 0 || j0 + kb < pc.tokens)
                    NR_LOAD_A_ACT(kfr[d],
                              nr_at16(pc.x_off, j0 + kb, kbase + d * 16u, X3), 16u);
                else kfr[d] = NR_FRAG_A(0.0);
#endif
            }
            // V^T as the A operand, hoisted out of the query blocks. Clamped to
            // the last live key tile for the reason the RowMajor form was: the
            // probabilities that multiply a padding tile are already zero, and a
            // load that is always in bounds is always the same basic block.
            NR_FRAG_A vf[2];
            for (uint n = 0u; n < 2u; ++n)
#if NR_VADDR && NR_VT_GUARD == 0
                NR_LOAD_A_COL_ACT(vf[n], vcb + kb * X3 + n * 256u, 16u);
#else
                NR_LOAD_A_COL_ACT(vf[n],
                              nr_at16(pc.x_off, min(j0 + kb, jlast),
                                      vbase + n * 16u, X3), 16u);
#endif
            for (uint b = 0u; b < uint(NR_QB); ++b) {
                NR_FRAG_ACC lg = NR_ACC_ZERO;
                for (uint d = 0u; d < 2u; ++d) NR_MMA(lg, kfr[d], qfr[b][d]);
#if NR_VPB16
                // The probabilities go straight into an f16 **B** fragment
                // (same component map as the Accumulator) and are converted to
                // e4m3 inside that Use - one pair-convert per dword, where the
                // element-wise e4m3 copy below became four byte-inserting
                // `v_perm_b32` per dword.
                NR_FRAG_B16 ph;
#else
                NR_FRAG_ACC16 ph;
#endif
                f16vec2 pv[4];
                for (int c = 0; c < lg.length(); c += 2) {
#if NR_ACC_F16 > 0
                    lg[c]   = nr_round_f16(float(lg[c]));
                    lg[c+1] = nr_round_f16(float(lg[c+1]));
#endif
                    vec2 a = vec2(float(lg[c]), float(lg[c+1]));
#if !NR_VPRENORMALIZED
                    // The query scale is this lane's and the key reciprocal is
                    // this component's: the two have traded places with the axes.
                    a *= vec2(qs[b]);
                    a *= vec2(lds_ki[kb + row0 + uint(c)],
                              lds_ki[kb + row0 + uint(c) + 1u]);
#endif
                    f16vec2 pp = nr_vit_exp2(a);
#if NR_VQP
                    pp = f16vec2(nr_quant_e4m3(pp.x * NR_F16(ps)),
                                 nr_quant_e4m3(pp.y * NR_F16(ps)));
#endif
                    pv[c >> 1] = pp;
                    ph[c] = pp.x; ph[c+1] = pp.y;
                }
                // The PTX reduction, in lane. `shuffleXor(16)` is the pair eight
                // keys apart - the other half wave holds them - and the four-pair
                // and even/odd sums are `part`'s own halves, in the original
                // order: `s.x` is ((p0+p2)+p4)+p6 and `s.y` the odd one beside it.
                for (int c = 0; c < lg.length(); c += 2) {
                    const f16vec2 pv2 = pv[c >> 1];
                    const f16vec2 pair = pv2 +
                        unpackFloat2x16(subgroupShuffleXor(packFloat2x16(pv2), 16u));
                    part[b][c >> 1] = (kb == 0u) ? pair : part[b][c >> 1] + pair;
                }
                if (kb + 16u == uint(NR_KC)) {
                    const f16vec2 s = ((part[b][0] + part[b][1]) + part[b][2]) + part[b][3];
                    den[b] = float(NR_F16(NR_F16(den[b]) + NR_F16(s.x + s.y)));
                }
                // **P^T is the B operand with no memory in between.** An
                // Accumulator's components and a B operand's are the same map
                // (coopmm.glsl's probe table), so this copy moves no data - it
                // is the whole point of the orientation. A padding key tile is
                // killed here exactly as it was before the transpose.
#if NR_VPB16
                NR_FRAG_B pf = NR_FRAG_B(ph);
                if (NR_VT_GUARD != 0 && j0 + kb >= pc.tokens) pf = NR_FRAG_B(0.0);
#else
                NR_FRAG_E4M3 pe = NR_FRAG_E4M3(ph);
                if (NR_VT_GUARD != 0 && j0 + kb >= pc.tokens) pe = NR_FRAG_E4M3(0.0);
                NR_FRAG_B pf;
                for (int c = 0; c < 8; ++c) pf[c] = pe[c];
#endif
                for (uint n = 0u; n < 2u; ++n) {
                    NR_MMA(ctx[b][n], vf[n], pf);
#if NR_ACC_F16 > 0
                    if ((kb/16u+1u)%uint(NR_ACC_F16)==0u)
                        for (int z=0; z<ctx[b][n].length(); ++z)
                            ctx[b][n][z]=nr_round_f16(float(ctx[b][n][z]));
#endif
                }
            }
        }
