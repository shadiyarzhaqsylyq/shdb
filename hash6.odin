package main

import "core:fmt"
import "core:mem"
import "core:hash/xxhash"

// The universal bucket indexer (from poweroftwo.odin)
get_bucket_index_power_of_two :: proc(hash: u64, capacity: u64) -> u64 {
    return hash & (capacity - 1)
}

// -------------------------------------------------------------
// CASE 1: Single Integer Column (using mm3)
// -------------------------------------------------------------
hash_single_int :: proc(id: u64) -> u64 {
    // mm3 bit-mixer
    h := id
    h = (h ~ (h >> 33)) * 0xff51afd7ed558ccd
    h = (h ~ (h >> 33)) * 0xc4ceb9fe1a85ec53
    return h ~ (h >> 33)
}

// -------------------------------------------------------------
// CASE 2: Composite Fixed Columns (using Packed Struct + One-Shot)
// -------------------------------------------------------------
Fixed_Key :: struct #packed {
    tenant_id: u32,
    user_id:   u64,
}

hash_composite_fixed :: proc(key: ^Fixed_Key, seed: u64) -> u64 {
    bytes := mem.byte_slice(key, size_of(Fixed_Key))
    return xxhash.XXH64(bytes, seed) // One-shot
}

// -------------------------------------------------------------
// CASE 3: Mixed Columns (using Packed Struct + Seed Chaining)
// -------------------------------------------------------------
hash_mixed_with_string :: proc(fixed: ^Fixed_Key, dept: string, seed: u64) -> u64 {
    fixed_bytes := mem.byte_slice(fixed, size_of(Fixed_Key))
    h1 := xxhash.XXH64(fixed_bytes, seed)                     // Hash fixed part
    return xxhash.XXH64(transmute([]u8)dept, seed = h1)       // Seed-chain into string
}

main :: proc() {
    CAPACITY: u64 = 65_536 // Must be power of 2
    query_seed: u64 = 0xdeadbeef

    // 1. Single int join
    h1 := hash_single_int(100921)
    bucket1 := get_bucket_index_power_of_two(h1, CAPACITY)

    // 2. Composite fixed join
    fixed_k := Fixed_Key{tenant_id = 1, user_id = 100921}
    h2 := hash_composite_fixed(&fixed_k, query_seed)
    bucket2 := get_bucket_index_power_of_two(h2, CAPACITY)

    // 3. Mixed key join (fixed + string)
    h3 := hash_mixed_with_string(&fixed_k, "engineering", query_seed)
    bucket3 := get_bucket_index_power_of_two(h3, CAPACITY)

    fmt.printfln("Single Int Bucket:      %d", bucket1)
    fmt.printfln("Composite Fixed Bucket: %d", bucket2)
    fmt.printfln("Mixed String Bucket:    %d", bucket3)
}
