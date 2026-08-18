package main

import "core:fmt"
import "core:math"
import "core:math/bits"

MAX_NODES :: 64

// =========================================================================
// Logical Join Types & Physical Operators
// =========================================================================
/*
ADD

Table_Stats :: struct {
    total_rows:     u64,
    page_count:     u64,
    distinct_keys:  map[string]u64, // column_name -> distinct values count
}

*/
Logical_Join_Type :: enum {
    INNER,
    LEFT_OUTER,
    RIGHT_OUTER,
    FULL_OUTER,
}

Physical_Op :: enum {
    SCAN,
    HASH_JOIN,
    NESTED_LOOP_JOIN,
    SORT_MERGE_JOIN,
}

// Cost Model Tuning Constants
CPU_TUPLE_COST     :: 0.01  // CPU cost to process 1 tuple
HASH_BUILD_FACTOR  :: 1.2   // Overhead multiplier for building a hash table
HASH_PROBE_FACTOR  :: 1.0   // Overhead multiplier for probing a hash table
HASH_SETUP_COST    :: 5.0   // Fixed overhead for hash table allocation
SORT_TUPLE_COST    :: 0.03  // Cost multiplier for sorting tuples (N log N)
MERGE_TUPLE_COST   :: 0.008 // Cost multiplier for merging two pre-sorted streams
OUTPUT_TUPLE_COST  :: 0.02  // Cost to construct an output tuple

// Structure representing a hyperedge with selectivity & logical join type
Hyper_Edge :: struct {
    u:           u64,
    v:           u64,
    selectivity: f64,
    join_type:   Logical_Join_Type,
}

// Structure for the HyperGraph
Hyper_Graph :: struct {
    num_nodes: int,
    edges:     [256]Hyper_Edge,
    num_edges: int,
}

