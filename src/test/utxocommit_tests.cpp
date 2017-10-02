

#include <boost/test/unit_test.hpp>
#include "utxocommit/utxocommit.h"
#include "hash.h"
#include "util.h"
#include "utilstrencodings.h"
#include "utxocommit/multiset.h"
#include "test_random.h"

#include "txdb.h"

typedef std::vector<std::vector<uint8_t>> elementset;

/**
 * Small wrapper to allow quick adding/removing in multisets
 */
class CTestMultiSet : public CMultiSet {
public:
    CTestMultiSet() : CMultiSet() {}


    CTestMultiSet(elementset::iterator begin, elementset::iterator end) : CMultiSet() {
        for(auto it = begin; it != end; it++) {
            Update(*it, false);
        }
    }

    CTestMultiSet &UpdateHex(const std::string &hex, int count, bool remove) {
        auto bytes = ParseHex(hex);
        for(;count>0;count--) {
            Update(bytes, remove);
        }
        return *this;
    }

};

/**
 * Fills in a 32-byte buffer using insecure_rand.
 */
static std::vector<uint8_t> get_rand_element() {
    std::vector<uint8_t> bytes(32);
    uint32_t *ptr = (uint32_t *)bytes.data();
    for (int j = 0; j < 8; ++j)
        *(ptr++) = insecure_rand();
    return bytes;
}


BOOST_AUTO_TEST_SUITE(utxocommit_tests)

BOOST_AUTO_TEST_CASE(test_empty) {

    CUtxoCommit utxocommit;

    // manually construct the commitment for no data

    CHashWriter h(SER_GETHASH, 0);
    for(int n=0; n < 16; n++) {
        CTestMultiSet().Hash(h);
    }
    auto empty_commitment = h.GetHash();

    LogPrintf("RESULT=%s", empty_commitment.GetHex());

    BOOST_CHECK(empty_commitment == utxocommit.GetHash());

    // add something
    auto arr = ParseHex("bd13372ddd4f9abf92d4b488d2069a614e27c8a13c060e279472518d6a2155fb");
    utxocommit.Update(arr, false);

    BOOST_CHECK(empty_commitment != utxocommit.GetHash());

    // remove it again
    utxocommit.Update(arr, true);

    BOOST_CHECK(empty_commitment == utxocommit.GetHash());

}

BOOST_AUTO_TEST_CASE(test_normalize) {

    CUtxoCommit utxocommit;

    /* Create 2000 elements all prefixed with 0x3d */
    elementset elms;
    for (int n=0; n < 2000; n++) {
        auto elm = get_rand_element();
        elm[0] = 0x3d;
        elms.push_back(elm);
    }
    /* and 1000 with 3e */
    for (int n=0; n < 1000; n++) {
        auto elm = get_rand_element();
        elm[0] = 0x3e;
        elms.push_back(elm);
    }


    // add 3000 to utxocommit

    for(int n=0; n < 3000; n++)
        utxocommit.Update(elms[n], false);

    // Now we should have a hash
    // with 3 empty hashes, than the hash of these elms, than another 12 empty
    CHashWriter h(SER_GETHASH, 0);
    for(int n=0; n < 3;  n++) { CTestMultiSet().Hash(h); }
    CTestMultiSet(elms.begin(), elms.begin()+3000).Hash(h);
    for(int n=0; n < 12; n++) { CTestMultiSet().Hash(h); }
    BOOST_ASSERT(utxocommit.GetHash() == h.GetHash());


    // Now normalize; this should split it up in two branches
    // TODO: test normalization individually
}


/* This implements a UtxoDataSet wrapper around std::set
 * In the production code, this will be encapsulation the leveldb chainstate
*/
typedef std::set<std::vector<uint8_t>> set_t;

class CSetCursor : public CUtxoDataSetCursor {
    set_t::const_iterator it;
    set_t::const_iterator begin;
    set_t::const_iterator end;

public:
    CSetCursor(set_t::const_iterator begin, set_t::const_iterator end)
        : it(begin), begin(begin), end(end) {};

    const std::vector<uint8_t> *Next() {
        if (it == end)
            return nullptr;
        else
            return &*(it++);
    }
};


class CSet : public  CUtxoDataSet {

    set_t set;
public:
    CSet(set_t &set) : set(set) {};
    uint64_t GetSize() const { return set.size(); }

    std::unique_ptr<CUtxoDataSetCursor> GetRange(const std::vector<uint8_t> &prefix, uint32_t bits)  const {

        // convert prefix to range
        std::vector<uint8_t> start = prefix;
        std::vector<uint8_t> end = prefix;
        start.resize((bits+4)/8);
        if (bits % 8 == 4) start.back() &= 0xf0;
        end.resize((bits+4)/8);
        if (bits % 8 == 4) end.back() |= 0xf;
        end.insert(end.end(), 36, 0xff);

        //LogPrintf("Acquiring range %s-%s\n", HexStr(start), HexStr(end));
        // we use twice lower bound here, because upper is given as prefix string of the next range
        return std::unique_ptr<CUtxoDataSetCursor> {
            new CSetCursor(set.lower_bound(start), set.lower_bound(end))
        };
    }

};


BOOST_AUTO_TEST_CASE(test_initial_load) {

    CUtxoCommit utxocommit;

    std::set<std::vector<uint8_t>> elms;
    for (int n=0; n < 100000; n++) {
        auto elm = get_rand_element();
        elms.insert(elm);
    }

    utxocommit.InitialLoad(CSet(elms));
    utxocommit.GetInfo();

    // do the same one-by-one
    CUtxoCommit utxoseq;
    for(auto elm : elms) {
        utxoseq.Update(elm, false);
    }
    utxoseq.Normalize(CSet(elms));
    utxocommit.GetInfo();

    BOOST_CHECK(utxocommit.GetHash() == utxoseq.GetHash());

}

BOOST_AUTO_TEST_CASE(test_initial_load_large) {

    CUtxoCommit utxocommit;
    auto size = 50*1000*1000;

    std::set<std::vector<uint8_t>> elms;
    for (int n=0; n < 50000000; n++) {
        auto elm = get_rand_element();
        elms.insert(elm);
    }

    utxocommit.InitialLoad(CSet(elms));

    utxocommit.GetInfo();

    // 100 blocks
    for(int n=0; n < 100; n++) {
        elms.
    }
}

BOOST_AUTO_TEST_SUITE_END();

