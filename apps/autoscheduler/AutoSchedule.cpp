/*
  This file is the core of the autoscheduler. Most of the code here is
  about navigating the search space and computing the
  featurization. This also contains the top-level interface into the
  autoscheduler. Interesting functions below are:
  TODO

  Environment variables used (directly or indirectly):

  HL_BEAM_SIZE
  Beam size to use in the beam search. Defaults to 32. Use 1 to get a greedy search instead.

  HL_CYOS
  "Choose-your-own-schedule". If set to 1, lets you navigate the search tree by hand in the terminal. Whee! This is for debugging the autoscheduler.

  HL_FEATURE_FILE -> output
  Write out a training sample for the selected schedule into this file. Needs to be augmented with the runtime using augment_sample before it can be used to train.

  HL_MACHINE_PARAMS
  An architecture description string. Used by Halide master to configure the cost model. We only use the first term. Set it to the number of cores to target.

  HL_PERMIT_FAILED_UNROLL
  Set to 1 to tell Halide not to freak out if we try to unroll a loop that doesn't have a constant extent. Should generally not be necessary, but sometimes the autoscheduler's model for what will and will not turn into a constant during lowering is inaccurate, because Halide isn't perfect at constant-folding.

  HL_SCHEDULE_FILE
  Write out a human-and-machine readable block of scheduling source code for the selected schedule into this file.

  HL_RANDOM_DROPOUT
  percent chance of accepting each state in the beam. Normalized by the number of decisions made, so 5 would be there's a 5 percent chance of never rejecting any states.

  HL_SEED
  Random seed used by the random dropout.

  HL_WEIGHTS_DIR
  When training or schedule, read weights from this directory

  HL_WEIGHTS_OUT_DIR
  When training, output updated weights here

  HL_NO_SUBTILING
  If set to 1, limits the search space to that of Mullapudi et al.

*/
#include <set>
#include <queue>
#include <algorithm>
#include <fstream>
#include <chrono>
#include <iostream>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <random>

#include "Halide.h"
#include "CostModel.h"
#include "Featurization.h"
#include "FunctionDAG.h"
#include "PerfectHashMap.h"
#include "Errors.h"
#include "NetworkSize.h"
#include "AutoSchedule.h"

namespace Halide {
namespace Internal {
namespace Autoscheduler {

// How small should an innermost loop cluster be before you just
// entirely unroll the thing. Sized for an architecture with 16 vector
// registers.
const int kUnrollLimit = 16;
const vector<int> gpu_serial_sizes({1,2,3,4,5,6,7,8});

namespace {

using std::string;
using std::vector;
using std::map;
using std::set;
using std::pair;

#define MAX_THREADS_PER_BLOCK 1024
#define TAG_MORE_LOOPS_WITH_GPU_THREADS_BLOCKS 0

struct RNG {
    std::mt19937 gen;
    std::uniform_real_distribution<double> dis;

    RNG(uint32_t seed)
        : gen{seed}
        , dis{0.0, 100.0}
    {}

    double operator()() {
        return dis(gen);
    }
};

int64_t get_shared_memory_limit() {
    // HL_SHARED_MEMORY_LIMIT is in KB
    std::string limit = get_env_variable("HL_SHARED_MEMORY_LIMIT");
    return atoi(limit.c_str()) * 1024; // Convert to bytes
}

bool compute_root_and_inline_only() {
    static bool only = get_env_variable("HL_COMPUTE_ROOT_AND_INLINE_ONLY") == "1";
    return only;
}

uint32_t get_dropout_threshold() {
    string random_dropout_str = get_env_variable("HL_RANDOM_DROPOUT");
    if (!random_dropout_str.empty()) {
        return atoi(random_dropout_str.c_str());
    } else {
        return 100;
    }
}

bool random_dropout(RNG &rng, size_t num_decisions) {
    static double random_dropout_threshold = get_dropout_threshold();
    if (random_dropout_threshold >= 100) return false;

    // The random dropout threshold is the chance that we operate
    // entirely greedily and never discard anything.
    double t = random_dropout_threshold;
    t /= 100;
    t = std::pow(t, 1.0f / num_decisions);
    t *= 100;

    double r = rng();
    bool drop_it = r >= t;
    return drop_it;
}

bool get_may_subtile() {
    string no_subtiling_str = get_env_variable("HL_NO_SUBTILING");
    if (no_subtiling_str == "1") {
        return false;
    } else {
        return true;
    }
}

bool may_subtile() {
    static bool b = get_may_subtile();
    return b;
}

// creates tilings for gpu blocks loops or gpu threads loops. 
// Innermost thread loop is always the vectorized dim and its extent is a multiple of 32. 
// Other loop extents are sized to be powers of 2 such that total extent is < 1024
// called either when we are creating parallel -> (blocks, threads) loop when computing at root
// OR when we are creating none -> (threads, SIMD) loop when computing at a serial loop 
// threads_inner = True when we're generating block tilings, False when generating thread tilings
// max_s hold max gpu_thread counts of all siblings in each dimension. Used to make sure union of
// thread counts is under 1024 threshold.
vector<vector<int64_t>> generate_gpu_tilings(const vector<int64_t> &s, const vector<int64_t> &max_s,
                                            int d, int outermost_dim, int vector_dim,
                                            bool threads_inner) {
    vector<vector<int64_t>> result;
    if (d == -1) {
        result.push_back(vector<int64_t>());
    } else {
        int factor = 2;
        vector<vector<int64_t>> v;
        v = generate_gpu_tilings(s, max_s, d - 1, outermost_dim, vector_dim, threads_inner);

        for (auto t : v) {
            t.push_back(0);
            // set max thread count 64 for now in all dims
            int64_t max_threads_extent = 64, total_threads_limit = 1024; 

            int64_t min_threads = ( (d == vector_dim) ? 32 : 1 ); 

            for (int64_t threads_ext = min_threads; threads_ext < s[d]; threads_ext *= factor) {
                // reject if inner exceeds hardware thread limit
                if (threads_ext > max_threads_extent) break;
                // reject if union of extents of this loop so far and sibling thread loops 
                // is greater than thread count limit or if there are more than three thread 
                // loops with extent > 1
                int64_t max_threads_used = (d < (int)(max_s.size())) ? std::max(threads_ext, max_s[d]) : threads_ext;

                int not_ext1 = (max_threads_used > 1) ? 1 : 0;
                for (int dim = d+1; dim < (int)(max_s.size()); dim++) {
                    max_threads_used *= max_s[dim];
                    not_ext1 += ( (max_s[dim] > 1) ? 1 : 0 );
                }
                for (int dim = 0; dim < d; dim++) {
                    int64_t blocks_at_dim, threads_at_dim;
                    if (threads_inner) {
                        blocks_at_dim = t[dim];
                        threads_at_dim = (s[dim] + blocks_at_dim - 1) / blocks_at_dim;
                    } else {
                        threads_at_dim = t[dim];
                    }
                    int64_t union_threads_used = std::max(threads_at_dim, max_s[dim]);
                    max_threads_used *= union_threads_used;
                    not_ext1 += ( (union_threads_used > 1) ? 1 : 0 );
                }
                if ( (max_threads_used > total_threads_limit) || not_ext1 > 3) break;

                if (threads_inner) {
                    int64_t other_ext = (s[d] + threads_ext - 1) / threads_ext;
                    t.back() = other_ext;
                }
                else {
                    t.back() = threads_ext;
                }

                result.push_back(t);
            }

            // The sequence above (in terms of the inner loop) goes 
            // (32 64 128 256 512 ... ) x (1 2 4 8 16 ... ) 
            // but 16 may be an important threads tiling factor 
            int64_t threads16 = 16;
            int64_t other16 = (s[d] + threads16 - 1) / threads16;
            if (threads16 < s[d] && other16 < s[d] && other16 > 1) {
                if (threads_inner)
                    t.back() = other16;
                else
                    t.back() = threads16;
                result.push_back(t);
            }            
        }
    }
    /**
    if (d == outermost_dim) {
        debug(0) << "thread block tiling loop with size: ";
        for (int i = 0; i < (int)s.size(); i++) {
            debug(0) << s[i] << ", ";
        }
    }
    **/
    return result;
}

// used for creating default serial loop tiling options inside gpu threads loop
vector<vector<int64_t>> generate_serial_tilings(const vector<int64_t> &s, int d, const vector<int> &serial_sizes) {
    vector<vector<int64_t>> result;
    if (d == -1) {
        result.push_back(vector<int64_t>());
    } else {
        vector<vector<int64_t>> v;
        v = generate_serial_tilings(s, d - 1, serial_sizes);
        for (auto t : v) {
            t.push_back(0);
            for (int inner : serial_sizes) {
                int outer = (s[d] + inner - 1) / inner;
                t.back() = outer;
                result.push_back(t);
            }
        }
    }
    debug(0) << "number of serial tilings: " << result.size() << "\n";
    return result;
}


// inner_sizes is optional vector of fixed sizes to choose from for inner loop.
// used for GPU schedules when we split a 'none' loop into a parallel loop and a serial loop
vector<vector<int64_t>> generate_tilings(const vector<int64_t> &s, int d, int factor, 
                                         bool allow_splits, const Target& target,
                                         const vector<int> &inner_sizes = vector<int>()) {
    vector<vector<int64_t>> result;
    if (d == -1) {
        result.push_back(vector<int64_t>());
    } else {
        vector<vector<int64_t>> v;
        v = generate_tilings(s, d - 1, factor, allow_splits, target);
        // If we're already generated tons of tiling configs for the
        // inner loops, search the outer loops with coarser
        // granularity.
        while (v.size() > (size_t)factor * 100) {
            factor *= 2;
        }

        for (auto t : v) {
            bool is_full = false, is_one = false;
            // Skip trivial tilings
            if ((size_t)d == s.size() - 1) {
                is_one = is_full = true;
                for (int i = 0; i < d; i++) {
                    is_one &= (t[i] == 1);
                    is_full &= (t[i] == s[i]);
                }
            }
            t.push_back(0);
            if (!allow_splits) {
                if (!is_one) {
                    t.back() = 1;
                    result.push_back(t);
                }
                if (s[d] != 1 && !is_full && is_one) {
                    t.back() = s[d];
                    result.push_back(t);
                }
            } else {
                if (!inner_sizes.empty()) { // using fixed set of inner loop extents
                    for (int inner : inner_sizes) {
                        int outer = (s[d] + inner - 1) / inner;
                        if (is_one && outer == 1) continue;
                        if (is_full && outer == s[d]) continue;
                        t.back() = outer;
                        result.push_back(t);
                    }
                } else {
                    int max_inner = 0;
                    for (int inner = 1; inner < s[d]; inner *= factor) {
                        int outer = (s[d] + inner - 1) / inner;
                        if (is_one && outer == 1) continue;
                        if (is_full && outer == s[d]) continue;
                        // Stop when we hit inner sizes that would do too much recompute
                        if (inner > 1 && inner * outer * 7 > s[d] * 8) break;
                        max_inner = inner;
                        t.back() = outer;
                        result.push_back(t);
                    }

                    for (int outer = 1; outer <= s[d]; outer *= factor) {
                        int inner = (s[d] + outer - 1) / outer;
                        if (is_one && outer == 1) continue;
                        if (is_full && outer == s[d]) continue;
                        // Stop when we get into the regime covered by the loop above.
                        if (outer > 1 && inner < max_inner * 2) break;
                        // Or when the wasted compute gets too bad.
                        if (inner * outer * 7 > s[d] * 8) break;
                        t.back() = outer;
                        result.push_back(t);
                    }

                    // The sequence above (in terms of the inner loop) goes 1 2 4 8 16 ...
                    // but 3 is an important inner tiling factor for matrix multiply ops.
                    int inner3 = 3;
                    int outer3 = (s[d] + inner3 - 1) / inner3;
                    if (factor == 2 && inner3 < s[d] && outer3 < s[d] && outer3 > 1) {
                        if (inner3 * outer3 * 7 <= s[d] * 8) {
                            t.back() = outer3;
                            result.push_back(t);
                        }
                    }
                }
            }
        }
    }
    return result;
}

template<typename T>
using NodeMap = PerfectHashMap<FunctionDAG::Node, T>;

template<typename T>
using StageMap = PerfectHashMap<FunctionDAG::Node::Stage, T>;

enum GPU_parallelism { block, thread, serial, simd, parallelized, none };

// We're going to do a tree search over possible schedules to find an
// optimal one. A tree search requires a state, and a function that
// gives you children of the state (with costs). The following struct
// represents the state, which is a partial schedule.
//
// A partial schedule is a tree. Each node is some portion of the for
// loop nest of some Func. If there are no children, it's the
// innermost set of loops. If there are children, it's a loop over
// tiles of that Func.
struct LoopNest {
    mutable RefCount ref_count;

    // The size of the outer loop, and the split factor used to create
    // the inner loop. Put another way, the number of tiles, and the
    // size of each tile. Sizes are stored from innermost dimension to outermost.
    vector<int64_t> size, split_factor;

    // The nodes inside the loop body
    vector<IntrusivePtr<const LoopNest>> children;

    // Funcs inlined into this inner loop, and the number of times they are called. Only valid if children is empty.
    NodeMap<int64_t> inlined;

    // Funcs realized inside this inner loop
    set<const FunctionDAG::Node *> store_at;

    // The total bounds required of the given Func for one
    // representative iteration of this loop. Computed lazily and
    // cached. entries are immutable so that bounds are shared across
    // different instances.
    mutable NodeMap<Bound> bounds;

    const FunctionDAG::Node *node = nullptr;
    const FunctionDAG::Node::Stage *stage = nullptr;
    int stage_idx = 0;

    // Is this the innermost loop of this func?
    bool innermost = false;

    // Are we permitted to tile this loop?
    bool tileable = false;

    // Is this the parallel outer loop?
    bool parallel = false;

    // What dimension is this Func vectorized over, in terms of the args of the Func?
    int vector_dim = -1;

    // Which loop corresponds to the innermost storage dimension and will be vectorized. -1 means none of them.
    int vectorized_loop_index = -1;

    // Apply gpu threads to this loop nest
    GPU_parallelism gpu_label = none;

    // given a newly inserted node f into this LoopNest, get union of thread counts in each dimension 
    // across all siblings of f. 
    vector<int64_t> get_union_thread_counts(const FunctionDAG::Node *f) {
        vector<int64_t> max_size{0,0,0};
        // find the loop nests we just created and get max gpu_thread extents of other children
        for (auto &c : children) {
            if (c->node != f && c->gpu_label == thread) {
                for (int dim = 0; dim < (int)(c->size.size()); dim++) {
                    if ( dim >= (int)(max_size.size()) ) {
                        max_size.push_back(c->size[dim]);
                    } else {
                        max_size[dim] = std::max(max_size[dim], c->size[dim]); 
                    }
                }
            }
        }
        return max_size;
    }

    // given a newly inserted node f into this LoopNest, gets the size of the new node
    const vector<int64_t> * get_pure_size(const FunctionDAG::Node *f) {
        for (auto &c : children) {
            if (c->node == f && f->dimensions > 0) {
                if (c->stage->index == 0) {
                    return &(c->size);
                }
            } 
        }
        return nullptr;
    }

    // get the loop nests of a newly inserted node, f, that is marked GPU threads. Tiles 
    // the newly inserted loop nests of f into a threads loop outside a serial loop. 
    // V is the vectorized dimension of f. Adds loopnests created from each tiling option in result.
    bool add_gpu_thread_tilings(const FunctionDAG::Node *f, 
                                const MachineParams &params,
                                const Target &target, 
                                int v, 
                                vector<IntrusivePtr<const LoopNest>> &result) {
        debug(0) << "inside add_gpu_thread_tilings\n";
        const vector<int64_t> *pure_size = this->get_pure_size(f);
        vector<int64_t> max_size = this->get_union_thread_counts(f);

        internal_assert(pure_size);
        auto tilings = generate_gpu_tilings(*pure_size, max_size, (int)(pure_size->size() - 1), 
                                            (int)(pure_size->size() - 1), v, false);
        if (tilings.size() > 0)
            debug(0) << "parent " << this->node->func.name() << " has label " << gpu_label << " is GPU THREAD TILING " << f->func.name() << " with " << tilings.size() << " options\n";
        else 
            debug(0) << "no gpu tilings for " << f->func.name() << "\n";

        bool made_child = false;
        for (const auto &t : tilings) {
            LoopNest *new_parent = new LoopNest;
            new_parent->copy_from(*(this));
            for (auto &c : new_parent->children) {
                if (c->node == f) {
                    c = c->parallelize_in_tiles(params, t, new_parent, target);
                }
            }
            result.emplace_back(new_parent);
            made_child = true;
        }
        return made_child;
    }

    void copy_from(const LoopNest &n) {
        size = n.size;
        children = n.children;
        inlined = n.inlined;
        store_at = n.store_at;
        bounds = n.bounds;
        node = n.node;
        stage = n.stage;
        stage_idx = n.stage_idx;
        innermost = n.innermost;
        tileable = n.tileable;
        parallel = n.parallel;
        vector_dim = n.vector_dim;
        vectorized_loop_index = n.vectorized_loop_index;
        gpu_label = n.gpu_label;
    };

    static void hash_combine(uint64_t &h, uint64_t next) {
        // From boost
        h ^= (next + 0x9e3779b9 + (h<<6) + (h>>2));
    }

