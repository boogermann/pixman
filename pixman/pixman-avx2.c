#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <immintrin.h> /* for AVX2 intrinsics */
#include "pixman-private.h"
#include "pixman-combine32.h"
#include "pixman-inlines.h"

#define MASK_0080_AVX2 _mm256_set1_epi16 (0x0080)
#define MASK_00FF_AVX2 _mm256_set1_epi16 (0x00ff)
#define MASK_0101_AVX2 _mm256_set1_epi16 (0x0101)

static force_inline __m256i
load_256_unaligned_masked (int* src, __m256i mask)
{
    return _mm256_maskload_epi32 (src, mask);
}

static force_inline __m256i
get_partial_256_data_mask (const int num_elem, const int total_elem)
{
    int maskint[16] = {-1,-1,-1,-1,-1,-1,-1,-1,1, 1, 1, 1, 1, 1, 1, 1};
    int* addr = maskint + total_elem - num_elem;
    return _mm256_loadu_si256 ((__m256i*) addr);
}

static force_inline void
negate_2x256 (__m256i  data_lo,
	      __m256i  data_hi,
	      __m256i* neg_lo,
	      __m256i* neg_hi)
{
    *neg_lo = _mm256_xor_si256 (data_lo, MASK_00FF_AVX2);
    *neg_hi = _mm256_xor_si256 (data_hi, MASK_00FF_AVX2);
}

static force_inline __m256i
pack_2x256_256 (__m256i lo, __m256i hi)
{
    return _mm256_packus_epi16 (lo, hi);
}

static force_inline void
pix_multiply_2x256 (__m256i* data_lo,
		    __m256i* data_hi,
		    __m256i* alpha_lo,
		    __m256i* alpha_hi,
		    __m256i* ret_lo,
		    __m256i* ret_hi)
{
    __m256i lo, hi;

    lo = _mm256_mullo_epi16 (*data_lo, *alpha_lo);
    hi = _mm256_mullo_epi16 (*data_hi, *alpha_hi);
    lo = _mm256_adds_epu16 (lo, MASK_0080_AVX2);
    hi = _mm256_adds_epu16 (hi, MASK_0080_AVX2);
    *ret_lo = _mm256_mulhi_epu16 (lo, MASK_0101_AVX2);
    *ret_hi = _mm256_mulhi_epu16 (hi, MASK_0101_AVX2);
}

static force_inline void
over_2x256 (__m256i* src_lo,
	    __m256i* src_hi,
	    __m256i* alpha_lo,
	    __m256i* alpha_hi,
	    __m256i* dst_lo,
	    __m256i* dst_hi)
{
    __m256i t1, t2;

    negate_2x256 (*alpha_lo, *alpha_hi, &t1, &t2);
    pix_multiply_2x256 (dst_lo, dst_hi, &t1, &t2, dst_lo, dst_hi);

    *dst_lo = _mm256_adds_epu8 (*src_lo, *dst_lo);
    *dst_hi = _mm256_adds_epu8 (*src_hi, *dst_hi);
}

static force_inline void
expand_alpha_2x256 (__m256i  data_lo,
		    __m256i  data_hi,
		    __m256i* alpha_lo,
		    __m256i* alpha_hi)
{
    __m256i lo, hi;

    lo = _mm256_shufflelo_epi16 (data_lo, _MM_SHUFFLE (3, 3, 3, 3));
    hi = _mm256_shufflelo_epi16 (data_hi, _MM_SHUFFLE (3, 3, 3, 3));

    *alpha_lo = _mm256_shufflehi_epi16 (lo, _MM_SHUFFLE (3, 3, 3, 3));
    *alpha_hi = _mm256_shufflehi_epi16 (hi, _MM_SHUFFLE (3, 3, 3, 3));
}

static force_inline  void
unpack_256_2x256 (__m256i data, __m256i* data_lo, __m256i* data_hi)
{
    *data_lo = _mm256_unpacklo_epi8 (data, _mm256_setzero_si256 ());
    *data_hi = _mm256_unpackhi_epi8 (data, _mm256_setzero_si256 ());
}

