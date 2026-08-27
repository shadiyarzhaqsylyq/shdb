package main
//Robin Hood Map N > 16
import "core:fmt"

Relation :: enum u8 { R1, R2, R3, R4, R5 }
Relations :: bit_set[Relation; u64]

BestPlan :: struct {
    cost:      f64,
    join_type: string,
}

main :: proc() {
    // 1. Map directly keyed by the Relations bit_set
    // Pass initial capacity to avoid re-hash allocations during DP
    memo := make(map[Relations]BestPlan, 1024) 
    defer delete(memo)

    set_a: Relations = {.R1, .R3}
    set_b: Relations = {.R2, .R4, .R5}

    // 2. Direct key assignment (no transmute needed!)
    memo[set_a] = BestPlan{cost = 15.4, join_type = "IndexScan"}
    memo[set_b] = BestPlan{cost = 89.1, join_type = "HashJoin"}

    // 3. Direct lookup using set literals
    if plan, ok := memo[{.R1, .R3}]; ok {
        fmt.println("Sub-plan cost for {.R1, .R3}:", plan.cost)
    }
}