    // Hash the loop structure and sizes up to a fixed depth
    void structural_hash(uint64_t &h, int depth, int parallelism) const {
        if (depth < 0) return;

        // Which Funcs are store_at this level?
        for (const auto *n : store_at) {
            hash_combine(h, n->id);
        }

        hash_combine(h, -1);

        // Which Funcs are compute_at this level?
        for (const auto &c : children) {
            hash_combine(h, c->stage->id);
        }

        // Add a barrier to ensure that moving something from the last
        // compute_at to the first inlined doesn't result in the same
        // hash.
        hash_combine(h, -1);

        // Which Funcs are inlined at this level?
        for (auto it = inlined.begin(); it != inlined.end(); it++) {
            hash_combine(h, it.key()->id);
        }

        hash_combine(h, -1);

        if (depth > 0) {
            // What are the loop sizes of the children?
            for (const auto &c : children) {
                for (int64_t s : c->size) {
                    if (depth == 1) {
                        // Just take the most significant bit: is it one or not?
                        s = (s > 1) ? 1 : 0;
                    }
                    hash_combine(h, s);
                }
            }

            // Which dimension are we vectorized over?
            hash_combine(h, vectorized_loop_index);
        }

        if (depth > 1) {
            // Descend into children
            for (const auto &c : children) {
                c->structural_hash(h, depth - 2, parallelism);
            }
        }
    }

    size_t funcs_realized_or_inlined() const {
        size_t count = inlined.size() + store_at.size();
        for (const auto &c : children) {
            count += c->funcs_realized_or_inlined();
        }
        return count;
    }

    struct Sites {
        const LoopNest *compute = nullptr, *store = nullptr, *produce = nullptr, *innermost = nullptr, *task = nullptr;
        bool inlined = false;
    };

    void get_sites(StageMap<Sites> &sites,
                   const LoopNest *task = nullptr,
                   const LoopNest *parent = nullptr) const {
        if (!task && !is_root()) {
            task = this;
        }
        for (const auto &c : children) {
            c->get_sites(sites, task, this);
        }
        if (parent && node != parent->node) {
            auto &s = sites.get_or_create(stage);
            s.compute = parent;
            s.produce = this;
            s.task = task;
        }
        for (auto f : store_at) {
            for (const auto &s : f->stages) {
                sites.get_or_create(&s).store = this;
            }
        }
        for (auto it = inlined.begin(); it != inlined.end(); it++) {
            auto &s = sites.get_or_create(&(it.key()->stages[0]));
            s.inlined = true;
            s.compute = s.store = s.produce = s.innermost = this;
            s.task = task;
        }
        if (innermost) {
            sites.get_or_create(stage).innermost = this;
        }
    }

    void set_working_set_at_task_feature(int64_t working_set,
                                         StageMap<ScheduleFeatures> *features) const {
        for (const auto &c : children) {
            c->set_working_set_at_task_feature(working_set, features);
            features->get(c->stage).working_set_at_task = working_set;
        }
    }

    void compute_features(const FunctionDAG &dag,
                          const MachineParams &params,
                          const StageMap<Sites> &sites,
                          int64_t instances,
                          int64_t parallelism,
                          const LoopNest *parent,
                          const LoopNest *grandparent,
                          const LoopNest &root,
                          int64_t *working_set,
                          StageMap<ScheduleFeatures> *features) const {

        int64_t working_set_here = 0;

        int64_t loop_instances = 1, parallel_tasks = 1;
        bool in_impure = false;
        for (int idx = (int)size.size() - 1; idx >= 0; idx--) {
            size_t i = size[idx];
            loop_instances *= i;
            if (stage->loop[idx].pure && !in_impure) {
                if (params.parallelism > 1 &&
                    (parallel || (parent->is_root() && parallel_tasks < params.parallelism))) {
                    // Either we've picked our parallel tiling, or
                    // it's not yet determined. Assume we'll not split
                    // any loops and just stop after we hit the
                    // required number of cores
                    parallel_tasks *= i;
                    if (!parallel && parallel_tasks > params.parallelism * 8) {
                        // We would split this loop
                        parallel_tasks = params.parallelism * 8;
                    }
                }
            } else if (i != 1) {
                in_impure = true;
            }
        }

        int64_t subinstances = instances * loop_instances;

        for (const auto *node : store_at) {
            // Figure out the features at the store_at level
            const auto &bounds = get_bounds(node);

            for (size_t s = 0; s < node->stages.size(); s++) {
                // TODO: Lift invariants from this loop. Most of it's the same for every stage.
                internal_assert(!node->is_input);
                ScheduleFeatures &feat = features->get_or_create(&(node->stages[s]));

                feat.num_realizations = subinstances;

                feat.points_computed_per_realization = 1;
                feat.num_scalars = feat.num_vectors = subinstances;
                bool vectorized = false;
                for (int i = 0; i < (int)node->stages[s].loop.size(); i++) {
                    const auto &p = bounds->loops(s, i);
                    int64_t extent = p.extent();
                    feat.points_computed_per_realization *= extent;
                    if (i == sites.get(&(node->stages[s])).produce->vectorized_loop_index) {
                        // Assumes that we're not going to split
                        // things such that non-native-width
                        // vectorization is a problem, except for the
                        // tail.
                        feat.num_vectors *= extent / node->stages[s].vector_size;
                        feat.num_scalars *= extent % node->stages[s].vector_size;
                        vectorized = true;
                    } else {
                        feat.num_vectors *= extent;
                        feat.num_scalars *= extent;
                    }
                }
                if (!vectorized) {
                    feat.num_vectors = 0;
                }
                feat.points_computed_total = feat.points_computed_per_realization * feat.num_realizations;

                feat.bytes_at_realization = node->bytes_per_point;
                for (int i = 0; i < node->dimensions; i++) {
                    const auto &p = bounds->region_computed(i);
                    feat.bytes_at_realization *= p.extent();
                }
                int64_t innermost_storage_extent = 1;
                int v = sites.get(&(node->stages[s])).produce->vector_dim;
                if (v >= 0 && node->dimensions > 0) {
                    innermost_storage_extent = bounds->region_computed(v).extent();
                }
                feat.innermost_bytes_at_realization = node->bytes_per_point * innermost_storage_extent;

                if (!is_root()) {
                    feat.bytes_at_task = feat.bytes_at_realization;
                    feat.innermost_bytes_at_task = feat.innermost_bytes_at_realization;
                }
            }
        }

        if (is_root()) {
            // TODO: This block of code is repeated below. Refactor
            for (const auto &c : children) {
                c->compute_features(dag, params, sites, subinstances, parallelism, this, parent, root, &working_set_here, features);
            }

            for (const auto *node : store_at) {
                auto &feat = features->get(&(node->stages[0]));
                working_set_here += feat.bytes_at_production;
            }
            for (const auto *node : store_at) {
                for (const auto &s : node->stages) {
                    auto &feat = features->get(&s);
                    feat.working_set_at_realization = working_set_here;
                }
            }
            for (const auto &c : children) {
                if (c->node != node) {
                    auto &feat = features->get(c->stage);
                    feat.working_set_at_production = working_set_here;
                }
            }

            // Figure out the root-level features for every Func
            for (auto it = features->begin(); it != features->end(); it++) {
                const auto *stage = it.key();
                const auto *node = stage->node;
                auto &feat = it.value();
                const auto &root_bounds = root.get_bounds(node);

                feat.bytes_at_root = node->bytes_per_point;
                for (int i = 0; i < node->dimensions; i++) {
                    const auto &p = root_bounds->region_computed(i);
                    feat.bytes_at_root *= p.extent();
                }

                feat.working_set_at_root = working_set_here;

                // What innermost storage extent means for inlined
                // Funcs is unclear, because we haven't selected which
                // storage dimension is innermost.
                auto *p = sites.get(stage).produce;
                if (p) {
                    int64_t innermost_storage_extent = 1;
                    int v = p->vector_dim;
                    if (v >= 0 && node->dimensions > 0) {
                        innermost_storage_extent = root_bounds->region_computed(v).extent();
                    }
                    feat.innermost_bytes_at_root = node->bytes_per_point * innermost_storage_extent;
                } else {
                    feat.innermost_bytes_at_root = 0;
                }

                feat.points_computed_minimum = 1;
                for (int i = 0; i < (int)stage->loop.size(); i++) {
                    const auto &p = root_bounds->loops(stage->index, i);
                    feat.points_computed_minimum *= p.extent();
                }

                if (node->stages.size() == 1 && !node->is_output) {
                    int64_t points_computed_minimum_if_inlined = 0;
                    for (auto *e : node->outgoing_edges) {
                        points_computed_minimum_if_inlined += features->get(e->consumer).points_computed_minimum * e->calls;
                    }
                    feat.points_computed_minimum = std::min(feat.points_computed_minimum, (double)points_computed_minimum_if_inlined);
                }
            }

            return;
        }

        int64_t subparallelism = parallel_tasks * parallelism;

        // Figure out the features at the compute_at level
        internal_assert(!stage->node->is_input);
        ScheduleFeatures &feat = features->get_or_create(stage);

        if (innermost) {
            if (vectorized_loop_index >= 0 && vectorized_loop_index < (int) size.size()) {
                feat.vector_size = size[vectorized_loop_index];
            } else {
                feat.vector_size = 1;
            }
            if (feat.vector_size == 1) {
                // They're all scalars
                feat.num_scalars += feat.num_vectors;
                feat.num_vectors = 0;
            }
        } else {
            // These will get progressively overwritten as we visit the children

            size_t idx = 0;
            feat.innermost_loop_extent = 1;
            feat.innermost_pure_loop_extent = 1;
            for (const auto &l : stage->loop) {
                feat.innermost_loop_extent *= size[idx];
                if (!l.rvar) {
                    feat.innermost_pure_loop_extent *= size[idx];
                }
                idx++;
            }
        }

        const bool at_task = parent->is_root();
        const bool at_production = parent->node != node;
        const bool at_pure_production = at_production && stage_idx == 0;

        if (at_task) {
            if (parallel) {
                const auto &bounds = get_bounds(node);
                feat.bytes_at_task = node->bytes_per_point;
                int64_t innermost_storage_extent = 1;
                for (int i = 0; i < node->dimensions; i++) {
                    int64_t outer = 1;
                    for (size_t l = 0; l < stage->loop.size(); l++) {
                        if (stage->loop[l].var == node->func.args()[i]) {
                            outer = size[l];
                            break;
                        }
                    }
                    const auto &p = bounds->region_computed(i);
                    int64_t extent = p.extent();
                    extent /= outer;
                    feat.bytes_at_task *= extent;
                    if (i == vector_dim) {
                        innermost_storage_extent = extent;
                    }
                }
                feat.innermost_bytes_at_task = node->bytes_per_point * innermost_storage_extent;
            } else {
                // How this loop will be parallelized is not yet
                // determined. Use optimistic values for the features.
                feat.bytes_at_task = (feat.bytes_at_realization + params.parallelism - 1) / params.parallelism;
                feat.innermost_bytes_at_task = std::min(feat.bytes_at_task, feat.innermost_bytes_at_realization);
            }

            feat.unique_bytes_read_per_task = 0;
            feat.unique_lines_read_per_task = 0;

            vector<const FunctionDAG::Edge *> pending;
            set<const FunctionDAG::Node *> done;
            for (const auto *e : stage->incoming_edges) {
                pending.push_back(e);
            }
            while (!pending.empty()) {
                const auto *e = pending.back();
                pending.pop_back();
                if (done.count(e->producer)) continue;
                done.insert(e->producer);
                const auto &site = sites.get(&(e->producer->stages[0]));
                if (site.store->is_root()) {
                    const auto &b = get_bounds(e->producer);
                    int64_t bytes = e->producer->bytes_per_point, lines = 1;
                    int64_t max_extent = 1;
                    int vector_dim = (e->producer->is_input ? 0 :
                                      site.produce != nullptr ? site.produce->vector_dim :
                                      -1);
                    for (int i = 0; i < e->producer->dimensions; i++) {
                        int64_t extent = b->region_required(i).extent();
                        max_extent = std::max(extent, max_extent);
                        bytes *= extent;
                        if (i != vector_dim) {
                            lines *= extent;
                        }
                    }
                    if (!e->producer->is_input && site.produce == nullptr) {
                        lines /= max_extent;
                    }
                    feat.unique_bytes_read_per_task += bytes;
                    feat.unique_lines_read_per_task += lines;

                } else if (site.produce != nullptr) {
                    // Computation must be nested inside this task or inlined into it.
                    for (const auto &s : e->producer->stages) {
                        for (const auto *e2 : s.incoming_edges) {
                            pending.push_back(e2);
                        }
                    }
                }
            }

        }

        if (at_production) {
            feat.num_productions = instances;
            feat.inner_parallelism = parallel_tasks;
            feat.outer_parallelism = parallelism;
            feat.native_vector_size = stage->vector_size;

            const auto &bounds = parent->get_bounds(node);

            feat.bytes_at_production = node->bytes_per_point;
            for (int i = 0; i < node->dimensions; i++) {
                const auto &p = bounds->region_computed(i);
                feat.bytes_at_production *= p.extent();
            }
            int64_t innermost_storage_extent = 1;
            if (vector_dim >= 0 && node->dimensions > 0) {
                innermost_storage_extent = bounds->region_computed(vector_dim).extent();
            }
            feat.innermost_bytes_at_production = node->bytes_per_point * innermost_storage_extent;
        }

        // Recurse inwards
        for (const auto &c : children) {
            c->compute_features(dag, params, sites, subinstances, subparallelism, this, parent, root, &working_set_here, features);
        }
        for (const auto *node : store_at) {
            auto &feat = features->get(&(node->stages[0]));
            working_set_here += feat.bytes_at_production;
        }
        for (const auto *node : store_at) {
            for (const auto &s : node->stages) {
                auto &feat = features->get(&s);
                feat.working_set_at_realization = working_set_here;
            }
        }
        for (const auto &c : children) {
            if (c->node != node) {
                auto &feat = features->get(c->stage);
                feat.working_set_at_production = working_set_here;
            }
        }

        if (at_task) {
            set_working_set_at_task_feature(working_set_here, features);
        }

        if (at_production) {
            feat.working_set = working_set_here;
        }

        if (innermost) {
            bool parent_unrolled =
                (feat.innermost_pure_loop_extent <= kUnrollLimit &&
                 parent->node == node);

            if (parent_unrolled) {
                const auto &grandparent_bounds = grandparent->get_bounds(node);
                for (size_t i = 0; i < parent->size.size(); i++) {
                    if (!stage->loop[i].rvar) {
                        const auto &l = grandparent_bounds->loops(parent->stage->index, i);
                        parent_unrolled &= l.constant_extent();
                    }
                }
            }

            if (parent_unrolled) {
                feat.unrolled_loop_extent = feat.innermost_pure_loop_extent;
            } else {
                feat.unrolled_loop_extent = 1;
            }
        }

        *working_set += working_set_here;

        int64_t bytes_loaded = 0, lines_loaded = 0, allocation_bytes_loaded = 0;
        double num_dense_loads = 0, num_broadcasts = 0, num_gathers = 0, num_stride_2_loads = 0, num_stride_3_loads = 0, num_stride_4_loads = 0, num_loads = 0;
        if (innermost || at_production) { // These are the sites at which we compute load footprints
            // Pick the site at which we will compute the footprint relationship
            const auto &consumer_site = sites.get(stage);
            const auto *consumer_store_site = innermost ? parent : consumer_site.store;
            const auto *consumer_task_site = consumer_site.task;
            int64_t consumer_instances = innermost ? instances : feat.num_realizations;
            if (consumer_instances == 0) {
                root.dump(" ", nullptr);
            }
            internal_assert(consumer_instances != 0) << node->func.name() << " " << innermost << " " << instances << " " << feat.num_realizations << "\n";

            vector<const FunctionDAG::Node::Stage *> pending;
            pending.emplace_back(stage);
            vector<pair<LoadJacobian, FunctionDAG::Node *>> jacobians;
            set<const FunctionDAG::Node *> done;
            while (!pending.empty()) {
                auto p = pending.back();
                pending.pop_back();
                const auto &next_edges = p->incoming_edges;
                for (const auto *e : next_edges) {
                    internal_assert(sites.contains(&(e->producer->stages[0]))) << "No site found for " << e->producer->func.name() << "\n";

                    const auto &site = sites.get(&(e->producer->stages[0]));

                    bool producer_has_been_scheduled = e->producer->is_input || (site.produce != nullptr);

                    if (innermost) {
                        if (e->consumer == stage) {
                            for (auto &j : e->load_jacobians) {
                                jacobians.emplace_back(j, e->producer);
                            }
                        } else {
                            // Consumer was inlined. Concat the jacobians to look through it.
                            decltype(jacobians) new_jacobians;
                            for (auto &j1 : jacobians) {
                                if (e->consumer->node == j1.second) {
                                    for (auto &j2 : e->load_jacobians) {
                                        LoadJacobian j = j2 * j1.first;
                                        new_jacobians.emplace_back(j, e->producer);
                                    }
                                } else {
                                    new_jacobians.emplace_back(std::move(j1));
                                }
                            }
                            jacobians.swap(new_jacobians);
                        }
                    }

                    if (site.inlined) {
                        // Recursively examine the inputs
                        pending.emplace_back(&(e->producer->stages[0]));
                        continue;
                    }

                    const auto *producer_compute_site = site.compute;
                    const auto *producer_store_site = site.store;
                    const auto &bounds = consumer_store_site->get_bounds(e->producer);
                    const auto &task_bounds = consumer_task_site->get_bounds(e->producer);
                    const auto &producer_compute_bounds = producer_compute_site->get_bounds(e->producer);
                    const auto &producer_store_bounds = producer_store_site->get_bounds(e->producer);
                    int64_t footprint = e->producer->bytes_per_point;
                    int64_t compute_footprint = footprint;
                    int64_t store_footprint = footprint;
                    int64_t task_footprint = footprint;
                    int64_t line_footprint = 1;
                    int64_t compute_line_footprint = 1;
                    int64_t store_line_footprint = 1;
                    int64_t task_line_footprint = 1;

                    if (e->producer->is_input) {
                        internal_assert(producer_store_site->is_root());
                        internal_assert(producer_compute_site->is_root());
                    }

                    if (innermost) {

                        // Grab the jacobians that describe the memory dependence
                        for (const auto &jac : jacobians) {
                            if (jac.second != e->producer) continue;
                            double n = jac.first.count();
                            // internal_assert(n < 1024 * 1024 * 1024) << "Implausibly large n: " << jac.count() << " " << next_count << "\n";
                            // Classify
                            bool vector_broadcast = true;
                            bool dense_vector_load = true;
                            bool stride_2_vector_load = true;
                            bool stride_3_vector_load = true;
                            bool stride_4_vector_load = true;
                            int producer_innermost_dim =
                                (e->producer->is_input ? 0 : // Assume default storage layout for inputs
                                 !producer_has_been_scheduled ? -1 :
                                 site.produce->vector_dim);
                            if (vectorized_loop_index >= 0) {
                                for (int i = 0; i < e->producer->dimensions; i++) {
                                    auto stride = jac.first(i, vectorized_loop_index);
                                    vector_broadcast &= stride == 0;
                                    if (i == producer_innermost_dim || !producer_has_been_scheduled) {
                                        dense_vector_load &= stride == 1;
                                        stride_2_vector_load &= stride == 2;
                                        stride_3_vector_load &= stride == 3;
                                        stride_4_vector_load &= stride == 4;
                                    } else {
                                        dense_vector_load &= stride == 0;
                                        stride_2_vector_load &= stride == 0;
                                        stride_3_vector_load &= stride == 0;
                                        stride_4_vector_load &= stride == 0;
                                        // TODO: Check for strided loads across non-innermost dims, and use to count the number of pages, cache lines, etc
                                    }
                                }
                            }

                            // Is this load loop-invariant over an
                            // unrolled block? If so, we amortize the
                            // number of loads to account for LICM.
                            int64_t amortization = 1;
                            if (feat.unrolled_loop_extent > 1) {
                                for (size_t idx = 0; idx < stage->loop.size(); idx++) {
                                    if (!stage->loop[idx].rvar) {
                                        bool loop_invariant = true;
                                        for (int i = 0; i < e->producer->dimensions; i++) {
                                            if (!(jac.first(i, idx) == 0)) {
                                                loop_invariant = false;
                                                break;
                                            }
                                        }
                                        if (loop_invariant) {
                                            amortization *= parent->size[idx];
                                        }
                                    }
                                }
                            }
                            // TODO: LICM still acts for the innermost loop of non-unrolled things

                            n /= amortization;

                            num_loads += n;
                            if (vector_broadcast) {
                                num_broadcasts += n;
                            } else if (dense_vector_load) {
                                num_dense_loads += n;
                            } else if (stride_2_vector_load) {
                                num_stride_2_loads += n;
                            } else if (stride_3_vector_load) {
                                num_stride_3_loads += n;
                            } else if (stride_4_vector_load) {
                                num_stride_4_loads += n;
                            } else {
                                num_gathers += n;
                            }
                        }
                    }

                    // Already dealt with the footprints for this producer via some other path
                    if (done.find(e->producer) != done.end()) {
                        continue;
                    }

                    done.insert(e->producer);

                    int64_t max_extent = 1, max_compute_extent = 1, max_store_extent = 1, max_task_extent = 1;

                    for (int i = 0; i < e->producer->dimensions; i++) {
                        auto p = bounds->region_required(i);
                        auto compute_p = producer_compute_bounds->region_computed(i);
                        auto store_p = producer_store_bounds->region_required(i);
                        auto task_p = task_bounds->region_required(i);

                        internal_assert(store_p.min() <= store_p.max()) << store_p.min() << " " << store_p.max() << "\n";
                        internal_assert(compute_p.min() <= compute_p.max()) << compute_p.min() << " " << compute_p.max() << "\n";
                        internal_assert(task_p.min() <= task_p.max()) << task_p.min() << " " << task_p.max() << "\n";

                        int64_t extent = p.extent();
                        int64_t compute_extent = compute_p.extent();
                        int64_t store_extent = store_p.extent();
                        int64_t task_extent = task_p.extent();

                        max_extent = std::max(extent, max_extent);
                        max_compute_extent = std::max(compute_extent, max_compute_extent);
                        max_store_extent = std::max(store_extent, max_store_extent);
                        max_task_extent = std::max(task_extent, max_task_extent);

                        footprint *= extent;
                        compute_footprint *= compute_extent;
                        store_footprint *= store_extent;
                        task_footprint *= task_extent;

                        bool dense = ((e->producer->is_input && i == 0) ||
                                      (site.produce != nullptr && i == site.produce->vector_dim));
                        if (!dense) {
                            line_footprint *= extent;
                            compute_line_footprint *= compute_extent;
                            store_line_footprint *= store_extent;
                            task_line_footprint *= task_extent;
                        }
                    }

                    if (!producer_has_been_scheduled) {
                        // Optimistically assume it gets vectorized
                        // along whatever dimension makes these
                        // numbers the smallest.
                        line_footprint /= max_extent;
                        compute_line_footprint /= max_compute_extent;
                        store_line_footprint /= max_store_extent;
                        task_line_footprint /= max_task_extent;
                    }

                    int64_t store_instances_per_consumption = 1;

                    if (producer_has_been_scheduled && !e->producer->is_input) {
                        const auto &producer_feat = features->get_or_create(&(e->producer->stages[0]));

                        if (producer_feat.num_realizations) {
                            // The producer's realization is nested inside this Func's realization
                            const int64_t producer_store_instances = producer_feat.num_realizations;
                            if (producer_store_instances > consumer_instances) {
                                store_instances_per_consumption = producer_store_instances / consumer_instances;
                            }
                        }
                    }

                    allocation_bytes_loaded += compute_footprint;

                    if (store_instances_per_consumption > 1) {
                        // The producer is nested inside the consumer
                        bytes_loaded += store_footprint; // * store_instances_per_consumption;
                        // Due to folding, the actual buffer size is smaller than the bounds at the store level
                        lines_loaded += store_line_footprint; // * store_instances_per_consumption;
                    } else {
                        // The consumer is consuming some portion of a larger producer computed earlier
                        bytes_loaded += footprint;
                        lines_loaded += line_footprint;
                    }
                }
            }
        }

        if (at_production) {
            // Properties of the realization, but the values are
            // computable at the production site because that's where
            // the consumers are.
            internal_assert(bytes_loaded >= 0) << "Negative bytes loaded: " << bytes_loaded << "\n";
            feat.allocation_bytes_read_per_realization = allocation_bytes_loaded;
            feat.unique_bytes_read_per_realization = bytes_loaded;
            feat.unique_lines_read_per_realization = lines_loaded;

            if (!at_pure_production) {
                // Also pessimistically assume this update definition relies on the entirety of the produced region so far.
                // TODO: This overbills scatters, or writes to a restriction region.
                internal_assert(bytes_loaded >= 0) << "Negative bytes at production: " << feat.bytes_at_production << "\n";
                feat.unique_bytes_read_per_realization += feat.bytes_at_production;
                feat.unique_lines_read_per_realization += feat.bytes_at_production / feat.innermost_bytes_at_production;
                feat.allocation_bytes_read_per_realization += feat.bytes_at_production;
            }
        }

        if (innermost) {
            feat.points_computed_per_production = subinstances / feat.num_productions;
            feat.vector_loads_per_vector = num_dense_loads + 2 * num_stride_2_loads + 3 * num_stride_3_loads + 4 * num_stride_4_loads;
            feat.scalar_loads_per_vector = num_broadcasts + feat.vector_size * num_gathers;
            feat.scalar_loads_per_scalar = num_loads;
            if (stage->index > 0) {
                // Assume a self-load
                feat.vector_loads_per_vector++;
                feat.scalar_loads_per_scalar++;
            }
            feat.unique_bytes_read_per_vector = bytes_loaded;
            feat.unique_lines_read_per_vector = lines_loaded;
        }

        // Track features for inlined Funcs
        for (auto it = inlined.begin(); it != inlined.end(); it++) {
            const auto *f = it.key();
            internal_assert(f);
            auto &inlined_feat = features->get_or_create(&(f->stages[0]));
            inlined_feat.inlined_calls += it.value() * subinstances;
            inlined_feat.num_vectors += it.value() * feat.num_vectors;
            inlined_feat.num_scalars += it.value() * feat.num_scalars;
            inlined_feat.native_vector_size = stage->vector_size;
            if (inlined_feat.vector_size > 0) {
                inlined_feat.vector_size = std::min(inlined_feat.vector_size, (double)stage->vector_size);
            } else {
                inlined_feat.vector_size = feat.vector_size;
            }
            if (inlined_feat.innermost_pure_loop_extent > 0) {
                inlined_feat.innermost_pure_loop_extent = std::min(inlined_feat.innermost_pure_loop_extent,
                                                                   feat.innermost_pure_loop_extent);
            } else {
                inlined_feat.innermost_pure_loop_extent = feat.innermost_pure_loop_extent;
            }
            inlined_feat.inner_parallelism = 1;
            inlined_feat.outer_parallelism = parallelism;
        }
    }

