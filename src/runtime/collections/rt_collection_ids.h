//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/collections/rt_collection_ids.h
// Purpose: Stable runtime class identifiers for collection heap objects.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines stable runtime class identifiers for collection objects.
///
/// These negative 64-bit tags are stored in runtime heap-object headers and
/// consumed by checked casts such as `rt_obj_is_instance()`. Each concrete
/// collection type must retain a unique value so opaque handles cannot be
/// mistaken for another layout. The identifiers therefore form part of the
/// runtime's internal object-tagging contract and must not be renumbered
/// casually.

#pragma once

#include <stdint.h>

/// Runtime class tag for Seq objects.
#define RT_SEQ_CLASS_ID INT64_C(-0x430300)
/// Runtime class tag for List objects.
#define RT_LIST_CLASS_ID INT64_C(-0x430301)
/// Runtime class tag for Set objects.
#define RT_SET_CLASS_ID INT64_C(-0x430302)
/// Runtime class tag for Stack objects.
#define RT_STACK_CLASS_ID INT64_C(-0x430303)
/// Runtime class tag for Queue objects.
#define RT_QUEUE_CLASS_ID INT64_C(-0x430304)
/// Runtime class tag for Ring objects.
#define RT_RING_CLASS_ID INT64_C(-0x430305)
/// Runtime class tag for Deque objects.
#define RT_DEQUE_CLASS_ID INT64_C(-0x430306)
/// Runtime class tag for Bag objects.
#define RT_BAG_CLASS_ID INT64_C(-0x430307)
/// Runtime class tag for OrderedMap objects.
#define RT_ORDEREDMAP_CLASS_ID INT64_C(-0x430308)
/// Runtime class tag for TreeMap objects.
#define RT_TREEMAP_CLASS_ID INT64_C(-0x430309)
/// Runtime class tag for FrozenMap objects.
#define RT_FROZENMAP_CLASS_ID INT64_C(-0x43030A)
/// Runtime class tag for FrozenSet objects.
#define RT_FROZENSET_CLASS_ID INT64_C(-0x43030B)
/// Runtime class tag for SparseArray objects.
#define RT_SPARSEARRAY_CLASS_ID INT64_C(-0x43030C)
/// Runtime class tag for IntMap objects.
#define RT_INTMAP_CLASS_ID INT64_C(-0x43030D)
/// Runtime class tag for DefaultMap objects.
#define RT_DEFAULTMAP_CLASS_ID INT64_C(-0x43030E)
/// Runtime class tag for MultiMap objects.
#define RT_MULTIMAP_CLASS_ID INT64_C(-0x43030F)
/// Runtime class tag for LruCache objects.
#define RT_LRUCACHE_CLASS_ID INT64_C(-0x430310)
/// Runtime class tag for Trie objects.
#define RT_TRIE_CLASS_ID INT64_C(-0x430311)
/// Runtime class tag for BiMap objects.
#define RT_BIMAP_CLASS_ID INT64_C(-0x430312)
/// Runtime class tag for BitSet objects.
#define RT_BITSET_CLASS_ID INT64_C(-0x430313)
/// Runtime class tag for BloomFilter objects.
#define RT_BLOOMFILTER_CLASS_ID INT64_C(-0x430314)
/// Runtime class tag for CountMap objects.
#define RT_COUNTMAP_CLASS_ID INT64_C(-0x430315)
/// Runtime class tag for priority Queue objects.
#define RT_PQUEUE_CLASS_ID INT64_C(-0x430316)
/// Runtime class tag for collection Iterator objects.
#define RT_ITERATOR_CLASS_ID INT64_C(-0x430317)
/// Runtime class tag for SortedSet objects.
#define RT_SORTEDSET_CLASS_ID INT64_C(-0x430318)
/// Runtime class tag for UnionFind objects.
#define RT_UNIONFIND_CLASS_ID INT64_C(-0x430319)
/// Runtime class tag for WeakMap objects.
#define RT_WEAKMAP_CLASS_ID INT64_C(-0x43031A)
/// Runtime class tag for Map objects.
#define RT_MAP_CLASS_ID INT64_C(-0x43031B)
/// Runtime class tag for F64Buffer objects.
#define RT_F64BUFFER_CLASS_ID INT64_C(-0x43031C)
/// Runtime class tag for I64Buffer objects.
#define RT_I64BUFFER_CLASS_ID INT64_C(-0x43031D)
/// Runtime class tag for LazySeq objects.
#define RT_LAZYSEQ_CLASS_ID INT64_C(-0x43031E)
