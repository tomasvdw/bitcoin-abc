
#include <thread>

#include "hash.h"
#include "util.h"
#include "utxocommit.h"
#include "node.h"


CUtxoCommit::CUtxoCommit() {

    // Use a pointer instead of an array only to hide the implementation
    this->trunkNodes = new CTrunkNode[BRANCH_COUNT];
}


CUtxoCommit::~CUtxoCommit()
{
    delete[] this->trunkNodes;
}


void CUtxoCommit::Update(const std::vector<uint8_t> &element, bool remove)
{
    static_assert(BRANCH_COUNT == 16);
    assert(element.size() >= MIN_ELEMENT_SIZE);

    // pass to the right trunk node
    this->trunkNodes[(element[0] >> 4) & 0xF].Update(element, remove);
}



void CUtxoCommit::Normalize(const CUtxoDataSet &set)
{
    for(int n=0; n < BRANCH_COUNT; n++) {
        this->trunkNodes[n].Normalize(set);
    }
}

uint256 CUtxoCommit::GetHash() const
{
    CHashWriter writer(SER_GETHASH, 0);
    for(int n=0; n < BRANCH_COUNT; n++) {
        this->trunkNodes[n].Hash(writer, 0);
    }

    return writer.GetHash();

}

static void initial_load_thread(const CUtxoDataSet *set, CTrunkNode *trunk, int trunknr)
{
    uint64_t est_count = set->GetSize();
    trunk->SetCapacity(est_count / BRANCH_COUNT, 0);

    /* create a range for this trunk */
    static_assert(BRANCH_COUNT == 16);
    std::vector<uint8_t> prefix = { uint8_t((trunknr << 4) & 0xF0) };
    std::unique_ptr<CUtxoDataSetCursor> cur = set->GetRange(prefix, BRANCH_BITS);

    int count=0;
    for(;;) {
        auto elm = cur->Next();
        if (!elm)
            break;

        trunk->Update(*elm, false);
        count++;
    }

    trunk->Normalize(*set);
}

void CUtxoCommit::InitialLoad(const CUtxoDataSet &set)
{
    LogPrintf("UTXO-Commit: Initial load of %d elements\n", set.GetSize());

    std::vector<std::thread> threads;
    for(int t = 0;t < BRANCH_COUNT;t++)
    {
        threads.push_back(std::thread(initial_load_thread, &set, trunkNodes+t, t));
    }
    std::for_each(threads.begin(),threads.end(),[](std::thread &t){t.join();});

    LogPrintf("UTXO-Commit: Initial load done\n");
}

void CUtxoCommit::GetInfo() const {
    for(int n=0; n < BRANCH_COUNT; n++) {
        this->trunkNodes[n].GetInfo();
    }
}