    bool is_root() const {
        return node == nullptr;
    }

    const Bound &set_bounds(const FunctionDAG::Node *f, BoundContents *b) const {
        return bounds.emplace(f, b);
    }

    const Bound &get_bounds(const FunctionDAG::Node *f) const {
        // debug(0) << "get_bounds of " << f->func.name() << " in loop over " << (is_root() ? "root" : node->func.name()) << '\n';
        if (bounds.contains(f)) {
            const Bound &b = bounds.get(f);
            // debug(0) << "Getting bounds of " << f->func.name() << " at site:\n";
            // dump("  ");
            b->validate();
            return b;
        }
        auto bound = f->make_bound();
        // Compute the region required
        if (f->is_output && is_root()) {
            internal_assert(f->outgoing_edges.empty()) << "Outputs that access other outputs not yet supported\n";
            // It's an output.
            // Use the bounds estimate
            for (int i = 0; i < f->dimensions; i++) {
                bound->region_required(i) = f->estimated_region_required[i];
            }
        } else {
            internal_assert(!f->outgoing_edges.empty())
                << "No consumers of " << f->func.name()
                << " at loop over " << (is_root() ? "root" : node->func.name()) << '\n';
            auto init = Span::empty_span();
            for (int i = 0; i < f->dimensions; i++) {
                bound->region_required(i) = init;
            }

            for (const auto *e : f->outgoing_edges) {
                // Ignore consumers outside of this loop nest
                if (!is_root() &&
                    (stage != e->consumer) &&
                    !stage->downstream_of(*(e->consumer->node))) {
                    continue;
                }
                /*
                if (!computes(e->consumer->node)) {
                    // debug(0) << "Skipping edge from " << e->producer->func.name() << " to " << e->consumer->func.name() << "\n";
                    continue;
                }
                */
                // debug(0) << "Expanding footprint along edge " << e->producer->func.name() << " -> " << e->consumer->func.name() << "\n";
                const auto &c_bounds = get_bounds(e->consumer->node);
                const auto *consumer_loop = &(c_bounds->loops(e->consumer->index, 0)); // For the concrete sizes of the loop
                e->expand_footprint(consumer_loop, &(bound->region_required(0)));
            }
        }

        f->required_to_computed(&(bound->region_required(0)), &(bound->region_computed(0)));

        for (int i = 0; i < (int)f->stages.size(); i++) {
            f->loop_nest_for_region(i, &(bound->region_computed(0)), &(bound->loops(i, 0)));
        }

        const Bound &b = set_bounds(f, bound);
        b->validate();
        return b;
    }

    void dump(string prefix, const LoopNest *parent) const {
        if (!is_root()) {
            debug(0) << prefix << node->func.name();
            prefix += " ";

            for (size_t i = 0; i < size.size(); i++) {
                debug(0) << " " << size[i];
                if (innermost && i == (size_t) vectorized_loop_index) {
                    debug(0) << 'v';
                }
                if (parent->get_bounds(node)->loops(stage->index, i).constant_extent()) {
                    debug(0) << 'c';
                }
            }
            /*
            const auto &bounds = get_bounds(node);
            for (size_t i = 0; i < size.size(); i++) {
                const auto &p = bounds->loops(stage->index, i);
                debug(0) << " [" << p.first << ", " << p.second << "]";
            }
            */

            debug(0) << " (" << vectorized_loop_index << ", " << vector_dim << ")";
        }

        if (tileable) {
            debug(0) << " t";
        }
        if (innermost) {
            debug(0) << " *\n";
        } else if (gpu_label == block) {
            debug(0) << " gpu_block\n";
        } else if (gpu_label == serial) {
            debug(0) << " gpu_serial\n";
        } else if (gpu_label == none) {
            debug(0) << " gpu_none\n";
        } else if (gpu_label == simd) {
            debug(0) << " gpu_simd\n";
        } else if (gpu_label == thread) {
            debug(0) << " gpu_thread\n";
        } else if (gpu_label == parallelized) {
            debug(0) << " gpu_parallelized\n";
        } else if (parallel) {
            debug(0) << " p\n";
        } else {
            debug(0) << '\n';
        }
        for (auto p : store_at) {
            debug(0) << prefix << "realize: " << p->func.name() << " with " << p->stages.size() << " stages\n";
        }
        for (size_t i = children.size(); i > 0; i--) {
            children[i-1]->dump(prefix, this);
        }
        for (auto it = inlined.begin(); it != inlined.end(); it++) {
            debug(0) << prefix << "inlined: " << it.key()->func.name() << " " << it.value() << '\n';
        }
        /*
          for (auto p : bounds) {
          debug(0) << prefix << "bounds: " << p.first.name();
          for (auto d : p.second.region) {
          debug(0) << " [" << d.first << ", " << d.second << "]";
          }
          debug(0) << '\n';
          }
        */
    }

    bool calls(const FunctionDAG::Node *f) const {
        for (const auto &c : children) {
            if (c->calls(f)) return true;
        }
        for (const auto *e : f->outgoing_edges) {
            if (e->consumer == stage) {
                return true;
            }
            if (inlined.contains(e->consumer->node)) {
                return true;
            }
        }
        return false;
    }

    int64_t max_inlined_calls() const {
        int64_t result = 0;
        for (auto it = inlined.begin(); it != inlined.end(); it++) {
            result = std::max(result, it.value());
        }
        for (const auto &c : children) {
            result = std::max(result, c->max_inlined_calls());
        }
        return result;
    }

    bool accesses_input_buffer() const {
        for (const auto &c : children) {
            if (c->accesses_input_buffer()) return true;
        }
        if (is_root()) return false;

        auto check = [&](const FunctionDAG::Node::Stage *s) {
            for (const auto *e : s->incoming_edges) {
                if (e->producer->is_input) return true;
            }

            for (int t = 0; t < (int)PipelineFeatures::ScalarType::NumScalarTypes; t++) {
                if (s->features.op_histogram[(int)PipelineFeatures::OpType::ImageCall][t] > 0) return true;
            }
            return false;
        };

        if (check(stage)) return true;
        for (auto it = inlined.begin(); it != inlined.end(); it++) {
            if (check(&(it.key()->stages[0]))) return true;
        }
        return false;
    }

    bool computes(const FunctionDAG::Node *f) const {
        if (f == node) {
            return true;
        }
        if (inlined.contains(f)) {
            return true;
        }
        for (const auto &c : children) {
            if (c->computes(f)) return true;
        }
        return false;
    }

    void inline_func(const FunctionDAG::Node *f) {
        // Inline it into the children
        for (size_t i = 0; i < children.size(); i++) {
            if (children[i]->calls(f)) {
                std::unique_ptr<LoopNest> new_child{new LoopNest};
                new_child->copy_from(*children[i]);
                new_child->inline_func(f);
                children[i] = new_child.release();
            }
        }

        // Inline it here if there are any direct calls
        if (innermost) {
            int64_t calls = 0;
            for (const auto *e : f->outgoing_edges) {
                if (inlined.contains(e->consumer->node)) {
                    calls += inlined.get(e->consumer->node) * e->calls;
                }
                if (e->consumer == stage) {
                    calls += e->calls;
                }
            }
            if (calls) {
                inlined.insert(f, calls);
            }
        }
    }

