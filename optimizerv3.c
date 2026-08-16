#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define MAX_NODES 64
typedef uint64_t BitSet;

// Structure representing a hyperedge (u, v)
typedef struct {
    BitSet u;
    BitSet v;
} HyperEdge;

// Structure for the HyperGraph
typedef struct {
    int num_nodes;
    HyperEdge edges[256];
    int num_edges;
} HyperGraph;

// Dynamic Programming Plan Structure
typedef struct {
    BitSet subset;
    double cost;
    BitSet left;
    BitSet right;
    bool is_valid;
} Plan;

// Hash Table Entry for dpTable
typedef struct {
    BitSet key;
    Plan plan;
    bool occupied;
} DPTableEntry;

// Dynamic Programming Table
typedef struct {
    DPTableEntry *entries;
    size_t capacity;
    size_t count;
} DPTable;

// Context structure for DPhyp
typedef struct {
    HyperGraph g;
    DPTable table;
    double base_costs[MAX_NODES];
    double cardinalities[MAX_NODES];
} DPhyp;

/* =========================================================================
 * Hash Table Operations for DP Table
 * ========================================================================= */

void dp_table_init(DPTable *table, size_t capacity) {
    table->capacity = capacity;
    table->count = 0;
    table->entries = (DPTableEntry *)calloc(capacity, sizeof(DPTableEntry));
}

void dp_table_free(DPTable *table) {
    free(table->entries);
}

