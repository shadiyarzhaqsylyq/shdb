#include <iostream>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <string>
#include <iomanip>
// Hypergraph Optimizer, Graph-based dynamic programmin
// Set of relations represented as a bitmask (up to 64 relations)
using RelationSet = uint64_t;

// Utility functions for relation sets
inline RelationSet min_set(RelationSet s) {
    if (s == 0) return 0;
    return s & -s; // Bitmask containing only the lowest set bit
}

inline int min_node(RelationSet s) {
    if (s == 0) return -1;
    return __builtin_ctzll(s); // Trailing zeros = 0-indexed position of min node
}

// B_v mask: set of all nodes <= v under node ordering <
inline RelationSet B_v(int v) {
    if (v >= 63) return ~0ULL;
    return (1ULL << (v + 1)) - 1;
}

// Structure representing a hyperedge (u, v) where u, v are disjoint relation sets
struct Hyperedge {
    RelationSet u;
    RelationSet v;
    double selectivity = 0.1; // Default join selectivity for demonstration
};

// Plan node for the dynamic programming table
struct Plan {
    RelationSet relations;
    double cost;
    double cardinality;
    std::shared_ptr<Plan> left;
    std::shared_ptr<Plan> right;
    std::string op;

    Plan(RelationSet rels, double c, double card,
         std::shared_ptr<Plan> l = nullptr, std::shared_ptr<Plan> r = nullptr,
         std::string operation = "Scan")
        : relations(rels), cost(c), cardinality(card), left(l), right(r), op(operation) {}
};

class DPhyp {
private:
    int num_relations;
    std::vector<Hyperedge> hyperedges;
    std::vector<double> base_cardinalities;
    std::unordered_map<RelationSet, std::shared_ptr<Plan>> dpTable;

    // Calculates the neighborhood N(S, X) according to Eq. (1) in the paper
    RelationSet Neighborhood(RelationSet S, RelationSet X) {
        std::vector<RelationSet> E_prime;

        // Find candidate hypernodes in E_down_0(S, X)
        for (const auto& edge : hyperedges) {
            // Direction 1: u <= S, v disjoint from S and X
            if ((edge.u & S) == edge.u && (edge.v & S) == 0 && (edge.v & X) == 0) {
                E_prime.push_back(edge.v);
            }
            // Direction 2: v <= S, u disjoint from S and X
            if ((edge.v & S) == edge.v && (edge.u & S) == 0 && (edge.u & X) == 0) {
                E_prime.push_back(edge.u);
            }
        }

        // Minimize E_prime to eliminate subsumed hypernodes -> E_down(S, X)
        std::vector<RelationSet> E_down;
        for (size_t i = 0; i < E_prime.size(); ++i) {
            bool subsumed = false;
            for (size_t j = 0; j < E_prime.size(); ++j) {
                if (i != j && (E_prime[j] & E_prime[i]) == E_prime[j] && E_prime[j] != E_prime[i]) {
                    subsumed = true;
                    break;
                }
            }
            if (!subsumed) {
                E_down.push_back(E_prime[i]);
            }
        }

        // Union of min(v) for all minimal hypernodes v
        RelationSet N = 0;
        for (auto v_hypernode : E_down) {
            N |= min_set(v_hypernode);
        }
        return N;
    }

    // Checks if there exists a hyperedge connecting S1 and S2
    bool is_connected(RelationSet S1, RelationSet S2) {
        for (const auto& edge : hyperedges) {
            if (((edge.u & S1) == edge.u && (edge.v & S2) == edge.v) ||
                ((edge.v & S1) == edge.v && (edge.u & S2) == edge.u)) {
                return true;
            }
        }
        return false;
    }

public:
    DPhyp(int n, std::vector<Hyperedge> edges, std::vector<double> cards)
        : num_relations(n), hyperedges(edges), base_cardinalities(cards) {}

    // 3.5 EmitCsgCmp: Formulates join plan for valid (S1, S2) csg-cmp pair
    void EmitCsgCmp(RelationSet S1, RelationSet S2) {
        auto it1 = dpTable.find(S1);
        auto it2 = dpTable.find(S2);
        if (it1 == dpTable.end() || it2 == dpTable.end()) return;

        auto plan1 = it1->second;
        auto plan2 = it2->second;
        RelationSet S = S1 | S2;

        // Calculate simple cost and cardinality
        double sel = 1.0;
        for (const auto& edge : hyperedges) {
            if (((edge.u & S) == edge.u) && ((edge.v & S) == edge.v) &&
                !((edge.u & S1) == edge.u && (edge.v & S1) == edge.v) &&
                !((edge.u & S2) == edge.u && (edge.v & S2) == edge.v)) {
                sel *= edge.selectivity;
            }
        }

        double card = plan1->cardinality * plan2->cardinality * sel;
        double cost = plan1->cost + plan2->cost + card;

        auto new_plan = std::make_shared<Plan>(S, cost, card, plan1, plan2, "Join");

        auto existing = dpTable.find(S);
        if (existing == dpTable.end() || cost < existing->second->cost) {
            dpTable[S] = new_plan;
        }

        // Account for commutative join option (S2 JOIN S1)
        auto new_plan_rev = std::make_shared<Plan>(S, cost, card, plan2, plan1, "Join");
        if (cost < dpTable[S]->cost) {
            dpTable[S] = new_plan_rev;
        }
    }

