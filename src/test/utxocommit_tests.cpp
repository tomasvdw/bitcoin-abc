// Copyright (c) 2014-2016 The Bitcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Tests for CUtxoCommit wrapper.
// Mostly redundant with libsecp256k1_multiset tests

#include "coins.h"
#include "test/test_bitcoin.h"
#include "util.h"
#include <vector>

#include <boost/test/unit_test.hpp>

#include "secp256k1/include/secp256k1_multiset.h"
#include "utxocommit.h"

static COutPoint RandomOutpoint() {
    const COutPoint op(InsecureRand256(), insecure_rand());
    return op;
}

static Coin RandomCoin() {
    const Coin c(CTxOut(Amount(InsecureRandRange(1000)),
                        CScript(InsecureRandBytes(insecure_rand() % 0x3f))),
                 insecure_rand(), InsecureRandBool());
    return c;
}

BOOST_FIXTURE_TEST_SUITE(utxocommit_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(utxo_commit_order) {

    // Test order independence

    const COutPoint op1 = RandomOutpoint();
    const COutPoint op2 = RandomOutpoint();
    const COutPoint op3 = RandomOutpoint();
    const Coin c1 = RandomCoin();
    const Coin c2 = RandomCoin();
    const Coin c3 = RandomCoin();

    CUtxoCommit uc1, uc2, uc3;
    BOOST_CHECK(uc1 == uc2);
    uc1.Add(op1, c1);
    uc1.Add(op2, c2);
    uc1.Add(op3, c3);

    uc2.Add(op2, c2);
    BOOST_CHECK(uc1 != uc2);
    uc2.Add(op3, c3);
    uc2.Add(op1, c1);
    BOOST_CHECK(uc1 == uc2);

    // remove ordering
    uc2.Remove(op2, c2);
    uc2.Remove(op3, c3);

    uc1.Remove(op2, c2);
    uc1.Remove(op3, c3);

    BOOST_CHECK(uc1 == uc2);

    // odd but allowed
    uc3.Remove(op2, c2);
    uc3.Add(op2, c2);
    uc3.Add(op1, c1);
    BOOST_CHECK(uc1 == uc3);
}

BOOST_AUTO_TEST_CASE(utxo_commit_serialize) {

    // Test whether the serialization is as expected

    // some coin & output
    const std::vector<uint8_t> txid = ParseHex(
        "38115d014104c6ec27cffce0823c3fecb162dbd576c88dd7cda0b7b32b096118");
    const uint32_t output = 2;
    const uint32_t height = 7;
    const uint64_t amount = 100;

    const auto script =
        CScript(ParseHex("76A9148ABCDEFABBAABBAABBAABBAABBAABBAABBAABBA88AC"));

    const COutPoint op(uint256(txid), output);
    const Coin coin = Coin(CTxOut(Amount(amount), script), height, false);
    CScript s;

    // find commit
    CUtxoCommit commit;
    commit.Add(op, coin);
    uint256 hash = commit.GetHash();

    // try the same manually
    std::vector<uint8_t> expected;

    // txid
    expected.insert(expected.end(), txid.begin(), txid.end());

    // output
    auto outputbytes = ParseHex("02000000");
    expected.insert(expected.end(), outputbytes.begin(), outputbytes.end());

    // height and coinbase => height * 2
    expected.push_back(14);

    // amount & script
    auto amountbytes = ParseHex("6400000000000000");
    expected.insert(expected.end(), amountbytes.begin(), amountbytes.end());
    expected.push_back(uint8_t(script.size()));
    expected.insert(expected.end(), script.begin(), script.end());

    secp256k1_context *ctx;
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    secp256k1_multiset multiset;
    secp256k1_multiset_init(ctx, &multiset);
    secp256k1_multiset_add(ctx, &multiset, expected.data(), expected.size());

    std::vector<uint8_t> expectedhash(32);
    secp256k1_multiset_finalize(ctx, expectedhash.data(), &multiset);

    secp256k1_context_destroy(ctx);
    BOOST_ASSERT(uint256(expectedhash) == hash);
}

BOOST_AUTO_TEST_SUITE_END()
