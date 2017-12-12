//
//  sprintz_delta_rle.cpp
//  Compress
//
//  Created by DB on 12/11/17.
//  Copyright © 2017 D Blalock. All rights reserved.
//

#include "sprintz_delta.h"

#include <stdio.h>

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "bitpack.h"
#include "format.h"
#include "util.h" // for memrep

#include "array_utils.hpp" // TODO rm
#include "debug_utils.hpp" // TODO rm

static const int debug = 0;
// static const int debug = 3;
// static const int debug = 4;

// TODO shuffle LUTs below should be in a shared header (prolly bitpack.h)

// byte shuffle values to construct data masks; note that nbits == 7 yields
// a byte of all ones (0xff); also note that rows 1 and 3 below are unused
static const __m256i nbits_to_mask_8b = _mm256_setr_epi8(
    0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0xff,
    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // unused
    0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0xff,
    0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00); // unused

static const __m256i nbits_to_mask_16b_low = _mm256_setr_epi8(
    0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff,   // 0-8
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,               // 9-15
    0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff,   // 0-8
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff);              // 9-15
static const __m256i nbits_to_mask_16b_high = _mm256_setr_epi8(
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,         // 0-7
    0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0xff,         // 8-15
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,         // 0-7
    0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0xff);        // 8-15


static const int kDefaultGroupSzBlocks = 2;

// TODO this should just be in one header
#define CHECK_INT_UINT_TYPES_VALID(int_t, uint_t)               \
    static_assert(sizeof(uint_t) == sizeof(int_t),              \
        "uint type and int type sizes must be the same!");      \
    static_assert(sizeof(uint_t) == 1 || sizeof(uint_t) == 2,   \
        "Only element sizes of 1 and 2 bytes are supported!");  \


// ========================================================== rowmajor delta rle

