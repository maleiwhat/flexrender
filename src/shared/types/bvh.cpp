#include "types/bvh.hpp"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <limits>
#include <iostream>
#include <sstream>

#include "types.hpp"
#include "utils.hpp"

using std::vector;
using std::pair;
using std::nth_element;
using std::partition;
using std::numeric_limits;
using std::function;
using std::string;
using std::stringstream;
using std::endl;
using glm::vec3;

namespace fr {

const uint32_t BVH::NUM_BUCKETS = 12;

BVH::BVH(const Mesh* mesh) :
 _nodes() {
    // Initialize build data from mesh triangles.
    vector<PrimitiveInfo> build_data;
    build_data.reserve(mesh->faces.size());
    for (size_t i = 0; i < mesh->faces.size(); i++) {
        BoundingBox bounds = mesh->faces[i].WorldBounds(mesh->vertices, mesh->xform);
        build_data.emplace_back(i, bounds);
    }

    // Actually build the tree.
    Build(build_data);
}

BVH::BVH(const vector<pair<uint32_t, BoundingBox>>& things) :
 _nodes() {
    if (things.size() == 0) {
        ZeroThings();
    } else if (things.size() == 1) {
        OneThing(things[0].first, things[0].second);
    } else {
        // Initialize the build data.
        vector<PrimitiveInfo> build_data;
        build_data.reserve(things.size());
        for (size_t i = 0; i < things.size(); i++) {
            uint32_t id = things[i].first;
            const BoundingBox& bounds = things[i].second;
            if (bounds.IsValid()) {
                build_data.emplace_back(id, bounds);
            }
        }

        if (build_data.size() == 0) {
            ZeroThings();
        } else if (build_data.size() == 1) {
            OneThing(build_data[0].index, build_data[0].bounds);
        } else {
            // Actually build the tree.
            Build(build_data);
        }
    }
}

BVH::BVH() :
 _nodes() {}

TraversalState BVH::Traverse(const SlimRay& ray, HitRecord* nearest,
 function<bool (uint32_t index, const SlimRay& ray, HitRecord* hit, bool* request_suspend)> intersector) {
    // Initialize fresh state.
    TraversalState traversal;

    // Quick test for special cases.
    if (!_nodes[0].bounds.IsValid()) {
        traversal.current = 0;
        traversal.state = TraversalState::State::FROM_CHILD;
        return traversal;
    }

    // Start by going down the root's near child.
    traversal.current = NearChild(0, ray.direction);
    traversal.state = TraversalState::State::FROM_PARENT;

    return Traverse(traversal, ray, nearest, intersector, false);
}

TraversalState BVH::Traverse(TraversalState state, const SlimRay& ray, HitRecord* nearest,
 function<bool (uint32_t index, const SlimRay& ray, HitRecord* hit, bool* request_suspend)> intersector,
 bool resume) {
    // Precompute the inverse direction of the ray.
    vec3 inv_dir(1.0f / ray.direction.x,
                 1.0f / ray.direction.y,
                 1.0f / ray.direction.z);

    // Initialize traversal based on passed state.
    TraversalState traversal = state;
    bool request_suspend = false;

    if (resume) {
        if (traversal.state == TraversalState::State::FROM_PARENT) {
            goto resume_parent;
        } else if (traversal.state == TraversalState::State::FROM_SIBLING) {
            goto resume_sibling;
        } else {
            TERRLN("Must be in FROM_PARENT or FROM_SIBLING state to resume traversal!");
        }
    }

    while (true) {
        switch (traversal.state) {
            case TraversalState::State::FROM_PARENT:
                if (!BoundingHit(_nodes[traversal.current].bounds, ray, inv_dir, nearest->t)) {
                    // Ray missed the near child, try the far child.
                    traversal.current = Sibling(traversal.current);
                    traversal.state = TraversalState::State::FROM_SIBLING;
                } else if (_nodes[traversal.current].leaf) {
                    // Ray hit the near child and it's a leaf node.
                    request_suspend = false;
                    traversal.hit = intersector(_nodes[traversal.current].index, ray, nearest, &request_suspend) || traversal.hit;
                    if (request_suspend) goto suspend_traversal;
resume_parent:      traversal.current = Sibling(traversal.current);
                    traversal.state = TraversalState::State::FROM_SIBLING;
                } else {
                    // Ray hit the near child and it's an interior node.
                    traversal.current = NearChild(traversal.current, ray.direction);
                    traversal.state = TraversalState::State::FROM_PARENT;
                }
                break;

            case TraversalState::State::FROM_SIBLING:
                if (!BoundingHit(_nodes[traversal.current].bounds, ray, inv_dir, nearest->t)) {
                    // Ray missed the far child, backtrack to the parent.
                    traversal.current = _nodes[traversal.current].parent;
                    traversal.state = TraversalState::State::FROM_CHILD;
                } else if (_nodes[traversal.current].leaf) {
                    // Ray hit the far child and it's a leaf node.
                    request_suspend = false;
                    traversal.hit = intersector(_nodes[traversal.current].index, ray, nearest, &request_suspend) || traversal.hit;
                    if (request_suspend) goto suspend_traversal;
resume_sibling:     traversal.current = _nodes[traversal.current].parent;
                    traversal.state = TraversalState::State::FROM_CHILD;
                } else {
                    // Ray hit the far child and it's an interior node.
                    traversal.current = NearChild(traversal.current, ray.direction);
                    traversal.state = TraversalState::State::FROM_PARENT;
                }
                break;

            case TraversalState::State::FROM_CHILD:
                if (traversal.current == 0) {
                    // Traversal has finished.
                    return traversal;
                }
                if (traversal.current == NearChild(_nodes[traversal.current].parent, ray.direction)) {
                    // Coming back up through the near child, so traverse
                    // to the far child.
                    traversal.current = Sibling(traversal.current);
                    traversal.state = TraversalState::State::FROM_SIBLING;
                } else {
                    // Coming back up through the far child, so continue
                    // backtracking through the parent.
                    traversal.current = _nodes[traversal.current].parent;
                    traversal.state = TraversalState::State::FROM_CHILD;
                }
                break;

            default:
                TERRLN("BVH traversal in unknown state!");
                exit(EXIT_FAILURE);
                break;
        }
    }

    /// Shouldn't ever get here...
    TERRLN("Unexpected exit path in BVH traversal.");
    return TraversalState();

suspend_traversal:
    return traversal;
}

void BVH::Build(vector<PrimitiveInfo>& build_data) {
    // Recursively build the BVH tree.
    size_t total_nodes = 0;
    LinkedNode* root = RecursiveBuild(build_data, 0, build_data.size(), &total_nodes);

    // Flatten the tree into a linear representation.
    _nodes.reserve(total_nodes);
    for (size_t i = 0; i < total_nodes; i++) {
        _nodes.emplace_back();
    }
    size_t offset = 0;
    FlattenTree(root, -1, &offset);
    assert(offset == total_nodes);

    // Release memory consumed by the linked tree.
    DeleteLinked(root);
}

LinkedNode* BVH::RecursiveBuild(vector<PrimitiveInfo>& build_data, size_t start,
 size_t end, size_t* total_nodes) {
    assert(start != end);
    
    (*total_nodes)++;
    LinkedNode* node = nullptr;

    // Compute the bounds of all primitives in this BVH node.
    BoundingBox bounds;
    for (size_t i = start; i < end; i++) {
        bounds = bounds.Union(build_data[i].bounds);
    }

    // How many primitives are we partitioning?
    size_t num_primitives = end - start;
    if (num_primitives == 1) {
        // Only one primitive left, create a leaf node.
        node = new LinkedNode(build_data[start].index, bounds);
    } else {
        // Find the bounds of the centroids.
        BoundingBox centroid_bounds;
        for (size_t i = start; i < end; i++) {
            centroid_bounds.Absorb(build_data[i].centroid);
        }

        // Split along the longest axis.
        BoundingBox::Axis split_axis = centroid_bounds.LongestAxis();
        float split_min = AxisComponent(centroid_bounds.min, split_axis);
        float split_max = AxisComponent(centroid_bounds.max, split_axis);

        size_t mid = (start + end) / 2;
        if (num_primitives <= 4 || split_min == split_max) {
            // Partition using equal size subsets, since SAH has diminishing
            // returns at this point. Also handles a degenerate case where we
            // have multiple primitives stacked on top  of each other with the
            // same centroid.
            nth_element(&build_data[start], &build_data[mid], &build_data[end - 1] + 1,
             [split_axis](const PrimitiveInfo& a, const PrimitiveInfo& b) {
                 return AxisComponent(a.centroid, split_axis) < AxisComponent(b.centroid, split_axis);
             });
        } else {
            // Partition using the surface area heuristic (SAH).
            uint32_t min_cost_split = ComputeSAH(build_data, start, end,
             split_min, split_max, bounds.SurfaceArea(), split_axis);

            PrimitiveInfo* pmid = partition(&build_data[start],
             &build_data[end - 1] + 1, [min_cost_split, split_min, split_max, split_axis](const PrimitiveInfo& p) {
                 uint32_t bucket = NUM_BUCKETS *
                  ((AxisComponent(p.centroid, split_axis) - split_min) /
                  (split_max - split_min));

                 if (bucket == NUM_BUCKETS) bucket = NUM_BUCKETS - 1;
                 assert(bucket >= 0 && bucket < NUM_BUCKETS);

                 return bucket <= min_cost_split;
             });

            mid = pmid - &build_data[0];

        }

        // Recursively build the subtrees for both partitions.
        node = new LinkedNode(RecursiveBuild(build_data, start, mid, total_nodes),
                              RecursiveBuild(build_data, mid, end, total_nodes),
                              split_axis);
    }

    return node;
}

uint32_t BVH::ComputeSAH(vector<PrimitiveInfo>& build_data, size_t start,
 size_t end, float min, float max, float surface_area, BoundingBox::Axis axis) {
    // Initialize each bucket for potential split candidates.
    BucketInfo buckets[NUM_BUCKETS];
    for (size_t i = start; i < end; i++) {
        uint32_t bucket = NUM_BUCKETS *
         ((AxisComponent(build_data[i].centroid, axis) - min) / (max - min));

        if (bucket == NUM_BUCKETS) bucket = NUM_BUCKETS - 1;
        assert(bucket >= 0 && bucket < NUM_BUCKETS);

        buckets[bucket].count++;
        buckets[bucket].bounds.Union(build_data[i].bounds);
    }

    // Compute the cost for splitting after each bucket.
    float cost[NUM_BUCKETS - 1];
    for (uint32_t i = 0; i < NUM_BUCKETS - 1; i++) {
        BoundingBox left_bounds, right_bounds;
        size_t left_count = 0;
        size_t right_count = 0;

        for (uint32_t j = 0; j <= i; j++) {
            left_bounds.Union(buckets[j].bounds);
            left_count += buckets[j].count;
        }

        for (uint32_t j = i + i; j < NUM_BUCKETS; j++) {
            right_bounds.Union(buckets[j].bounds);
            right_count += buckets[j].count;
        }

        cost[i] = 0.125f +
         (left_count * left_bounds.SurfaceArea() + right_count * right_bounds.SurfaceArea()) /
         surface_area;
    }
    
    // Find which bucket minimizes the cost.
    float min_cost = cost[0];
    uint32_t min_cost_split = 0;
    for (uint32_t i = 1; i < NUM_BUCKETS - 1; i++) {
        if (cost[i] < min_cost) {
            min_cost = cost[i];
            min_cost_split = i;
        }
    }

    return min_cost_split;
}

size_t BVH::FlattenTree(LinkedNode* current, size_t parent, size_t* offset) {
    LinearNode* node = &_nodes[*offset];
    size_t my_offset = (*offset)++;

    node->bounds = current->bounds;
    node->parent = parent;

    if (current->children[0] == nullptr) {
        // Create leaf node.
        node->leaf = 1;
        node->index = current->index;
    } else {
        // Create interior node.
        node->leaf = 0;
        node->axis = current->split;
        FlattenTree(current->children[0], my_offset, offset);
        node->right = FlattenTree(current->children[1], my_offset, offset);
    }

    return my_offset;
}

void BVH::DeleteLinked(LinkedNode* node) {
    if (node->children[0] != nullptr) {
        DeleteLinked(node->children[0]);
        node->children[0] = nullptr;
    }
    if (node->children[1] != nullptr) {
        DeleteLinked(node->children[1]);
        node->children[1] = nullptr;
    }
    delete node;
}

void BVH::ZeroThings() {
    LinearNode root;
    root.leaf = 0;
    root.index = numeric_limits<size_t>::max();
    root.parent = -1;
    root.right = 2;
    root.axis = BoundingBox::Axis::X;
    _nodes.push_back(root);

    LinearNode left;
    left.leaf = 1;
    left.index = numeric_limits<size_t>::max();
    left.parent = 0;
    left.right = -1;
    _nodes.push_back(left);

    LinearNode right;
    right.leaf = 1;
    right.index = numeric_limits<size_t>::max();
    right.parent = 0;
    right.right = -1;
    _nodes.push_back(right);
}

void BVH::OneThing(uint32_t id, const BoundingBox& bounds) {
    LinearNode root;
    root.bounds = bounds;
    root.leaf = 0;
    root.index = numeric_limits<size_t>::max();
    root.parent = -1;
    root.right = 2;
    root.axis = BoundingBox::Axis::X;
    _nodes.push_back(root);

    LinearNode left;
    left.bounds = bounds;
    left.leaf = 1;
    left.index = id;
    left.parent = 0;
    left.right = -1;
    _nodes.push_back(left);

    LinearNode right;
    right.leaf = 1;
    right.index = numeric_limits<size_t>::max();
    right.parent = 0;
    right.right = -1;
    _nodes.push_back(right);
}

string ToString(const BVH* bvh, const string& indent) {
    stringstream stream;
    string pad = indent + "| ";
    stream << "BVH {" << endl;
    for (size_t i = 0; i < bvh->_nodes.size(); i++) {
        stream << indent << "| [" << i << "] = " << ToString(bvh->_nodes[i], pad) << endl;
    }
    stream << indent << "}";
    return stream.str();
}

} // namespace fr
