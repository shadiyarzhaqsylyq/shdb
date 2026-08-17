package main

import "core:fmt"
import "core:math"
import "core:math/bits"
import "core:slice"
import "core:strings"

// ============================================================================
// Constants & Core Data Structures
// ============================================================================

MAX_RELATIONS :: 16
MAX_EDGES     :: 128

// A hyperedge connects two sets of relations (bitmasks)
Hyper_Edge :: struct {
    left:  u64, // bitmask of relations on the left side
    right: u64, // bitmask of relations on the right side
}

// Relation metadata (catalog entry)
Relation :: struct {
    name:         string,
    cardinality:  f64,   // |R|
    base_cost:    f64,   // cost of scanning the base table
}

// Dynamic Programming entry for a subset of relations
Plan :: struct {
    subset:   u64,
    cost:     f64,
    rows:     f64,   // estimated cardinality of the intermediate result
    left:     u64,
    right:    u64,
    is_valid: bool,
}

// Full optimizer context
DPhyp :: struct {
    relations:     [MAX_RELATIONS]Relation,
    num_relations: int,

    edges:         [MAX_EDGES]Hyper_Edge,
    num_edges:     int,

    table:         map[u64]Plan, // DP table: subset → best plan
}

// ============================================================================
// Hypergraph Helpers
// ============================================================================

add_edge :: proc(dp: ^DPhyp, left, right: u64) {
    // Store both directions so the graph is treated as undirected for connectivity
    if dp.num_edges + 2 > MAX_EDGES {
        fmt.println("Too many edges")
        return
    }
    dp.edges[dp.num_edges] = {left = left, right = right}
    dp.num_edges += 1
    dp.edges[dp.num_edges] = {left = right, right = left}
    dp.num_edges += 1
}

// Lowest set bit (min element of the bitset)
bitset_min :: proc(S: u64) -> u64 {
    return S & (~S + 1)
}

// Neighborhood N(S, X) – relations that can be joined to S without touching X
get_neighborhood :: proc(dp: ^DPhyp, S, X: u64) -> u64 {
    candidates: [MAX_EDGES]u64
    num_cand := 0

    for i in 0..<dp.num_edges {
        u := dp.edges[i].left
        v := dp.edges[i].right

        // u ⊆ S, v ∩ S = ∅, v ∩ X = ∅
        if (u & S) == u && (v & S) == 0 && (v & X) == 0 {
            candidates[num_cand] = v
            num_cand += 1
        }
    }

    // Keep only non-subsumed hypernodes (E↓)
    N: u64 = 0
    for i in 0..<num_cand {
        v_i := candidates[i]
        subsumed := false
        for j in 0..<num_cand {
            if i == j do continue
            v_j := candidates[j]
            if (v_j & v_i) == v_j && v_j != v_i {
                subsumed = true
                break
            }
        }
        if !subsumed {
            N |= bitset_min(v_i)
        }
    }
    return N
}

// True if there is a hyperedge that connects S1 and S2
has_connecting_edge :: proc(dp: ^DPhyp, S1, S2: u64) -> bool {
    for i in 0..<dp.num_edges {
        u := dp.edges[i].left
        v := dp.edges[i].right
        if (u & S1) == u && (v & S2) == v {
            return true
        }
    }
    return false
}

// ============================================================================
// Cost Model (cardinality-aware)
// ============================================================================

// Very simple selectivity model: assume each join reduces rows by a constant factor.
// In a real system this would come from histograms / NDV statistics.
DEFAULT_SELECTIVITY :: 0.1

estimate_join_rows :: proc(rows1, rows2: f64) -> f64 {
    return rows1 * rows2 * DEFAULT_SELECTIVITY
}

