//DPhyp
package main

import "core:fmt"
import "core:math/bits"

MAX_RELATIONS :: 64
MAX_EDGES     :: 128

Node_Set :: u64

/* --- Query Graph & Plan Structures --- */

Hyper_Edge :: struct {
	u:           Node_Set, // Hypernode u
	v:           Node_Set, // Hypernode v (u and v are disjoint)
	selectivity: f64,      // Predicate selectivity
}

Hyper_Graph :: struct {
	num_nodes:          int,
	node_names:         [MAX_RELATIONS]string,
	base_cardinalities: [MAX_RELATIONS]f64,
	edges:              [MAX_EDGES]Hyper_Edge,
	num_edges:          int,
}

Plan :: struct {
	relations:   Node_Set, // Set of relations joined in this plan
	cost:        f64,      // Plan cost
	cardinality: f64,      // Estimated output cardinality
	left:        ^Plan,    // Left subplan
	right:       ^Plan,    // Right subplan
}

// DP Table maps node sets to optimal plans
DP_Table :: map[Node_Set]^Plan

/* --- Helper Bit Utilities --- */

// Lowest index bit (min(S) in paper)
get_min_node :: proc(s: Node_Set) -> int {
	return int(bits.count_trailing_zeros(s))
}

// B_v = {w | w <= v}, where node ordering is 0 < 1 < ... < n-1
get_B :: proc(node_idx: int) -> Node_Set {
	if node_idx >= 63 do return ~Node_Set(0)
	return (Node_Set(1) << u32(node_idx + 1)) - 1
}

/* Checks if there is a hyperedge connecting S1 and S2 */
is_connected :: proc(g: ^Hyper_Graph, S1, S2: Node_Set) -> bool {
	for i in 0 ..< g.num_edges {
		u := g.edges[i].u
		v := g.edges[i].v
		if ((u & S1) == u && (v & S2) == v) || ((v & S1) == v && (u & S2) == u) {
			return true
		}
	}
	return false
}

/* Computes N(S, X): the neighborhood of S excluding X (Eq. 1 in paper) */
calc_neighborhood :: proc(g: ^Hyper_Graph, S, X: Node_Set) -> Node_Set {
	candidates: [MAX_EDGES * 2]Node_Set
	cand_count := 0

	// Collect all interesting target hypernodes
	for i in 0 ..< g.num_edges {
		u := g.edges[i].u
		v := g.edges[i].v

		if (u & S) == u && (v & (S | X)) == 0 {
			candidates[cand_count] = v
			cand_count += 1
		}
		if (v & S) == v && (u & (S | X)) == 0 {
			candidates[cand_count] = u
			cand_count += 1
		}
	}

	// Eliminate subsumed hypernodes (E_down(S, X))
	N: Node_Set = 0
	for i in 0 ..< cand_count {
		subsumed := false
		for j in 0 ..< cand_count {
			if i != j && (candidates[j] & candidates[i]) == candidates[j] && candidates[j] != candidates[i] {
				subsumed = true
				break
			}
		}
		if !subsumed {
			// Add min(v) to neighborhood
			min_elem := get_min_node(candidates[i])
			N |= (Node_Set(1) << u32(min_elem))
		}
	}
	return N
}

/* --- DPhyp Context & Subroutines --- */

DPhyp :: struct {
	g:        ^Hyper_Graph,
	dp_table: DP_Table,
}

/* Section 3.5: emit_csg_cmp joins plans for S1 and S2 */
emit_csg_cmp :: proc(ctx: ^DPhyp, S1, S2: Node_Set) {
	p1 := ctx.dp_table[S1] or_else nil
	p2 := ctx.dp_table[S2] or_else nil
	if p1 == nil || p2 == nil do return

	S := S1 | S2

	// Calculate selectivity across all connecting hyperedges
	sel: f64 = 1.0
	for i in 0 ..< ctx.g.num_edges {
		u := ctx.g.edges[i].u
		v := ctx.g.edges[i].v
		if ((u & S1) == u && (v & S2) == v) || ((v & S1) == v && (u & S2) == u) {
			sel *= ctx.g.edges[i].selectivity
		}
	}

	card := p1.cardinality * p2.cardinality * sel
	// Standard C_out cost model
	cost := p1.cost + p2.cost + card

	existing := ctx.dp_table[S] or_else nil
	if existing == nil || cost < existing.cost {
		new_plan := new(Plan)
		new_plan.relations = S
		new_plan.cardinality = card
		new_plan.cost = cost
		new_plan.left = p1
		new_plan.right = p2
		ctx.dp_table[S] = new_plan
	}
}

/* Section 3.4: enumerate_cmp_rec */
enumerate_cmp_rec :: proc(ctx: ^DPhyp, S1, S2, X_in: Node_Set) {
	X := X_in
	N_set := calc_neighborhood(ctx.g, S2, X)

	// Vance & Maier subset enumeration in ascending order
	N := (0 - N_set) & N_set
	for N != 0 {
		if (S2 | N) in ctx.dp_table && is_connected(ctx.g, S1, S2 | N) {
			emit_csg_cmp(ctx, S1, S2 | N)
		}
		N = (N - N_set) & N_set
	}

	X |= N_set

	N = (0 - N_set) & N_set
	for N != 0 {
		enumerate_cmp_rec(ctx, S1, S2 | N, X)
		N = (N - N_set) & N_set
	}
}