    void compute_here(const FunctionDAG::Node *f, 
                      bool tileable, 
                      int v, 
                      bool in_threads_loop) {
        if (this->node)
            debug(0) << "CALLING COMPUTE HERE ON " << f->func.name() << " INSIDE " << this->node->func.name() << "\n";
        else 
            debug(0) << "CALLING COMPUTE HERE ON " << f->func.name() << " INSIDE ROOT x\n";

        const auto &bounds = get_bounds(f);

        if (!may_subtile()) {
            // This loop is no longer tileable
            this->tileable = false;
        }

        for (int s = (int)f->stages.size() - 1; s >= 0; s--) {
            LoopNest *node = new LoopNest;
            node->node = f;
            node->stage_idx = s;
            node->stage = &f->stages[s];
            node->innermost = true;
            node->vectorized_loop_index = -1;
            node->tileable = tileable && (is_root() || may_subtile());

            // always set gpu_label as thread if legal. 
            // if !in_threads_loop we are computing either at root level or inside a serial loop 
            // set gpu_label to none, then call parallelize_in_tiles to create a parallel, serial, SIMD loop
            // if compute_root set gpu_label to none, parallelize_in_tiles creates block and thread loops later 
            // if computing at serial loop set gpu_label to thread.
            if (is_root()) {
                node->gpu_label = none;
            } else if (!in_threads_loop) {
                node->gpu_label = thread;
            } else {
                node->gpu_label = serial;
            }

            // Set up a bound for the inside of the
            // loop. computed/required is still the full region, but
            // the loop nest will be a single representative point.
            auto single_point = bounds->make_copy();
            size_t loop_dim = f->stages[s].loop.size();
            node->size.resize(loop_dim);

            int64_t total_extent = 1;
            int64_t vector_size = 1;
            for (size_t i = 0; i < loop_dim; i++) {
                const auto &l = bounds->loops(s, i);
                // Initialize the loop nest
                node->size[i] = l.extent();
                total_extent *= node->size[i];

                // Use the first loop iteration to represent the inner
                // loop. We'll shift it to a later one once we decide
                // on vectorization.
                single_point->loops(s, i) = Span(l.min(), l.min(), true);

                internal_assert(l.max() >= l.min()) << i << " " << l.max() << " " << l.min() << "\n";

                if (f->dimensions &&
                    node->size[i] >= 1 &&
                    f->stages[s].loop[i].var == f->func.args()[v]) {
                    node->vectorized_loop_index = (int)i;
                    vector_size = (int64_t)(node->stage->vector_size);
                    single_point->loops(s, i).set_extent(vector_size);
                    node->size[i] += vector_size - 1;
                    node->size[i] /= vector_size;

                    // Shift the loops along by some multiple of the
                    // vector size, to pick a more representative vector
                    // than the first.
                    int64_t shift = vector_size * (node->size[i] / 2);
                    single_point->loops(s, i).translate(shift);
                } else {
                    int64_t shift = node->size[i] / 2;
                    single_point->loops(s, i).translate(shift);
                }
            }
            // Leave region required blank inside the computation of a Func
            node->set_bounds(f, std::move(single_point));
            node->vector_dim = v;

            if (node->vectorized_loop_index >= 0) {
                // Split off the single vector as an inner loop nest.
                node->innermost = false;

                LoopNest *one_vector = new LoopNest;
                one_vector->node      = node->node;
                one_vector->stage     = node->stage;
                one_vector->stage_idx = node->stage_idx;
                one_vector->tileable  = false;
                one_vector->vectorized_loop_index = node->vectorized_loop_index;
                one_vector->vector_dim = v;
                one_vector->size.resize(loop_dim, 1);
                one_vector->innermost = true;
                one_vector->gpu_label = simd;
                auto b = node->get_bounds(f)->make_copy();
                // Set the region computed inside this node to be the first vector lane
                b->loops(s, node->vectorized_loop_index).set_extent(1);
                one_vector->set_bounds(f, b);
                one_vector->size[node->vectorized_loop_index] = vector_size;

                node->children.emplace_back(one_vector);
            }

            children.emplace_back(node);
        }
    }

    // Return all possible ways to parallelize this loop
    IntrusivePtr<const LoopNest> parallelize_in_tiles(const MachineParams &params,
                                                      const vector<int64_t> &tiling,
                                                      const LoopNest *parent, 
                                                      const Target& target) const {

        // Split this loop and move factors to the inner loop
        LoopNest *inner = new LoopNest, *outer = new LoopNest;
        inner->node      = outer->node      = node;
        inner->stage     = outer->stage     = stage;
        inner->stage_idx = outer->stage_idx = stage_idx;
        inner->tileable  = outer->tileable  = tileable && may_subtile();
        inner->vector_dim = outer->vector_dim = vector_dim;
        inner->vectorized_loop_index = outer->vectorized_loop_index = vectorized_loop_index;

        if (target.has_gpu_feature()) {
            if (gpu_label == none) {
                inner->gpu_label = serial;
                outer->gpu_label = parallelized;
                outer->parallel = true;
            } else if (gpu_label == parallelized) {
                inner->gpu_label = thread; // compute root funcs always allowed to use GPU threads
                outer->gpu_label = block;
                outer->parallel = true;
            } else if (gpu_label == thread) {
                inner->gpu_label = serial;
                outer->gpu_label = thread;
                outer->parallel = false;
            } else {
                internal_error << "invalid gpu label " << gpu_label << " for parallelized loop\n";
            }
        }
       
        outer->size = size;
        outer->innermost = false;

        if (!target.has_gpu_feature()) 
            outer->parallel = true; 
         
        outer->tileable = may_subtile();

        // First make an inner loop representing a 1x1x1... tile
        inner->size.resize(size.size(), 1);
        inner->innermost = innermost;
        inner->children = children;
        inner->inlined = inlined;
        inner->bounds = bounds;
        inner->store_at = store_at;

        auto b = inner->get_bounds(node)->make_copy();

        // Then move factors from the outer loop to the inner loop
        auto parent_bounds = parent->get_bounds(node);

        for (size_t i = 0; i < stage->loop.size(); i++) {
            int l = stage->loop[i].pure_dim;

            int64_t outer_extent;
            if (l >= 0) {
                internal_assert(l < (int)tiling.size()) << l << " " << tiling.size() << "\n";
                outer_extent = tiling[l];
            } else {
                // RVars are moved inwards
                outer_extent = 1;
            }

            inner->size[i] = (outer->size[i] + outer_extent - 1) / outer_extent;
            // Recompute the outer size given the selected inner size
            outer_extent = (outer->size[i] + inner->size[i] - 1) / inner->size[i];

            outer->size[i] = outer_extent;
            const auto &p = parent_bounds->loops(stage_idx, i);
            int64_t min = p.min();
            int64_t extent = p.extent();
            extent = (extent + outer_extent - 1) / outer_extent;
            // Pick a better representative loop iteration
            min += (outer_extent / 2) * extent;
            bool compile_time_constant_bounds = p.constant_extent() || ((outer_extent > 1) && stage->loop[i].pure);
            b->loops(stage_idx, i) = Span(min, min + extent - 1, compile_time_constant_bounds);
        }
        outer->set_bounds(node, b);

        outer->children.emplace_back(inner);
        return outer;
    }

    // Return all possible ways to compute f in tiles.
    // in_threads_loop tracks whether or not function is going to be placed inside a
    // loop marked gpu_threads, in which case f's loops cannot be gpu_threads
    vector<IntrusivePtr<const LoopNest>> compute_in_tiles(const FunctionDAG::Node *f,
                                                          const LoopNest *parent,
                                                          const MachineParams &params,
                                                          const Target &target,
                                                          int v,
                                                          bool in_realization,
                                                          bool in_threads_loop) const {
        debug(0) << "CALLING COMPUTE_IN_TILES for func " << f->func.name() << "\n";
        internal_assert(f);

        vector<IntrusivePtr<const LoopNest>> result;

        // Some pruning to not waste time on terrible states
        if (parent) {
            const auto &bounds_here = get_bounds(f);
            const auto &bounds_at_parent = parent->get_bounds(f);

            // Don't descend into loops that break our ability to
            // vectorize if we could have vectorized one level up.
            const auto &p = bounds_here->region_computed(v);
            const auto &p_parent = bounds_at_parent->region_computed(v);
            int64_t e = p.extent();
            int64_t ep = p_parent.extent();
            if (ep >= f->vector_size && e < f->vector_size) return result;

            // Don't descend into loops if the bounds required don't shrink
            int64_t total_here = 1, total_at_parent = 1;
            for (int i = 0; i < f->dimensions; i++) {
                const auto &range_here = bounds_here->region_computed(i);
                const auto &range_at_parent = bounds_at_parent->region_computed(i);
                total_here *= range_here.extent();
                total_at_parent *= range_at_parent.extent();
            }

            if (total_here >= total_at_parent) return result;
        }

        // Figure out which child we can fuse this into
        int child = -1;
        bool called_by_multiple_children = false;
        for (int i = 0; i < (int)children.size(); i++) {
            if (children[i]->calls(f)) {
                if (child != -1) {
                    called_by_multiple_children = true;
                }
                child = i;
            }
        }

        // HACK (when true)
        const bool force_only_output_compute_root = false;

        if ((!is_root() || f->is_output || !force_only_output_compute_root) &&
            !innermost &&
            (!in_realization || size.empty() || vector_dim == -1 || size[vector_dim] == 1)) {
            
            // Place the computation inside this loop
            std::unique_ptr<LoopNest> r{new LoopNest};
            r->copy_from(*this);
            if (this->node) {
                debug(0) << "copying from loop nest for " << this->node->func.name() << " with gpu label " << gpu_label << " is root: " << is_root() << " func inserted is: " << f->func.name() << "\n"; 
            }
            else {
                debug(0) << "copying from loop nest for root with gpu label " << gpu_label << " is root: " << is_root() << " func inserted is: " << f->func.name() << "\n"; 
            }
            if (parent) {
                if (parent->is_root()) {
                    debug(0) << "parent is root\n";
                } else {
                    debug(0) << "parent is: " << parent->node->func.name() << "\n";
                }
            } 

            r->compute_here(f, true, v, in_threads_loop);

            if (!in_realization) {
                r->store_at.insert(f);
            } else {
                r->tileable = false;
            }

            // if GPU and creating a threads loop INSIDE a block loop, create child for each thread tiling
            if ( !is_root() && !in_threads_loop && target.has_gpu_feature() ) {
                bool made_child = r->add_gpu_thread_tilings(f, params, target, v, result);
                if (!made_child) // no good thread tilings, just keep r with the untiled thread loop inserted
                    result.emplace_back(r.release());
            } else { // computing at root or inside a threads loop
                result.emplace_back(r.release());
            }
        }

        if (f->is_output || compute_root_and_inline_only()) {
            // Not permitted to compute at tiles of some consumer
            return result;
        }

        if (tileable) {
            // Generate a list of tile sizes to try
            auto tilings = generate_tilings(size, (int)(size.size() - 1), 2, !in_realization, target);

            if (tilings.size() > 10000) {
                debug(0) << "Warning: lots of tilings: " << tilings.size() << "\n";
            }

            for (auto t : tilings) {
                if (parent->is_root()) {
                    const auto &l = stage->loop;
                    // Skip root-level tilings that would leave too
                    // many cores idle, and root-level tilings that
                    // would force serialization of dimensions we have
                    // decided to parallelize over in an earlier pass.
                    int total = 1;
                    size_t idx = 0;
                    for (auto s : t) {
                        if (l[idx].pure) {
                            total *= s;
                        }
                        idx++;
                    }

                    const double tasks_per_core = (double)total / params.parallelism;
                    const double idle_cores = std::ceil(tasks_per_core) / tasks_per_core;
                    if (idle_cores > 1.1) continue;
                }

                // Tile this loop and place the computation at some coarser granularity
                LoopNest *inner = new LoopNest, *outer = new LoopNest;
                inner->node      = outer->node      = node;
                inner->stage     = outer->stage     = stage;
                inner->stage_idx = outer->stage_idx = stage_idx;
                inner->tileable  = outer->tileable  = tileable && may_subtile();
                inner->vector_dim = outer->vector_dim = vector_dim;
                inner->vectorized_loop_index = outer->vectorized_loop_index = vectorized_loop_index;
                outer->size = size;
                outer->innermost = false;
                outer->parallel = parallel;
                inner->parallel = false;

                // First make an inner loop representing a 1x1x1... tile
                inner->size.resize(size.size(), 1);
                inner->innermost = innermost;
                inner->children = children;
                inner->inlined = inlined;
                inner->bounds = bounds;
                inner->store_at = store_at;

                {
                    auto b = inner->get_bounds(node)->make_copy();

                    // Then move factors from the outer loop to the inner loop
                    auto parent_bounds = parent->get_bounds(node);

                    for (size_t i = 0; i < t.size(); i++) {
                        int64_t outer_extent = t[i];
                        inner->size[i] = (outer->size[i] + outer_extent - 1) / outer_extent;
                        outer->size[i] = outer_extent;
                        const auto &p = parent_bounds->loops(stage_idx, i);
                        int64_t min = p.min();
                        int64_t original_extent = p.extent();
                        int64_t inner_extent = (original_extent + outer_extent - 1) / outer_extent;
                        // Pick a more representative loop iteration
                        min += (outer_extent / 2) * inner_extent;
                        bool compile_time_constant_extent =
                            (p.constant_extent() || outer_extent > 1) &&
                            (inner_extent == 1 || outer_extent == 1 || stage->index == 0);
                        b->loops(stage_idx, i) = Span(min, min + inner_extent - 1, compile_time_constant_extent);
                    }

                    // Region_{computed/required} on outer is now
                    // wrong, but it doesn't matter because consumers
                    // only look at the loops in get_bounds. Still,
                    // this is weird.
                    outer->set_bounds(node, b);
                }

                if (!in_realization) {
                    outer->store_at.insert(f);
                }

                bool may_slide = (!in_realization &&
                                  f->stages.size() == 1 &&
                                  !target.has_gpu_feature()); // disable sliding for GPU, often not useful
                if (may_slide) {
                    // should NEVER get here for GPU schedules, no sliding on GPU
                    // Store here, but compute further in. Currently
                    // don't have to worry about the constraints this
                    // places on parallelism, as we forced all the
                    // parallelism to the outer loop.
                    auto opts = inner->compute_in_tiles(f, outer, params, target, v, true, in_threads_loop);
                    for (IntrusivePtr<const LoopNest> &n : opts) {
                        LoopNest *store_at_outer_compute_further_in = new LoopNest;
                        store_at_outer_compute_further_in->copy_from(*outer);
                        store_at_outer_compute_further_in->children.emplace_back(std::move(n));
                        result.emplace_back(store_at_outer_compute_further_in);
                    }
                }

                outer->tileable &= !in_realization;

                if (!target.has_gpu_feature()) {
                    outer->children.emplace_back(inner);
                    // Site the computation inside the outer loop
                    outer->compute_here(f, true, v, in_threads_loop);
                    result.emplace_back(outer);
                } else {
                    // Rules for assigning gpu_labels when splitting a loop:
                    // threaded loops can be split into: (threaded, serial) or (serial, threaded)
                    // block loops can only be split into: blocks, serial
                    // serial loops can only be split into: serial, serial
                    switch (gpu_label) { 
                        case thread: {
                            debug(0) << "splitting thread loop for " << this->node->func.name() << " and inserting " << f->func.name() << "\n";
                            // create new loop nests for gpu_label assignment (serial, threads)
                            LoopNest *serial_outer = new LoopNest, *threaded_inner = new LoopNest;
                            serial_outer->copy_from(*outer);
                            threaded_inner->copy_from(*inner);

                            serial_outer->gpu_label = serial;
                            threaded_inner->gpu_label = thread;

                            serial_outer->children.emplace_back(threaded_inner);
                            serial_outer->compute_here(f, true, v, false);

                            // find the child we just created inside serial_outer to retile it into 
                            // a gpu threads and serial loop
                            bool made_child = serial_outer->add_gpu_thread_tilings(f, params, target, v, result);

                            if (made_child) 
                                delete serial_outer;
                            else // no good thread tilings, just add serial_outer with untiled thread loop inserted
                                result.emplace_back(serial_outer);

                            // create (threads, serial) option
                            internal_assert(in_threads_loop); // threads loop can't be inside threads loop
                            outer->gpu_label = thread; 
                            inner->gpu_label = serial;

                            outer->children.emplace_back(inner);
                            outer->compute_here(f, true, v, true);
                            result.emplace_back(outer);
                            break;
                        }

                        case block: {
                            debug(0) << "splitting block loop for " << this->node->func.name() << " and inserting " << f->func.name() << "\n";

                            internal_assert(!in_threads_loop);
                            outer->gpu_label = block;
                            inner->gpu_label = serial;

                            outer->children.emplace_back(inner);
                            outer->compute_here(f, true, v, false);
                            
                            bool made_child = outer->add_gpu_thread_tilings(f, params, target, v, result);

                            if (made_child) 
                                delete outer;
                            else // no good thread tilings, just add the untiled thread loop
                                result.emplace_back(outer);
                            break;
                        }

                        case serial: {
                            debug(0) << "splitting serial loop for " << this->node->func.name() << " and inserting " << f->func.name() << "\n";

                            outer->gpu_label = serial;
                            inner->gpu_label = serial;

                            outer->children.emplace_back(inner);
                            outer->compute_here(f, true, v, in_threads_loop);

                            if (!in_threads_loop) {
                                bool made_child = outer->add_gpu_thread_tilings(f, params, target, v, result);
                    
                                if (made_child) 
                                    delete outer;
                                else // no good thread tilings, just add the untiled thread loop
                                    result.emplace_back(outer);
                            } else { // inside a threads loop, can't generate thread loop tilings
                                result.emplace_back(outer);
                            }
                            break;
                        }
                        case simd: {
                            internal_error << "attempting to split a SIMD loop\n";
                            break;
                        }
                        case none: {
                            internal_error << "attempting to split a loop with none gpu_label " << is_root() << " num children " << (int)(children.size()) << "\n";
                            break;
                        }
                        case parallelized: {
                            internal_error << "attempting to split a loop with parallelized gpu_label\n";
                            break;
                        }
                    } 
                } 
            }
        }

        if (child >= 0 && !called_by_multiple_children && !in_realization &&
            (may_subtile() || is_root())) {
            // Push the Func further inwards in the loop nest

            // See if it's appropriate to slide over this loop
            // Can't slide at the root level, or no parallelism
            bool may_slide = (params.parallelism == 1) || !is_root();

            // Disable sliding for GPU schedules because it's usually not useful 
            may_slide &= !target.has_gpu_feature();

            const auto &c = children[child];
            int num_ones = 0;
            for (size_t i = 0; i < c->size.size(); i++) {
                int64_t s = c->size[i];
                num_ones += (s == 1) ? 1 : 0;
            }

            // Only slide over single-dimensional loops
            may_slide &= num_ones == ((int)c->size.size() - 1);
            // Don't slide funcs with update stages
            may_slide &= f->stages.size() == 1;

            // Don't slide over the vector dimension
            may_slide &= (c->vectorized_loop_index == -1 ||
                          c->size[c->vectorized_loop_index] == 1);

            for (int store_here = 0; store_here < 2; store_here++) {
                if (store_here && !may_slide) {
                    // We place all our parallel loops at the root
                    // level, so this would constrain parallelism.
                    continue;
                }
                if (is_root() && num_ones == (int)c->size.size() && params.parallelism > 1) {
                    // Don't fuse into serial loops, or we could never parallelize this Func.
                    continue;
                }

                in_threads_loop |= (children[child]->gpu_label == thread);
                auto opts = children[child]->compute_in_tiles(f, this, params, target, v, store_here, in_threads_loop);
                for (IntrusivePtr<const LoopNest> &n : opts) {
                    // (Only valid if one child calls f) Push the
                    // computation into the child. Possibly leaving
                    // the storage out here.
                    LoopNest *r = new LoopNest;
                    r->copy_from(*this);
                    if (store_here) {
                        r->store_at.insert(f);
                    }
                    r->children[child] = n;
                    result.emplace_back(r);
                }
            }
        }

        return result;
    }