static uint64_t hash_bitset(BitSet key) {
    // 64-bit Mix Hash Function
    key = (key ^ (key >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    key = (key ^ (key >> 27)) * UINT64_C(0x94d049bb133111eb);
    return key ^ (key >> 31);
}

Plan *dp_table_get(DPTable *table, BitSet key) {
    if (key == 0) return NULL;
    uint64_t hash = hash_bitset(key);
    size_t idx = hash % table->capacity;
    
    while (table->entries[idx].occupied) {
        if (table->entries[idx].key == key) {
            return &table->entries[idx].plan;
        }
        idx = (idx + 1) % table->capacity;
    }
    return NULL;
}

bool dp_table_contains(DPTable *table, BitSet key) {
    return dp_table_get(table, key) != NULL;
}

void dp_table_insert(DPTable *table, BitSet key, Plan plan) {
    uint64_t hash = hash_bitset(key);
    size_t idx = hash % table->capacity;

    while (table->entries[idx].occupied) {
        if (table->entries[idx].key == key) {
            table->entries[idx].plan = plan;
            return;
        }
        idx = (idx + 1) % table->capacity;
    }

    table->entries[idx].key = key;
    table->entries[idx].plan = plan;
    table->entries[idx].occupied = true;
    table->count++;
}

/* =========================================================================
 * Hypergraph Helpers
 * ========================================================================= */

void add_hyperedge(HyperGraph *g, BitSet u, BitSet v) {
    // Undirected hyperedge: store both directions
    g->edges[g->num_edges].u = u;
    g->edges[g->num_edges].v = v;
    g->num_edges++;

    g->edges[g->num_edges].u = v;
    g->edges[g->num_edges].v = u;
    g->num_edges++;
}

// Compute lowest bit (min set element)
static inline BitSet bitset_min(BitSet S) {
    return S & -S;
}

// Computes the neighborhood N(S, X) according to Eq. (1) in Section 2.3
BitSet get_neighborhood(const HyperGraph *g, BitSet S, BitSet X) {
    BitSet candidate_hypernodes[256];
    int num_candidates = 0;

    for (int i = 0; i < g->num_edges; i++) {
        BitSet u = g->edges[i].u;
        BitSet v = g->edges[i].v;

        // Condition: u \subseteq S, v \cap S = \emptyset, v \cap X = \emptyset
        if ((u & S) == u && (v & S) == 0 && (v & X) == 0) {
            candidate_hypernodes[num_candidates++] = v;
        }
    }

    // Eliminate subsumed hypernodes to compute E_down(S, X)
    BitSet N_S_X = 0;
    for (int i = 0; i < num_candidates; i++) {
        BitSet v_i = candidate_hypernodes[i];
        bool subsumed = false;

        for (int j = 0; j < num_candidates; j++) {
            if (i == j) continue;
            BitSet v_j = candidate_hypernodes[j];
            // v_j is a strict subset of v_i
            if ((v_j & v_i) == v_j && v_j != v_i) {
                subsumed = true;
                break;
            }
        }

        if (!subsumed) {
            N_S_X |= bitset_min(v_i);
        }
    }
    return N_S_X;
}

// Checks if there exists a hyperedge connecting S1 and S2
bool has_hyperedge_connecting(const HyperGraph *g, BitSet S1, BitSet S2) {
    for (int i = 0; i < g->num_edges; i++) {
        BitSet u = g->edges[i].u;
        BitSet v = g->edges[i].v;

        if ((u & S1) == u && (v & S2) == v) {
            return true;
        }
    }
    return false;
}

/* =========================================================================
 * Core DPhyp Algorithm Functions
 * ========================================================================= */

// Forward Declarations
void EnumerateCsgRec(DPhyp *dp, BitSet S1, BitSet X);
void EmitCsg(DPhyp *dp, BitSet S1);
void EnumerateCmpRec(DPhyp *dp, BitSet S1, BitSet S2, BitSet X);
void EmitCsgCmp(DPhyp *dp, BitSet S1, BitSet S2);

// Simple cost model function (can be replaced with real optimizer costs)
double compute_join_cost(DPhyp *dp, BitSet S1, BitSet S2) {
    Plan *p1 = dp_table_get(&dp->table, S1);
    Plan *p2 = dp_table_get(&dp->table, S2);
    // Simple cost = sum of child costs + combined cardinality
    return p1->cost + p2->cost + (p1->cost * 0.1 + p2->cost * 0.1 + 10.0);
}

// Section 3.5: Join optimal plans for S1 and S2
void EmitCsgCmp(DPhyp *dp, BitSet S1, BitSet S2) {
    Plan *plan1 = dp_table_get(&dp->table, S1);
    Plan *plan2 = dp_table_get(&dp->table, S2);
    if (!plan1 || !plan2) return;

    BitSet S = S1 | S2;
    double join_cost = compute_join_cost(dp, S1, S2);

    Plan *existing = dp_table_get(&dp->table, S);
    if (!existing || join_cost < existing->cost) {
        Plan new_plan;
        new_plan.subset = S;
        new_plan.cost = join_cost;
        new_plan.left = S1;
        new_plan.right = S2;
        new_plan.is_valid = true;
        dp_table_insert(&dp->table, S, new_plan);
    }
}

// Section 3.4: Recursively extend complement S2
void EnumerateCmpRec(DPhyp *dp, BitSet S1, BitSet S2, BitSet X) {
    BitSet N = get_neighborhood(&dp->g, S2, X);

    // Subset enumeration over N
    for (BitSet subN = N; subN > 0; subN = (subN - 1) & N) {
        BitSet S2_N = S2 | subN;
        if (dp_table_contains(&dp->table, S2_N) && has_hyperedge_connecting(&dp->g, S1, S2_N)) {
            EmitCsgCmp(dp, S1, S2_N);
        }
    }

    BitSet X_new = X | N;
    for (BitSet subN = N; subN > 0; subN = (subN - 1) & N) {
        BitSet S2_N = S2 | subN;
        EnumerateCmpRec(dp, S1, S2_N, X_new);
    }
}

// Section 3.3: Seeds complements for connected subgraph S1
void EmitCsg(DPhyp *dp, BitSet S1) {
    int min_s1_idx = __builtin_ctzll(S1);
    BitSet B_min_s1 = (1ULL << (min_s1_idx + 1)) - 1;
    BitSet X = S1 | B_min_s1;

    BitSet N = get_neighborhood(&dp->g, S1, X);

    // Collect elements of N to iterate in descending order
    int nodes[MAX_NODES];
    int count = 0;
    for (int i = 0; i < dp->g.num_nodes; i++) {
        if ((N >> i) & 1) {
            nodes[count++] = i;
        }
    }

    // Process nodes descending according to ordering
    for (int i = count - 1; i >= 0; i--) {
        BitSet v = 1ULL << nodes[i];
        BitSet S2 = v;
        if (has_hyperedge_connecting(&dp->g, S1, S2)) {
            EmitCsgCmp(dp, S1, S2);
        }
        EnumerateCmpRec(dp, S1, S2, X);
    }
}

// Section 3.2: Extend connected subgraph S1
void EnumerateCsgRec(DPhyp *dp, BitSet S1, BitSet X) {
    BitSet N = get_neighborhood(&dp->g, S1, X);

    for (BitSet subN = N; subN > 0; subN = (subN - 1) & N) {
        BitSet S1_N = S1 | subN;
        if (dp_table_contains(&dp->table, S1_N)) {
            EmitCsg(dp, S1_N);
        }
    }

    for (BitSet subN = N; subN > 0; subN = (subN - 1) & N) {
        BitSet S1_N = S1 | subN;
        EnumerateCsgRec(dp, S1_N, X | N);
    }
}

// Section 3.1: Top-level Solve procedure
Plan *Solve(DPhyp *dp) {
    // 1. Initialize DP table for single relations
    for (int i = 0; i < dp->g.num_nodes; i++) {
        BitSet v = 1ULL << i;
        Plan plan = {
            .subset = v,
            .cost = dp->base_costs[i],
            .left = 0,
            .right = 0,
            .is_valid = true
        };
        dp_table_insert(&dp->table, v, plan);
    }

    // 2. Expand sets starting from singleton sets in descending order
    for (int i = dp->g.num_nodes - 1; i >= 0; i--) {
        BitSet v = 1ULL << i;
        EmitCsg(dp, v);

        BitSet B_v = (1ULL << (i + 1)) - 1;
        EnumerateCsgRec(dp, v, B_v);
    }

    // Return plan for full query graph (all nodes set)
    BitSet all_nodes = (1ULL << dp->g.num_nodes) - 1;
    return dp_table_get(&dp->table, all_nodes);
}

/* =========================================================================
 * Helper Utility to Print Optimal Join Tree
 * ========================================================================= */

void print_plan(DPhyp *dp, BitSet subset, int depth) {
    Plan *p = dp_table_get(&dp->table, subset);
    if (!p) return;

    for (int i = 0; i < depth; i++) printf("  ");

    if (p->left == 0 && p->right == 0) {
        int node_idx = __builtin_ctzll(p->subset);
        printf("- Relation R%d (Cost: %.2f)\n", node_idx + 1, p->cost);
    } else {
        printf("+ Join (Total Cost: %.2f)\n", p->cost);
        print_plan(dp, p->left, depth + 1);
        print_plan(dp, p->right, depth + 1);
    }
}

/* =========================================================================
 * Main Program: Example from Figure 2 in the paper
 * ========================================================================= */

int main() {
    DPhyp dp;
    dp.g.num_nodes = 6;
    dp.g.num_edges = 0;

    // Base costs for relations R1 to R6
    for (int i = 0; i < 6; i++) {
        dp.base_costs[i] = 10.0 * (i + 1);
    }

    // Bitsets for single relations R1..R6
    BitSet R1 = 1ULL << 0;
    BitSet R2 = 1ULL << 1;
    BitSet R3 = 1ULL << 2;
    BitSet R4 = 1ULL << 3;
    BitSet R5 = 1ULL << 4;
    BitSet R6 = 1ULL << 5;

    // Simple edges from Figure 2:
    // ({R1}, {R2}), ({R2}, {R3}), ({R4}, {R5}), ({R5}, {R6})
    add_hyperedge(&dp.g, R1, R2);
    add_hyperedge(&dp.g, R2, R3);
    add_hyperedge(&dp.g, R4, R5);
    add_hyperedge(&dp.g, R5, R6);

    // Hyperedge from Figure 2:
    // ({R1, R2, R3}, {R4, R5, R6})
    BitSet hypernode_left = R1 | R2 | R3;
    BitSet hypernode_right = R4 | R5 | R6;
    add_hyperedge(&dp.g, hypernode_left, hypernode_right);

    // Initialize DP table
    dp_table_init(&dp.table, 1024);

    // Execute DPhyp
    Plan *best_plan = Solve(&dp);

    if (best_plan) {
        printf("Optimal Plan Found!\n");
        print_plan(&dp, best_plan->subset, 0);
    } else {
        printf("No valid plan found for the graph.\n");
    }

    dp_table_free(&dp.table);
    return 0;
}
