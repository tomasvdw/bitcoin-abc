


#ifndef BITCOIN_UTXOCOMMIT_TRUNKNODE_H
#define BITCOIN_UTXOCOMMIT_TRUNKNODE_H

#include <cstdint>
#include <cassert>
#include <vector>
#include <vector>
#include <deque>
#include <mutex>


#include "multiset.h"

#define BRANCH_COUNT 16
#define BRANCH_BITS  4
#define MAX_LEAF_SIZE 2000
#define MIN_ELEMENT_SIZE 4


class CUtxoDataSet;

/* This is a reference to a node queued for normalization
 * it includes the depth and enough bytes to determine the prefix */
struct CNormalizeItem {
    uint32_t node_index;
    uint32_t bits;
    std::vector<uint8_t> prefix;

    CNormalizeItem(uint32_t node_index, uint32_t bits, std::vector<uint8_t> prefix) :
        node_index(node_index), bits(bits), prefix(prefix) {};
};

/* Storage for both branch and leaf nodes */
struct CNode {
    uint64_t count;
    uint32_t data; // index into CTrunkNode::multisets or CTrunkNode::branches
    bool     is_branch;

    CNode(uint64_t count, uint32_t data) : count(count), data(data), is_branch(false) {};
};


/*
 * CTrunkNodes are the 16 children of the root node
 *
 * They are specialized nodes to provide ownership of all their
 * descendants and provide locking.
 *
 * This allows the CUtxoCommitment to update its tree thread
 * safe, allowing upto 16 threads to work in parallel without
 * the overhead of per node locking throughout the tree
 *
 *
 */
class CTrunkNode {

    private:
        // non-copyable
        CTrunkNode( const CTrunkNode& other ) = delete;
        CTrunkNode& operator=( const CTrunkNode& ) = delete;

        /* Lock for all thread-safe public methods */
        std::mutex lock;


        // Stores each child node; both branch and leaf.
        // nodes[0] is this trunk node
        std::vector<CNode> nodes;

        // Extra storage for each branch node; accessed by index CNode::data
        std::vector<std::array<uint32_t, BRANCH_COUNT>> branches;

        // Extra storage for leaf nodes; accessed by index CNode::data
        std::vector<CMultiSet> multisets;

        // Maintain a list of nodes that need normalization
        std::deque<CNormalizeItem> denormalized;

        // Splits a leaf node into branches
        void SplitNode(uint32_t node_index);


        // Collects all multisets together; called on normalization to shrink a branch to a single leaf
        void SumAllLeaves(CMultiSet &multiset, uint32_t node_index);


    public:
        CTrunkNode() {
            // Initially we are a leaf node (index 0),
            // pointing to an empty multiset index 0.
            nodes.push_back(CNode(0,0));
            multisets.push_back(CMultiSet());
        }


        // Add or remove an element
        void Update(const std::vector<uint8_t> &element, bool remove);

        // Normalizes the tree; ensureing no leaf is too big and no branch is too small
        void Normalize(const CUtxoDataSet &set);


        // Calculates the hash for a node
        void Hash(CHashWriter &writer, uint32_t node_index) const;

        // Called on initial load to estimate the capacity, to reduce the needed number of normalizations
        void SetCapacity(uint64_t est_count, uint32_t node_index);

        // Dumps stats (TODO: return stats-structure
        void GetInfo() const;
};


#endif