    // Note that StageScheduleState is movable-but-not-copyable thanks to its ostringstream member.
    struct StageScheduleState {
        double num_cores = 0; // How much parallelism do we need to exploit with this Func?
        int vector_dim = -1; // Which storage dimension is vectorized? We need to reorder it innermost.
        int vectorized_loop_index = -1;
        struct FuncVar {
            VarOrRVar orig;
            VarOrRVar var;
            string accessor;
            int64_t extent = 0;
            size_t index = 0;
            bool innermost_pure_dim = false, outermost = false, parallel = false, exists = false, pure = false, constant_extent = false;
            bool vectorized = false;
            bool gpu_threads = false;
            FuncVar() : orig(Var()), var(Var()) {}
        };
        const FunctionDAG::Node* node;
        bool parallel = false;
        bool vectorized = false;
        FuncVar vectorized_var;
        vector<FuncVar> vars; // In order from innermost to outermost. Each group of d is one tiling.
        vector<FuncVar> ordered_vars; // In order from innermost to outermost. Each group of d is one tiling.
        vector<int64_t> gpu_thread_extents;
        vector<StageScheduleState*> ancestors; // From outermost in
        std::ostringstream schedule_source;
    };

    void apply(LoopLevel here,
               StageMap<std::unique_ptr<StageScheduleState>> &state_map,
               double num_cores,
               int depth,
               const LoopNest *parent,
               const LoopNest *compute_site,
               const Target& target,
               std::vector<StageScheduleState*>& ancestors) const {
        if (is_root()) {
            for (auto &c : children) {
                Func(c->node->func).compute_root();
                c->apply(LoopLevel::root(), state_map, num_cores, 1, this, c.get(), target, ancestors);
                if (c->stage_idx == 0) {
                    auto &state = state_map.get(c->stage);
                    state->schedule_source << "\n    .compute_root()";
                    // TODO: Omitting logic for printing store_root() assumes everything store_root is also compute root
                }
            }
        } else {
            if (parent && parent->node != node) {
                compute_site = this;
            }

            const auto &symbolic_loop = stage->loop;
            const auto &parent_bounds = parent->get_bounds(node);
            if (!state_map.contains(stage)) {
                StageScheduleState *state = new StageScheduleState;
                state->node = node;
                state->num_cores = num_cores;
                state->vector_dim = vector_dim;
                state->vectorized_loop_index = vectorized_loop_index;
                state->ancestors = ancestors;
                for (size_t i = 0; i < symbolic_loop.size(); i++) {
                    StageScheduleState::FuncVar fv;
                    const auto &l = symbolic_loop[i];
                    fv.var = VarOrRVar(l.var, !l.pure);
                    fv.orig = fv.var;
                    fv.accessor = l.accessor;
                    const auto &p = parent_bounds->loops(stage_idx, i);
                    fv.extent = p.extent();
                    fv.constant_extent = p.constant_extent();
                    fv.outermost = true;
                    fv.parallel = l.pure && parallel;
                    fv.exists = true;
                    fv.pure = l.pure;
                    fv.index = i;
                    fv.innermost_pure_dim = (i == (size_t) vectorized_loop_index);
                    state->vars.push_back(fv);
                }
                // Bubble the innermost pure dimension to the front of the pure dimensions
                for (int i = vectorized_loop_index - 1;
                     i >= 0 && state->vars[i].pure; i--) {
                    std::swap(state->vars[i], state->vars[i+1]);
                }
                state_map.emplace(stage, std::unique_ptr<StageScheduleState>(state));
            }
            auto &state = *(state_map.get(stage));

            // The getter for grabbing Func handles is reverse topological order
            Stage s = Func(node->func);
            if (stage_idx > 0) {
                s = Func(node->func).update(stage_idx - 1);
            }

            if (stage_idx == 0 && parent->node != node) {
                // Pick a memory type
                double bytes = node->bytes_per_point;
                for (int i = 0; i < node->dimensions; i++) {
                    const auto &p = parent_bounds->region_computed(i);
                    bytes *= p.extent();
                }
                if (bytes < 64000 && depth > 2) {
                    // If it's probably a small allocation, and it's
                    // made more than once, use stack-scoped
                    // storage. Otherwise let the compiler pick heap
                    // or stack as it likes.
                    if (!target.has_gpu_feature()) {
                        Func(node->func).store_in(MemoryType::Stack);
                        state.schedule_source << "\n    .store_in(MemoryType::Stack)";
                    }
                }
            }

            // Pick a tail strategy for any splits of pure vars. RVars always use guardwithif
            auto pure_var_tail_strategy = TailStrategy::Auto;
            if (!compute_site->accesses_input_buffer() && !node->is_output) {
                // Roundup is lowest overhead, provided it doesn't
                // expand the bounds read on the input or written on
                // the output. However, you can only really use it on
                // pure stages that don't access the input anywhere in
                // their loop nest.
                pure_var_tail_strategy = TailStrategy::RoundUp;
            } else if (stage_idx == 0) {
                // Pure stages that access the input use shiftinwards
                pure_var_tail_strategy = TailStrategy::ShiftInwards;
            } else {
                // For pure vars in update stages that access the
                // input, it's not safe to round up or redundantly
                // recompute
                pure_var_tail_strategy = TailStrategy::GuardWithIf;
            }

            if (!size.empty()) {
                if (innermost) {
                    if (vectorized_loop_index >= 0) {
                        size_t i = 0;
                        while (!state.vars[i].innermost_pure_dim) i++;
                        auto &v = state.vars[i];
                        internal_assert(v.innermost_pure_dim && v.exists) << v.var.name() << "\n";
                        // Is the result of a split

                        // The vector size for gpu depends on the width of the
                        // stage's types and will often be 1, in which case we
                        // don't want to vectorize the loop
                        if (!target.has_gpu_feature() || stage->vector_size > 1) {
                            state.schedule_source
                                << "\n    .vectorize(" << v.var.name() << ")";
                            s.vectorize(v.var);
                            v.vectorized = true;
                            state.vectorized = true;
                            state.vectorized_var = v;
                        }
                    }
                } else {
                    // Grab the innermost loop for this node
                    const LoopNest *innermost_loop = this, *child = nullptr;
                    while (!innermost_loop->innermost) {
                        for (const auto &c : innermost_loop->children) {
                            if (c->node == node) {
                                if (!child) {
                                    child = c.get();
                                }
                                innermost_loop = c.get();
                                break;
                            }
                        }
                    }

                    // Do the implied splits
                    vector<StageScheduleState::FuncVar> new_inner;
                    for (size_t i = 0; i < symbolic_loop.size(); i++) {
                        StageScheduleState::FuncVar v;
                        StageScheduleState::FuncVar &parent = state.vars[i];

                        parent.gpu_threads = gpu_label == thread && symbolic_loop[i].pure;

                        int64_t factor = (parent.extent + size[parent.index] - 1) / size[parent.index];
                        int64_t innermost_size = innermost_loop->size[parent.index];

                        if (child && parent.innermost_pure_dim) {
                            // Ensure the split is a multiple of the
                            // vector size. With all these rounded
                            // divs going on it can drift.
                            factor = ((factor + innermost_size - 1) / innermost_size) * innermost_size;
                        }

                        if (child && innermost_size > factor) {
                            factor = innermost_size;
                        }

                        if (!parent.exists || factor == 1) {
                            v.exists = false;
                            v.extent = 1;
                        } else if (size[parent.index] == 1 && !(child &&
                                                                child->innermost &&
                                                                parent.innermost_pure_dim &&
                                                                parent.var.name() == parent.orig.name())) {
                            // Not split in this dimension
                            v = parent;
                            v.parallel = false;

                            parent.exists = false;
                            parent.extent = 1;
                        } else {
                            VarOrRVar inner(Var(parent.var.name() + "i"));
                            if (parent.var.is_rvar) {
                                inner = RVar(parent.var.name() + "i");
                            }

                            auto tail_strategy = pure_var_tail_strategy;
                            // If it's an RVar, or not the outermost split and we're in an update, we need a guard with if instead.
                            if (parent.var.is_rvar || (stage_idx != 0 && !parent.outermost)) {
                                tail_strategy = TailStrategy::GuardWithIf;
                            }

                            if (factor > parent.extent && tail_strategy == TailStrategy::ShiftInwards) {
                                // Don't shift all the way off the image.
                                tail_strategy = TailStrategy::GuardWithIf;
                            }

                            s.split(parent.var, parent.var, inner, (int)factor, tail_strategy);
                            state.schedule_source
                                << "\n    .split("
                                << parent.var.name() << ", "
                                << parent.var.name() << ", "
                                << inner.name() << ", "
                                << factor << ", "
                                << "TailStrategy::" << tail_strategy << ")";
                            v = parent;
                            parent.extent = size[parent.index];
                            v.constant_extent = (tail_strategy != TailStrategy::GuardWithIf);
                            v.var = inner;
                            v.accessor.clear();
                            v.extent = factor;
                            v.parallel = false;
                            v.gpu_threads = false;
                            v.outermost = false;
                        }
                        new_inner.push_back(v);
                    }

                    if (child->innermost) {
                        // Maybe do some unrolling

                        int64_t product_of_pure_loops = 1;
                        bool all_pure_loops_constant_size = true;
                        for (size_t i = 0; i < symbolic_loop.size(); i++) {
                            if (state.vars[i].pure) {
                                product_of_pure_loops *= state.vars[i].extent;
                                all_pure_loops_constant_size &= state.vars[i].constant_extent;
                            }
                        }

                        if (product_of_pure_loops <= kUnrollLimit && all_pure_loops_constant_size) {
                            // There's a hope we can fit anything compute-at this level into registers if we fully unroll
                            // TODO: 16 should be the number of vector registers in the architecture
                            std::stable_sort(state.vars.begin(), state.vars.begin() + symbolic_loop.size(),
                                             [](const StageScheduleState::FuncVar &a, const StageScheduleState::FuncVar &b) {
                                                 return a.pure && !b.pure;
                                             });

                            for (size_t i = 0; i < symbolic_loop.size(); i++) {
                                if (state.vars[i].pure && state.vars[i].exists && state.vars[i].extent > 1) {
                                    s.unroll(state.vars[i].var);
                                    state.schedule_source << "\n    .unroll(" << state.vars[i].var.name() << ")";
                                }
                            }
                        }
                    }

                    bool found = false;
                    for (const auto &v : state.vars) {
                        if (!v.exists) continue;
                        here = LoopLevel(node->func, v.var);
                        found = true;
                        break;
                    }
                    if (!found) {
                        here = LoopLevel(node->func, Var::outermost());
                    }
                    // internal_assert(found) << "Could not find appropriate compute_at location for children of " << node->func.name() << "\n";
                    state.vars.insert(state.vars.begin(), new_inner.begin(), new_inner.end());
                }
            }
            if (innermost) {
                internal_assert(store_at.empty());
                internal_assert(children.empty());
                return;
            }


            for (auto f : store_at) {
                Func(f->func).store_at(here);
            }
            for (auto s : size) {
                num_cores /= s;
            }
            here.lock();
            string loop_level;
            if (here.is_root()) {
                loop_level = "_root()";
            } else {
                loop_level = "_at(" + here.func() + ", " + here.var().name() + ")";
            }
            for (auto &c : children) {
                if (c->node != node) {
                    Func(c->node->func).compute_at(here);
                }
                ancestors.push_back(state_map.get(stage).get());
                c->apply(here, state_map, num_cores, depth + 1, this, compute_site, target, ancestors);
                ancestors.pop_back();
                if (c->node != node && c->stage_idx == 0) {
                    auto &state = *(state_map.get(c->stage));
                    state.schedule_source << "\n    .compute" << loop_level;
                }
            }
            for (auto f : store_at) {
                bool computed_here = false;
                for (auto &c : children) {
                    if (c->node == f) {
                        computed_here = true;
                        break;
                    }
                }
                if (!computed_here) {
                    auto &state = *(state_map.get(&(f->stages[0])));
                    state.schedule_source << "\n    .store" << loop_level;
                }
            }
        }
    }

};

struct State {
    mutable RefCount ref_count;
    IntrusivePtr<const LoopNest> root;
    IntrusivePtr<const State> parent;
    double cost = 0;
    int num_funcs_scheduled = 0;
    bool penalized = false;

    State() = default;
    State(const State &) = delete;
    State(State &&) = delete;
    void operator=(const State &) = delete;
    void operator=(State &&) = delete;

    static int cost_calculations;

    uint64_t structural_hash(int depth, int parallelism) const {
        uint64_t h = num_funcs_scheduled;
        internal_assert(root.defined());
        root->structural_hash(h, depth, parallelism);
        return h;
    }

    // Compute the parent and depth of every loop nest node
    void compute_loop_nest_parents(map<const LoopNest *, pair<const LoopNest *, int>> &p,
                                   const LoopNest *here, int depth) {
        for (const auto &c : here->children) {
            p.emplace(c.get(), pair<const LoopNest *, int>{here, depth});
            compute_loop_nest_parents(p, c.get(), depth+1);
        }
    }

    const LoopNest *deepest_common_ancestor(const map<const LoopNest *, pair<const LoopNest *, int>> &parent,
                                            const LoopNest *a, const LoopNest *b) {
        if (a->is_root()) return a;
        if (b->is_root()) return b;
        if (a == b) return a;

        // Walk the deeper one up until they're at the same depth
        auto it_a = parent.find(a);
        auto it_b = parent.find(b);
        internal_assert(it_a != parent.end() && it_b != parent.end());
        while (it_a->second.second > it_b->second.second) {
            a = it_a->second.first;
            it_a = parent.find(a);
        }
        while (it_b->second.second > it_a->second.second) {
            b = it_b->second.first;
            it_b = parent.find(b);
        }

        while (1) {
            // Walk each up one
            a = it_a->second.first;
            b = it_b->second.first;
            if (a == b) return a;
            it_a = parent.find(a);
            it_b = parent.find(b);
            internal_assert(it_a != parent.end() && it_b != parent.end());
        }

        // unreachable
        return nullptr;
    }

    void compute_featurization(const FunctionDAG &dag, const MachineParams &params, StageMap<ScheduleFeatures> *features) {
        StageMap<LoopNest::Sites> sites;
        sites.make_large(dag.nodes[0].stages[0].max_id);
        features->make_large(dag.nodes[0].stages[0].max_id);
        internal_assert(root.defined());
        root->get_sites(sites);

        // For the input nodes and unscheduled outputs, the compute
        // and store sites are root, and the produce and innermost
        // sites are unset (nullptr)
        for (const auto &n : dag.nodes) {
            if (n.is_input || n.is_output) {
                for (const auto &stage : n.stages) {
                    auto &s = sites.get_or_create(&stage);
                    if (s.compute == nullptr) {
                        s.compute = root.get();
                        s.store = root.get();
                    }
                }
            }
        }

        // For the unscheduled nodes, give them sites as deep as they
        // could possibly be. We'll ignore the possibility of inlining
        // them for now.
        map<const LoopNest *, pair<const LoopNest *, int>> parent;
        compute_loop_nest_parents(parent, root.get(), 0);
        for (const auto &n : dag.nodes) {
            if (sites.contains(&(n.stages[0]))) {
                continue;
            }
            const LoopNest *loop = nullptr;
            for (const auto *e : n.outgoing_edges) {
                const auto &consumer_site = sites.get(e->consumer);
                const LoopNest *l = consumer_site.innermost;
                if (!l) l = consumer_site.compute;
                if (!l) {
                    dump();
                    internal_error << e->producer->func.name() << " -> " << e->consumer->name << "\n";
                }
                if (loop) {
                    loop = deepest_common_ancestor(parent, l, loop);
                } else {
                    loop = l;
                }
            }
            internal_assert(loop)
                << "Could not compute plausible site for unscheduled Func: "
                << n.func.name() << "\n";
            for (auto &stage : n.stages) {
                auto &site = sites.get_or_create(&stage);
                site.compute = loop;
                site.store = loop;
            }
        }

        root->compute_features(dag, params, sites, 1, 1, nullptr, nullptr, *root, nullptr, features);

        for (const auto &n : dag.nodes) {
            if (sites.get(&(n.stages[0])).produce == nullptr) {
                internal_assert(!features->contains(&(n.stages[0])))
                    << "Somehow an input or unscheduled node ended up in the featurization: "
                    << n.func.name() << "\n";
            }
        }
    }

