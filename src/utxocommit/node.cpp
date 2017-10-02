
#include <cassert>

#include "util.h"
#include "utilstrencodings.h"
#include "node.h"
#include "utxocommit.h"
#include "multiset.h"


/* Finds the branch number at the given depth */
static int get_branch(int depth, const std::vector<uint8_t> &element) {

    static_assert(BRANCH_COUNT == 16);

    return (element[depth / 2] >> 4*(1-(depth%2))) & 0xF;
}

/* Adds or removes an element to the node in this trunk
 *
 * This assumes the element belongs to this trunk
 */
void CTrunkNode::Update(const std::vector<uint8_t> &element, bool remove) {

    std::lock_guard<std::mutex> locker(lock);

    assert(element.size() >= MIN_ELEMENT_SIZE);

    // Loop into tree; No need for recursion.
    int delta = remove ? -1 : 1;
    uint32_t node_index = 0;
    for(uint32_t depth = 1;;depth++) {

        CNode *node = &nodes[node_index];
        node->count += delta;

        if (!node->is_branch) {

            if (node->count > MAX_LEAF_SIZE) {
                // push index to this node to normalization queue
                denormalized.push_back( CNormalizeItem(node_index, depth*BRANCH_BITS, element));
            }
            multisets[node->data].Update(element, remove);
            break;
        }
        else {

            // all but trunk branch can't have less then MAX_LEAF_SIZE
            if (node->count <= MAX_LEAF_SIZE) {
                denormalized.push_back( CNormalizeItem(node_index, depth*BRANCH_BITS, element));
            }

            node_index = branches[node->data][get_branch(depth, element)];
        }
    }
}

/* Shrinks all branches with <= MAX_LEAF_SIZE element to leafs
 * and expands all leafs with > MAX_LEAF_SIZE elements to branches
 *
 * The CUtxoDataSet must provide access to the whole set
 * on which range queries are issued needed for expansion */
void CTrunkNode::Normalize(const CUtxoDataSet &set)
{
    std::lock_guard<std::mutex> locker(lock);

    while(!denormalized.empty()) {

        CNormalizeItem &noderef = denormalized.front();
        uint32_t idx = noderef.node_index;

        if (nodes[idx].is_branch && nodes[idx].count <= MAX_LEAF_SIZE) {

            // Combine all multiset below this branch together
            CMultiSet multiset;
            SumAllLeaves(multiset, idx);
            nodes[idx].data = multisets.size(); // convert to leaf
            nodes[idx].is_branch = false;
            multisets.push_back(multiset);

            // this leaves some unused children in memory, but this is too
            // rare to worry about
        }
        else if (!nodes[idx].is_branch && nodes[idx].count > MAX_LEAF_SIZE) {

            //clear & split
            const uint64_t orgcount = nodes[idx].count;
            multisets[nodes[idx].data] = CMultiSet();
            SplitNode(idx);

            // Re-add the data to the new leafs
            auto new_leafs = nodes.end()-BRANCH_COUNT;
            const uint32_t depth = (noderef.bits/4);
            uint64_t added = 0;

            auto cursor = set.GetRange(noderef.prefix, noderef.bits);
            for(;;) {
                auto elm = cursor->Next();
                if (!elm)
                    break;

                const int branch = get_branch(depth, *elm);
                new_leafs[branch].count++;
                added++;

                multisets[new_leafs[branch].data].Update(*elm, false);
            }
            assert(orgcount == added);

            // enqueue children as they might also need normalization
            // we can assume children are still at the end of this->nodes
            for(uint8_t n=0; n < uint8_t(BRANCH_COUNT); n++) {

                // replace the nibble in prefix to identify the branch we want to queue
                static_assert(BRANCH_COUNT == 16);
                noderef.prefix[depth/2] = (noderef.prefix[depth/2]
                        & ((depth%2) ? 0xF0 : 0x0F))
                        | ((depth%2) ? n : (n <<4));

                denormalized.push_back({ uint32_t(nodes.size())-BRANCH_COUNT+n , depth*4+4, noderef.prefix});
            }
        }

        denormalized.pop_front();
    }
}

/* Splits the given leaf node into BRANCH_COUNT branches */
void CTrunkNode::SplitNode(uint32_t node_index)
{
    assert(!nodes[node_index].is_branch);

    // Add 1 leaf nodes, that takes over the multiset data of this node
    nodes.push_back(CNode(0, nodes[node_index].data));

    // And 15 leaf nodes with new multisets
    for(int n=0; n < BRANCH_COUNT-1; n++)  {
        nodes.push_back(CNode(0, uint32_t(multisets.size())));
        multisets.push_back(CMultiSet());
    }

    // now this node will become a branch node
    nodes[node_index].data = branches.size();
    nodes[node_index].is_branch = true;
    branches.push_back(std::array<uint32_t, BRANCH_COUNT>());
    for(int n=0; n < BRANCH_COUNT; n++) {
        branches.back()[n] = nodes.size() - BRANCH_COUNT + n;
    }
}

/* Reserves estimated space, by pre-splitting branches */
void CTrunkNode::SetCapacity(uint64_t est_count, uint32_t node_index)
{
    assert(!nodes[node_index].is_branch);
    assert(nodes[node_index].count == 0);

    // use some margin as shrinking is cheaper than growing
    if (est_count + (est_count/2) < MAX_LEAF_SIZE) {
        return;
    }

    SplitNode(node_index);

    // also estimate newly created child nodes
    for (int n=0; n < BRANCH_COUNT; n++) {
        SetCapacity(est_count / BRANCH_COUNT, branches[nodes[node_index].data][n]);
    }
}

/* Calculates the hash of a node; recursive */
void CTrunkNode::Hash(CHashWriter &writer, uint32_t node_index)  const {

    if (nodes[node_index].is_branch) {

        // hash 16 children together
        CHashWriter branch_writer(SER_GETHASH, 0);
        for (uint32_t childnode : branches[nodes[node_index].data]) {
            Hash(branch_writer, childnode);
        }
        writer << branch_writer.GetHash();
    }
    else {

        // hash the multiset
        multisets[nodes[node_index].data].Hash(writer);
    }
}

/* Sets the passed multiset to the combination of all multiset descendants */
void CTrunkNode::SumAllLeaves(CMultiSet &multiset, uint32_t node_index)
{
    if (nodes[node_index].is_branch) {
        for (uint32_t childnode : branches[nodes[node_index].data]) {
            SumAllLeaves(multiset, childnode);
        }
    }
    else {
        multiset.Combine(multisets[nodes[node_index].data]);
    }
}

void CTrunkNode::GetInfo() const
{
    LogPrintf("Nodes     (%d): %d kb\n", nodes.size(), nodes.size()*sizeof(CNode)/1024);
    LogPrintf("Branches  (%d): %d kb\n", branches.size(), branches.size()*sizeof(CNode)/1024);
    LogPrintf("Multisets (%d): %d kb\n", multisets.size(), multisets.size()*sizeof(CNode)/1024);
}
