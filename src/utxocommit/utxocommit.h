
#include <cstdint>
#include <vector>
#include <iostream>
#include "uint256.h"

class CUtxoDataSet;
class CTrunkNode;

/* CUtxoCommit maintains an in memory tree to quickly calculate and
 * update the utxo commitment as per BIP-UtxoCommitBucket */
class CUtxoCommit {

public:

    CUtxoCommit();
    ~CUtxoCommit();

    /* Adds or removes an element from the tree.
     *
     * Thread safe */
    void Update(const std::vector<uint8_t> &element, bool remove);


    /* Ensures each branch > MAX_LEAF_SIZE
     * and each leaf <= MAX_LEAF_SIZE
     *
     * The provider will be used to acquire the data
     * needed to split leafs 
     *
     * Thread safe */
    void Normalize(const CUtxoDataSet &set);


    /* Loads all elements from the data provider
     *
     * Not thread safe */
     void InitialLoad(const CUtxoDataSet &set);

    /* Retrieves the commitment hash
     *
     * Not thread safe (and meaningless if ops are in progress) */
    uint256 GetHash() const;

    /* TODOs */
    void Serialize(std::ostream &out) const;
    void Deserialize(std::istream &in);
    void GetInfo() const;

private:

    /* The 16 root branches called "trunknodes";
     * They are treated specially to provide per trunk-node
     * locking and data ownership.
     * We use a pointer instead of an array to hide the implementation */
    CTrunkNode *trunkNodes;

};



/*
 * Classes that must be inherited and encapsulate range-queries to the db
 */

class CUtxoDataSetCursor {

public:
    virtual const std::vector<uint8_t> *Next() = 0;
};


class CUtxoDataSet {
public:

    virtual uint64_t GetSize() const  = 0;
    virtual std::unique_ptr<CUtxoDataSetCursor> GetRange(const std::vector<uint8_t> &prefix, uint32_t bits) const = 0;
};

