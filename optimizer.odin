
package main

import "core:fmt"
import "core:math/bits"

MAX_NODES :: 64

// Structure representing a hyperedge (u, v)
Hyper_Edge :: struct {
    u: u64,
    v: u64,
}

// Structure for the HyperGraph
Hyper_Graph :: struct {
    num_nodes: int,
    edges:     [256]Hyper_Edge,
    num_edges: int,
}

// Dynamic Programming Plan Structure
Plan :: struct {
    subset:   u64,
    cost:     f64,
    left:     u64,
    right:    u64,
    is_valid: bool,
}

// Context structure for DPhyp
DPhyp :: struct {
    g:             Hyper_Graph,
    table:         map[u64]Plan,
    base_costs:    [MAX_NODES]f64,
    cardinalities: [MAX_NODES]f64,
}

/* =========================================================================
 * Hypergraph Helpers
 * ========================================================================= */

add_hyperedge :: proc(g: ^Hyper_Graph, u, v: u64) {
    // Undirected hyperedge: store both directions
    g.edges[g.num_edges] = {u = u, v = v}
    g.num_edges += 1

    g.edges[g.num_edges] = {u = v, v = u}
    g.num_edges += 1
}

// Compute lowest set bit (min set element) using 2's complement
bitset_min :: proc(S: u64) -> u64 {
    return S & (~S + 1)
}

// Computes the neighborhood N(S, X) according to Eq. (1) in Section 2.3
get_neighborhood :: proc(g: ^Hyper_Graph, S, X: u64) -> u64 {
    candidate_hypernodes: [256]u64
    num_candidates := 0

    for i in 0 ..< g.num_edges {
        u := g.edges[i].u
        v := g.edges[i].v

        // Condition: u \subseteq S, v \cap S = \emptyset, v \cap X = \emptyset
        if (u & S) == u && (v & S) == 0 && (v & X) == 0 {
            candidate_hypernodes[num_candidates] = v
            num_candidates += 1
        }
    }

    // Eliminate subsumed hypernodes to compute E_down(S, X)
    N_S_X: u64 = 0
    for i in 0 ..< num_candidates {
        v_i := candidate_hypernodes[i]
        subsumed := false

        for j in 0 ..< num_candidates {
            if i == j { continue }
            v_j := candidate_hypernodes[j]
            // v_j is a strict subset of v_i
            if (v_j & v_i) == v_j && v_j != v_i {
                subsumed = true
                break
            }
        }

        if !subsumed {
            N_S_X |= bitset_min(v_i)
        }
    }
    return N_S_X
}

// Checks if there exists a hyperedge connecting S1 and S2
has_hyperedge_connecting :: proc(g: ^Hyper_Graph, S1, S2: u64) -> bool {
    for i in 0 ..< g.num_edges {
        u := g.edges[i].u
        v := g.edges[i].v

        if (u & S1) == u && (v & S2) == v {
            return true
        }
    }
    return false
}

/* =========================================================================
 * Core DPhyp Algorithm Functions
 * ========================================================================= */

// Simple cost model function (can be replaced with real optimizer costs)
compute_join_cost :: proc(dp: ^DPhyp, S1, S2: u64) -> f64 {
    p1 := dp.table[S1]
    p2 := dp.table[S2]
    return p1.cost + p2.cost + (p1.cost * 0.1 + p2.cost * 0.1 + 10.0)
}

// Section 3.5: Join optimal plans for S1 and S2
EmitCsgCmp :: proc(dp: ^DPhyp, S1, S2: u64) {
    p1, ok1 := dp.table[S1]
    p2, ok2 := dp.table[S2]
    if !ok1 || !ok2 { return }

    S := S1 | S2
    join_cost := compute_join_cost(dp, S1, S2)

    existing, exists := dp.table[S]
    if !exists || join_cost < existing.cost {
        dp.table[S] = Plan{
            subset   = S,
            cost     = join_cost,
            left     = S1,
            right    = S2,
            is_valid = true,
        }
    }
}

// Section 3.4: Recursively extend complement S2
EnumerateCmpRec :: proc(dp: ^DPhyp, S1, S2, X: u64) {
    N := get_neighborhood(&dp.g, S2, X)

    // Subset enumeration over N
    subN := N
    for subN > 0 {
        S2_N := S2 | subN
        if (S2_N in dp.table) && has_hyperedge_connecting(&dp.g, S1, S2_N) {
            EmitCsgCmp(dp, S1, S2_N)
        }
        subN = (subN - 1) & N
    }

    X_new := X | N
    subN = N
    for subN > 0 {
        S2_N := S2 | subN
        EnumerateCmpRec(dp, S1, S2_N, X_new)
        subN = (subN - 1) & N
    }
}

