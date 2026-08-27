package main
//tables < 16
import "core:fmt"

Relation :: enum u8 {
	R1,
	R2,
	R3,
	R4,
	R5,
}

// 64-bit backed set
Relations :: bit_set[Relation; u64]

BestPlan :: struct {
	cost:  int,
	valid: bool,
}

// Total distinct subset states: 2^N (2^5 = 32 states)
N :: len(Relation)
NUM_STATES :: 1 << N

main :: proc() {
	// 1. Allocate DP memoization table for all 2^N states
	memo := make([]BestPlan, NUM_STATES)
	defer delete(memo)

	// Base costs assigned per relation
	costs := [Relation]int{
		.R1 = 10,
		.R2 = 15,
		.R3 = 20,
		.R4 = 25,
		.R5 = 30,
	}

	// 2. Populate DP memo table by iterating through all bitmask states
	for mask in 0 ..< u64(NUM_STATES) {
		set := transmute(Relations)mask

		total_cost := 0
		for r in set { // Odin natively iterates active set elements
			total_cost += costs[r]
		}

		// Direct zero-cost transmute indexing into memo slice
		idx := transmute(u64)set
		memo[idx] = BestPlan{
			cost  = total_cost,
			valid = true,
		}
	}

	// 3. Look up state for a specific set: {.R1, .R3, .R5}
	query_set: Relations = {.R1, .R3, .R5}
	query_idx := transmute(u64)query_set

	result := memo[query_idx]

	fmt.printf("Set:             %v\n", query_set)
	fmt.printf("Slice Index:     %d\n", query_idx)  // Bitmask: 1 + 4 + 16 = 21
	fmt.printf("Retrieved Cost:  %d\n", result.cost) // 10 + 20 + 30 = 60
}