static force_inline void
save_256_unaligned (__m256i* dst, __m256i data)
{
    _mm256_storeu_si256 (dst, data);
}

static force_inline void
save_256_unaligned_masked (int* dst, __m256i mask, __m256i data)
{
    _mm256_maskstore_epi32 (dst, mask, data);
}

static force_inline int
is_opaque_256 (__m256i x)
{
    __m256i ffs = _mm256_cmpeq_epi8 (x, x);
    return (_mm256_movemask_epi8
	    (_mm256_cmpeq_epi8 (x, ffs)) & 0x88888888) == 0x88888888;
}

static force_inline int
is_zero_256 (__m256i x)
{
    return _mm256_movemask_epi8 (
	_mm256_cmpeq_epi8 (x, _mm256_setzero_si256 ())) == 0xffffffff;
}

static force_inline int
is_transparent_256 (__m256i x)
{
    return (_mm256_movemask_epi8 (
                _mm256_cmpeq_epi8 (x, _mm256_setzero_si256 ())) & 0x88888888)
                    == 0x88888888;
}

static force_inline __m256i
load_256_unaligned (const __m256i* src)
{
    return _mm256_loadu_si256 (src);
}

static force_inline __m256i
combine8 (const __m256i *ps, const __m256i *pm)
{
    __m256i ymm_src_lo, ymm_src_hi;
    __m256i ymm_msk_lo, ymm_msk_hi;
    __m256i s;

    if (pm)
    {
        ymm_msk_lo = load_256_unaligned (pm);
        if (is_transparent_256 (ymm_msk_lo))
        {
            return _mm256_setzero_si256 ();
        }
    }

    s = load_256_unaligned (ps);

    if (pm)
    {
	    unpack_256_2x256 (s, &ymm_src_lo, &ymm_src_hi);
	    unpack_256_2x256 (ymm_msk_lo, &ymm_msk_lo, &ymm_msk_hi);

	    expand_alpha_2x256 (ymm_msk_lo, ymm_msk_hi,
                                &ymm_msk_lo, &ymm_msk_hi);

	    pix_multiply_2x256 (&ymm_src_lo, &ymm_src_hi,
                                &ymm_msk_lo, &ymm_msk_hi,
                                &ymm_src_lo, &ymm_src_hi);
	    s = pack_2x256_256 (ymm_src_lo, ymm_src_hi);
    }
    return s;
}

static force_inline __m256i
expand_alpha_1x256 (__m256i data)
{
    return _mm256_shufflehi_epi16 (_mm256_shufflelo_epi16 (data,
                                        _MM_SHUFFLE (3, 3, 3, 3)),
                                            _MM_SHUFFLE (3, 3, 3, 3));
}

static force_inline void
expand_alpha_rev_2x256 (__m256i  data_lo,
                        __m256i  data_hi,
                        __m256i* alpha_lo,
                        __m256i* alpha_hi)
{
    __m256i lo, hi;

    lo = _mm256_shufflelo_epi16 (data_lo, _MM_SHUFFLE (0, 0, 0, 0));
    hi = _mm256_shufflelo_epi16 (data_hi, _MM_SHUFFLE (0, 0, 0, 0));

    *alpha_lo = _mm256_shufflehi_epi16 (lo, _MM_SHUFFLE (0, 0, 0, 0));
    *alpha_hi = _mm256_shufflehi_epi16 (hi, _MM_SHUFFLE (0, 0, 0, 0));
}

static force_inline void
in_over_2x256 (__m256i* src_lo,
               __m256i* src_hi,
               __m256i* alpha_lo,
               __m256i* alpha_hi,
               __m256i* mask_lo,
               __m256i* mask_hi,
               __m256i* dst_lo,
               __m256i* dst_hi)
{
    __m256i s_lo, s_hi;
    __m256i a_lo, a_hi;

    pix_multiply_2x256 (src_lo,   src_hi, mask_lo, mask_hi, &s_lo, &s_hi);
    pix_multiply_2x256 (alpha_lo, alpha_hi, mask_lo, mask_hi, &a_lo, &a_hi);

    over_2x256 (&s_lo, &s_hi, &a_lo, &a_hi, dst_lo, dst_hi);
}