    // 3.4 EnumerateCmpRec: Recursively expands complement set S2
    void EnumerateCmpRec(RelationSet S1, RelationSet S2, RelationSet X) {
        RelationSet N_S2_X = Neighborhood(S2, X);

        // Iterate over all non-empty subsets N of N(S2, X)
        for (RelationSet N = N_S2_X; N > 0; N = (N - 1) & N_S2_X) {
            RelationSet S2_N = S2 | N;
            if (dpTable.count(S2_N) && is_connected(S1, S2_N)) {
                EmitCsgCmp(S1, S2_N);
            }
        }

        RelationSet X_next = X | N_S2_X;
        for (RelationSet N = N_S2_X; N > 0; N = (N - 1) & N_S2_X) {
            EnumerateCmpRec(S1, S2 | N, X_next);
        }
    }

    // 3.3 EmitCsg: Emits complement seeds for a given connected subgraph S1
    void EmitCsg(RelationSet S1) {
        int min_node_S1 = min_node(S1);
        RelationSet X = S1 | B_v(min_node_S1);
        RelationSet N = Neighborhood(S1, X);

        // Order nodes in N descending according to <
        std::vector<int> nodes;
        for (int i = num_relations - 1; i >= 0; --i) {
            if (N & (1ULL << i)) {
                nodes.push_back(i);
            }
        }

        for (int v : nodes) {
            RelationSet S2 = (1ULL << v);
            if (is_connected(S1, S2)) {
                EmitCsgCmp(S1, S2);
            }
            EnumerateCmpRec(S1, S2, X);
        }
    }

    // 3.2 EnumerateCsgRec: Recursively expands primary connected subgraph S1
    void EnumerateCsgRec(RelationSet S1, RelationSet X) {
        RelationSet N_S1_X = Neighborhood(S1, X);

        // Iterate over all non-empty subsets N of N(S1, X)
        for (RelationSet N = N_S1_X; N > 0; N = (N - 1) & N_S1_X) {
            RelationSet S1_N = S1 | N;
            if (dpTable.count(S1_N)) {
                EmitCsg(S1_N);
            }
        }

        RelationSet X_next = X | N_S1_X;
        for (RelationSet N = N_S1_X; N > 0; N = (N - 1) & N_S1_X) {
            EnumerateCsgRec(S1 | N, X_next);
        }
    }

    // 3.1 Solve: Entry point of the algorithm
    std::shared_ptr<Plan> Solve() {
        // Initialize dynamic programming table for single relations
        for (int v = 0; v < num_relations; ++v) {
            RelationSet set_v = (1ULL << v);
            dpTable[set_v] = std::make_shared<Plan>(
                set_v, 0.0, base_cardinalities[v], nullptr, nullptr, "R" + std::to_string(v + 1)
            );
        }

        // Iterate over all single relations in descending order of node ordering
        for (int v = num_relations - 1; v >= 0; --v) {
            RelationSet set_v = (1ULL << v);
            EmitCsg(set_v);
            EnumerateCsgRec(set_v, B_v(v));
        }

        RelationSet full_query = (1ULL << num_relations) - 1;
        return dpTable.count(full_query) ? dpTable[full_query] : nullptr;
    }
};

// Helper function to print execution plan tree
void print_plan(const std::shared_ptr<Plan>& plan, int indent = 0) {
    if (!plan) return;
    std::cout << std::string(indent, ' ') << "-> " << plan->op 
              << " [Cost: " << std::fixed << std::setprecision(1) << plan->cost 
              << ", Card: " << plan->cardinality << "]\n";
    if (plan->left) print_plan(plan->left, indent + 4);
    if (plan->right) print_plan(plan->right, indent + 4);
}

int main() {
    // Construct sample hypergraph from Figure 2 in the paper:
    // 6 Relations: R1, R2, R3, R4, R5, R6
    // Simple edges: (R1, R2), (R2, R3), (R4, R5), (R5, R6)
    // Hyperedge: ({R1, R2, R3}, {R4, R5, R6})
    int n = 6;
    std::vector<double> cards = {100.0, 200.0, 150.0, 300.0, 250.0, 180.0};

    // Helper macro for creating relation bitmasks (1-indexed for relations R1..R6)
    auto rel = [](std::initializer_list<int> list) {
        RelationSet set = 0;
        for (int r : list) set |= (1ULL << (r - 1));
        return set;
    };

    std::vector<Hyperedge> edges = {
        { rel({1}), rel({2}), 0.01 }, // (R1, R2)
        { rel({2}), rel({3}), 0.01 }, // (R2, R3)
        { rel({4}), rel({5}), 0.01 }, // (R4, R5)
        { rel({5}), rel({6}), 0.01 }, // (R5, R6)
        { rel({1, 2, 3}), rel({4, 5, 6}), 0.001 } // Hyperedge ({R1,R2,R3}, {R4,R5,R6})
    };

    DPhyp optimizer(n, edges, cards);
    auto best_plan = optimizer.Solve();

    if (best_plan) {
        std::cout << "Optimal Join Plan Found:\n";
        print_plan(best_plan);
    } else {
        std::cout << "No plan found.\n";
    }

    return 0;
}
