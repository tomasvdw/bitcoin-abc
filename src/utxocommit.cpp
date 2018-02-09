// Copyright (c) 2017 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "utxocommit.h"

namespace {
secp256k1_context *secp256k1_context_multiset;
int secp256k1_context_refcount = 0;
}

// Constructs empty CUtxoCommit
CUtxoCommit::CUtxoCommit() {
    if (secp256k1_context_refcount == 0) {
        secp256k1_context_multiset =
            secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    }
    secp256k1_context_refcount++;
    secp256k1_multiset_init(secp256k1_context_multiset, &multiset);
}

CUtxoCommit::~CUtxoCommit() {
    secp256k1_context_refcount--;
    if (secp256k1_context_refcount == 0) {
        secp256k1_context_destroy(secp256k1_context_multiset);
    }
}

// Construct by combining two other CUtxoCommits
CUtxoCommit::CUtxoCommit(const CUtxoCommit &commit1, const CUtxoCommit &commit2)
    : CUtxoCommit() {
    secp256k1_multiset_combine(secp256k1_context_multiset, &this->multiset,
                               &commit1.multiset);
    secp256k1_multiset_combine(secp256k1_context_multiset, &this->multiset,
                               &commit2.multiset);
}

// Adds a TXO from multiset
void CUtxoCommit::Add(const COutPoint &out, const Coin &element) {

    CDataStream txo(SER_NETWORK, PROTOCOL_VERSION);
    txo << out << element;
    secp256k1_multiset_add(secp256k1_context_multiset, &multiset,
                           (const uint8_t *)&txo[0], txo.size());
}

// Removes a TXO from multiset
void CUtxoCommit::Remove(const COutPoint &out, const Coin &element) {

    CDataStream txo(SER_NETWORK, PROTOCOL_VERSION);
    txo << out << element;
    secp256k1_multiset_remove(secp256k1_context_multiset, &multiset,
                              (const uint8_t *)&txo[0], txo.size());
}

void CUtxoCommit::Clear() {
    secp256k1_multiset_init(secp256k1_context_multiset, &multiset);
}

uint256 CUtxoCommit::GetHash() const {

    std::vector<uint8_t> hash(32);
    secp256k1_multiset_finalize(secp256k1_context_multiset, hash.data(),
                                &multiset);
    return uint256(hash);
}
