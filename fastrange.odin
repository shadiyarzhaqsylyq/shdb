package main

import "core:fmt"

// 1. Hash function (scrambles the key into a 64-bit integer)
mm3 :: proc(input_id: u64) -> u64 {
    hash := input_id
    hash = (hash ~ (hash >> 33)) * 0xff51afd7ed558ccd
    hash = (hash ~ (hash >> 33)) * 0xc4ceb9fe1a85ec53
    hash = hash ~ (hash >> 33)
    return hash
}

// 2. Sizing function (maps ANY 64-bit hash into 0..capacity-1)
get_bucket_index :: proc(hash: u64, capacity: u64) -> u64 {
    product := u128(hash) * u128(capacity)
    return u64(product >> 64)
}

main :: proc() {
    CAPACITY: u64 = 50_000

    // Imagine these are keys from your database table:
    user_ids := []u64{1, 2, 3, 100_000, 999_999}

    for id in user_ids {
        // Step 1: Compute hash (this produces a pseudo-random u64 like 0xabcdef...)
        hash_value := mm3(id)

        // Step 2: Map it to a bucket index in your table
        bucket := get_bucket_index(hash_value, CAPACITY)

        fmt.printf("User ID: % -7d -> Hash: 0x%016x -> Bucket: %d\n", id, hash_value, bucket)
    }
}

/*

User ID: 1       -> Hash: 0x4be14b0b14436577 -> Bucket: 14818
User ID: 2       -> Hash: 0xa9b1fa9f12ab5641 -> Bucket: 33144
User ID: 3       -> Hash: 0x054d5b27376c7ceb -> Bucket: 1039
User ID: 100000  -> Hash: 0xc4bc82cb05ff3a21 -> Bucket: 38459
User ID: 999999  -> Hash: 0x762ef4f141443d3b -> Bucket: 23094


*/