// Cost of joining two intermediate results.
// Model: cost = cost(left) + cost(right) + rows(left)*rows(right)*factor
// This is a classic System-R style cost with a small constant overhead.
compute_join_cost :: proc(dp: ^DPhyp, S1, S2: u64) -> (cost, rows: f64) {
    p1 := dp.table[S1]
    p2 := dp.table[S2]

    join_rows := estimate_join_rows(p1.rows, p2.rows)

    // CPU cost of the join + cost of producing the inputs
    join_cost := p1.cost + p2.cost + (p1.rows * p2.rows * 0.001) + 5.0

    return join_cost, join_rows
}

// ============================================================================
// Core DPhyp Algorithm (Moerkotte & Neumann)
// ============================================================================

EmitCsgCmp :: proc(dp: ^DPhyp, S1, S2: u64) {
    p1, ok1 := dp.table[S1]
    p2, ok2 := dp.table[S2]
    if !ok1 || !ok2 do return

    S := S1 | S2
    join_cost, join_rows := compute_join_cost(dp, S1, S2)

    existing, exists := dp.table[S]
    if !exists || join_cost < existing.cost {
        dp.table[S] = Plan{
            subset   = S,
            cost     = join_cost,
            rows     = join_rows,
            left     = S1,
            right    = S2,
            is_valid = true,
        }
    }
}

EnumerateCmpRec :: proc(dp: ^DPhyp, S1, S2, X: u64) {
    N := get_neighborhood(dp, S2, X)

    // Enumerate all non-empty subsets of N
    subN := N
    for subN > 0 {
        S2_ext := S2 | subN
        if (S2_ext in dp.table) && has_connecting_edge(dp, S1, S2_ext) {
            EmitCsgCmp(dp, S1, S2_ext)
        }
        subN = (subN - 1) & N
    }

    // Recurse with the new exclusion set
    X_new := X | N
    subN = N
    for subN > 0 {
        S2_ext := S2 | subN
        EnumerateCmpRec(dp, S1, S2_ext, X_new)
        subN = (subN - 1) & N
    }
}

EmitCsg :: proc(dp: ^DPhyp, S1: u64) {
    // B↓(min(S1)) – all relations with smaller indices than the smallest in S1
    min_idx := bits.count_trailing_zeros(S1)
    B_min := (u64(1) << u32(min_idx + 1)) - 1
    X := S1 | B_min

    N := get_neighborhood(dp, S1, X)

    // Process neighborhood in descending order (important for correctness)
    nodes: [MAX_RELATIONS]int
    count := 0
    for i in 0..<dp.num_relations {
        if ((N >> u32(i)) & 1) != 0 {
            nodes[count] = i
            count += 1
        }
    }

    for i := count - 1; i >= 0; i -= 1 {
        v := u64(1) << u32(nodes[i])
        if has_connecting_edge(dp, S1, v) {
            EmitCsgCmp(dp, S1, v)
        }
        EnumerateCmpRec(dp, S1, v, X)
    }
}

EnumerateCsgRec :: proc(dp: ^DPhyp, S1, X: u64) {
    N := get_neighborhood(dp, S1, X)

    subN := N
    for subN > 0 {
        S1_ext := S1 | subN
        if S1_ext in dp.table {
            EmitCsg(dp, S1_ext)
        }
        subN = (subN - 1) & N
    }

    subN = N
    for subN > 0 {
        S1_ext := S1 | subN
        EnumerateCsgRec(dp, S1_ext, X | N)
        subN = (subN - 1) & N
    }
}

// Top-level Solve – returns pointer to the optimal plan for the full query
Solve :: proc(dp: ^DPhyp) -> ^Plan {
    // 1. Seed DP table with base relations
    for i in 0..<dp.num_relations {
        v := u64(1) << u32(i)
        dp.table[v] = Plan{
            subset   = v,
            cost     = dp.relations[i].base_cost,
            rows     = dp.relations[i].cardinality,
            left     = 0,
            right    = 0,
            is_valid = true,
        }
    }

    // 2. Enumerate connected subgraphs in the required order
    for i := dp.num_relations - 1; i >= 0; i -= 1 {
        v := u64(1) << u32(i)
        EmitCsg(dp, v)

        B_v := (u64(1) << u32(i + 1)) - 1
        EnumerateCsgRec(dp, v, B_v)
    }

    // 3. Return plan for the full set of relations
    all := (u64(1) << u32(dp.num_relations)) - 1
    if plan, ok := &dp.table[all]; ok {
        return plan
    }
    return nil
}