// Section 3.3: Seeds complements for connected subgraph S1
EmitCsg :: proc(dp: ^DPhyp, S1: u64) {
    min_s1_idx := bits.count_trailing_zeros(S1)
    B_min_s1 := (u64(1) << u32(min_s1_idx + 1)) - 1
    X := S1 | B_min_s1

    N := get_neighborhood(&dp.g, S1, X)

    // Collect elements of N to iterate in descending order
    nodes: [MAX_NODES]int
    count := 0
    for i in 0 ..< dp.g.num_nodes {
        if ((N >> u32(i)) & 1) != 0 {
            nodes[count] = i
            count += 1
        }
    }

    // Process nodes descending according to ordering
    for i := count - 1; i >= 0; i -= 1 {
        v := u64(1) << u32(nodes[i])
        S2 := v
        if has_hyperedge_connecting(&dp.g, S1, S2) {
            EmitCsgCmp(dp, S1, S2)
        }
        EnumerateCmpRec(dp, S1, S2, X)
    }
}

// Section 3.2: Extend connected subgraph S1
EnumerateCsgRec :: proc(dp: ^DPhyp, S1, X: u64) {
    N := get_neighborhood(&dp.g, S1, X)

    subN := N
    for subN > 0 {
        S1_N := S1 | subN
        if S1_N in dp.table {
            EmitCsg(dp, S1_N)
        }
        subN = (subN - 1) & N
    }

    subN = N
    for subN > 0 {
        S1_N := S1 | subN
        EnumerateCsgRec(dp, S1_N, X | N)
        subN = (subN - 1) & N
    }
}

// Section 3.1: Top-level Solve procedure
Solve :: proc(dp: ^DPhyp) -> ^Plan {
    // 1. Initialize DP table for single relations
    for i in 0 ..< dp.g.num_nodes {
        v := u64(1) << u32(i)
        dp.table[v] = Plan{
            subset   = v,
            cost     = dp.base_costs[i],
            left     = 0,
            right    = 0,
            is_valid = true,
        }
    }

    // 2. Expand sets starting from singleton sets in descending order
    for i := dp.g.num_nodes - 1; i >= 0; i -= 1 {
        v := u64(1) << u32(i)
        EmitCsg(dp, v)

        B_v := (u64(1) << u32(i + 1)) - 1
        EnumerateCsgRec(dp, v, B_v)
    }

    // Return plan for full query graph (all nodes set)
    all_nodes := (u64(1) << u32(dp.g.num_nodes)) - 1
    if plan, ok := &dp.table[all_nodes]; ok {
        return plan
    }
    return nil
}

/* =========================================================================
 * Helper Utility to Print Optimal Join Tree
 * ========================================================================= */

print_plan :: proc(dp: ^DPhyp, subset: u64, depth: int) {
    p, ok := dp.table[subset]
    if !ok { return }

    for _ in 0 ..< depth {
        fmt.print("  ")
    }

    if p.left == 0 && p.right == 0 {
        node_idx := bits.count_trailing_zeros(p.subset)
        fmt.printf("- Relation R%d (Cost: %.2f)\n", node_idx + 1, p.cost)
    } else {
        fmt.printf("+ Join (Total Cost: %.2f)\n", p.cost)
        print_plan(dp, p.left, depth + 1)
        print_plan(dp, p.right, depth + 1)
    }
}



main :: proc() {
    dp: DPhyp
    dp.g.num_nodes = 6
    dp.g.num_edges = 0
    dp.table = make(map[u64]Plan)
    defer delete(dp.table)

    // Base costs for relations R1 to R6
    for i in 0 ..< 6 {
        dp.base_costs[i] = 10.0 * f64(i + 1)
    }

    // Bitsets for single relations R1..R6
    R1: u64 = 1 << 0
    R2: u64 = 1 << 1
    R3: u64 = 1 << 2
    R4: u64 = 1 << 3
    R5: u64 = 1 << 4
    R6: u64 = 1 << 5

    // Simple edges from Figure 2:
    // ({R1}, {R2}), ({R2}, {R3}), ({R4}, {R5}), ({R5}, {R6})
    add_hyperedge(&dp.g, R1, R2)
    add_hyperedge(&dp.g, R2, R3)
    add_hyperedge(&dp.g, R4, R5)
    add_hyperedge(&dp.g, R5, R6)

    // Hyperedge from Figure 2:
    // ({R1, R2, R3}, {R4, R5, R6})
    hypernode_left := R1 | R2 | R3
    hypernode_right := R4 | R5 | R6
    add_hyperedge(&dp.g, hypernode_left, hypernode_right)

    // Execute DPhyp
    best_plan := Solve(&dp)

    if best_plan != nil {
        fmt.println("Optimal Plan Found!")
        print_plan(&dp, best_plan.subset, 0)
    } else {
        fmt.println("No valid plan found for the graph.")
    }
}
/*
output:
Optimal Plan Found!
+ Join (Total Cost: 324.82)
  + Join (Total Cost: 90.30)
    + Join (Total Cost: 43.00)
      - Relation R1 (Cost: 10.00)
      - Relation R2 (Cost: 20.00)
    - Relation R3 (Cost: 30.00)
  + Join (Total Cost: 195.90)
    + Join (Total Cost: 109.00)
      - Relation R4 (Cost: 40.00)
      - Relation R5 (Cost: 50.00)
    - Relation R6 (Cost: 60.00)




*/
