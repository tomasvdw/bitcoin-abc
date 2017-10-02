
#ifndef BITCOIN_UTXOCOMMIT_MULTISET_H
#define BITCOIN_UTXOCOMMIT_MULTISET_H


#include <vector>
#include "secp256k1/include/secp256k1_multiset.h"
#include "hash.h"

static secp256k1_context *context = nullptr;

/* C++ wrapper around multiset library */
class CMultiSet {
private:
    secp256k1_multiset multiset;

public:

    CMultiSet() {
        secp256k1_multiset_init(context, &multiset);
    }

    // add or remove element from multiset
    void Update(const std::vector<uint8_t> &element, bool remove) {
        if (remove) {
            secp256k1_multiset_remove(context, &multiset, element.data(), element.size());

        }
        else {
            secp256k1_multiset_add(context, &multiset, element.data(), element.size());

        }
    }

    // this = this + other
    void Combine(const CMultiSet &other) {
        secp256k1_multiset_combine(context, &multiset, &other.multiset);
    }


    void Hash(CHashWriter &writer) const {
        std::vector<uint8_t> hash(32);
        secp256k1_multiset_finalize(context, hash.data(), &multiset);

        writer << hash;
    }
};



#endif // MULTISET_H