static force_inline __m256i
create_mask_2x32_256 (uint32_t mask0,
                      uint32_t mask1)
{
    return _mm256_set_epi32 (mask0, mask1, mask0, mask1,
                             mask0, mask1, mask0, mask1);
}

static force_inline __m256i
unpack_32_1x256 (uint32_t data)
{
    return _mm256_unpacklo_epi8 (
                _mm256_broadcastd_epi32 (
                    _mm_cvtsi32_si128 (data)), _mm256_setzero_si256 ());
}

static force_inline __m256i
expand_pixel_32_1x256 (uint32_t data)
{
    return _mm256_shuffle_epi32 (unpack_32_1x256 (data),
                                    _MM_SHUFFLE (1, 0, 1, 0));
}

static force_inline void
core_combine_over_u_avx2_mask (uint32_t *         pd,
                               const uint32_t*	  ps,
                               const uint32_t*	  pm,
                               int	          w)
{
    __m256i data_mask, mask;
    data_mask = _mm256_set1_epi32 (-1);

    while (w > 0)
    {
        if (w < 8)
        {
            data_mask = get_partial_256_data_mask (w, 8);
        }

        mask = load_256_unaligned_masked ((int *)pm, data_mask);

        if (!is_zero_256 (mask))
        {
            __m256i src, dst;
            __m256i src_hi, src_lo;
            __m256i dst_hi, dst_lo;
            __m256i mask_hi, mask_lo;
            __m256i alpha_hi, alpha_lo;

            src = load_256_unaligned_masked ((int *)ps, data_mask);

            if (is_opaque_256 (_mm256_and_si256 (src, mask)))
            {
                save_256_unaligned_masked ((int *)pd, data_mask, src);
            }
            else
            {
                dst = load_256_unaligned_masked ((int *)pd, data_mask);

                unpack_256_2x256 (mask, &mask_lo, &mask_hi);
                unpack_256_2x256 (src, &src_lo, &src_hi);

                expand_alpha_2x256 (mask_lo, mask_hi, &mask_lo, &mask_hi);

                pix_multiply_2x256 (&src_lo, &src_hi,
                                    &mask_lo, &mask_hi,
                                    &src_lo, &src_hi);

                unpack_256_2x256 (dst, &dst_lo, &dst_hi);
                expand_alpha_2x256 (src_lo, src_hi,
                                    &alpha_lo, &alpha_hi);

                over_2x256 (&src_lo, &src_hi, &alpha_lo, &alpha_hi,
                            &dst_lo, &dst_hi);

                save_256_unaligned_masked ((int *)pd, data_mask,
                                          pack_2x256_256 (dst_lo, dst_hi));
            }
        }
        pm += 8;
        ps += 8;
        pd += 8;
        w -= 8;
    }
}

static force_inline void
core_combine_over_u_avx2_no_mask (uint32_t *	        pd,
                                  const uint32_t*       ps,
                                  int                   w)
{
    __m256i src, dst;
    __m256i src_hi, src_lo, dst_hi, dst_lo;
    __m256i alpha_hi, alpha_lo;
    __m256i data_mask = _mm256_set1_epi32 (-1);

    while (w > 0)
    {
        if (w < 8) {
            data_mask = get_partial_256_data_mask (w, 8);
        }

        src = load_256_unaligned_masked ((int*)ps, data_mask);

        if (!is_zero_256 (src))
        {
            if (is_opaque_256 (src))
            {
                save_256_unaligned_masked ((int*)pd, data_mask, src);
            }
            else
            {
                dst = load_256_unaligned_masked ((int*)pd, data_mask);

                unpack_256_2x256 (src, &src_lo, &src_hi);
                unpack_256_2x256 (dst, &dst_lo, &dst_hi);

                expand_alpha_2x256 (src_lo, src_hi,
                                    &alpha_lo, &alpha_hi);
                over_2x256 (&src_lo, &src_hi, &alpha_lo, &alpha_hi,
                            &dst_lo, &dst_hi);

                save_256_unaligned_masked ((int*)pd, data_mask,
                                          pack_2x256_256 (dst_lo, dst_hi));
            }
        }

        ps += 8;
        pd += 8;
        w -= 8;
    }
}