    void save_featurization(const FunctionDAG &dag, const MachineParams &params, const std::string &feature_file) {
        StageMap<ScheduleFeatures> features;
        compute_featurization(dag, params, &features);

        std::ofstream binfile(feature_file, std::ios::binary | std::ios_base::trunc);
        for (const auto &n : dag.nodes) {
            if (n.is_input) continue;
            for (size_t stage_idx = n.stages.size(); stage_idx > 0; stage_idx--) {
                const auto &s = n.stages[stage_idx - 1];
                const size_t num_schedule_features = ScheduleFeatures::num_features();
                const size_t num_pipeline_features = PipelineFeatures::num_features();
                const auto &sched_feat = features.get(&s);

                float buf[num_schedule_features + num_pipeline_features];
                // Save them as floats
                for (size_t i = 0; i < num_schedule_features; i++) {
                    buf[i] = sched_feat[i];
                }

                for (size_t i = 0; i < num_pipeline_features; i++) {
                    buf[i + num_schedule_features] = s.features[i];
                }

                binfile.write((const char *)buf, sizeof(buf));
            }
        }
        binfile.close();
        internal_assert(!binfile.fail()) << "Failed to write " << feature_file;
    }

    bool contains_store_at(const set<const FunctionDAG::Node *>& outermost_store_at, const IntrusivePtr<const LoopNest>& parent) {
        for (const auto& c : parent->children) {
            if (c->store_at.size() > 0) {
                return true;
            }

            // At production for c: if not store_at root or outermost, then it
            // must implicitly be store_at parent's level, so reject it
            bool at_production = c->node != parent->node;
            if (at_production && root->store_at.count(c->node) == 0 && outermost_store_at.count(c->node) == 0) {
                return true;
            }

            if (contains_store_at(outermost_store_at, c)) {
                return true;
            }
        }

        return false;
    }

    // For GPU, only allow store_at root or inside the outermost loop nest. Any
    // store_ats further in will be hoisted and expanded, increasing the
    // amount of shared memory required.
    bool contains_store_at_further_in_than_outermost() {
        for (const auto& child : root->children) {
            for (const auto& grandchild : child->children) {
                if (contains_store_at(child->store_at, grandchild)) {
                    return true;
                }
            }
        }
        return false;
    }

    std::pair<int64_t, int64_t> working_set_total(const StageMap<ScheduleFeatures>& features, const IntrusivePtr<const LoopNest>& loop_nest) {
        int64_t working_set_r = 0;
        int64_t working_set_p = 0;
        for (const auto* n : loop_nest->store_at) {
            working_set_r += features.get(&(n->stages[0])).bytes_at_realization;
            working_set_p += features.get(&(n->stages[0])).bytes_at_production;
        }

        for (const auto& c : loop_nest->children) {
            auto result = working_set_total(features, c);
            working_set_r += result.first;
            working_set_p += result.second;
        }

        return {working_set_r, working_set_p};
    }

    bool exceeds_shared_memory_limit(const StageMap<ScheduleFeatures>& features, const Target &target) {
        if (!target.has_gpu_feature()) {
            return false;
        }

        static int64_t limit = get_shared_memory_limit();

        if (limit == 0) {
            return false;
        }

        for (const auto& c : root->children) {
            // If the working set is too large on the GPU, shared memory will be
            // exhausted, so reject any such schedules
            auto result = working_set_total(features, c);
            if (result.first > limit) {
                return true;
            }
        }

        return false;
    }

    bool calculate_cost(const FunctionDAG &dag, const MachineParams &params, const Target& target, CostModel *cost_model, bool verbose = false) {
        StageMap<ScheduleFeatures> features;
        compute_featurization(dag, params, &features);

        cost = 0;

        if (verbose) {
            for (auto it = features.begin(); it != features.end(); it++) {
                auto &stage = *(it.key());
                const auto &feat = it.value();
                debug(0) << "Schedule features for " << stage.stage.name() << "\n";
                feat.dump();
            }
        }

        if (exceeds_shared_memory_limit(features, target)) {
            return false;
        }

        // use either deep network or linear model to predict cost
        if (cost_model) {

            // Perform any quick rejection tests before enqueuing this
            for (auto it = features.begin(); it != features.end(); it++) {
                auto &feat = it.value();
                if (!it.key()->node->is_wrapper) { // It's OK to repeatedly stage data
                    if (feat.points_computed_total + feat.inlined_calls > 8 * feat.points_computed_minimum) {
                        cost = 1e50;
                        return true; //false;
                    } else if (false && feat.points_computed_total + feat.inlined_calls < feat.points_computed_minimum) {
                        // Some kind of shift-invariance failure
                        // probably. The representative loop iteration
                        // picked does a lot less than the average
                        // amount of work that must be done by a loop
                        // iteration, assuming no redundant work is
                        // done, so none of the features can be
                        // trusted. A benign reason for this to occur
                        // is if you either need 1 or 2 values from a
                        // func depending on the loop iteration, and
                        // we were unlucky and picked one of the
                        // iterations that only required 1 value as
                        // the representative one.
                        /*
                        user_warning << "Shift invariance failure for " << it.key()->name
                                     << ", discarding potential schedule: "
                                     << feat.points_computed_total
                                     << " " << feat.inlined_calls
                                     << " " << feat.points_computed_minimum << "\n";
                        dump();
                        */
                        cost = 1e50;
                        return false;
                    }
                }
            }

            // Avoid code size explosion from recursive inlining.
            if (root->max_inlined_calls() >= 256) {
                cost = 1e50;
                return false;
            }

            int num_stages = (int)features.size();

            Runtime::Buffer<float> schedule_features;

            // Won't actually run anything until we call evaluate_costs...
            cost_model->enqueue(num_stages, &schedule_features, &cost);

            // index of current stage whose features we are reading
            int stage = 0;
            // load schedule features into input buffer
            for (const auto &n : dag.nodes) {
                if (n.is_input) continue; // Inputs are computed outside of the pipeline and don't count.
                if (stage >= num_stages) break;
                for (auto it = n.stages.rbegin(); it != n.stages.rend(); it++) {
                    internal_assert(features.contains(&*it)) << n.func.name() << "\n";
                    const auto &feat = features.get(&*it);
                    for (size_t i = 0; i < ScheduleFeatures::num_features(); i++) {
                        schedule_features(i, stage) = feat[i];
                    }
                    stage += 1;
                }
            }
            internal_assert(stage == num_stages);
        } else {
            // We have no throughput predictor.
            for (auto it = features.begin(); it != features.end(); it++) {
                auto &stage = *(it.key());
                const auto &feat = it.value();

                // Reject silly schedules. They're not even useful for
                // training data, as they potentially take the age of
                // the universe to benchmark.
                if (feat.points_computed_total + feat.inlined_calls > 1000*feat.points_computed_minimum) return false;
                if (feat.inlined_calls >= 64) return false;

                double compute_cost = 0;
                const int *pipeline_feat = (const int *)(&stage.features.op_histogram[0][0]);
                double per_element_compute_cost = 0;
                for (size_t i = 0; i < sizeof(stage.features.op_histogram) / sizeof(int); i++) {
                    per_element_compute_cost += pipeline_feat[i];
                }

                if (feat.inlined_calls > 0) {
                    const double per_element_compute_cost_of_memcpy = 1 + 2*stage.node->dimensions;
                    per_element_compute_cost = std::max(0.0, per_element_compute_cost - per_element_compute_cost_of_memcpy);
                }

                // Assume that narrow types are cheaper because they
                // vectorize wider, and just count the number of
                // vectors computed.
                compute_cost = per_element_compute_cost * (feat.num_vectors + feat.num_scalars);

                // Figure out vector overcompute
                const int native_vector_size = feat.native_vector_size;
                const double idle_simd_lanes = (double)native_vector_size / feat.vector_size;
                compute_cost *= idle_simd_lanes;

                {
                    // Few parallel tasks may be a bad idea due to
                    // waiting for the long pole to finish.  Say
                    // we have a huge number of tasks relative to
                    // cores. We'd expect their start times to
                    // eventually become evenly spaced, which
                    // means we get a little triangle of idle
                    // cores with total area 0.5 * task_size *
                    // num_cores at the end. This bloats the total
                    // amount of work by:
                    //   (0.5 * task_size * num_cores + task_size * num_tasks) / (task_size * num_tasks)
                    // = (0.5 * num_cores + num_tasks) / num_tasks

                    internal_assert(feat.inner_parallelism > 0 && feat.outer_parallelism > 0);

                    const double num_tasks = feat.inner_parallelism;
                    const double num_cores = (double)params.parallelism / feat.outer_parallelism;
                    double idle_core_wastage = (0.5 * num_cores + num_tasks) / num_tasks;

                    // Evaluated at num_tasks = num_cores, this
                    // gives a ridiculous 1.5x multiplier. Our
                    // argument doesn't hold because the tasks
                    // start synchronized. Just cap it at 20%
                    // wastage.
                    idle_core_wastage = std::min(idle_core_wastage, 1.2);

                    if (verbose) {
                        debug(0) << "idle_core_wastage_1 = " << idle_core_wastage << "\n";
                    }

                    // Cores can also be idle if the number of
                    // tasks is small and not a multiple of the
                    // number of cores. E.g. 9 tasks on 8 cores
                    // takes about the same amount of time as 16
                    // tasks.
                    idle_core_wastage *= std::ceil(num_tasks / num_cores) * (num_cores / num_tasks);

                    compute_cost *= idle_core_wastage;

                    if (verbose) {
                        debug(0) << "idle_core_wastage_2 = " << idle_core_wastage << "\n";
                    }
                }

                double cold_cache_misses = 0, cost_of_cold_miss = 0, capacity_cache_misses = 0, cost_of_capacity_miss = 0;
                if (feat.inlined_calls == 0) {
                    // Estimate the number of cold cache misses on the data that this reads from and their cost
                    // Cost dominated by lines not bytes due to streaming prefetchers
                    cold_cache_misses = (feat.unique_lines_read_per_realization +
                                         feat.unique_bytes_read_per_realization * 1e-3);

                    cold_cache_misses *= feat.num_realizations;
                    //int64_t footprint = std::min(feat.allocation_bytes_read_per_realization, feat.bytes_read_per_realization);
                    int64_t footprint = feat.allocation_bytes_read_per_realization;
                    //cost_of_miss = std::sqrt(footprint) * 40 * 5e-3;
                    cost_of_cold_miss = footprint * 40 * 1e-4;

                    // Now estimate the number of capacity-related cache misses using the total number of bytes read.

                    // We have a number of unique bytes read. Call the
                    // cache level large enough to fit it L(n+1). The
                    // next cache level in is Ln. How many misses will
                    // we incur in Ln? If we load randomly within the
                    // footprint, we'll miss some constant fraction of
                    // the time. The cost of such a miss is the cost
                    // of going out to cache level L(n+1). Note that
                    // *cold* misses, by contrast, go out to the cache
                    // level that fits the entire source allocation,
                    // not just the footprint accessed of it.
                    capacity_cache_misses = feat.num_vectors * (feat.vector_loads_per_vector + feat.scalar_loads_per_vector);
                    capacity_cache_misses += feat.num_scalars * feat.scalar_loads_per_scalar;
                    capacity_cache_misses *= 1e-2;
                    cost_of_capacity_miss = feat.unique_bytes_read_per_realization * 40 * 1e-4;

                    // We'll assume multiway caches work well and ignore the other 'C' (conflict cache misses).
                }

                double memory_load_cost = cold_cache_misses * cost_of_cold_miss + capacity_cache_misses * cost_of_capacity_miss;

                double cache_misses = 0, cost_of_miss = 0;
                if (feat.inlined_calls == 0) {
                    // Estimate the number of cache misses on the data that this writes to and their cost
                    int64_t lines_written_per_realization = feat.inner_parallelism * (feat.bytes_at_task / feat.innermost_bytes_at_task);
                    cache_misses = 1e1 * lines_written_per_realization + feat.bytes_at_realization * 1e-2;
                    cache_misses *= feat.num_realizations;
                    //cost_of_miss = std::sqrt(feat.bytes_at_production) * 40 * 5e-3;
                    cost_of_miss = feat.bytes_at_production * 40 * 2e-6;
                }

                double memory_store_cost = cache_misses * cost_of_miss;

                // Penalize writing partial cache lines. Assume a cache line is two simd vectors.
                const double native_cache_line_size = 2 * idle_simd_lanes; // two full vectors
                const double cache_line_wastage = std::max(1.0, native_cache_line_size / feat.innermost_pure_loop_extent);
                memory_store_cost *= cache_line_wastage;

                // Malloc aint free. Small allocations should go on the stack, but this isn't totally reliable.
                double cost_of_mallocs = feat.num_realizations * 1e2;

                // Penalize working sets that start to fall out of cache
                double ws = 1e-6 * feat.working_set;
                double cost_of_working_set = ws * ws * ws * 40 * feat.num_realizations;

                if (verbose) {
                    debug(0) << "Cost model for " << stage.stage.name() << " "
                             << compute_cost << " + "
                             << memory_load_cost << " + "
                             << memory_store_cost << " + "
                             << cost_of_mallocs << " + "
                             << cost_of_working_set << '\n';
                }

                cost += compute_cost + memory_load_cost + memory_store_cost + cost_of_mallocs + cost_of_working_set;
            }
        }
        cost_calculations++;
        return true;
    }

    IntrusivePtr<State> make_child() const {
        State *s = new State;
        s->parent = this;
        s->root = root;
        s->cost = cost;
        s->num_funcs_scheduled = num_funcs_scheduled;
        return s;
    }

    IntrusivePtr<State> random_child(const FunctionDAG &dag,
                                     const MachineParams &params,
                                     const Target &target,
                                     std::mt19937 &rng) const {
        int count = 0;
        IntrusivePtr<State> child;
        std::function<void(IntrusivePtr<State> &&)> accept = [&](IntrusivePtr<State> &&candidate) {
            count++;
            if (rng() % count == 0) {
                child = std::move(candidate);
            }
        };

        generate_children(dag, params, target, nullptr, accept);
        return child;
    }