template<typename int_t, typename uint_t>
int64_t compress_rowmajor_delta_rle(const uint_t* src, uint64_t len,
    int_t* dest, uint16_t ndims, bool write_size)
{
    CHECK_INT_UINT_TYPES_VALID(int_t, uint_t);
    static const uint8_t elem_sz = sizeof(uint_t);
    static const uint8_t elem_sz_nbits = 8 * elem_sz;
    static const uint8_t nbits_sz_bits = elem_sz == 1 ? 3 : 4; // XXX only {8,16}b
    // constants that could, in principle, be changed (but not in this impl)
    static const int block_sz = 8;
    static const int stripe_sz_nbytes = 8;
    // constants that could actually be changed in this impl
    static const int group_sz_blocks = kDefaultGroupSzBlocks;
    static const int length_header_nbytes = 8; // TODO indirect to format.h
    static const uint16_t max_run_nblocks = 0x7fff; // 15 bit counter
    // static const uint16_t max_run_nblocks = 2; // TODO rm
    // derived consts
    static const uint32_t min_data_size = 8 * block_sz * group_sz_blocks;
    static const int stripe_sz = stripe_sz_nbytes / elem_sz;

    // int8_t* orig_dest = dest;
    // const uint8_t* src_end = src + len;

    const uint_t* orig_src = src;
    const uint_t* src_end = src + len;
    int_t* orig_dest = dest;

    int gDebug = debug;
    int debug = (elem_sz == 2) ? gDebug : 0;

    // ================================ one-time initialization

    // ------------------------ stats derived from ndims
    uint16_t nstripes = DIV_ROUND_UP(ndims, stripe_sz);
    uint32_t group_sz = ndims * block_sz * group_sz_blocks;
    uint32_t total_header_bits = ndims * nbits_sz_bits * group_sz_blocks;
    uint32_t total_header_bytes = DIV_ROUND_UP(total_header_bits, 8);

    // ------------------------ store data size and number of dimensions

    // handle low dims and low length; we'd read way past the end of the
    // input in this case
    if (len < min_data_size) {
        assert(min_data_size < ((uint64_t)1) << 16);
        // printf("data less than min data size: %lu\n", min_data_size);
        if (write_size) {
            dest += write_metadata_rle_8b(dest, ndims, 0, (uint16_t)len);
        }
        memcpy(dest, src, len);
        return (dest - orig_dest) + len;
    }
    if (write_size) {
        dest += length_header_nbytes;
    }

    if (debug > 0) {
        printf("-------- compression (len = %lld)\n", (int64_t)len);
        if (debug > 2) {
            printf("saw original data:\n"); dump_elements(src, len, ndims);
        }
    }
    if (debug > 1) { printf("total header bits, bytes: %d, %d\n", total_header_bits, total_header_bytes); }

    // ------------------------ temp storage
    uint8_t*  stripe_bitwidths  = (uint8_t*) malloc(nstripes*sizeof(uint8_t));
    uint32_t* stripe_bitoffsets = (uint32_t*)malloc(nstripes*sizeof(uint32_t));
    uint64_t* stripe_masks      = (uint64_t*)malloc(nstripes*sizeof(uint64_t));
    uint32_t* stripe_headers    = (uint32_t*)malloc(nstripes*sizeof(uint32_t));

    uint32_t total_header_bytes_padded = total_header_bytes + 4;
    uint8_t* header_bytes = (uint8_t*)calloc(total_header_bytes_padded, 1);

    // extra row is for storing previous values
    // TODO just look at src and special case first row
    int_t* deltas = (int_t*)calloc(elem_sz, (block_sz + 1) * ndims);
    uint_t* prev_vals_ar = (uint_t*)(deltas + block_sz * ndims);
    // int8_t* deltas = (int8_t*)calloc(1, (block_sz + 1) * ndims);
    // uint8_t* prev_vals_ar = (uint8_t*)(deltas + block_sz * ndims);

    // ================================ main loop

    uint16_t run_length_nblocks = 0;

    const uint8_t* last_full_group_start = src_end - group_sz;
    uint32_t ngroups = 0;
    // printf("group_sz elements: %d\n", group_sz);
    // printf("src end offset, last_full_group_start offset = %d, %d\n", (int)(src_end - src), (int)(last_full_group_start - src));
    while (src <= last_full_group_start) {
        ngroups++;  // invariant: groups we start are always finished

        // printf("==== group %d\n", (int)ngroups - 1);

        // int8_t* header_dest = dest;
        // dest += total_header_bytes;
        int8_t* header_dest = (int8_t*)dest;
        dest = (int_t*)(((int8_t*)dest) + total_header_bytes);

        memset(header_bytes, 0, total_header_bytes_padded);
        memset(header_dest, 0, total_header_bytes);

        uint32_t header_bit_offset = 0;
        int b = 0;
        while (b < group_sz_blocks) { // for each block

            // ------------------------ zero stripe info from previous iter
            memset(stripe_bitwidths, 0, nstripes * sizeof(stripe_bitwidths[0]));
            memset(stripe_masks,     0, nstripes * sizeof(stripe_masks[0]));
            memset(stripe_headers,   0, nstripes * sizeof(stripe_headers[0]));

            // ------------------------ compute info for each stripe
            for (uint16_t dim = 0; dim < ndims; dim++) {
                // compute maximum number of bits used by any value of this dim,
                // while simultaneously computing deltas
                uint_t mask = 0;
                uint_t prev_val = prev_vals_ar[dim];
                for (uint8_t i = 0; i < block_sz; i++) {
                    uint32_t offset = (i * ndims) + dim;
                    uint_t val = src[offset];
                    int_t delta = (int8_t)(val - prev_val);
                    uint_t bits = ZIGZAG_ENCODE_SCALAR(delta);
                    mask |= bits;
                    deltas[offset] = bits;
                    prev_val = val;
                }
                // write out value for delta encoding of next block
                // mask = NBITS_MASKS_U8[mask];
                if (elem_sz == 1) {
                    mask = NBITS_MASKS_U8[mask];
                } else if (elem_sz == 2) {
                    uint8_t upper_mask = NBITS_MASKS_U8[mask >> 8];
                    mask = upper_mask > 0 ? (upper_mask << 8) + 255 : NBITS_MASKS_U8[mask];
                    // mask = 0xffff; // TODO rm
                }
                prev_vals_ar[dim] = prev_val;

                // if (mask > 0) { mask = NBITS_MASKS_U8[255]; } // TODO rm
                uint8_t max_nbits = (32 - _lzcnt_u32((uint32_t)mask));

                // printf("\tmax nbits: %d\n", max_nbits);

                uint16_t stripe = dim / stripe_sz;
                uint8_t idx_in_stripe = dim % stripe_sz;

                // accumulate stats about this stripe
                stripe_bitwidths[stripe] += max_nbits;
                stripe_masks[stripe] |= ((uint64_t)mask) << (idx_in_stripe * elem_sz_nbits);

                // accumulate header info for this stripe
                uint32_t write_nbits = max_nbits - (max_nbits == elem_sz_nbits); // map 8 to 7
                stripe_headers[stripe] |= write_nbits << (idx_in_stripe * nbits_sz_bits);
            }

            // compute start offsets of each stripe (in bits)
            stripe_bitoffsets[0] = 0;
            for (uint32_t stripe = 1; stripe < nstripes; stripe++) {
                stripe_bitoffsets[stripe] = stripe_bitoffsets[stripe - 1] +
                    stripe_bitwidths[stripe - 1];
            }
            // compute width of each row (in bytes); note that we byte align
            uint32_t row_width_bits = stripe_bitoffsets[nstripes - 1] +
                stripe_bitwidths[nstripes-1];
            uint32_t out_row_nbytes =
                (row_width_bits >> 3) + ((row_width_bits % 8) > 0);

            // printf("mask for zigzag val of 2: %d\n", NBITS_MASKS_U8[2]);
            // printf("stripe bitwidth 0: %d\n", stripe_bitwidths[0]);
            // printf("--- block %d, in offset = %d, row_width_bits = %u\n", b, (int)(src - orig_src), row_width_bits);

just_read_block:

            // ------------------------ handle runs of zeros
            bool do_rle = row_width_bits == 0 && run_length_nblocks < max_run_nblocks;

            // printf("row width bits: %d\n", row_width_bits);

            if (do_rle) {
do_rle:
                run_length_nblocks++; // TODO uncomment
                src += block_sz * ndims;

                if (src < last_full_group_start) {
                    continue; // still enough data to finish group
                } else { // not enough data to finish group
                    // write out headers for this block; they're all zero, so
                    // just increment addr at which to write next time
                    header_bit_offset += ndims * nbits_sz_bits;

                    // printf("aborting and compressing rle block of length %d ending at src offset %d\n", run_length_nblocks, (int)(src - orig_src));

                    b++; // we're finishing off this block

                    // write out length of the current constant section
                    *dest = run_length_nblocks & 0x7f; // bottom 7 bits
                    dest++;
                    if (run_length_nblocks > 0x7f) { // need another byte
                        *(dest-1) |= 0x80; // set MSB of previous byte
                        *dest = (uint8_t)(run_length_nblocks >> 7);
                        dest++;
                    }

                    // write out this const section, and use empty const
                    // sections to fill up rest of block
                    for (; b < group_sz_blocks; b++) {
                        *dest = 0;
                        dest++;
                    };

                    // we shouldn't need this write, but makes our invariants
                    // consistent
                    run_length_nblocks = 0;

                    goto main_loop_end; // just memcpy remaining data
                }
            }

            if (run_length_nblocks > 0) { // just finished a run
                // printf("compressing rle block of length %d ending at offset %d\n", run_length_nblocks, (int)(src - orig_src));
                // printf("initial header bit offset: %d\n", header_bit_offset);
                b++;

                *dest = run_length_nblocks & 0x7f; // bottom 7 bits
                // printf("wrote low byte: "); dump_bits(*dest);
                dest++;
                if (run_length_nblocks > 0x7f) { // need another byte
                    *(dest-1) |= 0x80; // set MSB of previous byte
                    // printf("adding another byte to encode run length\n");
                    *dest = (uint8_t)(run_length_nblocks >> 7);
                    dest++;
                }

                run_length_nblocks = 0;

                // write out header (equivalent since already zeroed)
                header_bit_offset += ndims * nbits_sz_bits;

                // if closing the run finished this block, life is hard; we
                // can't just execute the below code because it would try to
                // write past the end of the header bytes and write a data
                // block the decoder isn't expecting
                if (b == group_sz_blocks) {
                    // printf("reseting prev vals...\n");
                    // memcpy(prev_vals_ar, src - ndims, ndims);
                    // continue;

                    // start new group, and pretend the block we read
                    // was the first block in that group (which it is now)
                    ngroups++;  // invariant: groups we start are always finished
                    header_bit_offset = 0;
                    b = 0;
                    header_dest = dest;
                    dest += total_header_bytes;
                    memset(header_bytes, 0, total_header_bytes_padded);
                    memset(header_dest, 0, total_header_bytes);

                    goto just_read_block;
                }

                // have to enforce invariant that bitwidth of 0 always gets
                // run-length encoded; this case can happen when the current
                // block was zeros, but we hit the limit `max_run_nblocks`
                if (row_width_bits == 0) { goto do_rle; }

                // printf("row width bits for this nonzero block: %d\n", row_width_bits);
                // printf("modified header bit offset: %d\n", header_bit_offset);
            }

            // ------------------------ write out header bits for this block
            for (uint32_t stripe = 0; stripe < nstripes; stripe++) {
                uint16_t byte_offset = header_bit_offset >> 3;
                uint16_t bit_offset = header_bit_offset & 0x07;
                // Note: this write will get more complicated when upper byte
                // of each stripe_header isn't guaranteed to be 0
                *(uint32_t*)(header_dest + byte_offset) |= \
                    stripe_headers[stripe] << bit_offset;

                // if (ngroups == 3) { printf(" writing stripe width: %d at bit offset %d\n",
                //     stripe_headers[stripe], header_bit_offset); }

                uint8_t is_final_stripe = stripe == (nstripes - 1);
                uint8_t has_trailing_dims = (ndims % stripe_sz) > 0;
                uint8_t add_ndims = is_final_stripe && has_trailing_dims ?
                    ndims % stripe_sz : stripe_sz;
                header_bit_offset += nbits_sz_bits * add_ndims;
            }

            // ------------------------ write out block data

            // zero output so that we can just OR in bits (and also touch each
            // cache line in order to ensure that we prefetch, despite our
            // weird pattern of writes)
            // memset(dest, 0, nstripes * stripe_sz * block_sz);
            memset(dest, 0, out_row_nbytes * block_sz); // above line can overrun dest buff

            // write out packed data; we iterate thru stripes in reverse order
            // since (nbits % stripe_sz) != 0 will make the last stripe in each
            // row write into the start of the first stripe in the next row
            for (int16_t stripe = 0; stripe < nstripes; stripe++) {
                // load info for this stripe
                uint8_t offset_bits = (uint8_t)(stripe_bitoffsets[stripe] & 0x07);
                uint32_t offset_bytes = stripe_bitoffsets[stripe] >> 3;
                uint64_t mask = stripe_masks[stripe];
                uint16_t nbits = stripe_bitwidths[stripe];
                uint16_t total_bits = nbits + offset_bits;

                int8_t* outptr = dest + offset_bytes;
                const uint8_t* inptr = (const uint8_t*)(deltas +
                    (stripe * stripe_sz));

                if (total_bits <= 64) { // always fits in one u64
                    for (int i = 0; i < block_sz; i++) { // for each sample in block
                        uint64_t data = *(uint64_t*)inptr;
                        uint64_t packed_data = _pext_u64(data, mask);
                        uint64_t write_data = packed_data << offset_bits;
                        *(uint64_t*)outptr = write_data | (*(uint64_t*)outptr);

                        outptr += out_row_nbytes;
                        inptr += ndims;
                    }
                } else { // data spans 9 bytes
                    uint8_t nbits_lost = total_bits - 64;
                    for (int i = 0; i < block_sz; i++) { // for each sample in block
                        uint64_t data = *(uint64_t*)inptr;
                        uint64_t packed_data = _pext_u64(data, mask);
                        uint8_t extra_byte = (uint8_t)(packed_data >> (nbits - nbits_lost));
                        uint64_t write_data = packed_data << offset_bits;
                        *(uint64_t*)outptr = write_data | (*(uint64_t*)outptr);
                        *(outptr + 8) = extra_byte;

                        outptr += out_row_nbytes;
                        inptr += ndims;
                    }
                }
                // printf("read back header: "); dumpEndianBits(*(uint32_t*)(header_dest - stripe_header_sz));
            } // for each stripe
            src += block_sz * ndims;
            dest += block_sz * out_row_nbytes;

            // run_length_nblocks = 0;
            b++;
        } // for each block
    } // for each group

main_loop_end:

    free(stripe_bitwidths);
    free(stripe_bitoffsets);
    free(stripe_masks);
    free(stripe_headers);
    free(deltas);

    uint32_t remaining_len = (uint32_t)(src_end - src);
    if (write_size) {
        write_metadata_rle_8b(orig_dest, ndims, ngroups, remaining_len);
        // *(uint32_t*)orig_dest = ngroups;
        // *(uint16_t*)(orig_dest + 4) = (uint16_t)remaining_len;
        // *(uint16_t*)(orig_dest + 6) = ndims;
    }
    // printf("trailing len: %d\n", (int)remaining_len);
    memcpy(dest, src, remaining_len);
    return dest + remaining_len - orig_dest;
}