static force_inline void
avx2_combine_over_u (pixman_implementation_t *imp,
		     pixman_op_t	      op,
		     uint32_t *		      pd,
		     const uint32_t *	      ps,
		     const uint32_t *	      pm,
		     int		      w)
{
    if (pm)
        core_combine_over_u_avx2_mask (pd, ps, pm, w);
    else
        core_combine_over_u_avx2_no_mask (pd, ps, w);
}

static force_inline void
avx2_combine_add_u (pixman_implementation_t *imp,
                    pixman_op_t              op,
                    uint32_t *               dst,
                    const uint32_t *         src,
                    const uint32_t *         mask,
                    int                      width)
{
    uint32_t* pd = dst;
    const uint32_t* ps = src;
    const uint32_t* pm = mask;
    int w = width;
    __m256i data_mask = _mm256_set1_epi32 (-1);
    __m256i s;

    while (w > 0)
    {
        if (w < 8) {
            data_mask = get_partial_256_data_mask (w, 8);
        }

	s = combine8 ((__m256i*)ps, (__m256i*)pm);

	save_256_unaligned_masked ((int*)pd, data_mask,
                        _mm256_adds_epu8 (s,
                            load_256_unaligned_masked ((int*)pd, data_mask)));

	pd += 8;
	ps += 8;
	if (pm)
	    pm += 8;
	w -= 8;
    }
}

static void
avx2_composite_add_8888_8888 (pixman_implementation_t *imp,
                              pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t    *dst_line, *dst;
    uint32_t    *src_line, *src;
    int dst_stride, src_stride;

    PIXMAN_IMAGE_GET_LINE (
	src_image, src_x, src_y, uint32_t, src_stride, src_line, 1);
    PIXMAN_IMAGE_GET_LINE (
	dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);

    while (height--)
    {
	dst = dst_line;
	dst_line += dst_stride;
	src = src_line;
	src_line += src_stride;

	avx2_combine_add_u (imp, op, dst, src, NULL, width);
    }
}

static void
avx2_combine_over_reverse_u (pixman_implementation_t   *imp,
                             pixman_op_t	        op,
                             uint32_t *	                pd,
                             const uint32_t *	        ps,
                             const uint32_t *	        pm,
                             int		        w)
{
    __m256i ymm_dst_lo, ymm_dst_hi;
    __m256i ymm_src_lo, ymm_src_hi;
    __m256i ymm_alpha_lo, ymm_alpha_hi;
    __m256i data_mask = _mm256_set1_epi32 (-1);

    while (w > 0)
    {
        if (w < 8) {
            data_mask = get_partial_256_data_mask (w, 8);
        }

        ymm_src_hi = combine8 ((__m256i*)ps, (__m256i*)pm);
        ymm_dst_hi = load_256_unaligned_masked ((int *) pd, data_mask);

        unpack_256_2x256 (ymm_src_hi, &ymm_src_lo, &ymm_src_hi);
        unpack_256_2x256 (ymm_dst_hi, &ymm_dst_lo, &ymm_dst_hi);

        expand_alpha_2x256 (ymm_dst_lo, ymm_dst_hi,
                            &ymm_alpha_lo, &ymm_alpha_hi);

        over_2x256 (&ymm_dst_lo, &ymm_dst_hi,
                    &ymm_alpha_lo, &ymm_alpha_hi,
                    &ymm_src_lo, &ymm_src_hi);

        save_256_unaligned_masked ((int *)pd, data_mask,
                              pack_2x256_256 (ymm_src_lo, ymm_src_hi));

        w -= 8;
        ps += 8;
        pd += 8;
        if (pm)
            pm += 8;
    }
}

