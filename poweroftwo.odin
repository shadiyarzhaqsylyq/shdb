package main

import "core:fmt"

// Step 1: Hash Function (STILL REQUIRED)
mm3 :: proc(input_id: u64) -> u64 {
    hash := input_id
    hash = (hash ~ (hash >> 33)) * 0xff51afd7ed558ccd
    hash = (hash ~ (hash >> 33)) * 0xc4ceb9fe1a85ec53
    hash = hash ~ (hash >> 33)
    return hash
}

// Step 2: Sizing Function using POWER-OF-TWO instead of FastRange
get_bucket_index_power_of_two :: proc(hash: u64, capacity: u64) -> u64 {
    return hash & (capacity - 1) // Bitwise AND mask
}

main :: proc() {
    CAPACITY: u64 = 65_536 // MUST be a power of 2 (2^16)

    user_ids := []u64{1, 2, 3, 100_000, 999_999}

    for id in user_ids {
        hash_value := mm3(id) // Step 1
        bucket := get_bucket_index_power_of_two(hash_value, CAPACITY) // Step 2 (Power-of-Two)

        fmt.printf("User ID: % -7d -> Bucket: %d\n", id, bucket)
    }
}