// Dynamic Programming Plan Structure
Plan :: struct {
    subset:       u64,
    cost:         f64,
    cardinality:  f64,
    op:           Physical_Op,
    logical_type: Logical_Join_Type,
    is_sorted:    bool, // Indicates if output stream is sorted
    left:         u64,
    right:        u64,
    is_valid:     bool,
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

add_hyperedge :: proc(g: ^Hyper_Graph, u, v: u64, selectivity: f64, join_type: Logical_Join_Type = .INNER) {
    g.edges[g.num_edges] = {u = u, v = v, selectivity = selectivity, join_type = join_type}
    g.num_edges += 1

    // Reverse direction handling for outer joins
    rev_type := join_type
    if join_type == .LEFT_OUTER {
        rev_type = .RIGHT_OUTER
    } else if join_type == .RIGHT_OUTER {
        rev_type = .LEFT_OUTER
    }

    g.edges[g.num_edges] = {u = v, v = u, selectivity = selectivity, join_type = rev_type}
    g.num_edges += 1
}

bitset_min :: proc(S: u64) -> u64 {
    return S & (~S + 1)
}

get_neighborhood :: proc(g: ^Hyper_Graph, S, X: u64) -> u64 {
    candidate_hypernodes: [256]u64
    num_candidates := 0

    for i in 0 ..< g.num_edges {
        u := g.edges[i].u
        v := g.edges[i].v

        if (u & S) == u && (v & S) == 0 && (v & X) == 0 {
            candidate_hypernodes[num_candidates] = v
            num_candidates += 1
        }
    }

    N_S_X: u64 = 0
    for i in 0 ..< num_candidates {
        v_i := candidate_hypernodes[i]
        subsumed := false

        for j in 0 ..< num_candidates {
            if i == j { continue }
            v_j := candidate_hypernodes[j]
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
 * Cardinality Estimator (Handles INNER, LEFT, RIGHT, FULL OUTER)
 * ========================================================================= */

compute_join_info :: proc(dp: ^DPhyp, S1, S2: u64) -> (cardinality: f64, join_type: Logical_Join_Type) {
    card1 := dp.table[S1].cardinality
    card2 := dp.table[S2].cardinality

    total_selectivity := 1.0
    primary_join_type := Logical_Join_Type.INNER

    for i in 0 ..< dp.g.num_edges {
        u := dp.g.edges[i].u
        v := dp.g.edges[i].v

        if (u & S1) == u && (v & S2) == v {
            total_selectivity *= dp.g.edges[i].selectivity
            primary_join_type = dp.g.edges[i].join_type
        }
    }

    inner_card := card1 * card2 * total_selectivity
    final_card := inner_card

    // Adjust cardinality based on Logical Join Type semantics
    switch primary_join_type {
    case .INNER:
        final_card = inner_card
    case .LEFT_OUTER:
        // Guarantees at least card1 rows
        final_card = max(inner_card, card1)
    case .RIGHT_OUTER:
        // Guarantees at least card2 rows
        final_card = max(inner_card, card2)
    case .FULL_OUTER:
        // Preserves unmatched rows from both sides
        final_card = max(inner_card, card1) + max(0.0, card2 - inner_card)
    }

    return final_card, primary_join_type
}

/* =========================================================================
 * Physical Operator Cost Engine (Hash Join, NLJ, Sort-Merge Join)
 * ========================================================================= */

log2_safe :: proc(x: f64) -> f64 {
    if x <= 1.0 { return 0.0 }
    return math.log2(x)
}

evaluate_best_physical_join :: proc(dp: ^DPhyp, S1, S2: u64, join_card: f64) -> (best_cost: f64, best_op: Physical_Op, is_sorted: bool) {
    p1 := dp.table[S1]
    p2 := dp.table[S2]

    base_input_cost := p1.cost + p2.cost

    // 1. Hash Join Cost
    build_card := min(p1.cardinality, p2.cardinality)
    probe_card := max(p1.cardinality, p2.cardinality)

    hash_cost := base_input_cost + HASH_SETUP_COST +
                 (build_card * CPU_TUPLE_COST * HASH_BUILD_FACTOR) +
                 (probe_card * CPU_TUPLE_COST * HASH_PROBE_FACTOR) +
                 (join_card * OUTPUT_TUPLE_COST)

    // 2. Nested Loop Join Cost
    nlj_cost := base_input_cost +
                (p1.cardinality * p2.cardinality * CPU_TUPLE_COST) +
                (join_card * OUTPUT_TUPLE_COST)

    // 3. Sort-Merge Join Cost
    // If input stream is already sorted, sort cost is 0
    sort_cost_1 := p1.is_sorted ? 0.0 : (p1.cardinality * log2_safe(p1.cardinality) * SORT_TUPLE_COST)
    sort_cost_2 := p2.is_sorted ? 0.0 : (p2.cardinality * log2_safe(p2.cardinality) * SORT_TUPLE_COST)
    merge_cost  := (p1.cardinality + p2.cardinality) * MERGE_TUPLE_COST

    smj_cost := base_input_cost + sort_cost_1 + sort_cost_2 + merge_cost + (join_card * OUTPUT_TUPLE_COST)

    // Select physical operator with lowest total cost
    if smj_cost <= hash_cost && smj_cost <= nlj_cost {
        return smj_cost, .SORT_MERGE_JOIN, true // Output of Sort-Merge Join is sorted!
    } else if hash_cost <= nlj_cost {
        return hash_cost, .HASH_JOIN, false
    } else {
        return nlj_cost, .NESTED_LOOP_JOIN, false
    }
}

/* =========================================================================
 * Core DPhyp Optimization Algorithm
 * ========================================================================= */

EmitCsgCmp :: proc(dp: ^DPhyp, S1, S2: u64) {
    p1, ok1 := dp.table[S1]
    p2, ok2 := dp.table[S2]
    if !ok1 || !ok2 { return }

    S := S1 | S2

    join_card, logical_type := compute_join_info(dp, S1, S2)
    join_cost, best_op, is_sorted := evaluate_best_physical_join(dp, S1, S2, join_card)

    existing, exists := dp.table[S]
    if !exists || join_cost < existing.cost {
        dp.table[S] = Plan{
            subset       = S,
            cost         = join_cost,
            cardinality  = join_card,
            op           = best_op,
            logical_type = logical_type,
            is_sorted    = is_sorted,
            left         = S1,
            right        = S2,
            is_valid     = true,
        }
    }
}

EnumerateCmpRec :: proc(dp: ^DPhyp, S1, S2, X: u64) {
    N := get_neighborhood(&dp.g, S2, X)

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

EmitCsg :: proc(dp: ^DPhyp, S1: u64) {
    min_s1_idx := bits.count_trailing_zeros(S1)
    B_min_s1 := (u64(1) << u32(min_s1_idx + 1)) - 1
    X := S1 | B_min_s1

    N := get_neighborhood(&dp.g, S1, X)

    nodes: [MAX_NODES]int
    count := 0
    for i in 0 ..< dp.g.num_nodes {
        if ((N >> u32(i)) & 1) != 0 {
            nodes[count] = i
            count += 1
        }
    }

    for i := count - 1; i >= 0; i -= 1 {
        v := u64(1) << u32(nodes[i])
        S2 := v
        if has_hyperedge_connecting(&dp.g, S1, S2) {
            EmitCsgCmp(dp, S1, S2)
        }
        EnumerateCmpRec(dp, S1, S2, X)
    }
}

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

Solve :: proc(dp: ^DPhyp) -> ^Plan {
    for i in 0 ..< dp.g.num_nodes {
        v := u64(1) << u32(i)
        dp.table[v] = Plan{
            subset       = v,
            cost         = dp.base_costs[i],
            cardinality  = dp.cardinalities[i],
            op           = .SCAN,
            logical_type = .INNER,
            is_sorted    = false,
            left         = 0,
            right        = 0,
            is_valid     = true,
        }
    }

    for i := dp.g.num_nodes - 1; i >= 0; i -= 1 {
        v := u64(1) << u32(i)
        EmitCsg(dp, v)

        B_v := (u64(1) << u32(i + 1)) - 1
        EnumerateCsgRec(dp, v, B_v)
    }

    all_nodes := (u64(1) << u32(dp.g.num_nodes)) - 1
    if plan, ok := &dp.table[all_nodes]; ok {
        return plan
    }
    return nil
}

/* =========================================================================
 * Helper Utility to Print Optimal Physical Plan
 * ========================================================================= */

print_plan :: proc(dp: ^DPhyp, subset: u64, depth: int) {
    p, ok := dp.table[subset]
    if !ok { return }

    for _ in 0 ..< depth {
        fmt.print("  ")
    }

    if p.op == .SCAN {
        node_idx := bits.count_trailing_zeros(p.subset)
        fmt.printf("- Scan R%d (Card: %.0f, Cost: %.2f)\n", node_idx + 1, p.cardinality, p.cost)
    } else {
        op_str: string
        switch p.op {
        case .SCAN:            op_str = "Scan"
        case .HASH_JOIN:       op_str = "Hash Join"
        case .NESTED_LOOP_JOIN: op_str = "Nested Loop Join"
        case .SORT_MERGE_JOIN:  op_str = "Sort-Merge Join"
        }

        logical_str: string
        switch p.logical_type {
        case .INNER:       logical_str = "INNER"
        case .LEFT_OUTER:  logical_str = "LEFT OUTER"
        case .RIGHT_OUTER: logical_str = "RIGHT OUTER"
        case .FULL_OUTER:  logical_str = "FULL OUTER"
        }

        sorted_tag := p.is_sorted ? " [Sorted Output]" : ""

        fmt.printf("+ %s (%s) (Card: %.0f, Total Cost: %.2f)%s\n", 
            op_str, logical_str, p.cardinality, p.cost, sorted_tag)
        print_plan(dp, p.left, depth + 1)
        print_plan(dp, p.right, depth + 1)
    }
}

// =========================================================================
// Demonstration
// =========================================================================

main :: proc() {
    dp: DPhyp
    dp.g.num_nodes = 6
    dp.g.num_edges = 0
    dp.table = make(map[u64]Plan)
    defer delete(dp.table)

    // Base Table Cardinalities (Rows) & Scan Costs
    dp.cardinalities[0] = 10.0;     dp.base_costs[0] = 10.0 * CPU_TUPLE_COST
    dp.cardinalities[1] = 100.0;    dp.base_costs[1] = 100.0 * CPU_TUPLE_COST
    dp.cardinalities[2] = 1000.0;   dp.base_costs[2] = 1000.0 * CPU_TUPLE_COST
    dp.cardinalities[3] = 500.0;    dp.base_costs[3] = 500.0 * CPU_TUPLE_COST
    dp.cardinalities[4] = 600.0;    dp.base_costs[4] = 600.0 * CPU_TUPLE_COST
    dp.cardinalities[5] = 100000.0; dp.base_costs[5] = 100000.0 * CPU_TUPLE_COST

//R1,R2,R3 and etc. are tables

    R1: u64 = 1 << 0
    R2: u64 = 1 << 1
    R3: u64 = 1 << 2
    R4: u64 = 1 << 3
    R5: u64 = 1 << 4
    R6: u64 = 1 << 5

/*

Selectivity
R1 10 rows
R2 100 rows


*/



    // Edges with Logical Join Types and Selectivities:
    // 1. R1 ⋈ R2: INNER JOIN (Selectivity 0.05)
    add_hyperedge(&dp.g, R1, R2, 0.05, .INNER)

    // 2. R2 ⋈ R3: LEFT OUTER JOIN (Selectivity 0.0001)
    //    An inner join would yield 10 rows, but Left Outer guarantees at least |R2| = 100 rows survive!
    add_hyperedge(&dp.g, R2, R3, 0.0001, .LEFT_OUTER)

    // 3. R4 ⋈ R5: Sort-Merge Candidate! R4 (500) and R5 (600) are similarly sized.
    add_hyperedge(&dp.g, R4, R5, 0.01, .INNER)

    // 4. R5 ⋈ R6: FULL OUTER JOIN (Selectivity 0.0001)
    add_hyperedge(&dp.g, R5, R6, 0.0001, .FULL_OUTER)

    // 5. Hyperedge connecting ({R1,R2,R3}, {R4,R5,R6})
    hypernode_left := R1 | R2 | R3
    hypernode_right := R4 | R5 | R6
    add_hyperedge(&dp.g, hypernode_left, hypernode_right, 0.005, .INNER)

    // Execute Optimizer
    best_plan := Solve(&dp)

    if best_plan != nil {
        fmt.println("Optimal Query Execution Plan")
        fmt.println("========================================")
        print_plan(&dp, best_plan.subset, 0)
    } else {
        fmt.println("No valid plan found for the graph.")
    }
}
