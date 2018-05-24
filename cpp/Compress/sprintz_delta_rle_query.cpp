//
//  sprintz_delta_rle.cpp
//  Compress
//
//  Created by DB on 12/11/17.
//  Copyright © 2017 D Blalock. All rights reserved.
//

#include "sprintz_delta.h"
#include "sprintz_delta_rle_query.hpp"

#include "format.h"
#include "query.hpp"
#include "util.h"  // for DIV_ROUND_UP


template<bool Materialize, class IntT, class UintT>
// int64_t call_appropriate_query_func(const IntT* src, UintT* dest,
int64_t poop(const IntT* src, UintT* dest,
    uint16_t ndims, uint32_t ngroups, uint16_t remaining_len, QueryParams qp)
{
    // printf(">>>>>>>>> called poop!\n");

    // exit(1); // TODO rm

    // printf("call_appropriate_query_func: materialize? %d\n(int)", (int)Materialize);
    // volatile int foo = 0;
    // printf("================================ I can and will make you print this you compilery bastard.\n");
    // return rand()%512;  // TODO rm and uncomment below
    // return Materialize ? 7 : -7;  // TODO rm and uncomment below

    // exit(1); // TODO rm

    // ensure that the compiler doesn't optimize everything away
    #define DUMMY_READ_QUERY_RESULT(q)                              \
        do {                                                        \
            auto res = q.result();                                  \
            auto elemsz = sizeof(res[0]);                           \
            auto ptr = (uint8_t*)res.data();                        \
            volatile uint8_t max = 0;                               \
            for (int i = 0; i < res.size() * elemsz; i++) {         \
                if (ptr[i] > max) { max = ptr[i]; }                 \
            }                                                       \
        } while (0)

    // NoopQuery<UintT> qNoop(ndims);
    MaxQuery<UintT> qMax(ndims);
    SumQuery<UintT> qSum(ndims);
    NoopQuery<UintT> qNoop(ndims);
    int64_t ret = -1;

    switch (qp.op) {
    case (REDUCE_MAX):
        ret = query_rowmajor_delta_rle<Materialize>(src, dest, ndims, ngroups,
            remaining_len, qMax);
        DUMMY_READ_QUERY_RESULT(qMax);
        break;
    case (REDUCE_SUM):
        ret = query_rowmajor_delta_rle<Materialize>(src, dest, ndims, ngroups,
            remaining_len, qSum);
        DUMMY_READ_QUERY_RESULT(qSum);
        break;
    default:
        ret = query_rowmajor_delta_rle<Materialize>(src, dest, ndims, ngroups,
            remaining_len, qNoop);
        DUMMY_READ_QUERY_RESULT(qNoop);
        break;
    }

    #undef DUMMY_READ_QUERY_RESULT

    // printf("about to return ret = %lld\n", ret);


    return ret;

    return 7;
}

// template<bool Materialize, class IntT, class UintT>
// int64_t call_appropriate_query_func(const IntT* src, UintT* dest,
//     uint16_t ndims, uint32_t ngroups, uint16_t remaining_len, QueryParams qp)
// {

//     exit(1); // TODO rm

//     printf("call_appropriate_query_func: materialize? %d\n(int)", (int)Materialize);
//     // volatile int foo = 0;
//     printf("================================ I can and will make you print this you compilery bastard.\n");
//     // return rand()%512;  // TODO rm and uncomment below
//     // return Materialize ? 7 : -7;  // TODO rm and uncomment below

//     exit(1); // TODO rm

//     // ensure that the compiler doesn't optimize everything away
//     #define DUMMY_READ_QUERY_RESULT(q)                              \
//         do {                                                        \
//             auto res = q.result();                                  \
//             auto elemsz = sizeof(res[0]);                           \
//             auto ptr = (uint8_t*)res.data();                        \
//             volatile uint8_t max = 0;                               \
//             for (int i = 0; i < res.size() * elemsz; i++) {         \
//                 if (ptr[i] > max) { max = ptr[i]; }                 \
//             }                                                       \
//         } while (0)

//     // NoopQuery<UintT> qNoop(ndims);
//     MaxQuery<UintT> qMax(ndims);
//     SumQuery<UintT> qSum(ndims);
//     NoopQuery<UintT> qNoop(ndims);
//     int64_t ret = -1;

//     switch (qp.op) {
//     case (REDUCE_MAX):
//         ret = query_rowmajor_delta_rle<Materialize>(src, dest, ndims, ngroups,
//             remaining_len, qMax);
//         DUMMY_READ_QUERY_RESULT(qMax);
//         break;
//     case (REDUCE_SUM):
//         ret = query_rowmajor_delta_rle<Materialize>(src, dest, ndims, ngroups,
//             remaining_len, qSum);
//         DUMMY_READ_QUERY_RESULT(qSum);
//         break;
//     default:
//         ret = query_rowmajor_delta_rle<Materialize>(src, dest, ndims, ngroups,
//             remaining_len, qNoop);
//         DUMMY_READ_QUERY_RESULT(qNoop);
//         break;
//     }

//     #undef DUMMY_READ_QUERY_RESULT

//     printf("about to return ret = %lld\n", ret);


//     return ret;
// }

int64_t query_rowmajor_delta_rle_8b(const int8_t* src, uint8_t* dest,
                                    const QueryParams& qp)
{
    uint16_t ndims;
    uint32_t ngroups;
    uint16_t remaining_len;
    src += read_metadata_rle(src, &ndims, &ngroups, &remaining_len);
    // printf("------------------------\nqp op, qp.materialize: %d, %d\n", (int)qp.op, (int)qp.materialize);
    if (qp.materialize) {
        // printf("about to call appropriate query func; qp.materialize: %d\n", (int)qp.materialize);
        // auto ret = call_appropriate_query_func<true>(src, dest, ndims, ngroups,
        //     remaining_len, qp);
        auto ret = poop<true>(src, dest, ndims, ngroups,
            remaining_len, qp);
        // printf("got back ret = %d\n", (int)ret);
        return ret;
    } else {
//        return call_appropriate_query_func<false>(src, dest, ndims, ngroups,
//            remaining_len, qp);
        return poop<false>(src, dest, ndims, ngroups,
                           remaining_len, qp);
    }
}
int64_t query_rowmajor_delta_rle_16b(const int16_t* src, uint16_t* dest,
    const QueryParams& qp)
{
    uint16_t ndims;
    uint32_t ngroups;
    uint16_t remaining_len;
    src += read_metadata_rle(src, &ndims, &ngroups, &remaining_len);
    // NoopQuery<uint16_t> q(ndims);

    if (qp.materialize) {
//        return call_appropriate_query_func<true>(src, dest, ndims, ngroups,
//            remaining_len, qp);
        return poop<true>(src, dest, ndims, ngroups,
                                                 remaining_len, qp);
    } else {
//        return call_appropriate_query_func<false>(src, dest, ndims, ngroups,
//            remaining_len, qp);
        return poop<false>(src, dest, ndims, ngroups,
                                                  remaining_len, qp);
    }
}