static void
avx2_composite_over_reverse_n_8888 (pixman_implementation_t *imp,
                                    pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    uint32_t src;
    uint32_t    *dst_line, *dst;
    __m256i xmm_src;
    __m256i xmm_dst, xmm_dst_lo, xmm_dst_hi;
    __m256i xmm_dsta_hi, xmm_dsta_lo;
    __m256i data_mask;
    __m256i tmp_lo, tmp_hi;
    int dst_stride;
    int32_t w;

    src = _pixman_image_get_solid (imp, src_image, dest_image->bits.format);

    if (src == 0)
        return;

    PIXMAN_IMAGE_GET_LINE (
            dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);

    xmm_src = expand_pixel_32_1x256 (src);

    while (height--)
    {
        dst = dst_line;
        dst_line += dst_stride;
        w = width;
        data_mask = _mm256_set1_epi32 (-1);

        while (w > 0)
        {
            if (w < 8) {
                data_mask = get_partial_256_data_mask (w, 8);
            }

            xmm_dst = load_256_unaligned_masked ((int*)dst, data_mask);

            unpack_256_2x256 (xmm_dst, &xmm_dst_lo, &xmm_dst_hi);
            expand_alpha_2x256 (xmm_dst_lo, xmm_dst_hi,
                                &xmm_dsta_lo, &xmm_dsta_hi);

            tmp_lo = xmm_src;
            tmp_hi = xmm_src;

            over_2x256 (&xmm_dst_lo, &xmm_dst_hi,
                        &xmm_dsta_lo, &xmm_dsta_hi,
                        &tmp_lo, &tmp_hi);

            save_256_unaligned_masked ((int*)dst, data_mask,
                                        pack_2x256_256 (tmp_lo, tmp_hi));
            w -= 8;
            dst += 8;
        }
    }
}

static void
avx2_composite_over_8888_8888 (pixman_implementation_t *imp,
                               pixman_composite_info_t *info)
{
    PIXMAN_COMPOSITE_ARGS (info);
    int dst_stride, src_stride;
    uint32_t    *dst_line, *dst;
    uint32_t    *src_line, *src;

    PIXMAN_IMAGE_GET_LINE (
            dest_image, dest_x, dest_y, uint32_t, dst_stride, dst_line, 1);
    PIXMAN_IMAGE_GET_LINE (
            src_image, src_x, src_y, uint32_t, src_stride, src_line, 1);

    dst = dst_line;
    src = src_line;

    while (height--)
    {
        avx2_combine_over_u (imp, op, dst, src, NULL, width);
        dst += dst_stride;
        src += src_stride;
    }
}

static uint32_t *
avx2_fetch_x8r8g8b8 (pixman_iter_t *iter, const uint32_t *mask)
{
    int w = iter->width;
    __m256i ff000000 = create_mask_2x32_256 (0xff000000, 0xff000000);
    __m256i data_mask = _mm256_set1_epi32 (-1);
    uint32_t *dst = iter->buffer;
    uint32_t *src = (uint32_t *)iter->bits;

    iter->bits += iter->stride;

    while (w > 0)
    {
        if (w < 8) {
            data_mask = get_partial_256_data_mask (w, 8);
        }

        save_256_unaligned_masked ((int *)dst, data_mask,
                                            _mm256_or_si256 (
                                                load_256_unaligned (
                                                    (__m256i *)src), ff000000));
        dst += 8;
        src += 8;
        w -= 8;
    }
    return iter->buffer;
}