int64_t compress_rowmajor_delta_rle_8b(const uint8_t* src, uint64_t len,
    int8_t* dest, uint16_t ndims, bool write_size)
{
    return compress_rowmajor_delta_rle(src, len, dest, ndims, write_size);
}
// int64_t compress_rowmajor_delta_rle_16b(const uint16_t* src, uint64_t len,
//     int16_t* dest, uint16_t ndims, bool write_size)
// {
//     return compress_rowmajor_delta_rle(src, len, dest, ndims, write_size);
// }

// int64_t decompress8b_rowmajor_delta_rle(const int8_t* src, uint8_t* dest) {
SPRINTZ_FORCE_INLINE int64_t decompress8b_rowmajor_delta_rle(const int8_t* src,
    uint8_t* dest, uint16_t ndims, uint64_t ngroups, uint16_t remaining_len)
{
    // constants that could, in principle, be changed (but not in this impl)
    static const uint8_t block_sz = 8;
    static const uint8_t vector_sz = 32;
    static const uint8_t stripe_sz = 8;
    static const uint8_t nbits_sz_bits = 3;
    // constants that could actually be changed in this impl
    static const uint8_t group_sz_blocks = kDefaultGroupSzBlocks;
    // derived constants
    // static const int group_sz_per_dim = block_sz * group_sz_blocks;
    const uint8_t elem_sz = sizeof(*src);
    static const uint8_t stripe_header_sz = nbits_sz_bits * stripe_sz / 8;
    static const uint8_t nbits_sz_mask = (1 << nbits_sz_bits) - 1;
    static const uint64_t kHeaderUnpackMask = TILE_BYTE(nbits_sz_mask);
    static const uint32_t min_data_size = 8 * block_sz * group_sz_blocks;
    assert(stripe_sz % 8 == 0);
    assert(vector_sz % stripe_sz == 0);
    assert(vector_sz >= stripe_sz);

    uint8_t* orig_dest = dest;
    // const int8_t* orig_src = src;

    // ================================ one-time initialization

    // ------------------------ read original data size, ndims
    // static const uint32_t len_nbytes = 6;
    // uint64_t one = 1; // make next line legible
    // uint64_t len_mask = (one << (8 * len_nbytes)) - 1;
    // uint64_t orig_len = (*(uint64_t*)src) & len_mask;
    // uint16_t ndims = (*(uint16_t*)(src + len_nbytes));
    // static const uint32_t len_nbytes = 4;
    // uint64_t one = 1; // make next line legible
    // uint64_t len_mask = (one << (8 * len_nbytes)) - 1;
    // uint64_t ngroups = (*(uint64_t*)src) & len_mask;
    // uint16_t remaining_len = (*(uint16_t*)(src + len_nbytes));
    // uint16_t ndims = (*(uint16_t*)(src + len_nbytes + 2));
    // src += 8;

    // bool just_cpy = orig_len < min_data_size;
    bool just_cpy = (ngroups == 0) && remaining_len < min_data_size;
    // just_cpy = just_cpy || orig_len & (((uint64_t)1) << 47);
    if (just_cpy) { // if data was too small or failed to compress
        // printf("decomp: data less than min data size: %lu\n", min_data_size);
        memcpy(dest, src, (uint32_t)remaining_len);
        return remaining_len;
    }
    if (ndims == 0) {
        perror("ERROR: Received ndims of 0!");
        return 0;
    }

    // printf(">>>> ---- decomp: saw original ngroups, ndims = %llu, %d\n", ngroups, ndims);
    // printf("saw compressed data (or at least some of it):\n");
    // dump_bytes(src, 12);

    // printf("-------- decompression (orig_len = %lld)\n", (int64_t)orig_len);
    // printf("saw compressed data (with possible extra at end):\n");
    // dump_bytes(src, orig_len + 24);

    // ------------------------ stats derived from ndims
    // header stats
    uint32_t nheader_vals = ndims * group_sz_blocks;
    uint32_t nheader_stripes = nheader_vals / stripe_sz + \
        ((nheader_vals % stripe_sz) > 0);
    uint32_t total_header_bits = ndims * nbits_sz_bits * group_sz_blocks;
    uint32_t total_header_bytes = (total_header_bits / 8) + ((total_header_bits % 8) > 0);

    // final header can be shorter than others; construct mask so we don't
    // read in start of packed data as header bytes
    uint8_t remaining_header_sz = total_header_bytes % stripe_header_sz;
    uint8_t final_header_sz = remaining_header_sz ? remaining_header_sz : stripe_header_sz;
    uint32_t shift_bits = 8 * (4 - final_header_sz);
    uint32_t final_header_mask = ((uint32_t)0xffffffff) >> shift_bits;

    // stats for main decompression loop
    // uint32_t group_sz = ndims * group_sz_per_dim;
    uint16_t nstripes = ndims / stripe_sz + ((ndims % stripe_sz) > 0);
    uint32_t padded_ndims = round_up_to_multiple(ndims, vector_sz);
    uint16_t nvectors = padded_ndims / vector_sz + ((padded_ndims % vector_sz) > 0);

    // stats for sizing temp storage
    uint32_t nstripes_in_group = nstripes * group_sz_blocks;
    uint32_t group_header_sz = round_up_to_multiple(
        nstripes_in_group * stripe_sz, vector_sz);
    uint32_t nstripes_in_vectors = group_header_sz / stripe_sz;
    uint16_t nvectors_in_group = group_header_sz / vector_sz;

    // ------------------------ temp storage
    // allocate temp vars of minimal possible size such that we can
    // do vector loads and stores (except bitwidths, which are u64s so
    // that we can store directly after sad_epu8)
    uint64_t* headers_tmp       = (uint64_t*)calloc(nheader_stripes, 8);
    uint8_t*  headers           = (uint8_t*) calloc(1, group_header_sz);
    uint64_t* data_masks        = (uint64_t*)calloc(nstripes_in_vectors, 8);
    uint64_t* stripe_bitwidths  = (uint64_t*)calloc(nstripes_in_vectors, 8);
    uint32_t* stripe_bitoffsets = (uint32_t*)calloc(nstripes, 4);

    // extra row in deltas is to store last decoded values
    // TODO just special case very first row
    uint8_t* deltas = (uint8_t*)calloc((block_sz + 1) * padded_ndims, 1);

    // printf("nvectors, padded_ndims = %d, %d\n", nvectors, padded_ndims);

    // ================================ main loop

    // uint64_t ngroups = orig_len / group_sz; // if we get an fp error, it's this
    for (uint64_t g = 0; g < ngroups; g++) {
        const uint8_t* header_src = (uint8_t*)src;
        src += total_header_bytes;

        // printf("==== group %d\n", (int)g);

        // ------------------------ create unpacked headers array
        // unpack headers for all the blocks; note that this is tricky
        // because we need to zero-pad final stripe's headers in each block
        // so that stripe widths don't get messed up (from initial data in
        // next block's header)
        uint64_t* header_write_ptr = (uint64_t*)headers_tmp;
        for (uint32_t stripe = 0; stripe < nheader_stripes - 1; stripe++) {
            uint64_t packed_header = *(uint32_t*)header_src;
            header_src += stripe_header_sz;
            uint64_t header = _pdep_u64(packed_header, kHeaderUnpackMask);
            *header_write_ptr = header;
            header_write_ptr++;
        }
        // unpack header for the last stripe in the last block
        uint64_t packed_header = (*(uint32_t*)header_src) & final_header_mask;
        uint64_t header = _pdep_u64(packed_header, kHeaderUnpackMask);
        *header_write_ptr = header;

        // insert zeros between the unpacked headers so that the stripe
        // bitwidths, etc, are easy to compute
        uint8_t* header_in_ptr = (uint8_t*)headers_tmp;
        for (uint32_t b = 0; b < group_sz_blocks; b++) {
            uint32_t src_offset = b * ndims;
            uint32_t dest_offset = b * nstripes * stripe_sz;
            memcpy(headers + dest_offset, header_in_ptr + src_offset, ndims);
        }

        // // printf("header_tmp:    "); dump_bytes(headers_tmp, nheader_stripes * 8);
        // if (dump_group) { printf("padded headers:"); dump_bytes(headers, nstripes_in_vectors * 8); }
        // printf("padded headers:"); dump_bytes(headers, nstripes_in_vectors * 8);

        // ------------------------ masks and bitwidths for all stripes
        for (uint32_t v = 0; v < nvectors_in_group; v++) {
            __m256i raw_header = _mm256_loadu_si256(
                (const __m256i*)(headers + v * vector_sz));
            // map nbits of 7 to 8
            static const __m256i sevens = _mm256_set1_epi8(0x07);
            __m256i header = _mm256_sub_epi8(
                raw_header, _mm256_cmpeq_epi8(raw_header, sevens));

            // compute and store bit widths
            __m256i bitwidths = _mm256_sad_epu8(
                header, _mm256_setzero_si256());
            uint8_t* store_addr = ((uint8_t*)stripe_bitwidths) + v * vector_sz;
            _mm256_storeu_si256((__m256i*)store_addr, bitwidths);

            // compute and store masks
            __m256i masks = _mm256_shuffle_epi8(nbits_to_mask_8b, raw_header);
            uint8_t* store_addr2 = ((uint8_t*)data_masks) + v * vector_sz;
            _mm256_storeu_si256((__m256i*)store_addr2, masks);
        }

        // printf("padded masks:     "); dump_bytes(data_masks, group_header_sz);
        // printf("padded bitwidths: "); dump_elements(stripe_bitwidths, nstripes_in_vectors);

        // ------------------------ inner loop; decompress each block
        uint64_t* masks = data_masks;
        uint64_t* bitwidths = stripe_bitwidths;
        for (int b = 0; b < group_sz_blocks; b++) { // for each block in group
            // compute where each stripe begins, as well as width of a row
            stripe_bitoffsets[0] = 0;
            for (uint32_t stripe = 1; stripe < nstripes; stripe++) {
                stripe_bitoffsets[stripe] = (uint32_t)(stripe_bitoffsets[stripe - 1]
                    + bitwidths[stripe - 1]);
            }
            uint32_t in_row_nbits = (uint32_t)(stripe_bitoffsets[nstripes - 1] +
                bitwidths[nstripes - 1]);
            uint32_t in_row_nbytes = (in_row_nbits >> 3) + ((in_row_nbits % 8) > 0);

            // if (in_row_nbits > 99999) { // TODO rm
            if (in_row_nbits == 0) {
                int8_t low_byte = (int8_t)*src;
                uint8_t high_byte = (uint8_t)*(src + 1);
                high_byte = high_byte & (low_byte >> 7); // 0 if low msb == 0
                uint16_t length = (low_byte & 0x7f) | (((uint16_t)high_byte) << 7);

                // write out the run
                if (g > 0 || b > 0) { // if not at very beginning of data
                    const uint8_t* inptr = dest - ndims;
                    uint32_t ncopies = length * block_sz;
                    memrep(dest, inptr, ndims * elem_sz, ncopies);
                    dest += ndims * ncopies;
                } else { // deltas of 0 at very start -> all zeros
                    uint32_t num_zeros = length * block_sz * ndims;
                    memset(dest, 0, num_zeros);
                    dest += num_zeros;
                }
                // printf("decompressed rle block of length %d at offset %d\n", length, (int)(dest - orig_dest));

                src++;
                src += (high_byte > 0); // if 0, wasn't used for run length

                masks += nstripes;
                bitwidths += nstripes;
                continue;
            }


            // ------------------------ unpack data for each stripe
            // for (uint32_t stripe = 0; stripe < nstripes; stripe++) {
            for (int stripe = nstripes - 1; stripe >= 0; stripe--) {
                uint32_t offset_bits = stripe_bitoffsets[stripe] & 0x07;
                uint32_t offset_bytes = stripe_bitoffsets[stripe] >> 3;

                uint64_t mask = masks[stripe];
                uint8_t nbits = bitwidths[stripe];
                uint8_t total_bits = nbits + offset_bits;

                const int8_t* inptr = src + offset_bytes;
                // uint8_t* outptr = dest + (stripe * stripe_sz);
                // uint32_t out_row_nbytes = ndims;
                uint8_t* outptr = deltas + (stripe * stripe_sz);
                uint32_t out_row_nbytes = padded_ndims;

                // printf("nbits at idx %d = %d\n", stripe + nstripes * b, nbits);
                // printf("total bits, offset bytes, bits = %u, %u, %u\n", total_bits, offset_bytes, offset_bits);
                // printf("using mask: "); dump_bytes(mask);

                // this is the hot loop
                if (total_bits <= 64) { // input guaranteed to fit in 8B
                    for (int i = 0; i < block_sz; i++) {
                        uint64_t packed_data = (*(uint64_t*)inptr) >> offset_bits;
                        // printf("packed_data "); dump_bytes(packed_data);
                        *(uint64_t*)outptr = _pdep_u64(packed_data, mask);
                        inptr += in_row_nbytes;
                        outptr += out_row_nbytes;
                    }
                } else { // input spans 9 bytes
                    // printf(">>> executing the slow path!\n");
                    uint8_t nbits_lost = total_bits - 64;
                    for (int i = 0; i < block_sz; i++) {
                        uint64_t packed_data = (*(uint64_t*)inptr) >> offset_bits;
                        // printf("packed_data "); dump_bytes(packed_data);
                        packed_data |= (*(uint64_t*)(inptr + 8)) << (nbits - nbits_lost);
                        // printf("packed_data after OR "); dump_bytes(packed_data);
                        *(uint64_t*)outptr = _pdep_u64(packed_data, mask);
                        inptr += in_row_nbytes;
                        outptr += out_row_nbytes;
                    }
                }
            } // for each stripe

            // also works when we don't actually delta code
            for (int32_t v = nvectors - 1; v >= 0; v--) {
                uint32_t vstripe_start = v * vector_sz;
                uint32_t prev_vals_offset = block_sz * padded_ndims
                    + vstripe_start;
                __m256i prev_vals = _mm256_loadu_si256((const __m256i*)
                    (deltas + prev_vals_offset));
                __m256i vals = _mm256_setzero_si256();
                for (uint8_t i = 0; i < block_sz; i++) {
                    uint32_t in_offset = i * padded_ndims + vstripe_start;
                    uint32_t out_offset = i * ndims + vstripe_start;

                    __m256i raw_vdeltas = _mm256_loadu_si256(
                        (const __m256i*)(deltas + in_offset));
                    __m256i vdeltas = mm256_zigzag_decode_epi8(raw_vdeltas);
                    vals = _mm256_add_epi8(prev_vals, vdeltas);

                    _mm256_storeu_si256((__m256i*)(dest + out_offset), vals);
                    prev_vals = vals;
                }
                _mm256_storeu_si256((__m256i*)(deltas+prev_vals_offset), vals);
            }

            src += block_sz * in_row_nbytes;
            dest += block_sz * ndims;
            masks += nstripes;
            bitwidths += nstripes;
        } // for each block
        // printf("will now write to dest at offset %lld\n", (uint64_t)(dest - orig_dest));
    } // for each group

    free(headers_tmp);
    free(headers);
    free(data_masks);
    free(stripe_bitwidths);
    free(stripe_bitoffsets);

    // printf("bytes read: %lld\n", (uint64_t)(src - orig_src));

    // uint32_t remaining_len = orig_len - (dest - orig_dest);

    // copy over trailing data
    // uint32_t remaining_len = orig_len % group_sz;
    // uint32_t remaining_len = orig_len - (src - orig_src);
    // printf("remaining len: %d\n", (int)remaining_len);
    // printf("read bytes: %lu\n", remaining_len);
    // printf("remaining data: "); ar::print(src, remaining_len);
    memcpy(dest, src, remaining_len);

    // uint32_t dest_sz = dest + remaining_len - orig_dest;
    // printf("decompressed data:\n"); dump_bytes(orig_dest, dest_sz, ndims);
    // printf("decompressed data:\n"); dump_bytes(orig_dest, dest_sz, ndims * 2, 4);
    // ar::print(orig_dest, dest_sz, "decompressed data");

    // printf("reached end of decomp\n");
    return dest + remaining_len - orig_dest;
}

int64_t decompress8b_rowmajor_delta_rle(const int8_t* src,uint8_t* dest) {
    uint16_t ndims;
    uint64_t ngroups;
    uint16_t remaining_len;
    src += read_metadata_rle_8b(src, &ndims, &ngroups, &remaining_len);
    return decompress8b_rowmajor_delta_rle(
        src, dest, ndims, ngroups, remaining_len);
}