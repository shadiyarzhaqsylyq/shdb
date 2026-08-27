package main
// Table > 16
/*

memo: []BestPlan // Allocated for 2^N elements

// Slices require integer indices:
plan := memo[transmute(u64)set1]
  
*/
import "core:fmt"

Relation :: enum u8 { R1, R2, R3, R4, R5 }
Relations :: bit_set[Relation; u64]

BestPlan :: struct {
    cost:     f64,
    join_type: string,
}

main :: proc() {
    // 1. Create a Robin Hood hash table keyed by raw u64 bitmasks
    memo := make(map[u64]BestPlan)
    defer delete(memo)

    set_a: Relations = {.R1, .R3}
    set_b: Relations = {.R2, .R4, .R5}

    // Transmute set to raw u64 mask for the key
    key_a := transmute(u64)set_a
    key_b := transmute(u64)set_b

    // Store sub-plans
    memo[key_a] = BestPlan{cost = 15.4, join_type = "IndexScan"}
    memo[key_b] = BestPlan{cost = 89.1, join_type = "HashJoin"}

    // Fast O(1) lookup
    if plan, ok := memo[key_a]; ok {
        fmt.println("Sub-plan cost for {.R1, .R3}:", plan.cost)
    }
}