static void
avx2_combine_out_reverse_u (pixman_implementation_t *imp,
                            pixman_op_t              op,
                            uint32_t *               pd,
                            const uint32_t *         ps,
                            const uint32_t *         pm,
                            int                      w)
{
    __m256i xmm_src_lo, xmm_src_hi;
    __m256i xmm_dst_lo, xmm_dst_hi;
    __m256i data_mask = _mm256_set1_epi32 (-1);

    while (w > 0)
    {
        if (w < 8) {
            data_mask = get_partial_256_data_mask (w, 8);
        }

	xmm_src_hi = combine8 ((__m256i*)ps, (__m256i*)pm);
	xmm_dst_hi = load_256_unaligned_masked ((int*) pd, data_mask);

	unpack_256_2x256 (xmm_src_hi, &xmm_src_lo, &xmm_src_hi);
	unpack_256_2x256 (xmm_dst_hi, &xmm_dst_lo, &xmm_dst_hi);

	expand_alpha_2x256 (xmm_src_lo, xmm_src_hi, &xmm_src_lo, &xmm_src_hi);
	negate_2x256       (xmm_src_lo, xmm_src_hi, &xmm_src_lo, &xmm_src_hi);

	pix_multiply_2x256 (&xmm_dst_lo, &xmm_dst_hi,
			    &xmm_src_lo, &xmm_src_hi,
			    &xmm_dst_lo, &xmm_dst_hi);

	save_256_unaligned_masked (
	    (int*)pd, data_mask, pack_2x256_256 (xmm_dst_lo, xmm_dst_hi));

	ps += 8;
	pd += 8;
	if (pm)
	    pm += 8;
	w -= 8;
    }
}

static const pixman_fast_path_t avx2_fast_paths[] =
{
    PIXMAN_STD_FAST_PATH (OVER, a8r8g8b8, null, a8r8g8b8, avx2_composite_over_8888_8888),
    PIXMAN_STD_FAST_PATH (OVER, a8r8g8b8, null, x8r8g8b8, avx2_composite_over_8888_8888),
    PIXMAN_STD_FAST_PATH (OVER, a8b8g8r8, null, a8b8g8r8, avx2_composite_over_8888_8888),
    PIXMAN_STD_FAST_PATH (OVER, a8b8g8r8, null, x8b8g8r8, avx2_composite_over_8888_8888),
    /* PIXMAN_OP_OVER_REVERSE */
    PIXMAN_STD_FAST_PATH (OVER_REVERSE, solid, null, a8r8g8b8, avx2_composite_over_reverse_n_8888),
    PIXMAN_STD_FAST_PATH (OVER_REVERSE, solid, null, a8b8g8r8, avx2_composite_over_reverse_n_8888),
    PIXMAN_STD_FAST_PATH (ADD, a8r8g8b8, null, a8r8g8b8, avx2_composite_add_8888_8888),
    PIXMAN_STD_FAST_PATH (ADD, a8b8g8r8, null, a8b8g8r8, avx2_composite_add_8888_8888),
    { PIXMAN_OP_NONE },
};

#define IMAGE_FLAGS							\
    (FAST_PATH_STANDARD_FLAGS | FAST_PATH_ID_TRANSFORM |		\
     FAST_PATH_BITS_IMAGE | FAST_PATH_SAMPLES_COVER_CLIP_NEAREST)

static const pixman_iter_info_t avx2_iters[] =
{
    { PIXMAN_x8r8g8b8, IMAGE_FLAGS, ITER_NARROW,
      _pixman_iter_init_bits_stride, avx2_fetch_x8r8g8b8, NULL
    },
    { PIXMAN_null },
};

#if defined(__GNUC__) && !defined(__x86_64__) && !defined(__amd64__)
__attribute__((__force_align_arg_pointer__))
#endif
pixman_implementation_t *
_pixman_implementation_create_avx2 (pixman_implementation_t *fallback)
{
    pixman_implementation_t *imp = _pixman_implementation_create (fallback, avx2_fast_paths);

    /* Set up function pointers */
    imp->combine_32[PIXMAN_OP_OVER] = avx2_combine_over_u;
    imp->combine_32[PIXMAN_OP_OVER_REVERSE] = avx2_combine_over_reverse_u;
    imp->combine_32[PIXMAN_OP_ADD] = avx2_combine_add_u;
    imp->combine_32[PIXMAN_OP_OUT_REVERSE] = avx2_combine_out_reverse_u;

    imp->iter_info = avx2_iters;

    return imp;
}
