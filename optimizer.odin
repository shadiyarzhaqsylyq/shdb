package main

import "core:fmt"

// Represent a subset of relations using a 64-bit integer mask
RelSet :: u64

Relation :: struct {
	id:          u32,
	name:        string,
	cardinality: f64,
}

Plan :: struct {
	cost:        f64,
	cardinality: f64,
	rel_set:     RelSet,

	is_join:     bool,

	// If scan node:
	rel_id:      u32,

	// If left-deep join node:
	left:        ^Plan,
	right_rel:   u32,
}

// Generate a unique lookup key for a join condition pair (unordered)
make_edge_key :: proc(r1, r2: u32) -> u64 {
	u1 := min(r1, r2)
	u2 := max(r1, r2)
	return (u64(u1) << 32) | u64(u2)
}

// Estimate join selectivity between a subset of left relations and a new right relation
get_selectivity :: proc(left_set: RelSet, right_rel: u32, selectivities: map[u64]f64) -> f64 {
	sel := 1.0
	has_predicate := false

	for i in u32(0) ..< 63 {
		if (left_set & (1 << i)) != 0 {
			key := make_edge_key(i, right_rel)
			if s, ok := selectivities[key]; ok {
				sel *= s
				has_predicate = true
			}
		}
	}

	if has_predicate {
		return sel
	}
	return 1.0 // Cross product default
}

// Count number of set bits (population count)
pop_count :: proc(mask: RelSet) -> u32 {
	count: u32 = 0
	m := mask
	for m > 0 {
		if (m & 1) == 1 {
			count += 1
		}
		m >>= 1
	}
	return count
}

// System R (Selinger) Dynamic Programming Optimization Algorithm
selinger_optimize :: proc(
	relations: []Relation,
	selectivities: map[u64]f64,
	allocator := context.allocator,
) -> ^Plan {
	n := u32(len(relations))
	if n == 0 do return nil

	// Dynamic Programming lookup table: BitSet -> Lowest Cost Plan
	dp := make(map[RelSet]^Plan, allocator = allocator)
	defer delete(dp)

	// Step 1: Base Case — Single-relation access paths (k = 1)
	for i in u32(0) ..< n {
		rel_mask := RelSet(1 << i)

		p := new(Plan, allocator)
		p.cost = relations[i].cardinality // Base scan cost model
		p.cardinality = relations[i].cardinality
		p.rel_set = rel_mask
		p.is_join = false
		p.rel_id = i

		dp[rel_mask] = p
	}

	// Step 2: Dynamic Programming — Build plans of subset size k from 2 to N
	max_mask := RelSet((1 << n) - 1)

	for size in u32(2) ..= n {
		for mask in RelSet(1) ..= max_mask {
			if pop_count(mask) != size do continue

			best_plan: ^Plan = nil

			// For Left-Deep Trees:
			// Test each relation `i` in `mask` as the right relation,
			// joining onto the optimal plan for `mask \ {i}` on the left.
			for i in u32(0) ..< n {
				if (mask & (1 << i)) == 0 do continue // Relation i not in subset

				left_mask := mask &~ (1 << i) // Bitwise AND NOT
				left_plan, exists := dp[left_mask]
				if !exists do continue

				right_rel := relations[i]

				// Estimate cardinality and cost
				sel := get_selectivity(left_mask, i, selectivities)
				out_card := left_plan.cardinality * right_rel.cardinality * sel

				// Cost model: accumulator of intermediate join cardinalities + base costs
				join_cost := left_plan.cost + out_card

				// Keep the join combination with the lowest total cost
				if best_plan == nil || join_cost < best_plan.cost {
					p := new(Plan, allocator)
					p.cost = join_cost
					p.cardinality = out_card
					p.rel_set = mask
					p.is_join = true
					p.left = left_plan
					p.right_rel = i

					best_plan = p
				}
			}

			if best_plan != nil {
				dp[mask] = best_plan
			}
		}
	}

	// Return the optimal plan covering all N relations
	return dp[max_mask]
}

// Pretty-print execution plan tree
print_plan :: proc(plan: ^Plan, relations: []Relation, depth := 0) {
	if plan == nil do return

	indent :: proc(d: int) {
		for i in 0 ..< d {
			fmt.print("  ")
		}
	}

	indent(depth)
	if !plan.is_join {
		fmt.printf("-> SCAN %s (rows: %.0f, cost: %.1f)\n",
			relations[plan.rel_id].name, plan.cardinality, plan.cost)
	} else {
		fmt.printf("-> JOIN (est. rows: %.0f, total cost: %.1f)\n",
			plan.cardinality, plan.cost)

		// Left child (Subtree)
		print_plan(plan.left, relations, depth + 1)

		// Right child (Relation Scan)
		indent(depth + 1)
		fmt.printf("-> SCAN %s (rows: %.0f)\n",
			relations[plan.right_rel].name, relations[plan.right_rel].cardinality)
	}
}

main :: proc() {
	// Define base relations with cardinalities
	relations := []Relation{
		{id = 0, name = "R1",     cardinality = 50},
		{id = 1, name = "R2",    cardinality = 100},
		{id = 2, name = "R3", cardinality = 30},
		{id = 3, name = "R4",  cardinality = 40},
	}

	// Define join predicate selectivities between relation pairs
	selectivities := make(map[u64]f64)
	defer delete(selectivities)

	// Users <-> Orders
	selectivities[make_edge_key(0, 1)] = 0.001
	// Orders <-> LineItems
	selectivities[make_edge_key(1, 2)] = 0.0001
	// LineItems <-> Products
	selectivities[make_edge_key(2, 3)] = 0.0005

	fmt.println("--- Running System R (Selinger) Join Optimizer ---")
	best_plan := selinger_optimize(relations, selectivities)

	if best_plan != nil {
		fmt.println("\nOptimal Join Plan Tree:")
		print_plan(best_plan, relations)
	} else {
		fmt.println("Failed to construct query plan.")
	}
}