    void generate_children(const FunctionDAG &dag,
                           const MachineParams &params,
                           const Target &target,
                           CostModel *cost_model,
                           std::function<void(IntrusivePtr<State> &&)> &accept_child) const {

        internal_assert(root.defined() && root->is_root());

        if (num_funcs_scheduled == 2*(int)dag.nodes.size()) {
            return;
        }

        int next_node = num_funcs_scheduled / 2;
        int phase = num_funcs_scheduled % 2;

        if (!may_subtile()) {
            // When emulating the older search space, we do all
            // parallelizing last, so that it is independent of the
            // tiling decisions.
            next_node = num_funcs_scheduled % dag.nodes.size();
            phase = num_funcs_scheduled / dag.nodes.size();
        }

        // Enumerate all legal ways to schedule the next Func
        const FunctionDAG::Node *node = &dag.nodes[next_node];
        for (const auto *e : node->outgoing_edges) {
            internal_assert(root->computes(e->consumer->node))
                << "Partially scheduled code doesn't compute " << e->consumer->name
                << ", which is one of the consumers of " << node->func.name();
        }

        if (node->is_input) {
            // We don't need to schedule nodes that represent inputs,
            // and there are no other decisions to be made about them
            // at this time.
            // debug(0) << "Skipping over scheduling input node: " << node->func.name() << "\n";
            auto child = make_child();
            child->num_funcs_scheduled++;
            accept_child(std::move(child));
            return;
        }

        if (!node->outgoing_edges.empty() && !root->calls(node)) {
            debug(0) << "In state:\n";
            dump();
            debug(0) << node->func.name() << " is consumed by:\n";
            for (const auto *e : node->outgoing_edges) {
                debug(0) << e->consumer->name << "\n";
                debug(0) << "Which in turn consumes:\n";
                for (const auto *e2 : e->consumer->incoming_edges) {
                    debug(0) << "  " << e2->producer->func.name() << "\n";
                }
            }
            internal_error << "Pipeline so far doesn't use next Func: " << node->func.name() << '\n';
        }

        int num_children = 0;

        if (phase == 0) {
            // Injecting realizations
            {
                // 1) Inline it
                if (node->stages.size() == 1 && !node->is_output) {
                    auto child = make_child();
                    LoopNest *new_root = new LoopNest;
                    new_root->copy_from(*root);
                    new_root->inline_func(node);
                    child->root = new_root;
                    child->num_funcs_scheduled++;
                    // TODO: filter children here instead of calculating the cost of children we don't want.
                    if (child->calculate_cost(dag, params, target, cost_model)) {
                        internal_assert(child->root->computes(node)) << "Failed to inline " << node->func.name() << '\n';
                        num_children++;
                        accept_child(std::move(child));
                    } else {
                        // Discarding state....
                    }
                }
            }

            // Some search-space pruning. If a node is pointwise, and
            // so are all its inputs and so is its sole output, and
            // inlining it is legal, just inline it. This saves time
            // on long chains of pointwise things.
            bool must_inline = node->is_pointwise && (num_children > 0) && (node->outgoing_edges.size() == 1);
            if (must_inline) {
                for (const auto *e : node->stages[0].incoming_edges) {
                    must_inline &= e->producer->is_pointwise;
                }
                for (const auto *e : node->outgoing_edges) {
                    must_inline &= (e->consumer->node->is_pointwise || e->consumer->node->is_boundary_condition);
                }
                if (must_inline) {
                    return;
                }
            }

            // Construct a list of plausible dimensions to vectorize over
            // TODO: Pre-prune the list of sane dimensions to
            // vectorize a Func over to reduce branching factor.
            vector<int> vector_dims;
            for (int v = 0; v < node->dimensions; v++) {
                const auto &p = root->get_bounds(node)->region_computed(v);
                if ((node->is_output && v == 0) || p.extent() >= node->vector_size) {
                    vector_dims.push_back(v);
                }
            }

            if (vector_dims.empty()) {
                vector_dims.push_back(0);
            }

            // HACK: May only vectorize across x, if there is one
            /*
            for (int v = 0; v < node->dimensions; v++) {
                if (node->func.args()[v] == "x") {
                    vector_dims.clear();
                    vector_dims.push_back(v);
                    break;
                }
            }
            */

            // 2) Realize it somewhere
            for (int vector_dim : vector_dims) {
                // Outputs must be vectorized over their innermost
                // dimension, because we don't have control of the
                // storage. TODO: Go inspect to see which dimension has a
                // stride==1 constraint instead of assuming 0.
                if (vector_dim > 0 && (node->is_output || node->is_input)) break;

                auto tile_options = root->compute_in_tiles(node, nullptr, params, target, vector_dim, false, false);
                for (IntrusivePtr<const LoopNest> &n : tile_options) {
                    auto child = make_child();
                    child->root = std::move(n);
                    child->num_funcs_scheduled++;
                    if (child->calculate_cost(dag, params, target, cost_model)) {
                        internal_assert(child->root->computes(node)) << "Failed to inject realization of " << node->func.name() << '\n';
                        num_children++;
                        accept_child(std::move(child));
                    }
                }
            }
        } else { // second phase, parallelize compute root funcs 
            bool should_parallelize = false;
            const vector<int64_t> *pure_size = nullptr;
            if (params.parallelism > 1) {
                for (auto &c : root->children) {
                    if (c->node == node && node->dimensions > 0) {
                        if (c->stage->index == 0) {
                            pure_size = &(c->size);
                        }
                        should_parallelize = true;
                    }
                }
            }
            if (!should_parallelize) {
                // The Func must not be compute_root, so just return a copy of the parent state
                num_children++;
                auto child = make_child();
                child->num_funcs_scheduled++;
                accept_child(std::move(child));
            } else {
                // Sort / filter the options for the outermost parallel (CPU) or block (GPU) loop tilings
                struct Option {
                    vector<int64_t> tiling;
                    double idle_core_wastage;
                    bool entire;
                    bool operator<(const Option &other) const {
                        return idle_core_wastage < other.idle_core_wastage;
                    }
                };

                internal_assert(pure_size);

                if (target.has_gpu_feature()) {
                    // When GPU scheduling we approach tiling differently and in two steps.
                    // step 1) convert (none, SIMD) loops to (parallel, serial, SIMD) loops with specialized serial sizes
                    auto parallel_tilings = generate_serial_tilings(*pure_size, node->dimensions-1, gpu_serial_sizes);
                    internal_assert(parallel_tilings.size() > 0) << " zero parallel tilings\n";

                    for (auto &parallel_t: parallel_tilings) {
                        debug(0) << "parallelizing " << node->func.name() << " into PARALLEL, SERIAL\n";
                        LoopNest *parallel_root = new LoopNest;
                        parallel_root->copy_from(*root);

                        vector<int64_t> max_size = parallel_root->get_union_thread_counts(node);
                        // step 1) parallelize all loop nests for this node into (parallel, serial) with given serial tiles
                        for (auto &c : parallel_root->children) {
                            if (c->node == node) { // c is a reference to a IntrusivePtr<const LoopNest>
                                c = c->parallelize_in_tiles(params, parallel_t, parallel_root, target);
                            }
                        }
                        // step 2) split all parallel loops for this node into to (blocks, thread) loop
                        const vector<int64_t> *parallel_size = nullptr;
                        int vector_dim = -1;
                        for (auto &c : parallel_root->children) {
                            if (c->node == node) {
                                if (c->stage->index == 0) {
                                    parallel_size = &(c->size);
                                    vector_dim = c->vector_dim;
                                }
                            }
                        }
                
                        auto block_tilings = 
                            generate_gpu_tilings(*parallel_size, max_size, node->dimensions-1, 
                                                node->dimensions-1, vector_dim, true);

                        debug(0) << "parallelizing " << node->func.name() << " into BLOCK, THREAD. NUMBER OF BLOCK TILINGS " << block_tilings.size() << "\n";

                        // If no options, create a thread tiling as large as possible with block size (1,1,1).
                        // This can happen if the loops are too small to generate desired gpu tiles.
                        if (block_tilings.empty()) {
                            debug(0) << "no block tilings, creating max possible block size\n";
                            auto child = make_child();
                            LoopNest *new_root = new LoopNest;
                            new_root->copy_from(*parallel_root); 
                            for (auto &c : new_root->children) {
                                if (c->node == node) {
                                    vector<int64_t> tiling((int)(c->size.size()), 1);
                                    c = c->parallelize_in_tiles(params, tiling, new_root, target);
                                    debug(0) << "parallelizing " << node->func.name() << " into block, thread which has gpu label " << c->gpu_label << "\n";
                                    debug(0) << "new loops for " << node->func.name() << " has gpu_label " << c->gpu_label << "\n";
                                }
                            }
                            child->root = new_root;
                            child->num_funcs_scheduled++;
                            if (child->calculate_cost(dag, params, target, cost_model)) {
                                num_children++;
                                accept_child(std::move(child));
                            }
                            return;
                        }

                        for (const auto &block_t : block_tilings) {
                            auto child = make_child();
                            LoopNest *new_root = new LoopNest;
                            new_root->copy_from(*parallel_root); // copies parallel_root's info and intrusive pointers for parallel_root's children
                            for (auto &c : new_root->children) {
                                if (c->node == node) {
                                    c = c->parallelize_in_tiles(params, block_t, new_root, target);
                                }
                            }
                            child->root = new_root;
                            child->num_funcs_scheduled++;
                            if (child->calculate_cost(dag, params, target, cost_model)) {
                                num_children++;
                                accept_child(std::move(child));
                            }
                        }
                        delete parallel_root;
                    }
                } else { // scheduling for CPU, just do regular tilings
                    // Deciding on parallel task size/shape.
                    auto tilings = generate_tilings(*pure_size, node->dimensions - 1, 2, true, target);
                    // We could just parallelize the outer loop entirely
                    std::vector<int64_t> ones;
                    ones.resize(pure_size->size(), 1);
                    tilings.emplace_back(std::move(ones));
                          
                    vector<Option> options;
                    for (size_t i = 0; i < tilings.size(); i++) {
                        auto &t = tilings[i];
                        Option o;
                        o.entire = (i == tilings.size() - 1);
                        
                        for (size_t j = 0; j < pure_size->size(); j++) {
                            t[j] = ((*pure_size)[j] + t[j] - 1) / t[j];
                        }
                        
                        t.swap(o.tiling);

                        // Compute max idle cores across the other stages of the Func
                        int64_t min_total = 0, max_total = 0;
                        o.idle_core_wastage = 1;
                        for (const auto &c : root->children) {
                            if (c->node == node) {
                                int64_t total = 1;
                                for (auto &l : c->stage->loop) {
                                    if (!l.rvar) {
                                        total *= o.tiling[l.pure_dim];
                                    }
                                }
                                if (min_total != 0) {
                                    min_total = std::min(min_total, total);
                                } else {
                                    min_total = total;
                                }
                                max_total = std::max(max_total, total);
                                const double tasks_per_core = ((double)total) / params.parallelism;
                                o.idle_core_wastage = std::max(o.idle_core_wastage,
                                                               std::ceil(tasks_per_core) /
                                                               tasks_per_core);
                            }
                        }

                        // Filter out the less useful options
                        bool ok =
                            ((o.entire || min_total >= params.parallelism) &&
                             (max_total <= params.parallelism * 16 || target.has_gpu_feature()));

                        if (!ok) continue;

                        options.emplace_back(std::move(o));
                    }
                    std::sort(options.begin(), options.end());

                    // If none of the options were acceptable, don't
                    // parallelize. This tends to happen for things like
                    // compute_root color matrices.
                    if (options.empty()) {
                        num_children++;
                        auto child = make_child();
                        child->num_funcs_scheduled++;
                        accept_child(std::move(child));
                        return;
                    }

                    for (const auto &o : options) {
                        if (num_children >= 1 && (o.idle_core_wastage > 1.2 || !may_subtile())) {
                            // We have considered several options, and the
                            // remaining ones leave lots of cores idle.
                            break;
                        }

                        auto child = make_child();
                        LoopNest *new_root = new LoopNest;
                        new_root->copy_from(*root);
                        for (auto &c : new_root->children) {
                            if (c->node == node) {
                                if (may_subtile()) {
                                    c = c->parallelize_in_tiles(params, o.tiling, new_root, target);
                                } else {
                                    // Emulate the single parallelization
                                    // option from the old autoscheduler's
                                    // search space - just keep
                                    // parallelizing outer loops until
                                    // enough are parallel.
                                    vector<int64_t> tiling = c->size;
                                    int64_t total = 1;
                                    for (size_t i = c->size.size(); i > 0; i--) {
                                        if (!c->stage->loop[i-1].pure || total >= params.parallelism) {
                                            tiling[i-1] = 1;
                                        }
                                        while (tiling[i-1] > 1 &&
                                               total * tiling[i-1] > params.parallelism * 8) {
                                            tiling[i-1] /= 2;
                                        }
                                        total *= tiling[i-1];
                                    }
                                    c = c->parallelize_in_tiles(params, tiling, new_root, target);
                                }
                            }
                        }
                        child->root = new_root;
                        child->num_funcs_scheduled++;
                        if (child->calculate_cost(dag, params, target, cost_model)) {
                            num_children++;
                            accept_child(std::move(child));
                        }
                    }
                }
            }
        }


        if (num_children == 0) {
            debug(0) << "Warning: Found no legal way to schedule "
                     << node->func.name() << " in the following State:\n";
            dump();
            // internal_error << "Aborted";
        }

    }

    void dump() const {
        debug(0) << "State with cost " << cost << ":\n";
        root->dump("", nullptr);
        debug(0) << schedule_source;
    }

    string schedule_source;

    void mark_gpu_blocks(LoopNest::StageScheduleState* state, Stage& stage, const vector<VarOrRVar>& parallel_vars) {
        uint8_t n_loops_tagged_gpu_blocks = 0;

        for (auto& v : parallel_vars) {
            if (n_loops_tagged_gpu_blocks >= 3) {
                break;
            }

            state->schedule_source << "\n    .gpu_blocks(" << v.name() << ")";
            stage.gpu_blocks(v);
            n_loops_tagged_gpu_blocks++;
        }
    }

    void mark_gpu_threads(LoopNest::StageScheduleState* state, Stage& stage) {
        uint8_t num_loops_tagged_gpu_thread = 0;
        int64_t total_threads = 1;
        int max_threads[3] = {1024, 1024, 64};

        for (const auto& v : state->vars) {
            if (!v.exists || !v.gpu_threads)  {
                continue;
            }

            if (num_loops_tagged_gpu_thread >= 3 || total_threads >= MAX_THREADS_PER_BLOCK || v.extent > max_threads[num_loops_tagged_gpu_thread]) {
                break;
            }

            state->schedule_source << "\n    .gpu_threads(" << v.var.name() << ")";
            stage.gpu_threads(v.var);
            num_loops_tagged_gpu_thread++;
        }
    }

    bool can_fuse_gpu(const vector<int64_t>& parallel_extents) {
        int64_t total = 1;
        for (auto extent : parallel_extents) {
            total *= extent;
        }

        // Max grid size
        return total < 65535;
    }

    void apply_schedule(const FunctionDAG &dag, const MachineParams &params, const Target &target) {
        StageMap<std::unique_ptr<LoopNest::StageScheduleState>> state_map;
        std::vector<LoopNest::StageScheduleState*> ancestors;

        root->apply(LoopLevel::root(), state_map, params.parallelism, 0, nullptr, nullptr, target, ancestors);

        std::ostringstream src;

        // Print handles for all the Funcs
        int i = (int)(dag.nodes.size() - 1);
        for (const auto &n : dag.nodes) {
            if (!n.is_input) {
                src << "Func " << n.func.name() << " = get_pipeline().get_func(" << i << ");\n";
            }
            i--;
        }

        // Gather all Vars and RVars so that we can declare them in the emitted source
        map<string, string> vars, rvars;
        for (auto &p : state_map) {
            for (auto &v : p.second->vars) {
                if (v.exists) {
                    if (v.var.is_rvar) {
                        rvars.emplace(v.var.name(), v.accessor);
                    } else {
                        vars.emplace(v.var.name(), v.accessor);
                    }
                }
            }
        }
        if (!vars.empty()) {
            string prefix = "Var ";
            for (const auto &p : vars) {
                if (p.second.empty()) {
                    src << prefix << p.first << "(\"" << p.first << "\")";
                } else {
                    src << prefix << p.first << "(" << p.second << ")";
                }
                prefix = ", ";
            }
            src << ";\n";
        }
        if (!rvars.empty()) {
            string prefix = "RVar ";
            for (const auto &p : rvars) {
                if (p.second.empty()) {
                    src << prefix << p.first << "(\"" << p.first << "\")";
                } else {
                    src << prefix << p.first << "(" << p.second << ")";
                }
                prefix = ", ";
            }
            src << ";\n";
        }

        for (auto &p : state_map) {
            if (p.first->node->is_input) continue;

            Stage stage(p.first->stage);

            // Do all the reorders and pick which vars to
            // parallelize.
            vector<VarOrRVar> vars;
            int64_t parallel_tasks = 1;
            vector<VarOrRVar> parallel_vars;
            vector<int64_t> parallel_extents;
            bool any_parallel_vars = false, any_parallel_rvars = false;
            for (auto it = p.second->vars.rbegin(); it != p.second->vars.rend(); it++) {
                if (!it->exists || it->extent == 1) continue;
                if (!it->parallel) break;
                any_parallel_rvars |= it->var.is_rvar;
                any_parallel_vars |= !it->var.is_rvar;
                parallel_tasks *= it->extent;
                parallel_extents.push_back(it->extent);
                parallel_vars.push_back(it->var);
            }

            if (p.second->vars.size() > 1) {
                p.second->schedule_source << "\n    .reorder(";

                bool first = true;
                for (auto &v : p.second->vars) {
                    if (v.exists) {
                        vars.push_back(v.var);
                        p.second->ordered_vars.push_back(v);
                        if (!first) {
                            p.second->schedule_source << ", ";
                        }
                        first = false;
                        p.second->schedule_source << v.var.name();
                    }
                }
                p.second->schedule_source << ")";
                stage.reorder(vars);
            }

            // Halide doesn't let you fuse an RVar with a Var, even if
            // they are both pure.
            bool can_fuse = !(any_parallel_vars && any_parallel_rvars) && (!target.has_gpu_feature() || can_fuse_gpu(parallel_extents));
            if (can_fuse && (!target.has_gpu_feature() || can_fuse_gpu(parallel_extents))) {
                for (size_t i = 1; i < parallel_vars.size(); i++) {
                    // Outermost, and next outermost. Preserve the inner
                    // name to not invalidate any compute_ats.
                    p.second->schedule_source << "\n    .fuse(" << parallel_vars[i].name()
                                              << ", " << parallel_vars[i-1].name()
                                              << ", " << parallel_vars[i].name() << ")";
                    stage.fuse(parallel_vars[i], parallel_vars[i-1], parallel_vars[i]);
                }
                if (!parallel_vars.empty()) {
                    if (target.has_gpu_feature()) {
                        p.second->schedule_source << "\n    .gpu_blocks(" << parallel_vars.back().name() << ")";
                        stage.gpu_blocks(parallel_vars.back());
                    } else {
                        p.second->schedule_source << "\n    .parallel(" << parallel_vars.back().name() << ")";
                        stage.parallel(parallel_vars.back());
                    }
                }
            } else {
                if (target.has_gpu_feature()) {
                    mark_gpu_blocks(p.second.get(), stage, parallel_vars);
                } else {
                    for (const auto &v : parallel_vars) {
                        p.second->schedule_source << "\n    .parallel(" << v.name() << ")";
                        stage.parallel(v);
                    }
                }
            }

            if (!parallel_vars.empty()) {
                p.second->parallel = true;
            }

            // Reorder the vector dimension innermost
            if (p.first->index == 0 && p.second->vector_dim > 0) {
                vector<Var> storage_vars = Func(p.first->node->func).args();
                for (int i = p.second->vector_dim; i > 0; i--) {
                    std::swap(storage_vars[i], storage_vars[i-1]);
                }
                p.second->schedule_source << "\n    .reorder_storage(";
                bool first = true;
                for (auto v : storage_vars) {
                    if (!first) {
                        p.second->schedule_source << ", ";
                    }
                    first = false;
                    p.second->schedule_source << v.name();
                }
                p.second->schedule_source << ")";
                Func(p.first->node->func).reorder_storage(storage_vars);
            }

            if (target.has_gpu_feature()) {
                mark_gpu_threads(p.second.get(), stage);
            }
        }

        for (auto &p : state_map) {
            if (p.first->node->is_input) continue;

            // Dump the schedule source string
            src << p.first->name
                << p.second->schedule_source.str()
                << ";\n";
        }

        schedule_source = src.str();
        bool in_quotes = false;
        for (auto &c : schedule_source) {
            in_quotes ^= (c == '"');
            if (!in_quotes && c == '$') c = '_';
        }
    }
};



int State::cost_calculations = 0;

// A priority queue of states, sorted according to increasing
// cost. Never shrinks, to avoid reallocations.
// Can't use std::priority_queue because it doesn't support unique_ptr.
class StateQueue {
private:
    struct CompareStates {
        bool operator()(const IntrusivePtr<State> &a, const IntrusivePtr<State> &b) const {
            return a->cost > b->cost;
        }
    };