// ============================================================================
// Pretty-printing the optimal join tree
// ============================================================================

print_plan :: proc(dp: ^DPhyp, subset: u64, depth: int) {
    p, ok := dp.table[subset]
    if !ok do return

    indent := strings.repeat("  ", depth)
    defer delete(indent)

    if p.left == 0 && p.right == 0 {
        idx := bits.count_trailing_zeros(p.subset)
        fmt.printf("%s- %s  (rows=%.0f, cost=%.2f)\n",
            indent, dp.relations[idx].name, p.rows, p.cost)
    } else {
        fmt.printf("%s+ Join  (rows=%.0f, cost=%.2f)\n",
            indent, p.rows, p.cost)
        print_plan(dp, p.left,  depth + 1)
        print_plan(dp, p.right, depth + 1)
    }
}

// ============================================================================
// Demo / Example
// ============================================================================

main :: proc() {
    dp: DPhyp
    dp.table = make(map[u64]Plan)
    defer delete(dp.table)

    // ------------------------------------------------------------------
    // Catalog – 6 relations (typical star / chain + one hyperedge)
    // ------------------------------------------------------------------
    dp.num_relations = 6

    dp.relations[0] = {"R1 (Orders)",     100_000,  20.0}
    dp.relations[1] = {"R2 (Customers)",   50_000,  15.0}
    dp.relations[2] = {"R3 (Products)",    20_000,  10.0}
    dp.relations[3] = {"R4 (Suppliers)",   5_000,    8.0}
    dp.relations[4] = {"R5 (Nations)",     25,       2.0}
    dp.relations[5] = {"R6 (Regions)",     5,        1.0}

    // Bitmasks for convenience
    R1 :: u64(1) << 0
    R2 :: u64(1) << 1
    R3 :: u64(1) << 2
    R4 :: u64(1) << 3
    R5 :: u64(1) << 4
    R6 :: u64(1) << 5

    // ------------------------------------------------------------------
    // Query graph (same shape as Figure 2 in the original paper)
    // ------------------------------------------------------------------
    // Simple binary edges
    add_edge(&dp, R1, R2)          // Orders ⋈ Customers
    add_edge(&dp, R2, R3)          // Customers ⋈ Products
    add_edge(&dp, R4, R5)          // Suppliers ⋈ Nations
    add_edge(&dp, R5, R6)          // Nations ⋈ Regions

    // One hyperedge: (R1∪R2∪R3) ⋈ (R4∪R5∪R6)
    // This models a complex predicate that involves three relations on each side
    add_edge(&dp, R1|R2|R3, R4|R5|R6)

    // ------------------------------------------------------------------
    // Run DPhyp
    // ------------------------------------------------------------------
    fmt.println("=== DPhyp Join Order Optimizer ===")
    fmt.println("Relations:")
    for i in 0..<dp.num_relations {
        r := dp.relations[i]
        fmt.printf("  %s  |R|=%.0f  scan_cost=%.1f\n", r.name, r.cardinality, r.base_cost)
    }
    fmt.println()

    best := Solve(&dp)

    if best != nil {
        fmt.println("Optimal bushy plan found:")
        fmt.printf("Total estimated cost : %.2f\n", best.cost)
        fmt.printf("Final result rows    : %.0f\n\n", best.rows)
        print_plan(&dp, best.subset, 0)
    } else {
        fmt.println("No valid plan could be constructed.")
    }
}