/* Section 3.3: emit_csg */
emit_csg :: proc(ctx: ^DPhyp, S1: Node_Set) {
	min_s1 := get_min_node(S1)
	X := S1 | get_B(min_s1)
	N_set := calc_neighborhood(ctx.g, S1, X)

	// Iterate over v in N descending
	temp := N_set
	for temp > 0 {
		v_idx := 63 - int(bits.count_leading_zeros(temp))
		v := Node_Set(1) << u32(v_idx)
		temp &= ~v

		S2 := v
		if is_connected(ctx.g, S1, S2) {
			emit_csg_cmp(ctx, S1, S2)
		}
		// Exclude nodes <= v in N to avoid duplicates
		B_v_N := N_set & get_B(v_idx)
		enumerate_cmp_rec(ctx, S1, S2, X | B_v_N)
	}
}

/* Section 3.2: enumerate_csg_rec */
enumerate_csg_rec :: proc(ctx: ^DPhyp, S1, X: Node_Set) {
	N_set := calc_neighborhood(ctx.g, S1, X)

	// First loop: emit connected subgraphs
	N := (0 - N_set) & N_set
	for N != 0 {
		if (S1 | N) in ctx.dp_table {
			emit_csg(ctx, S1 | N)
		}
		N = (N - N_set) & N_set
	}

	// Second loop: recursive expansion
	N = (0 - N_set) & N_set
	for N != 0 {
		enumerate_csg_rec(ctx, S1 | N, X | N_set)
		N = (N - N_set) & N_set
	}
}

/* Section 3.1: solve */
solve :: proc(ctx: ^DPhyp) -> ^Plan {
	n := ctx.g.num_nodes
	ctx.dp_table = make(DP_Table)

	// Initialize dp_table with single relations
	for i in 0 ..< n {
		p := new(Plan)
		p.relations = Node_Set(1) << u32(i)
		p.cardinality = ctx.g.base_cardinalities[i]
		p.cost = 0.0
		p.left = nil
		p.right = nil
		ctx.dp_table[p.relations] = p
	}

	// Process nodes descending according to <
	for i := n - 1; i >= 0; i -= 1 {
		v := Node_Set(1) << u32(i)
		emit_csg(ctx, v)
		enumerate_csg_rec(ctx, v, get_B(i))
	}

	all_nodes := ~Node_Set(0) if n == 64 else (Node_Set(1) << u32(n)) - 1
	return ctx.dp_table[all_nodes] or_else nil
}

/* --- Pretty Printing of Resulting Plan --- */

print_plan :: proc(g: ^Hyper_Graph, p: ^Plan) {
	if p == nil do return
	if p.left == nil && p.right == nil {
		idx := get_min_node(p.relations)
		fmt.printf("%s", g.node_names[idx])
		return
	}
	fmt.printf("(")
	print_plan(g, p.left)
	fmt.printf(" ⨝ ")
	print_plan(g, p.right)
	fmt.printf(")")
}

/* --- Example: Hypergraph from Figure 2 in the paper --- */

add_edge :: proc(g: ^Hyper_Graph, u_mask, v_mask: Node_Set, sel: f64) {
	g.edges[g.num_edges].u = u_mask
	g.edges[g.num_edges].v = v_mask
	g.edges[g.num_edges].selectivity = sel
	g.num_edges += 1
}

main :: proc() {
	g: Hyper_Graph
	g.num_nodes = 6

	names := [6]string{"R1", "R2", "R3", "R4", "R5", "R6"}
	for i in 0 ..< 6 {
		g.node_names[i] = names[i]
		g.base_cardinalities[i] = 1000.0 * f64(i + 1) // 1K, 2K, ..., 6K tuples
	}

	g.num_edges = 0

	// Simple edges: ({R1}, {R2}), ({R2}, {R3}), ({R4}, {R5}), ({R5}, {R6})
	add_edge(&g, 1 << 0, 1 << 1, 0.01)
	add_edge(&g, 1 << 1, 1 << 2, 0.01)
	add_edge(&g, 1 << 3, 1 << 4, 0.01)
	add_edge(&g, 1 << 4, 1 << 5, 0.01)

	// Hyperedge: ({R1, R2, R3}, {R4, R5, R6})
	// Corresponding to: R1.a + R2.b + R3.c = R4.d + R5.e + R6.f
	u_hyper := Node_Set((1 << 0) | (1 << 1) | (1 << 2))
	v_hyper := Node_Set((1 << 3) | (1 << 4) | (1 << 5))
	add_edge(&g, u_hyper, v_hyper, 0.005)

	ctx: DPhyp
	ctx.g = &g

	fmt.println("Executing DPhyp for Figure 2 query hypergraph...")
	best_plan := solve(&ctx)
	defer delete(ctx.dp_table)

	if best_plan != nil {
		fmt.printf("\nOptimal Join Plan:\n  ")
		print_plan(&g, best_plan)
		fmt.printf("\nEstimated Cost:        %.2f\n", best_plan.cost)
		fmt.printf("Estimated Cardinality: %.2f\n", best_plan.cardinality)
	} else {
		fmt.println("No plan found (query hypergraph is disconnected).")
	}
}
/*
output
Optimal Join Plan:
  (((R1 ⨝ R2) ⨝ R3) ⨝ ((R4 ⨝ R5) ⨝ R6))
Estimated Cost:        36012820000.00
Estimated Cardinality: 36000000000.00


*/