    std::vector<IntrusivePtr<State>> storage;
    size_t sz = 0;
public:
    void emplace(IntrusivePtr<State> &&s) {
        if (sz >= storage.size()) {
            storage.resize(std::max(sz * 2, (size_t)64));
        }
        internal_assert(sz < storage.size()) << sz << " " << storage.size() << "\n";
        storage[sz] = std::move(s);
        sz++;
        std::push_heap(storage.begin(), storage.begin() + sz, CompareStates{});
    }

    IntrusivePtr<State> pop() {
        internal_assert(sz <= storage.size()) << sz << " " << storage.size() << "\n";
        std::pop_heap(storage.begin(), storage.begin() + sz, CompareStates{});
        sz--;
        return std::move(storage[sz]);
    }

    const IntrusivePtr<State> &top() {
        return storage[0];
    }

    bool empty() const {
        return sz == 0;
    }

    size_t size() const {
        return sz;
    }

    void swap(StateQueue &other) {
        storage.swap(other.storage);
        std::swap(sz, other.sz);
    }

    IntrusivePtr<State> operator[](int idx) const {
        return storage[idx];
    }

    void resort() {
        std::make_heap(storage.begin(), storage.begin() + sz, CompareStates{});
    }

    void clear() {
        for (size_t i = 0; i < sz; i++) {
            storage[i] = IntrusivePtr<State>{};
        }
        sz = 0;
    }
};

void configure_pipeline_features(const FunctionDAG &dag,
                                 const MachineParams &params,
                                 CostModel *cost_model) {
    cost_model->reset();
    const int pipeline_feat_size = head1_w * head1_h;
    static_assert(sizeof(PipelineFeatures) - 7 * sizeof(int) ==
                  sizeof(int) * pipeline_feat_size,
                  "Incorrect size for pipeline features");
    int num_stages = 0;
    for (const auto &n : dag.nodes) {
        if (!n.is_input) num_stages += (int)n.stages.size();
    }
    Runtime::Buffer<float> pipeline_features(head1_w, head1_h, num_stages);
    int stage = 0;
    for (const auto &n : dag.nodes) {
        if (n.is_input) continue;
        for (auto it = n.stages.rbegin(); it != n.stages.rend(); it++) {
            const auto &s = *it;
            const int *pipeline_feats = (const int *)(&(s.features)) + 7;
            // skip the first 7 features
            for (int i = 0; i < pipeline_feat_size; i++) {
                int x = i/7;
                int y = i%7;
                pipeline_features(x, y, stage) = pipeline_feats[i];
            }
            stage += 1;
        }
    }
    internal_assert(stage == num_stages);
    cost_model->set_pipeline_features(pipeline_features, params.parallelism);
}

IntrusivePtr<State> optimal_schedule_pass(FunctionDAG &dag,
                                          vector<Function> outputs,
                                          const MachineParams &params,
                                          const Target &target,
                                          CostModel *cost_model,
                                          RNG &rng,
                                          int beam_size,
                                          int pass_idx,
                                          std::unordered_set<uint64_t> &permitted_hashes) {

    if (cost_model) {
        configure_pipeline_features(dag, params, cost_model);
    }

    StateQueue q, pending;

    {
        IntrusivePtr<State> initial{new State};
        initial->root = new LoopNest;
        q.emplace(std::move(initial));
    }

    // A progress bar.
    uint32_t counter = 0;
    bool draw_progress_bar = isatty(2);
    auto tick = [&](double progress) {
        if (!draw_progress_bar) return;
        counter++;
        const int bits = 11;
        if (counter & ((1 << bits) - 1)) return;
        progress *= 78;
        debug(0) << '[';
        for (int j = 0; j < 78; j++) {
            if (j < progress) {
                debug(0) << '.';
            } else if (j - 1 < progress) {
                debug(0) << "/-\\|"[(counter >> bits) % 4];
            } else {
                debug(0) << ' ';
            }
        }
        debug(0) << ']';
        for (int j = 0; j < 80; j++) {
            debug(0) << '\b';
        }
    };

    int expanded;

    std::function<void(IntrusivePtr<State> &&)> enqueue_new_children =
        [&](IntrusivePtr<State> &&s) {

        // debug(0) << "\n** Generated child: ";
        // s->dump();
        // s->calculate_cost(dag, params, nullptr, true);

        internal_assert(s->num_funcs_scheduled == s->parent->num_funcs_scheduled + 1);

        int progress = s->num_funcs_scheduled * beam_size + expanded;
        size_t max_progress = dag.nodes.size() * beam_size * 2;
        tick(double(progress) / max_progress);
        s->penalized = false;

        q.emplace(std::move(s));
    };

    string cyos_str = get_env_variable("HL_CYOS");

    for (int i = 0; ; i++) {
        std::unordered_map<uint64_t, int> hashes;
        q.swap(pending);

        internal_assert(!pending.empty());

        if ((int)pending.size() > beam_size * 10000) {
            debug(0) << "Warning: Huge number of states generated (" << pending.size() << ").\n";
        }

        expanded = 0;
        while (expanded < beam_size && !pending.empty()) {

            IntrusivePtr<State> state {pending.pop()};

            if (beam_size > 1) {
                // Apply cost penalties to the queue according to
                // structural uniqueness.
                if (!state->penalized) {
                    uint64_t h1 = state->structural_hash(pass_idx + 1, params.parallelism);
                    uint64_t h0 = state->structural_hash(pass_idx - 1, params.parallelism);
                    int penalty = ++hashes[h1];
                    if (pass_idx > 0 && !permitted_hashes.count(h0)) {
                        // It's possible to get yourself into a state
                        // where the only things in the beam that match
                        // the hash were quick-rejected due to details not
                        // captured in the hash, so we apply a huge
                        // penalty, but leave the impermissible state in
                        // the beam.
                        // debug(0) << "\nImpermissible hash " << pass_idx << " at " << state->num_funcs_scheduled << " " << h0 << ":\n";
                        // state->dump();
                        penalty += 10;
                    }
                    if (penalty > 1) {
                        state->penalized = true;
                        state->cost *= penalty;
                        // After penalizing this state, it's no longer the
                        // best, defer it.
                        if (!pending.empty() && state->cost > pending.top()->cost) {
                            pending.emplace(std::move(state));
                            continue;
                        }
                    }
                }
            }

            if (pending.size() > 1 && random_dropout(rng, dag.nodes.size() * 2)) {
                // debug(0) << "Dropping state\n";
                continue;
            }

            if (state->num_funcs_scheduled == 2*(int)dag.nodes.size()) {
                if (false) {
                    debug(0) << "Optimal state?\n";
                    state->dump();

                    debug(0) << "\nRest of queue:\n";
                    while (!pending.empty()) {
                        pending.pop()->dump();
                    }
                }

                auto best = state;

                // Bless the reasonable stuff in the beam as permissible states to visit again
                int blessed = 0;
                while (state->cost <= 1.2 * best->cost && blessed < beam_size) {
                    const State *s = state.get();
                    while (s) {
                        uint64_t h1 = s->structural_hash(pass_idx, params.parallelism);
                        permitted_hashes.insert(h1);
                        s = s->parent.get();
                    }
                    if (pending.empty()) break;
                    state = pending.pop();
                    blessed++;
                }

                return best;
            }

            /*
            if (state->num_funcs_scheduled > 0 &&
                dag.nodes[state->num_funcs_scheduled].func.name() == "downsampled_x") {
            */
            if (false) {
                debug(0) << "\n\n**** Beam: (" << expanded << "):\n";
                state->dump();
            }

            /*
              debug(0) << "Expanding state:";
              state->dump();
              state->calculate_cost(dag, params, nullptr, true);
            */

            state->generate_children(dag, params, target, cost_model, enqueue_new_children);
            expanded++;
        }

        // Drop the other states unconsidered.
        pending.clear();

        if (cost_model) {
            // Now evaluate all the costs and re-sort them in the priority queue
            cost_model->evaluate_costs();
            q.resort();
        }

        if (cyos_str == "1") {
            // Manually discard everything in the queue except for the user-chosen option
            // Print user choices.
            debug(0) << "\n--------------------\n";
            debug(0) << "Select a schedule:\n";
            for (int choice_label = (int)q.size() - 1; choice_label >= 0; choice_label--) {
                auto state = q[choice_label];
                debug(0) << "\n[" << choice_label << "]:\n";
                state->dump();
                state->calculate_cost(dag, params, target, cost_model, true);
            }
            cost_model->evaluate_costs();

            // Select next partial schedule to expand.
            int selection = -1;
            while (selection < 0 || selection >= (int)q.size()) {
                debug(0) << "\nEnter selection: ";
                std::cin >> selection;
            }

            auto selected = q[selection];
            selected->dump();
            q.clear();
            q.emplace(std::move(selected));
        }
    }
}

IntrusivePtr<State> optimal_schedule(FunctionDAG &dag,
                                     vector<Function> outputs,
                                     const MachineParams &params,
                                     const Target &target,
                                     CostModel *cost_model,
                                     RNG &rng,
                                     int beam_size) {

    IntrusivePtr<State> best;

    std::unordered_set<uint64_t> permitted_hashes;
    int num_passes = (beam_size == 1) ? 1 : 5;

    string cyos_str = get_env_variable("HL_CYOS");
    if (cyos_str == "1") {
        num_passes = 1;
    }

    string num_passes_str = get_env_variable("HL_NUM_PASSES");
    if (!num_passes_str.empty()) {
        num_passes = std::atoi(num_passes_str.c_str());
    }

    for (int i = 0; i < num_passes; i++) {
        auto pass = optimal_schedule_pass(dag, outputs, params, target, cost_model, rng, beam_size, i, permitted_hashes);
        debug(0) << "\nPass " << i << " result:\n";
        pass->dump();

        if (i == 0 || pass->cost < best->cost) {
            best = pass;
        }
    }

    debug(0) << "Best cost: " << best->cost << "\n";

    return best;
}

void estimate_num_schedules(FunctionDAG &dag,
                            vector<Function> outputs,
                            const MachineParams &params,
                            const Target &target,
                            std::mt19937 &rng) {

    std::unordered_set<uint64_t> seen_states;

    IntrusivePtr<State> initial{new State};
    initial->root = new LoopNest;

    auto draw_sample = [&]() {
        auto prev = initial;
        while (1) {
            auto next = prev->random_child(dag, params, target, rng);
            if (!next.defined()) return prev->structural_hash(10000000, params.parallelism);
            prev = next;
        }
    };

    // From https://arxiv.org/pdf/1512.07901.pdf
    size_t w = 0, r = 0;
    double w1 = 0;
    while (1) {
        // Overflow is well-defined for size_t
        size_t next_w = w + seen_states.size();
        if (next_w < w) {
            w1 += w;
            w = seen_states.size();
        } else {
            w = next_w;
        }
        uint64_t h = draw_sample();
        if (seen_states.count(h)) {
            r++;
        } else {
            seen_states.insert(h);
        }

        debug(0) << "Estimated number of schedules: " << ((w1 + w) / std::max(r, (size_t)1)) << " (" << r << ")\n";
    }

}

}

std::string generate_schedules_new(const std::vector<Function> &outputs,
                                   const Target &target,
                                   const MachineParams &params) {

    HALIDE_TIC;

    State::cost_calculations = 0;
    string seed_str = get_env_variable("HL_SEED");
    int seed = (int)time(NULL);
    if (!seed_str.empty()) {
        seed = atoi(seed_str.c_str());
    }
    debug(0) << "Dropout seed = " << seed << '\n';
    RNG rng{(uint32_t)seed};

    string beam_size_str = get_env_variable("HL_BEAM_SIZE");
    size_t beam_size = 32;
    if (!beam_size_str.empty()) {
        beam_size = atoi(beam_size_str.c_str());
    }

    string time_limit_str = get_env_variable("HL_AUTO_SCHEDULE_TIME_LIMIT");
    double time_limit = 0;
    if (!time_limit_str.empty()) {
        time_limit = atof(time_limit_str.c_str());
    }

    string weights_in_dir = get_env_variable("HL_WEIGHTS_DIR");
    string weights_out_dir = get_env_variable("HL_WEIGHTS_OUT_DIR");
    if (weights_out_dir.empty()) {
        weights_out_dir = weights_in_dir;
    }

    string randomize_weights_str = get_env_variable("HL_RANDOMIZE_WEIGHTS");
    bool randomize_weights = randomize_weights_str == "1";

    string weights_server_hostname = get_env_variable("HL_WEIGHTS_SERVER_HOSTNAME");

    string weights_server_port_str = get_env_variable("HL_WEIGHTS_SERVER_PORT");
    int weights_server_port = 0;
    if (!weights_server_port_str.empty()) {
        weights_server_port = atoi(weights_server_port_str.c_str());
    }

    string weights_server_experiment_id_str = get_env_variable("HL_WEIGHTS_SERVER_EXPERIMENT_ID");
    int weights_server_experiment_id = 0;
    if (!weights_server_experiment_id_str.empty()) {
        weights_server_experiment_id = atoi(weights_server_experiment_id_str.c_str());
    }

    FunctionDAG dag(outputs, params, target);

    dag.dump();

    std::unique_ptr<CostModel> cost_model;
    if (get_env_variable("HL_USE_MANUAL_COST_MODEL") != "1") {
        cost_model = CostModel::make_default(weights_in_dir, weights_out_dir, randomize_weights, weights_server_hostname, weights_server_port, weights_server_experiment_id);
    }

    // HACK
    // estimate_num_schedules(dag, outputs, params, rng);

    IntrusivePtr<State> optimal;

    if (time_limit) {
        // Use a fixed running time
        auto start = std::chrono::steady_clock::now();
        for (size_t beam_size = 1; ; beam_size *= 2) {
            auto s = optimal_schedule(dag, outputs, params, target, cost_model.get(), rng, beam_size);
            if (beam_size == 1 || s->cost < optimal->cost) {
                optimal = s;
            }
            auto t = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(t - start).count();
            if (elapsed > time_limit / 2) {
                break;
            }
        }
    } else {
        // Use a fixed beam size
        optimal = optimal_schedule(dag, outputs, params, target, cost_model.get(), rng, beam_size);
    }

    HALIDE_TOC;

    debug(0) << "Cost evaluated this many times: " << State::cost_calculations << '\n';

    debug(0) << "** Optimal schedule:\n";

    // Just to get the debugging prints to fire
    optimal->calculate_cost(dag, params, target, cost_model.get(), true);

    // Apply the schedules
    optimal->apply_schedule(dag, params, target);

    // Print out the schedule
    optimal->dump();

    string schedule_file = get_env_variable("HL_SCHEDULE_FILE");
    if (!schedule_file.empty()) {
        debug(0) << "Writing schedule to " << schedule_file << "...\n";
        std::ofstream f(schedule_file);
        f << "// --- BEGIN machine-generated schedule\n"
          << optimal->schedule_source
          << "// --- END machine-generated schedule\n";
        f.close();
        internal_assert(!f.fail()) << "Failed to write " << schedule_file;
    }

    // Print out the predicted runtime of each Func, so we can compare them to a profile
    // optimal->print_predicted_runtimes(params);

    string feature_file = get_env_variable("HL_FEATURE_FILE");
    if (!feature_file.empty()) {
        optimal->save_featurization(dag, params, feature_file);
    }

    return "";
}

// Register this as the autoscheduler
struct AutoScheduler {
    AutoScheduler() {
        debug(0) << "Registering autoscheduler...\n";
        Pipeline::set_custom_auto_scheduler(*this);
    }

    string operator()(Pipeline p, const Target &target, const MachineParams &params) {
        std::vector<Function> outputs;
        for (Func f : p.outputs()) {
            outputs.push_back(f.function());
        }
        return generate_schedules_new(outputs, target, params);
    }
} auto_scheduler;

}


template<>
RefCount &ref_count<Autoscheduler::LoopNest>(const Autoscheduler::LoopNest *t) {return t->ref_count;}

template<>
void destroy<Autoscheduler::LoopNest>(const Autoscheduler::LoopNest *t) {delete t;}

template<>
RefCount &ref_count<Autoscheduler::State>(const Autoscheduler::State *t) {return t->ref_count;}

template<>
void destroy<Autoscheduler::State>(const Autoscheduler::State *t) {delete t;}

}
}
