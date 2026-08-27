package main

import "core:fmt"

Relation :: enum u8 { R1, R2, R3, R4, R5 }
Relations :: bit_set[Relation; u64]

BestPlan :: struct { cost: int }

main :: proc() {
    set1: Relations = {.R1, .R3, .R4}
    raw_mask := transmute(u64)set1 // Evaluates to 13 (0b00001101)

    // --- DIRECT ARRAY INDEXING ---
    // 1. Allocate a flat array for all 2^5 (32) possible subset bitmasks
    memo: [1 << len(Relation)]BestPlan 

    // 2. Use raw_mask (13) DIRECTLY as the memory index (No hash map needed!)
    memo[raw_mask] = BestPlan{cost = 45}

    // 3. Instant O(1) lookup: CPU jumps directly to byte offset (memo_ptr + 13 * size)
    retrieved := memo[raw_mask]
    fmt.println("Cost for set1:", retrieved.cost) // Output: 45
}
