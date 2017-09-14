// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/wallet.h"

#include "base58.h"
#include "chain.h"
#include "checkpoints.h"
#include "config.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "key.h"
#include "keystore.h"
#include "net.h"
#include "policy/policy.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "script/sign.h"
#include "timedata.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validation.h"
#include "wallet/coincontrol.h"
#include "wallet/finaltx.h"

#include <cassert>

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>

CWallet *pwalletMain = nullptr;

/** Transaction fee set by the user */
CFeeRate payTxFee(DEFAULT_TRANSACTION_FEE);
unsigned int nTxConfirmTarget = DEFAULT_TX_CONFIRM_TARGET;
bool bSpendZeroConfChange = DEFAULT_SPEND_ZEROCONF_CHANGE;
bool fSendFreeTransactions = DEFAULT_SEND_FREE_TRANSACTIONS;

const char *DEFAULT_WALLET_DAT = "wallet.dat";
const uint32_t BIP32_HARDENED_KEY_LIMIT = 0x80000000;

/**
 * Fees smaller than this (in satoshi) are considered zero fee (for transaction
 * creation)
 * Override with -mintxfee
 */
CFeeRate CWallet::minTxFee = CFeeRate(DEFAULT_TRANSACTION_MINFEE);

/**
 * If fee estimation does not have enough data to provide estimates, use this
 * fee instead. Has no effect if not using fee estimation.
 * Override with -fallbackfee
 */
CFeeRate CWallet::fallbackFee = CFeeRate(DEFAULT_FALLBACK_FEE);

const uint256 CMerkleTx::ABANDON_HASH(uint256S(
    "0000000000000000000000000000000000000000000000000000000000000001"));

/** @defgroup mapWallet
 *
 * @{
 */

struct CompareValueOnly {
    bool operator()(
        const std::pair<CAmount, std::pair<const CWalletTx *, unsigned int>>
            &t1,
        const std::pair<CAmount, std::pair<const CWalletTx *, unsigned int>>
            &t2) const {
        return t1.first < t2.first;
    }
};

std::string COutput::ToString() const {
    return strprintf("COutput(%s, %d, %d) [%s]", tx->GetId().ToString(), i,
                     nDepth, FormatMoney(tx->tx->vout[i].nValue));
}

const CWalletTx *CWallet::GetWalletTx(const utxid_t &utxid) const {
    LOCK(cs_wallet);
    auto wtx_iter = std::find_if(
                mapWallet.begin(),
                mapWallet.end(),
                [&utxid](const std::pair<txid_t, CWalletTx> & t) -> bool {
                      return t.second.tx->GetUtxid(MALFIX_MODE_LEGACY) == utxid;
    });

    if (wtx_iter == mapWallet.end()) {
        return nullptr;
    }
    return &(wtx_iter->second);

}

const CWalletTx *CWallet::GetWalletTx(const txid_t &txid) const {
    LOCK(cs_wallet);

    std::map<txid_t, CWalletTx>::const_iterator it = mapWallet.find(txid);
    if (it == mapWallet.end()) {
        return nullptr;
    }

    return &(it->second);

}

CPubKey CWallet::GenerateNewKey() {
    // mapKeyMetadata
    AssertLockHeld(cs_wallet);
    // default to compressed public keys if we want 0.6.0 wallets
    bool fCompressed = CanSupportFeature(FEATURE_COMPRPUBKEY);

    CKey secret;

    // Create new metadata
    int64_t nCreationTime = GetTime();
    CKeyMetadata metadata(nCreationTime);

    // use HD key derivation if HD was enabled during wallet creation
    if (IsHDEnabled()) {
        DeriveNewChildKey(metadata, secret);
    } else {
        secret.MakeNewKey(fCompressed);
    }

    // Compressed public keys were introduced in version 0.6.0
    if (fCompressed) {
        SetMinVersion(FEATURE_COMPRPUBKEY);
    }

    CPubKey pubkey = secret.GetPubKey();
    assert(secret.VerifyPubKey(pubkey));

    mapKeyMetadata[pubkey.GetID()] = metadata;
    UpdateTimeFirstKey(nCreationTime);

    if (!AddKeyPubKey(secret, pubkey)) {
        throw std::runtime_error(std::string(__func__) + ": AddKey failed");
    }

    return pubkey;
}

void CWallet::DeriveNewChildKey(CKeyMetadata &metadata, CKey &secret) {
    // for now we use a fixed keypath scheme of m/0'/0'/k
    // master key seed (256bit)
    CKey key;
    // hd master key
    CExtKey masterKey;
    // key at m/0'
    CExtKey accountKey;
    // key at m/0'/0'
    CExtKey externalChainChildKey;
    // key at m/0'/0'/<n>'
    CExtKey childKey;

    // try to get the master key
    if (!GetKey(hdChain.masterKeyID, key)) {
        throw std::runtime_error(std::string(__func__) +
                                 ": Master key not found");
    }

    masterKey.SetMaster(key.begin(), key.size());

    // derive m/0'
    // use hardened derivation (child keys >= 0x80000000 are hardened after
    // bip32)
    masterKey.Derive(accountKey, BIP32_HARDENED_KEY_LIMIT);

    // derive m/0'/0'
    accountKey.Derive(externalChainChildKey, BIP32_HARDENED_KEY_LIMIT);

    // derive child key at next index, skip keys already known to the wallet
    do {
        // always derive hardened keys
        // childIndex | BIP32_HARDENED_KEY_LIMIT = derive childIndex in hardened
        // child-index-range
        // example: 1 | BIP32_HARDENED_KEY_LIMIT == 0x80000001 == 2147483649
        externalChainChildKey.Derive(childKey, hdChain.nExternalChainCounter |
                                                   BIP32_HARDENED_KEY_LIMIT);
        metadata.hdKeypath =
            "m/0'/0'/" + std::to_string(hdChain.nExternalChainCounter) + "'";
        metadata.hdMasterKeyID = hdChain.masterKeyID;
        // increment childkey index
        hdChain.nExternalChainCounter++;
    } while (HaveKey(childKey.key.GetPubKey().GetID()));
    secret = childKey.key;

    // update the chain model in the database
    if (!CWalletDB(strWalletFile).WriteHDChain(hdChain)) {
        throw std::runtime_error(std::string(__func__) +
                                 ": Writing HD chain model failed");
    }
}

bool CWallet::AddKeyPubKey(const CKey &secret, const CPubKey &pubkey) {
    // mapKeyMetadata
    AssertLockHeld(cs_wallet);
    if (!CCryptoKeyStore::AddKeyPubKey(secret, pubkey)) {
        return false;
    }

    // Check if we need to remove from watch-only.
    CScript script;
    script = GetScriptForDestination(pubkey.GetID());
    if (HaveWatchOnly(script)) {
        RemoveWatchOnly(script);
    }

    script = GetScriptForRawPubKey(pubkey);
    if (HaveWatchOnly(script)) {
        RemoveWatchOnly(script);
    }

    if (!fFileBacked) {
        return true;
    }

    if (IsCrypted()) {
        return true;
    }

    return CWalletDB(strWalletFile)
        .WriteKey(pubkey, secret.GetPrivKey(), mapKeyMetadata[pubkey.GetID()]);
}

bool CWallet::AddCryptedKey(const CPubKey &vchPubKey,
                            const std::vector<uint8_t> &vchCryptedSecret) {
    if (!CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret)) {
        return false;
    }

    if (!fFileBacked) {
        return true;
    }

    LOCK(cs_wallet);
    if (pwalletdbEncryption) {
        return pwalletdbEncryption->WriteCryptedKey(
            vchPubKey, vchCryptedSecret, mapKeyMetadata[vchPubKey.GetID()]);
    }

    return CWalletDB(strWalletFile)
        .WriteCryptedKey(vchPubKey, vchCryptedSecret,
                         mapKeyMetadata[vchPubKey.GetID()]);
}

bool CWallet::LoadKeyMetadata(const CTxDestination &keyID,
                              const CKeyMetadata &meta) {
    // mapKeyMetadata
    AssertLockHeld(cs_wallet);
    UpdateTimeFirstKey(meta.nCreateTime);
    mapKeyMetadata[keyID] = meta;
    return true;
}

bool CWallet::LoadCryptedKey(const CPubKey &vchPubKey,
                             const std::vector<uint8_t> &vchCryptedSecret) {
    return CCryptoKeyStore::AddCryptedKey(vchPubKey, vchCryptedSecret);
}

void CWallet::UpdateTimeFirstKey(int64_t nCreateTime) {
    AssertLockHeld(cs_wallet);
    if (nCreateTime <= 1) {
        // Cannot determine birthday information, so set the wallet birthday to
        // the beginning of time.
        nTimeFirstKey = 1;
    } else if (!nTimeFirstKey || nCreateTime < nTimeFirstKey) {
        nTimeFirstKey = nCreateTime;
    }
}

bool CWallet::AddCScript(const CScript &redeemScript) {
    if (!CCryptoKeyStore::AddCScript(redeemScript)) {
        return false;
    }

    if (!fFileBacked) {
        return true;
    }

    return CWalletDB(strWalletFile)
        .WriteCScript(Hash160(redeemScript), redeemScript);
}

bool CWallet::LoadCScript(const CScript &redeemScript) {
    /**
     * A sanity check was added in pull #3843 to avoid adding redeemScripts that
     * never can be redeemed. However, old wallets may still contain these. Do
     * not add them to the wallet and warn.
     */
    if (redeemScript.size() > MAX_SCRIPT_ELEMENT_SIZE) {
        std::string strAddr =
            CBitcoinAddress(CScriptID(redeemScript)).ToString();
        LogPrintf("%s: Warning: This wallet contains a redeemScript of size %i "
                  "which exceeds maximum size %i thus can never be redeemed. "
                  "Do not use address %s.\n",
                  __func__, redeemScript.size(), MAX_SCRIPT_ELEMENT_SIZE,
                  strAddr);
        return true;
    }

    return CCryptoKeyStore::AddCScript(redeemScript);
}

bool CWallet::AddWatchOnly(const CScript &dest) {
    if (!CCryptoKeyStore::AddWatchOnly(dest)) {
        return false;
    }

    const CKeyMetadata &meta = mapKeyMetadata[CScriptID(dest)];
    UpdateTimeFirstKey(meta.nCreateTime);
    NotifyWatchonlyChanged(true);

    if (!fFileBacked) {
        return true;
    }

    return CWalletDB(strWalletFile).WriteWatchOnly(dest, meta);
}

bool CWallet::AddWatchOnly(const CScript &dest, int64_t nCreateTime) {
    mapKeyMetadata[CScriptID(dest)].nCreateTime = nCreateTime;
    return AddWatchOnly(dest);
}

bool CWallet::RemoveWatchOnly(const CScript &dest) {
    AssertLockHeld(cs_wallet);
    if (!CCryptoKeyStore::RemoveWatchOnly(dest)) {
        return false;
    }

    if (!HaveWatchOnly()) {
        NotifyWatchonlyChanged(false);
    }

    if (fFileBacked && !CWalletDB(strWalletFile).EraseWatchOnly(dest)) {
        return false;
    }

    return true;
}

bool CWallet::LoadWatchOnly(const CScript &dest) {
    return CCryptoKeyStore::AddWatchOnly(dest);
}

bool CWallet::Unlock(const SecureString &strWalletPassphrase) {
    CCrypter crypter;
    CKeyingMaterial vMasterKey;

    LOCK(cs_wallet);
    for (const MasterKeyMap::value_type &pMasterKey : mapMasterKeys) {
        if (!crypter.SetKeyFromPassphrase(
                strWalletPassphrase, pMasterKey.second.vchSalt,
                pMasterKey.second.nDeriveIterations,
                pMasterKey.second.nDerivationMethod)) {
            return false;
        }

        if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey)) {
            // try another master key
            continue;
        }

        if (CCryptoKeyStore::Unlock(vMasterKey)) {
            return true;
        }
    }

    return false;
}

bool CWallet::ChangeWalletPassphrase(
    const SecureString &strOldWalletPassphrase,
    const SecureString &strNewWalletPassphrase) {
    bool fWasLocked = IsLocked();

    LOCK(cs_wallet);
    Lock();

    CCrypter crypter;
    CKeyingMaterial vMasterKey;
    for (MasterKeyMap::value_type &pMasterKey : mapMasterKeys) {
        if (!crypter.SetKeyFromPassphrase(
                strOldWalletPassphrase, pMasterKey.second.vchSalt,
                pMasterKey.second.nDeriveIterations,
                pMasterKey.second.nDerivationMethod)) {
            return false;
        }

        if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, vMasterKey)) {
            return false;
        }

        if (CCryptoKeyStore::Unlock(vMasterKey)) {
            int64_t nStartTime = GetTimeMillis();
            crypter.SetKeyFromPassphrase(strNewWalletPassphrase,
                                         pMasterKey.second.vchSalt,
                                         pMasterKey.second.nDeriveIterations,
                                         pMasterKey.second.nDerivationMethod);
            pMasterKey.second.nDeriveIterations =
                pMasterKey.second.nDeriveIterations *
                (100 / ((double)(GetTimeMillis() - nStartTime)));

            nStartTime = GetTimeMillis();
            crypter.SetKeyFromPassphrase(strNewWalletPassphrase,
                                         pMasterKey.second.vchSalt,
                                         pMasterKey.second.nDeriveIterations,
                                         pMasterKey.second.nDerivationMethod);
            pMasterKey.second.nDeriveIterations =
                (pMasterKey.second.nDeriveIterations +
                 pMasterKey.second.nDeriveIterations * 100 /
                     double(GetTimeMillis() - nStartTime)) /
                2;

            if (pMasterKey.second.nDeriveIterations < 25000) {
                pMasterKey.second.nDeriveIterations = 25000;
            }

            LogPrintf(
                "Wallet passphrase changed to an nDeriveIterations of %i\n",
                pMasterKey.second.nDeriveIterations);

            if (!crypter.SetKeyFromPassphrase(
                    strNewWalletPassphrase, pMasterKey.second.vchSalt,
                    pMasterKey.second.nDeriveIterations,
                    pMasterKey.second.nDerivationMethod)) {
                return false;
            }

            if (!crypter.Encrypt(vMasterKey, pMasterKey.second.vchCryptedKey)) {
                return false;
            }

            CWalletDB(strWalletFile)
                .WriteMasterKey(pMasterKey.first, pMasterKey.second);
            if (fWasLocked) {
                Lock();
            }

            return true;
        }
    }

    return false;
}

void CWallet::SetBestChain(const CBlockLocator &loc) {
    CWalletDB walletdb(strWalletFile);
    walletdb.WriteBestBlock(loc);
}

bool CWallet::SetMinVersion(enum WalletFeature nVersion, CWalletDB *pwalletdbIn,
                            bool fExplicit) {
    // nWalletVersion
    LOCK(cs_wallet);
    if (nWalletVersion >= nVersion) {
        return true;
    }

    // When doing an explicit upgrade, if we pass the max version permitted,
    // upgrade all the way.
    if (fExplicit && nVersion > nWalletMaxVersion) {
        nVersion = FEATURE_LATEST;
    }

    nWalletVersion = nVersion;

    if (nVersion > nWalletMaxVersion) {
        nWalletMaxVersion = nVersion;
    }

    if (fFileBacked) {
        CWalletDB *pwalletdb =
            pwalletdbIn ? pwalletdbIn : new CWalletDB(strWalletFile);
        if (nWalletVersion > 40000) {
            pwalletdb->WriteMinVersion(nWalletVersion);
        }

        if (!pwalletdbIn) {
            delete pwalletdb;
        }
    }

    return true;
}

bool CWallet::SetMaxVersion(int nVersion) {
    // nWalletVersion, nWalletMaxVersion
    LOCK(cs_wallet);

    // Cannot downgrade below current version
    if (nWalletVersion > nVersion) {
        return false;
    }

    nWalletMaxVersion = nVersion;

    return true;
}

std::set<txid_t> CWallet::GetConflicts(const txid_t &txid) const {
    std::set<txid_t> result;
    AssertLockHeld(cs_wallet);

    std::map<txid_t, CWalletTx>::const_iterator it = mapWallet.find(txid);
    if (it == mapWallet.end()) {
        return result;
    }

    const CWalletTx &wtx = it->second;

    std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range;

    for (const CTxIn &txin : wtx.tx->vin) {
        if (mapTxSpends.count(txin.prevout) <= 1) {
            // No conflict if zero or one spends.
            continue;
        }

        range = mapTxSpends.equal_range(txin.prevout);
        for (TxSpends::const_iterator _it = range.first; _it != range.second;
             ++_it) {
            result.insert(_it->second);
        }
    }

    return result;
}

void CWallet::Flush(bool shutdown) {
    bitdb.Flush(shutdown);
}

bool CWallet::Verify() {
    if (GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET)) {
        return true;
    }

    LogPrintf("Using BerkeleyDB version %s\n", DbEnv::version(0, 0, 0));
    std::string walletFile = GetArg("-wallet", DEFAULT_WALLET_DAT);

    LogPrintf("Using wallet %s\n", walletFile);
    uiInterface.InitMessage(_("Verifying wallet..."));

    // Wallet file must be a plain filename without a directory.
    if (walletFile !=
        boost::filesystem::basename(walletFile) +
            boost::filesystem::extension(walletFile)) {
        return InitError(
            strprintf(_("Wallet %s resides outside data directory %s"),
                      walletFile, GetDataDir().string()));
    }

    if (!bitdb.Open(GetDataDir())) {
        // Try moving the database env out of the way.
        boost::filesystem::path pathDatabase = GetDataDir() / "database";
        boost::filesystem::path pathDatabaseBak =
            GetDataDir() / strprintf("database.%d.bak", GetTime());
        try {
            boost::filesystem::rename(pathDatabase, pathDatabaseBak);
            LogPrintf("Moved old %s to %s. Retrying.\n", pathDatabase.string(),
                      pathDatabaseBak.string());
        } catch (const boost::filesystem::filesystem_error &) {
            // Failure is ok (well, not really, but it's not worse than what we
            // started with)
        }

        // try again
        if (!bitdb.Open(GetDataDir())) {
            // If it still fails, it probably means we can't even create the
            // database env.
            return InitError(strprintf(
                _("Error initializing wallet database environment %s!"),
                GetDataDir()));
        }
    }

    if (GetBoolArg("-salvagewallet", false)) {
        // Recover readable keypairs:
        if (!CWalletDB::Recover(bitdb, walletFile, true)) return false;
    }

    if (boost::filesystem::exists(GetDataDir() / walletFile)) {
        CDBEnv::VerifyResult r = bitdb.Verify(walletFile, CWalletDB::Recover);
        if (r == CDBEnv::RECOVER_OK) {
            InitWarning(strprintf(
                _("Warning: Wallet file corrupt, data salvaged!"
                  " Original %s saved as %s in %s; if"
                  " your balance or transactions are incorrect you should"
                  " restore from a backup."),
                walletFile, "wallet.{timestamp}.bak", GetDataDir()));
        }

        if (r == CDBEnv::RECOVER_FAIL) {
            return InitError(
                strprintf(_("%s corrupt, salvage failed"), walletFile));
        }
    }

    return true;
}

void CWallet::SyncMetaData(
    std::pair<TxSpends::iterator, TxSpends::iterator> range) {
    // We want all the wallet transactions in range to have the same metadata as
    // the oldest (smallest nOrderPos).
    // So: find smallest nOrderPos:

    int nMinOrderPos = std::numeric_limits<int>::max();
    const CWalletTx *copyFrom = nullptr;
    for (TxSpends::iterator it = range.first; it != range.second; ++it) {
        const txid_t &hash = it->second;
        int n = mapWallet[hash].nOrderPos;
        if (n < nMinOrderPos) {
            nMinOrderPos = n;
            copyFrom = &mapWallet[hash];
        }
    }

    // Now copy data from copyFrom to rest:
    for (TxSpends::iterator it = range.first; it != range.second; ++it) {
        const txid_t &hash = it->second;
        CWalletTx *copyTo = &mapWallet[hash];
        if (copyFrom == copyTo) {
            continue;
        }

        if (!copyFrom->IsEquivalentTo(*copyTo)) {
            continue;
        }

        copyTo->mapValue = copyFrom->mapValue;
        copyTo->vOrderForm = copyFrom->vOrderForm;
        // fTimeReceivedIsTxTime not copied on purpose nTimeReceived not copied
        // on purpose.
        copyTo->nTimeSmart = copyFrom->nTimeSmart;
        copyTo->fFromMe = copyFrom->fFromMe;
        copyTo->strFromAccount = copyFrom->strFromAccount;
        // nOrderPos not copied on purpose cached members not copied on purpose.
    }
}

/**
 * Outpoint is spent if any non-conflicted transaction, spends it:
 */
bool CWallet::IsSpent(const COutPoint &outpoint) const {

    std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range;
    range = mapTxSpends.equal_range(outpoint);

    for (TxSpends::const_iterator it = range.first; it != range.second; ++it) {
        const txid_t &wtxid = it->second;
        std::map<txid_t, CWalletTx>::const_iterator mit =
            mapWallet.find(wtxid);
        if (mit != mapWallet.end()) {
            int depth = mit->second.GetDepthInMainChain();
            if (depth > 0 || (depth == 0 && !mit->second.isAbandoned())) {
                // Spent
                return true;
            }
        }
    }

    return false;
}

void CWallet::AddToSpends(const COutPoint &outpoint, const txid_t &wtxid) {
    mapTxSpends.insert(std::make_pair(outpoint, wtxid));

    std::pair<TxSpends::iterator, TxSpends::iterator> range;
    range = mapTxSpends.equal_range(outpoint);
    SyncMetaData(range);
}

void CWallet::AddToSpends(const txid_t &wtxid) {
    assert(mapWallet.count(wtxid));
    CWalletTx &thisTx = mapWallet[wtxid];
    // Coinbases don't spend anything!
    if (thisTx.IsCoinBase()) {
        return;
    }

    for (const CTxIn &txin : thisTx.tx->vin) {
        AddToSpends(txin.prevout, wtxid);
    }
}

bool CWallet::EncryptWallet(const SecureString &strWalletPassphrase) {
    if (IsCrypted()) {
        return false;
    }

    CKeyingMaterial vMasterKey;

    vMasterKey.resize(WALLET_CRYPTO_KEY_SIZE);
    GetStrongRandBytes(&vMasterKey[0], WALLET_CRYPTO_KEY_SIZE);

    CMasterKey kMasterKey;

    kMasterKey.vchSalt.resize(WALLET_CRYPTO_SALT_SIZE);
    GetStrongRandBytes(&kMasterKey.vchSalt[0], WALLET_CRYPTO_SALT_SIZE);

    CCrypter crypter;
    int64_t nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, 25000,
                                 kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations =
        2500000 / ((double)(GetTimeMillis() - nStartTime));

    nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt,
                                 kMasterKey.nDeriveIterations,
                                 kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations =
        (kMasterKey.nDeriveIterations +
         kMasterKey.nDeriveIterations * 100 /
             ((double)(GetTimeMillis() - nStartTime))) /
        2;

    if (kMasterKey.nDeriveIterations < 25000) {
        kMasterKey.nDeriveIterations = 25000;
    }

    LogPrintf("Encrypting Wallet with an nDeriveIterations of %i\n",
              kMasterKey.nDeriveIterations);

    if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt,
                                      kMasterKey.nDeriveIterations,
                                      kMasterKey.nDerivationMethod)) {
        return false;
    }

    if (!crypter.Encrypt(vMasterKey, kMasterKey.vchCryptedKey)) {
        return false;
    }

    {
        LOCK(cs_wallet);
        mapMasterKeys[++nMasterKeyMaxID] = kMasterKey;
        if (fFileBacked) {
            assert(!pwalletdbEncryption);
            pwalletdbEncryption = new CWalletDB(strWalletFile);
            if (!pwalletdbEncryption->TxnBegin()) {
                delete pwalletdbEncryption;
                pwalletdbEncryption = nullptr;
                return false;
            }

            pwalletdbEncryption->WriteMasterKey(nMasterKeyMaxID, kMasterKey);
        }

        if (!EncryptKeys(vMasterKey)) {
            if (fFileBacked) {
                pwalletdbEncryption->TxnAbort();
                delete pwalletdbEncryption;
            }

            // We now probably have half of our keys encrypted in memory, and
            // half not... die and let the user reload the unencrypted wallet.
            assert(false);
        }

        // Encryption was introduced in version 0.4.0
        SetMinVersion(FEATURE_WALLETCRYPT, pwalletdbEncryption, true);

        if (fFileBacked) {
            if (!pwalletdbEncryption->TxnCommit()) {
                delete pwalletdbEncryption;
                // We now have keys encrypted in memory, but not on disk... die
                // to avoid confusion and let the user reload the unencrypted
                // wallet.
                assert(false);
            }

            delete pwalletdbEncryption;
            pwalletdbEncryption = nullptr;
        }

        Lock();
        Unlock(strWalletPassphrase);

        // If we are using HD, replace the HD master key (seed) with a new one.
        if (IsHDEnabled()) {
            CKey key;
            CPubKey masterPubKey = GenerateNewHDMasterKey();
            if (!SetHDMasterKey(masterPubKey)) {
                return false;
            }
        }

        NewKeyPool();
        Lock();

        // Need to completely rewrite the wallet file; if we don't, bdb might
        // keep bits of the unencrypted private key in slack space in the
        // database file.
        CDB::Rewrite(strWalletFile);
    }

    NotifyStatusChanged(this);
    return true;
}

DBErrors CWallet::ReorderTransactions() {
    LOCK(cs_wallet);
    CWalletDB walletdb(strWalletFile);

    // Old wallets didn't have any defined order for transactions. Probably a
    // bad idea to change the output of this.

    // First: get all CWalletTx and CAccountingEntry into a sorted-by-time
    // multimap.
    typedef std::pair<CWalletTx *, CAccountingEntry *> TxPair;
    typedef std::multimap<int64_t, TxPair> TxItems;
    TxItems txByTime;

    for (std::map<txid_t, CWalletTx>::iterator it = mapWallet.begin();
         it != mapWallet.end(); ++it) {
        CWalletTx *wtx = &((*it).second);
        txByTime.insert(
            std::make_pair(wtx->nTimeReceived, TxPair(wtx, nullptr)));
    }

    std::list<CAccountingEntry> acentries;
    walletdb.ListAccountCreditDebit("", acentries);
    for (CAccountingEntry &entry : acentries) {
        txByTime.insert(std::make_pair(entry.nTime, TxPair(nullptr, &entry)));
    }

    nOrderPosNext = 0;
    std::vector<int64_t> nOrderPosOffsets;
    for (TxItems::iterator it = txByTime.begin(); it != txByTime.end(); ++it) {
        CWalletTx *const pwtx = (*it).second.first;
        CAccountingEntry *const pacentry = (*it).second.second;
        int64_t &nOrderPos =
            (pwtx != 0) ? pwtx->nOrderPos : pacentry->nOrderPos;

        if (nOrderPos == -1) {
            nOrderPos = nOrderPosNext++;
            nOrderPosOffsets.push_back(nOrderPos);

            if (pwtx) {
                if (!walletdb.WriteTx(*pwtx)) {
                    return DB_LOAD_FAIL;
                }
            } else if (!walletdb.WriteAccountingEntry(pacentry->nEntryNo,
                                                      *pacentry)) {
                return DB_LOAD_FAIL;
            }
        } else {
            int64_t nOrderPosOff = 0;
            for (const int64_t &nOffsetStart : nOrderPosOffsets) {
                if (nOrderPos >= nOffsetStart) {
                    ++nOrderPosOff;
                }
            }

            nOrderPos += nOrderPosOff;
            nOrderPosNext = std::max(nOrderPosNext, nOrderPos + 1);

            if (!nOrderPosOff) {
                continue;
            }

            // Since we're changing the order, write it back.
            if (pwtx) {
                if (!walletdb.WriteTx(*pwtx)) {
                    return DB_LOAD_FAIL;
                }
            } else if (!walletdb.WriteAccountingEntry(pacentry->nEntryNo,
                                                      *pacentry)) {
                return DB_LOAD_FAIL;
            }
        }
    }

    walletdb.WriteOrderPosNext(nOrderPosNext);

    return DB_LOAD_OK;
}

int64_t CWallet::IncOrderPosNext(CWalletDB *pwalletdb) {
    // nOrderPosNext
    AssertLockHeld(cs_wallet);
    const int64_t nRet = nOrderPosNext++;
    if (pwalletdb) {
        pwalletdb->WriteOrderPosNext(nOrderPosNext);
    } else {
        CWalletDB(strWalletFile).WriteOrderPosNext(nOrderPosNext);
    }

    return nRet;
}

bool CWallet::AccountMove(std::string strFrom, std::string strTo,
                          CAmount nAmount, std::string strComment) {
    CWalletDB walletdb(strWalletFile);
    if (!walletdb.TxnBegin()) {
        return false;
    }

    const int64_t nNow = GetAdjustedTime();

    // Debit
    CAccountingEntry debit;
    debit.nOrderPos = IncOrderPosNext(&walletdb);
    debit.strAccount = strFrom;
    debit.nCreditDebit = -nAmount;
    debit.nTime = nNow;
    debit.strOtherAccount = strTo;
    debit.strComment = strComment;
    AddAccountingEntry(debit, &walletdb);

    // Credit
    CAccountingEntry credit;
    credit.nOrderPos = IncOrderPosNext(&walletdb);
    credit.strAccount = strTo;
    credit.nCreditDebit = nAmount;
    credit.nTime = nNow;
    credit.strOtherAccount = strFrom;
    credit.strComment = strComment;
    AddAccountingEntry(credit, &walletdb);

    if (!walletdb.TxnCommit()) {
        return false;
    }

    return true;
}

bool CWallet::GetAccountPubkey(CPubKey &pubKey, std::string strAccount,
                               bool bForceNew) {
    CWalletDB walletdb(strWalletFile);

    CAccount account;
    walletdb.ReadAccount(strAccount, account);

    if (!bForceNew) {
        if (!account.vchPubKey.IsValid()) {
            bForceNew = true;
        } else {
            // Check if the current key has been used.
            CScript scriptPubKey =
                GetScriptForDestination(account.vchPubKey.GetID());
            for (std::map<txid_t, CWalletTx>::iterator it = mapWallet.begin();
                 it != mapWallet.end() && account.vchPubKey.IsValid(); ++it) {
                for (const CTxOut &txout : (*it).second.tx->vout) {
                    if (txout.scriptPubKey == scriptPubKey) {
                        bForceNew = true;
                        break;
                    }
                }
            }
        }
    }

    // Generate a new key
    if (bForceNew) {
        if (!GetKeyFromPool(account.vchPubKey)) {
            return false;
        }

        SetAddressBook(account.vchPubKey.GetID(), strAccount, "receive");
        walletdb.WriteAccount(strAccount, account);
    }

    pubKey = account.vchPubKey;

    return true;
}

void CWallet::MarkDirty() {
    LOCK(cs_wallet);
    for (std::pair<const txid_t, CWalletTx> &item : mapWallet) {
        item.second.MarkDirty();
    }
}


void CWallet::MarkDirty(const COutPoint &outpoint) {
    LOCK(cs_wallet);
    const CWalletTx *pwtx = GetWalletTx(outpoint.utxid);
    if (pwtx) {
        // Get non-const
        CWalletTx &wtx = mapWallet[pwtx->GetId()];
        wtx.MarkDirty();
    }
}



bool CWallet::AddToWallet(const CWalletTx &wtxIn, bool fFlushOnClose) {
    LOCK(cs_wallet);

    CWalletDB walletdb(strWalletFile, "r+", fFlushOnClose);

    const txid_t hash = wtxIn.GetId();

    // Inserts only if not already there, returns tx inserted or tx found.
    std::pair<std::map<txid_t, CWalletTx>::iterator, bool> ret =
        mapWallet.insert(std::make_pair(hash, wtxIn));
    CWalletTx &wtx = (*ret.first).second;
    wtx.BindWallet(this);
    bool fInsertedNew = ret.second;
    if (fInsertedNew) {
        wtx.nTimeReceived = GetAdjustedTime();
        wtx.nOrderPos = IncOrderPosNext(&walletdb);
        wtxOrdered.insert(std::make_pair(wtx.nOrderPos, TxPair(&wtx, nullptr)));

        wtx.nTimeSmart = wtx.nTimeReceived;
        if (!wtxIn.hashUnset()) {
            if (mapBlockIndex.count(wtxIn.hashBlock)) {
                int64_t latestNow = wtx.nTimeReceived;
                int64_t latestEntry = 0;
                {
                    // Tolerate times up to the last timestamp in the wallet not
                    // more than 5 minutes into the future.
                    int64_t latestTolerated = latestNow + 300;
                    const TxItems &txOrdered = wtxOrdered;
                    for (TxItems::const_reverse_iterator it =
                             txOrdered.rbegin();
                         it != txOrdered.rend(); ++it) {
                        CWalletTx *const pwtx = (*it).second.first;
                        if (pwtx == &wtx) {
                            continue;
                        }

                        CAccountingEntry *const pacentry = (*it).second.second;
                        int64_t nSmartTime;
                        if (pwtx) {
                            nSmartTime = pwtx->nTimeSmart;
                            if (!nSmartTime) {
                                nSmartTime = pwtx->nTimeReceived;
                            }
                        } else {
                            nSmartTime = pacentry->nTime;
                        }

                        if (nSmartTime <= latestTolerated) {
                            latestEntry = nSmartTime;
                            if (nSmartTime > latestNow) {
                                latestNow = nSmartTime;
                            }

                            break;
                        }
                    }
                }

                int64_t blocktime =
                    mapBlockIndex[wtxIn.hashBlock]->GetBlockTime();
                wtx.nTimeSmart =
                    std::max(latestEntry, std::min(blocktime, latestNow));
            } else {
                LogPrintf("AddToWallet(): found %s in block %s not in index\n",
                          wtxIn.GetId().ToString(), wtxIn.hashBlock.ToString());
            }
        }

        AddToSpends(hash);
    }

    bool fUpdated = false;
    if (!fInsertedNew) {
        // Merge
        if (!wtxIn.hashUnset() && wtxIn.hashBlock != wtx.hashBlock) {
            wtx.hashBlock = wtxIn.hashBlock;
            fUpdated = true;
        }

        // If no longer abandoned, update
        if (wtxIn.hashBlock.IsNull() && wtx.isAbandoned()) {
            wtx.hashBlock = wtxIn.hashBlock;
            fUpdated = true;
        }

        if (wtxIn.nIndex != -1 && (wtxIn.nIndex != wtx.nIndex)) {
            wtx.nIndex = wtxIn.nIndex;
            fUpdated = true;
        }

        if (wtxIn.fFromMe && wtxIn.fFromMe != wtx.fFromMe) {
            wtx.fFromMe = wtxIn.fFromMe;
            fUpdated = true;
        }
    }

    //// debug print
    LogPrintf("AddToWallet %s  %s%s\n", wtxIn.GetId().ToString(),
              (fInsertedNew ? "new" : ""), (fUpdated ? "update" : ""));

    // Write to disk
    if ((fInsertedNew || fUpdated) && !walletdb.WriteTx(wtx)) {
        return false;
    }

    // Break debit/credit balance caches:
    wtx.MarkDirty();

    // Notify UI of new or updated transaction.
    NotifyTransactionChanged(this, wtx.GetId(), fInsertedNew ? CT_NEW : CT_UPDATED);

    // Notify an external script when a wallet transaction comes in or is
    // updated.
    std::string strCmd = GetArg("-walletnotify", "");

    if (!strCmd.empty()) {
        boost::replace_all(strCmd, "%s", wtxIn.GetId().GetHex());
        // Thread runs free.
        boost::thread t(runCommand, strCmd);
    }

    return true;
}

bool CWallet::LoadToWallet(const CWalletTx &wtxIn) {
    const txid_t txid = wtxIn.GetId();

    mapWallet[txid] = wtxIn;
    CWalletTx &wtx = mapWallet[txid];
    wtx.BindWallet(this);
    wtxOrdered.insert(std::make_pair(wtx.nOrderPos, TxPair(&wtx, nullptr)));
    AddToSpends(txid);
    for (const CTxIn &txin : wtx.tx->vin) {
        const CWalletTx *prevtx = GetWalletTx(txin.prevout.utxid);
        if (prevtx && prevtx->nIndex == -1 && !prevtx->hashUnset()) {
            MarkConflicted(prevtx->hashBlock, wtx.GetId());
        }
    }

    return true;
}

/**
 * Add a transaction to the wallet, or update it. pIndex and posInBlock should
 * be set when the transaction was known to be included in a block. When
 * posInBlock = SYNC_TRANSACTION_NOT_IN_BLOCK (-1), then wallet state is not
 * updated in AddToWallet, but notifications happen and cached balances are
 * marked dirty. If fUpdate is true, existing transactions will be updated.
 *
 * TODO: One exception to this is that the abandoned state is cleared under the
 * assumption that any further notification of a transaction that was considered
 * abandoned is an indication that it is not safe to be considered abandoned.
 * Abandoned state should probably be more carefuly tracked via different
 * posInBlock signals or by checking mempool presence when necessary.
 */
bool CWallet::AddToWalletIfInvolvingMe(const CTransaction &tx,
                                       const CBlockIndex *pIndex,
                                       int posInBlock, bool fUpdate) {
    AssertLockHeld(cs_wallet);

    if (posInBlock != -1) {
        for (const CTxIn &txin : tx.vin) {
            std::pair<TxSpends::const_iterator, TxSpends::const_iterator>
                range = mapTxSpends.equal_range(txin.prevout);
            while (range.first != range.second) {
                if (range.first->second != tx.GetUtxid(MALFIX_MODE_LEGACY)) {
                    LogPrintf("Transaction %s (in block %s) conflicts with "
                              "wallet transaction %s (both spend %s:%i)\n",
                              tx.GetId().ToString(),
                              pIndex->GetBlockHash().ToString(),
                              range.first->second.ToString(),
                              range.first->first.utxid.ToString(),
                              range.first->first.n);
                    MarkConflicted(pIndex->GetBlockHash(), range.first->second);
                }
                range.first++;
            }
        }
    }

    bool fExisted = mapWallet.count(tx.GetId()) != 0;
    if (fExisted && !fUpdate) {
        return false;
    }

    if (fExisted || IsMine(tx) || IsFromMe(tx)) {
        CWalletTx wtx(this, MakeTransactionRef(tx));

        // Get merkle branch if transaction was found in a block.
        if (posInBlock != -1) {
            wtx.SetMerkleBranch(pIndex, posInBlock);
        }

        return AddToWallet(wtx, false);
    }

    return false;
}

bool CWallet::AbandonTransaction(const txid_t &hashTx) {
    LOCK2(cs_main, cs_wallet);

    CWalletDB walletdb(strWalletFile, "r+");

    std::set<txid_t> todo;
    std::set<txid_t> done;

    // Can't mark abandoned if confirmed or in mempool.
    const CWalletTx *origtx = GetWalletTx(hashTx);
    const utxid_t origUtxid = origtx->tx->GetUtxid(MALFIX_MODE_LEGACY);
    assert(origtx);
    if (origtx->GetDepthInMainChain() > 0 || origtx->InMempool()) {
        return false;
    }

    todo.insert(hashTx);

    while (!todo.empty()) {
        txid_t now = *todo.begin();
        todo.erase(now);
        done.insert(now);
        assert(mapWallet.count(now));
        CWalletTx &wtx = mapWallet[now];
        utxid_t utxid = wtx.tx->GetUtxid(MALFIX_MODE_LEGACY);

        int currentconfirm = wtx.GetDepthInMainChain();
        // If the orig tx was not in block, none of its spends can be.
        assert(currentconfirm <= 0);
        // If (currentconfirm < 0) {Tx and spends are already conflicted, no
        // need to abandon}
        if (currentconfirm == 0 && !wtx.isAbandoned()) {
            // If the orig tx was not in block/mempool, none of its spends can
            // be in mempool.
            assert(!wtx.InMempool());
            wtx.nIndex = -1;
            wtx.setAbandoned();
            wtx.MarkDirty();
            walletdb.WriteTx(wtx);
            NotifyTransactionChanged(this, now, CT_UPDATED);
            // Iterate over all its outputs, and mark transactions in the wallet
            // that spend them abandoned too.
            TxSpends::const_iterator iter =
                mapTxSpends.lower_bound(COutPoint(origUtxid, 0));
            while (iter != mapTxSpends.end() && iter->first.utxid == utxid) {
                if (!done.count(iter->second)) {
                    todo.insert(iter->second);
                }
                iter++;
            }

            // If a transaction changes 'conflicted' state, that changes the
            // balance available of the outputs it spends. So force those to be
            // recomputed.
            for (const CTxIn &txin : wtx.tx->vin) {
                MarkDirty(txin.prevout);

            }
        }
    }

    return true;
}

void CWallet::MarkConflicted(const uint256 &hashBlock, const txid_t &hashTx) {
    LOCK2(cs_main, cs_wallet);

    int conflictconfirms = 0;
    if (mapBlockIndex.count(hashBlock)) {
        CBlockIndex *pindex = mapBlockIndex[hashBlock];
        if (chainActive.Contains(pindex)) {
            conflictconfirms = -(chainActive.Height() - pindex->nHeight + 1);
        }
    }

    // If number of conflict confirms cannot be determined, this means that the
    // block is still unknown or not yet part of the main chain, for example
    // when loading the wallet during a reindex. Do nothing in that case.
    if (conflictconfirms >= 0) {
        return;
    }

    // Do not flush the wallet here for performance reasons
    CWalletDB walletdb(strWalletFile, "r+", false);

    std::set<txid_t> todo;
    std::set<txid_t> done;

    todo.insert(hashTx);

    while (!todo.empty()) {
        txid_t now = *todo.begin();
        todo.erase(now);
        done.insert(now);
        assert(mapWallet.count(now));
        CWalletTx &wtx = mapWallet[now];
        int currentconfirm = wtx.GetDepthInMainChain();
        if (conflictconfirms < currentconfirm) {
            // Block is 'more conflicted' than current confirm; update.
            // Mark transaction as conflicted with this block.
            wtx.nIndex = -1;
            wtx.hashBlock = hashBlock;
            wtx.MarkDirty();
            walletdb.WriteTx(wtx);
            utxid_t utxid = wtx.tx->GetUtxid(MALFIX_MODE_LEGACY);
            // Iterate over all its outputs, and mark transactions in the wallet
            // that spend them conflicted too.
            TxSpends::const_iterator iter =
                mapTxSpends.lower_bound(COutPoint(utxid, 0));
            while (iter != mapTxSpends.end() && iter->first.utxid == utxid) {
                if (!done.count(iter->second)) {
                    todo.insert(iter->second);
                }
                iter++;
            }

            // If a transaction changes 'conflicted' state, that changes the
            // balance available of the outputs it spends. So force those to be
            // recomputed.
            for (const CTxIn &txin : wtx.tx->vin) {
                MarkDirty(txin.prevout);
            }
        }
    }
}

void CWallet::SyncTransaction(const CTransaction &tx, const CBlockIndex *pindex,
                              int posInBlock) {
    LOCK2(cs_main, cs_wallet);

    if (!AddToWalletIfInvolvingMe(tx, pindex, posInBlock, true)) {
        // Not one of ours
        return;
    }

    // If a transaction changes 'conflicted' state, that changes the balance
    // available of the outputs it spends. So force those to be recomputed,
    // also:
    for (const CTxIn &txin : tx.vin) {
        MarkDirty(txin.prevout);
    }
}

isminetype CWallet::IsMine(const CTxIn &txin) const {
    LOCK(cs_wallet);
    const CWalletTx *wtx = GetWalletTx(txin.prevout.utxid);
    if (wtx) {
        if (txin.prevout.n < wtx->tx->vout.size()) {
            return IsMine(wtx->tx->vout[txin.prevout.n]);
        }
    }

    return ISMINE_NO;
}

// Note that this function doesn't distinguish between a 0-valued input, and a
// not-"is mine" (according to the filter) input.
CAmount CWallet::GetDebit(const CTxIn &txin, const isminefilter &filter) const {
    LOCK(cs_wallet);
    const CWalletTx *wtx = GetWalletTx(txin.prevout.utxid);
    if (wtx) {
        if (txin.prevout.n < wtx->tx->vout.size()) {
            if (IsMine(wtx->tx->vout[txin.prevout.n]) & filter) {
                return wtx->tx->vout[txin.prevout.n].nValue;
            }
        }
    }

    return 0;
}

isminetype CWallet::IsMine(const CTxOut &txout) const {
    return ::IsMine(*this, txout.scriptPubKey);
}

CAmount CWallet::GetCredit(const CTxOut &txout,
                           const isminefilter &filter) const {
    if (!MoneyRange(txout.nValue)) {
        throw std::runtime_error(std::string(__func__) +
                                 ": value out of range");
    }

    return (IsMine(txout) & filter) ? txout.nValue : 0;
}

bool CWallet::IsChange(const CTxOut &txout) const {
    // TODO: fix handling of 'change' outputs. The assumption is that any
    // payment to a script that is ours, but is not in the address book is
    // change. That assumption is likely to break when we implement
    // multisignature wallets that return change back into a
    // multi-signature-protected address; a better way of identifying which
    // outputs are 'the send' and which are 'the change' will need to be
    // implemented (maybe extend CWalletTx to remember which output, if any, was
    // change).
    if (::IsMine(*this, txout.scriptPubKey)) {
        CTxDestination address;
        if (!ExtractDestination(txout.scriptPubKey, address)) {
            return true;
        }

        LOCK(cs_wallet);
        if (!mapAddressBook.count(address)) {
            return true;
        }
    }

    return false;
}

CAmount CWallet::GetChange(const CTxOut &txout) const {
    if (!MoneyRange(txout.nValue)) {
        throw std::runtime_error(std::string(__func__) +
                                 ": value out of range");
    }

    return (IsChange(txout) ? txout.nValue : 0);
}

bool CWallet::IsMine(const CTransaction &tx) const {
    for (const CTxOut &txout : tx.vout) {
        if (IsMine(txout)) {
            return true;
        }
    }

    return false;
}

bool CWallet::IsFromMe(const CTransaction &tx) const {
    return GetDebit(tx, ISMINE_ALL) > 0;
}

CAmount CWallet::GetDebit(const CTransaction &tx,
                          const isminefilter &filter) const {
    CAmount nDebit = 0;
    for (const CTxIn &txin : tx.vin) {
        nDebit += GetDebit(txin, filter);
        if (!MoneyRange(nDebit)) {
            throw std::runtime_error(std::string(__func__) +
                                     ": value out of range");
        }
    }

    return nDebit;
}

bool CWallet::IsAllFromMe(const CTransaction &tx,
                          const isminefilter &filter) const {
    LOCK(cs_wallet);

    for (const CTxIn &txin : tx.vin) {

        const CWalletTx *prev = GetWalletTx(txin.prevout.utxid);
        if (!prev) {
            // Any unknown inputs can't be from us.
            return false;
        }


        if (txin.prevout.n >= prev->tx->vout.size()) {
            // Invalid input!
            return false;
        }

        if (!(IsMine(prev->tx->vout[txin.prevout.n]) & filter)) {
            return false;
        }
    }

    return true;
}

CAmount CWallet::GetCredit(const CTransaction &tx,
                           const isminefilter &filter) const {
    CAmount nCredit = 0;
    for (const CTxOut &txout : tx.vout) {
        nCredit += GetCredit(txout, filter);
        if (!MoneyRange(nCredit)) {
            throw std::runtime_error(std::string(__func__) +
                                     ": value out of range");
        }
    }

    return nCredit;
}

CAmount CWallet::GetChange(const CTransaction &tx) const {
    CAmount nChange = 0;
    for (const CTxOut &txout : tx.vout) {
        nChange += GetChange(txout);
        if (!MoneyRange(nChange)) {
            throw std::runtime_error(std::string(__func__) +
                                     ": value out of range");
        }
    }

    return nChange;
}

CPubKey CWallet::GenerateNewHDMasterKey() {
    CKey key;
    key.MakeNewKey(true);

    int64_t nCreationTime = GetTime();
    CKeyMetadata metadata(nCreationTime);

    // Calculate the pubkey.
    CPubKey pubkey = key.GetPubKey();
    assert(key.VerifyPubKey(pubkey));

    // Set the hd keypath to "m" -> Master, refers the masterkeyid to itself.
    metadata.hdKeypath = "m";
    metadata.hdMasterKeyID = pubkey.GetID();

    LOCK(cs_wallet);

    // mem store the metadata
    mapKeyMetadata[pubkey.GetID()] = metadata;

    // Write the key&metadata to the database.
    if (!AddKeyPubKey(key, pubkey)) {
        throw std::runtime_error(std::string(__func__) +
                                 ": AddKeyPubKey failed");
    }

    return pubkey;
}

bool CWallet::SetHDMasterKey(const CPubKey &pubkey) {
    LOCK(cs_wallet);

    // Ensure this wallet.dat can only be opened by clients supporting HD.
    SetMinVersion(FEATURE_HD);

    // Store the keyid (hash160) together with the child index counter in the
    // database as a hdchain object.
    CHDChain newHdChain;
    newHdChain.masterKeyID = pubkey.GetID();
    SetHDChain(newHdChain, false);

    return true;
}

bool CWallet::SetHDChain(const CHDChain &chain, bool memonly) {
    LOCK(cs_wallet);
    if (!memonly && !CWalletDB(strWalletFile).WriteHDChain(chain)) {
        throw std::runtime_error(std::string(__func__) +
                                 ": writing chain failed");
    }

    hdChain = chain;
    return true;
}

bool CWallet::IsHDEnabled() {
    return !hdChain.masterKeyID.IsNull();
}

int64_t CWalletTx::GetTxTime() const {
    int64_t n = nTimeSmart;
    return n ? n : nTimeReceived;
}

int CWalletTx::GetRequestCount() const {
    LOCK(pwallet->cs_wallet);

    // Returns -1 if it wasn't being tracked.
    int nRequests = -1;

    if (IsCoinBase()) {
        // Generated block.
        if (!hashUnset()) {
            std::map<uint256, int>::const_iterator mi =
                pwallet->mapRequestCount.find(hashBlock);
            if (mi != pwallet->mapRequestCount.end()) {
                nRequests = (*mi).second;
            }
        }
    } else {
        // Did anyone request this transaction?
        std::map<uint256, int>::const_iterator mi =
            pwallet->mapRequestCount.find(GetId());
        if (mi != pwallet->mapRequestCount.end()) {
            nRequests = (*mi).second;

            // How about the block it's in?
            if (nRequests == 0 && !hashUnset()) {
                std::map<uint256, int>::const_iterator _mi =
                    pwallet->mapRequestCount.find(hashBlock);
                if (_mi != pwallet->mapRequestCount.end()) {
                    nRequests = (*_mi).second;
                } else {
                    // If it's in someone else's block it must have got out.
                    nRequests = 1;
                }
            }
        }
    }

    return nRequests;
}

void CWalletTx::GetAmounts(std::list<COutputEntry> &listReceived,
                           std::list<COutputEntry> &listSent, CAmount &nFee,
                           std::string &strSentAccount,
                           const isminefilter &filter) const {
    nFee = 0;
    listReceived.clear();
    listSent.clear();
    strSentAccount = strFromAccount;

    // Compute fee:
    CAmount nDebit = GetDebit(filter);
    // debit>0 means we signed/sent this transaction.
    if (nDebit > 0) {
        CAmount nValueOut = tx->GetValueOut();
        nFee = nDebit - nValueOut;
    }

    // Sent/received.
    for (unsigned int i = 0; i < tx->vout.size(); ++i) {
        const CTxOut &txout = tx->vout[i];
        isminetype fIsMine = pwallet->IsMine(txout);
        // Only need to handle txouts if AT LEAST one of these is true:
        //   1) they debit from us (sent)
        //   2) the output is to us (received)
        if (nDebit > 0) {
            // Don't report 'change' txouts
            if (pwallet->IsChange(txout)) {
                continue;
            }
        } else if (!(fIsMine & filter)) {
            continue;
        }

        // In either case, we need to get the destination address.
        CTxDestination address;

        if (!ExtractDestination(txout.scriptPubKey, address) &&
            !txout.scriptPubKey.IsUnspendable()) {
            LogPrintf("CWalletTx::GetAmounts: Unknown transaction type found, "
                      "txid %s\n",
                      this->GetId().ToString());
            address = CNoDestination();
        }

        COutputEntry output = {address, txout.nValue, (int)i};

        // If we are debited by the transaction, add the output as a "sent"
        // entry.
        if (nDebit > 0) {
            listSent.push_back(output);
        }

        // If we are receiving the output, add it as a "received" entry.
        if (fIsMine & filter) {
            listReceived.push_back(output);
        }
    }
}

void CWalletTx::GetAccountAmounts(const std::string &strAccount,
                                  CAmount &nReceived, CAmount &nSent,
                                  CAmount &nFee,
                                  const isminefilter &filter) const {
    nReceived = nSent = nFee = 0;

    CAmount allFee;
    std::string strSentAccount;
    std::list<COutputEntry> listReceived;
    std::list<COutputEntry> listSent;
    GetAmounts(listReceived, listSent, allFee, strSentAccount, filter);

    if (strAccount == strSentAccount) {
        for (const COutputEntry &s : listSent) {
            nSent += s.amount;
        }
        nFee = allFee;
    }

    LOCK(pwallet->cs_wallet);
    for (const COutputEntry &r : listReceived) {
        if (pwallet->mapAddressBook.count(r.destination)) {
            std::map<CTxDestination, CAddressBookData>::const_iterator mi =
                pwallet->mapAddressBook.find(r.destination);
            if (mi != pwallet->mapAddressBook.end() &&
                (*mi).second.name == strAccount) {
                nReceived += r.amount;
            }
        } else if (strAccount.empty()) {
            nReceived += r.amount;
        }
    }
}

/**
 * Scan the block chain (starting in pindexStart) for transactions from or to
 * us. If fUpdate is true, found transactions that already exist in the wallet
 * will be updated.
 *
 * Returns pointer to the first block in the last contiguous range that was
 * successfully scanned.
 */
CBlockIndex *CWallet::ScanForWalletTransactions(CBlockIndex *pindexStart,
                                                bool fUpdate) {
    LOCK2(cs_main, cs_wallet);

    CBlockIndex *ret = nullptr;
    int64_t nNow = GetTime();
    const CChainParams &chainParams = Params();

    CBlockIndex *pindex = pindexStart;

    // No need to read and scan block, if block was created before our wallet
    // birthday (as adjusted for block time variability)
    while (pindex && nTimeFirstKey &&
           (pindex->GetBlockTime() < (nTimeFirstKey - 7200))) {
        pindex = chainActive.Next(pindex);
    }

    // Show rescan progress in GUI as dialog or on splashscreen, if -rescan on
    // startup.
    ShowProgress(_("Rescanning..."), 0);
    double dProgressStart =
        GuessVerificationProgress(chainParams.TxData(), pindex);
    double dProgressTip =
        GuessVerificationProgress(chainParams.TxData(), chainActive.Tip());
    while (pindex) {
        if (pindex->nHeight % 100 == 0 && dProgressTip - dProgressStart > 0.0) {
            ShowProgress(
                _("Rescanning..."),
                std::max(1,
                         std::min(99, (int)((GuessVerificationProgress(
                                                 chainParams.TxData(), pindex) -
                                             dProgressStart) /
                                            (dProgressTip - dProgressStart) *
                                            100))));
        }

        CBlock block;
        if (ReadBlockFromDisk(block, pindex, Params().GetConsensus())) {
            for (size_t posInBlock = 0; posInBlock < block.vtx.size();
                 ++posInBlock) {
                AddToWalletIfInvolvingMe(*block.vtx[posInBlock], pindex,
                                         posInBlock, fUpdate);
            }

            if (!ret) {
                ret = pindex;
            }
        } else {
            ret = nullptr;
        }

        pindex = chainActive.Next(pindex);
        if (GetTime() >= nNow + 60) {
            nNow = GetTime();
            LogPrintf("Still rescanning. At block %d. Progress=%f\n",
                      pindex->nHeight,
                      GuessVerificationProgress(chainParams.TxData(), pindex));
        }
    }

    // Hide progress dialog in GUI.
    ShowProgress(_("Rescanning..."), 100);

    return ret;
}

void CWallet::ReacceptWalletTransactions() {
    // If transactions aren't being broadcasted, don't let them into local
    // mempool either.
    if (!fBroadcastTransactions) {
        return;
    }

    LOCK2(cs_main, cs_wallet);
    std::map<int64_t, CWalletTx *> mapSorted;

    // Sort pending wallet transactions based on their initial wallet insertion
    // order.
    for (std::pair<const txid_t, CWalletTx> &item : mapWallet) {
        const uint256 &wtxid = item.first;
        CWalletTx &wtx = item.second;
        assert(wtx.GetId() == wtxid);

        int nDepth = wtx.GetDepthInMainChain();

        if (!wtx.IsCoinBase() && (nDepth == 0 && !wtx.isAbandoned())) {
            mapSorted.insert(std::make_pair(wtx.nOrderPos, &wtx));
        }
    }

    // Try to add wallet transactions to memory pool.
    for (std::pair<const int64_t, CWalletTx *> &item : mapSorted) {
        CWalletTx &wtx = *(item.second);

        LOCK(mempool.cs);
        CValidationState state;
        wtx.AcceptToMemoryPool(maxTxFee, state);
    }
}

bool CWalletTx::RelayWalletTransaction(CConnman *connman) {
    assert(pwallet->GetBroadcastTransactions());
    if (IsCoinBase() || isAbandoned() || GetDepthInMainChain() != 0) {
        return false;
    }

    CValidationState state;
    // GetDepthInMainChain already catches known conflicts.
    if (InMempool() || AcceptToMemoryPool(maxTxFee, state)) {
        LogPrintf("Relaying wtx %s\n", GetId().ToString());
        if (connman) {
            CInv inv(MSG_TX, GetId());
            connman->ForEachNode(
                [&inv](CNode *pnode) { pnode->PushInventory(inv); });
            return true;
        }
    }

    return false;
}

std::set<txid_t> CWalletTx::GetConflicts() const {
    std::set<txid_t> result;
    if (pwallet != nullptr) {
        txid_t myHash = tx->GetId();
        result = pwallet->GetConflicts(myHash);
        result.erase(myHash);
    }

    return result;
}

CAmount CWalletTx::GetDebit(const isminefilter &filter) const {
    if (tx->vin.empty()) return 0;

    CAmount debit = 0;
    if (filter & ISMINE_SPENDABLE) {
        if (fDebitCached) {
            debit += nDebitCached;
        } else {
            nDebitCached = pwallet->GetDebit(*this, ISMINE_SPENDABLE);
            fDebitCached = true;
            debit += nDebitCached;
        }
    }

    if (filter & ISMINE_WATCH_ONLY) {
        if (fWatchDebitCached) {
            debit += nWatchDebitCached;
        } else {
            nWatchDebitCached = pwallet->GetDebit(*this, ISMINE_WATCH_ONLY);
            fWatchDebitCached = true;
            debit += nWatchDebitCached;
        }
    }

    return debit;
}

CAmount CWalletTx::GetCredit(const isminefilter &filter) const {
    // Must wait until coinbase is safely deep enough in the chain before
    // valuing it.
    if (IsCoinBase() && GetBlocksToMaturity() > 0) {
        return 0;
    }

    CAmount credit = 0;
    if (filter & ISMINE_SPENDABLE) {
        // GetBalance can assume transactions in mapWallet won't change.
        if (fCreditCached) {
            credit += nCreditCached;
        } else {
            nCreditCached = pwallet->GetCredit(*this, ISMINE_SPENDABLE);
            fCreditCached = true;
            credit += nCreditCached;
        }
    }

    if (filter & ISMINE_WATCH_ONLY) {
        if (fWatchCreditCached) {
            credit += nWatchCreditCached;
        } else {
            nWatchCreditCached = pwallet->GetCredit(*this, ISMINE_WATCH_ONLY);
            fWatchCreditCached = true;
            credit += nWatchCreditCached;
        }
    }

    return credit;
}

CAmount CWalletTx::GetImmatureCredit(bool fUseCache) const {
    if (IsCoinBase() && GetBlocksToMaturity() > 0 && IsInMainChain()) {
        if (fUseCache && fImmatureCreditCached) return nImmatureCreditCached;
        nImmatureCreditCached = pwallet->GetCredit(*this, ISMINE_SPENDABLE);
        fImmatureCreditCached = true;
        return nImmatureCreditCached;
    }

    return 0;
}

CAmount CWalletTx::GetAvailableCredit(bool fUseCache) const {
    if (pwallet == 0) {
        return 0;
    }

    // Must wait until coinbase is safely deep enough in the chain before
    // valuing it.
    if (IsCoinBase() && GetBlocksToMaturity() > 0) {
        return 0;
    }

    if (fUseCache && fAvailableCreditCached) {
        return nAvailableCreditCached;
    }

    CAmount nCredit = 0;
    utxid_t utxid = tx->GetUtxid(MALFIX_MODE_LEGACY);
    for (unsigned int i = 0; i < tx->vout.size(); i++) {
        if (!pwallet->IsSpent(COutPoint(utxid, i))) {
            const CTxOut &txout = tx->vout[i];
            nCredit += pwallet->GetCredit(txout, ISMINE_SPENDABLE);
            if (!MoneyRange(nCredit)) {
                throw std::runtime_error(
                    "CWalletTx::GetAvailableCredit() : value out of range");
            }
        }
    }

    nAvailableCreditCached = nCredit;
    fAvailableCreditCached = true;
    return nCredit;
}

CAmount CWalletTx::GetImmatureWatchOnlyCredit(const bool &fUseCache) const {
    if (IsCoinBase() && GetBlocksToMaturity() > 0 && IsInMainChain()) {
        if (fUseCache && fImmatureWatchCreditCached) {
            return nImmatureWatchCreditCached;
        }

        nImmatureWatchCreditCached =
            pwallet->GetCredit(*this, ISMINE_WATCH_ONLY);
        fImmatureWatchCreditCached = true;
        return nImmatureWatchCreditCached;
    }

    return 0;
}

CAmount CWalletTx::GetAvailableWatchOnlyCredit(const bool &fUseCache) const {
    if (pwallet == 0) {
        return 0;
    }

    // Must wait until coinbase is safely deep enough in the chain before
    // valuing it.
    if (IsCoinBase() && GetBlocksToMaturity() > 0) {
        return 0;
    }

    if (fUseCache && fAvailableWatchCreditCached) {
        return nAvailableWatchCreditCached;
    }

    CAmount nCredit = 0;
    utxid_t utxid = tx->GetUtxid(MALFIX_MODE_LEGACY);
    for (unsigned int i = 0; i < tx->vout.size(); i++) {
        if (!pwallet->IsSpent(COutPoint(utxid,i))) {
            const CTxOut &txout = tx->vout[i];
            nCredit += pwallet->GetCredit(txout, ISMINE_WATCH_ONLY);
            if (!MoneyRange(nCredit)) {
                throw std::runtime_error(
                    "CWalletTx::GetAvailableCredit() : value out of range");
            }
        }
    }

    nAvailableWatchCreditCached = nCredit;
    fAvailableWatchCreditCached = true;
    return nCredit;
}

CAmount CWalletTx::GetChange() const {
    if (fChangeCached) {
        return nChangeCached;
    }

    nChangeCached = pwallet->GetChange(*this);
    fChangeCached = true;
    return nChangeCached;
}

bool CWalletTx::InMempool() const {
    LOCK(mempool.cs);
    if (mempool.exists(GetId())) {
        return true;
    }

    return false;
}

bool CWalletTx::IsTrusted() const {
    // Quick answer in most cases
    if (!CheckFinalTx(*this)) {
        return false;
    }

    int nDepth = GetDepthInMainChain();
    if (nDepth >= 1) {
        return true;
    }

    if (nDepth < 0) {
        return false;
    }

    // using wtx's cached debit
    if (!bSpendZeroConfChange || !IsFromMe(ISMINE_ALL)) {
        return false;
    }

    // Don't trust unconfirmed transactions from us unless they are in the
    // mempool.
    if (!InMempool()) {
        return false;
    }

    // Trusted if all inputs are from us and are in the mempool:
    for (const CTxIn &txin : tx->vin) {
        // Transactions not sent by us: not trusted
        const CWalletTx *parent = pwallet->GetWalletTx(txin.prevout.utxid);
        if (parent == nullptr) {
            return false;
        }

        const CTxOut &parentOut = parent->tx->vout[txin.prevout.n];
        if (pwallet->IsMine(parentOut) != ISMINE_SPENDABLE) {
            return false;
        }
    }

    return true;
}

bool CWalletTx::IsEquivalentTo(const CWalletTx &_tx) const {
    CMutableTransaction tx1 = *this->tx;
    CMutableTransaction tx2 = *_tx.tx;
    for (unsigned int i = 0; i < tx1.vin.size(); i++) {
        tx1.vin[i].scriptSig = CScript();
    }

    for (unsigned int i = 0; i < tx2.vin.size(); i++) {
        tx2.vin[i].scriptSig = CScript();
    }

    return CTransaction(tx1) == CTransaction(tx2);
}

std::vector<txid_t>
CWallet::ResendWalletTransactionsBefore(int64_t nTime, CConnman *connman) {
    std::vector<txid_t> result;

    LOCK(cs_wallet);
    // Sort them in chronological order
    std::multimap<unsigned int, CWalletTx *> mapSorted;
    for (std::pair<const txid_t, CWalletTx> &item : mapWallet) {
        CWalletTx &wtx = item.second;
        // Don't rebroadcast if newer than nTime:
        if (wtx.nTimeReceived > nTime) {
            continue;
        }

        mapSorted.insert(std::make_pair(wtx.nTimeReceived, &wtx));
    }

    for (std::pair<const unsigned int, CWalletTx *> &item : mapSorted) {
        CWalletTx &wtx = *item.second;
        if (wtx.RelayWalletTransaction(connman)) {
            result.push_back(wtx.GetId());
        }
    }

    return result;
}

void CWallet::ResendWalletTransactions(int64_t nBestBlockTime,
                                       CConnman *connman) {
    // Do this infrequently and randomly to avoid giving away that these are our
    // transactions.
    if (GetTime() < nNextResend || !fBroadcastTransactions) {
        return;
    }

    bool fFirst = (nNextResend == 0);
    nNextResend = GetTime() + GetRand(30 * 60);
    if (fFirst) {
        return;
    }

    // Only do it if there's been a new block since last time
    if (nBestBlockTime < nLastResend) {
        return;
    }

    nLastResend = GetTime();

    // Rebroadcast unconfirmed txes older than 5 minutes before the last block
    // was found:
    std::vector<txid_t> relayed =
        ResendWalletTransactionsBefore(nBestBlockTime - 5 * 60, connman);
    if (!relayed.empty()) {
        LogPrintf("%s: rebroadcast %u unconfirmed transactions\n", __func__,
                  relayed.size());
    }
}

/** @} */ // end of mapWallet

/**
 * @defgroup Actions
 *
 * @{
 */
CAmount CWallet::GetBalance() const {
    LOCK2(cs_main, cs_wallet);

    CAmount nTotal = 0;
    for (std::map<txid_t, CWalletTx>::const_iterator it = mapWallet.begin();
         it != mapWallet.end(); ++it) {
        const CWalletTx *pcoin = &(*it).second;
        if (pcoin->IsTrusted()) {
            nTotal += pcoin->GetAvailableCredit();
        }
    }

    return nTotal;
}

CAmount CWallet::GetUnconfirmedBalance() const {
    LOCK2(cs_main, cs_wallet);

    CAmount nTotal = 0;
    for (std::map<txid_t, CWalletTx>::const_iterator it = mapWallet.begin();
         it != mapWallet.end(); ++it) {
        const CWalletTx *pcoin = &(*it).second;
        if (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0 &&
            pcoin->InMempool()) {
            nTotal += pcoin->GetAvailableCredit();
        }
    }

    return nTotal;
}

CAmount CWallet::GetImmatureBalance() const {
    LOCK2(cs_main, cs_wallet);

    CAmount nTotal = 0;
    for (std::map<txid_t, CWalletTx>::const_iterator it = mapWallet.begin();
         it != mapWallet.end(); ++it) {
        const CWalletTx *pcoin = &(*it).second;
        nTotal += pcoin->GetImmatureCredit();
    }

    return nTotal;
}

CAmount CWallet::GetWatchOnlyBalance() const {
    LOCK2(cs_main, cs_wallet);

    CAmount nTotal = 0;
    for (std::map<txid_t, CWalletTx>::const_iterator it = mapWallet.begin();
         it != mapWallet.end(); ++it) {
        const CWalletTx *pcoin = &(*it).second;
        if (pcoin->IsTrusted()) {
            nTotal += pcoin->GetAvailableWatchOnlyCredit();
        }
    }

    return nTotal;
}

CAmount CWallet::GetUnconfirmedWatchOnlyBalance() const {
    LOCK2(cs_main, cs_wallet);

    CAmount nTotal = 0;
    for (std::map<txid_t, CWalletTx>::const_iterator it = mapWallet.begin();
         it != mapWallet.end(); ++it) {
        const CWalletTx *pcoin = &(*it).second;
        if (!pcoin->IsTrusted() && pcoin->GetDepthInMainChain() == 0 &&
            pcoin->InMempool()) {
            nTotal += pcoin->GetAvailableWatchOnlyCredit();
        }
    }

    return nTotal;
}

CAmount CWallet::GetImmatureWatchOnlyBalance() const {
    LOCK2(cs_main, cs_wallet);

    CAmount nTotal = 0;
    for (std::map<txid_t, CWalletTx>::const_iterator it = mapWallet.begin();
         it != mapWallet.end(); ++it) {
        const CWalletTx *pcoin = &(*it).second;
        nTotal += pcoin->GetImmatureWatchOnlyCredit();
    }

    return nTotal;
}

void CWallet::AvailableCoins(std::vector<COutput> &vCoins, bool fOnlyConfirmed,
                             const CCoinControl *coinControl,
                             bool fIncludeZeroValue) const {
    vCoins.clear();

    LOCK2(cs_main, cs_wallet);
    for (std::map<txid_t, CWalletTx>::const_iterator it = mapWallet.begin();
         it != mapWallet.end(); ++it) {

        const CWalletTx *pcoin = &(*it).second;
        const utxid_t &utxid = pcoin->tx->GetUtxid(MALFIX_MODE_LEGACY);

        if (!CheckFinalTx(*pcoin)) {
            continue;
        }

        if (fOnlyConfirmed && !pcoin->IsTrusted()) {
            continue;
        }

        if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0) {
            continue;
        }

        int nDepth = pcoin->GetDepthInMainChain();
        if (nDepth < 0) {
            continue;
        }

        // We should not consider coins which aren't at least in our mempool.
        // It's possible for these to be conflicted via ancestors which we may
        // never be able to detect.
        if (nDepth == 0 && !pcoin->InMempool()) {
            continue;
        }

        // Bitcoin-ABC: Removed check that prevents consideration of coins from
        // transactions that are replacing other transactions. This check based
        // on pcoin->mapValue.count("replaces_txid") which was not being set
        // anywhere.

        // Similarly, we should not consider coins from transactions that have
        // been replaced. In the example above, we would want to prevent
        // creation of a transaction A' spending an output of A, because if
        // transaction B were initially confirmed, conflicting with A and A', we
        // wouldn't want to the user to create a transaction D intending to
        // replace A', but potentially resulting in a scenario where A, A', and
        // D could all be accepted (instead of just B and D, or just A and A'
        // like the user would want).

        // Bitcoin-ABC: retained this check as 'replaced_by_txid' is still set
        // in the wallet code.
        if (nDepth == 0 && fOnlyConfirmed &&
            pcoin->mapValue.count("replaced_by_txid")) {
            continue;
        }

        for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
            isminetype mine = IsMine(pcoin->tx->vout[i]);
            if (!(IsSpent(COutPoint(utxid, i))) && mine != ISMINE_NO &&
                !IsLockedCoin(COutPoint(utxid, i)) &&
                (pcoin->tx->vout[i].nValue > 0 || fIncludeZeroValue) &&
                (!coinControl || !coinControl->HasSelected() ||
                 coinControl->fAllowOtherInputs ||
                 coinControl->IsSelected(COutPoint(utxid, i)))) {
                vCoins.push_back(COutput(
                    pcoin, i, nDepth,
                    ((mine & ISMINE_SPENDABLE) != ISMINE_NO) ||
                        (coinControl && coinControl->fAllowWatchOnly &&
                         (mine & ISMINE_WATCH_SOLVABLE) != ISMINE_NO),
                    (mine & (ISMINE_SPENDABLE | ISMINE_WATCH_SOLVABLE)) !=
                        ISMINE_NO));
            }
        }
    }
}

static void ApproximateBestSubset(
    std::vector<std::pair<CAmount, std::pair<const CWalletTx *, unsigned int>>>
        vValue,
    const CAmount &nTotalLower, const CAmount &nTargetValue,
    std::vector<char> &vfBest, CAmount &nBest, int iterations = 1000) {
    std::vector<char> vfIncluded;

    vfBest.assign(vValue.size(), true);
    nBest = nTotalLower;

    FastRandomContext insecure_rand;

    for (int nRep = 0; nRep < iterations && nBest != nTargetValue; nRep++) {
        vfIncluded.assign(vValue.size(), false);
        CAmount nTotal = 0;
        bool fReachedTarget = false;
        for (int nPass = 0; nPass < 2 && !fReachedTarget; nPass++) {
            for (size_t i = 0; i < vValue.size(); i++) {
                // The solver here uses a randomized algorithm, the randomness
                // serves no real security purpose but is just needed to prevent
                // degenerate behavior and it is important that the rng is fast.
                // We do not use a constant random sequence, because there may
                // be some privacy improvement by making the selection random.
                if (nPass == 0 ? insecure_rand.randbool() : !vfIncluded[i]) {
                    nTotal += vValue[i].first;
                    vfIncluded[i] = true;
                    if (nTotal >= nTargetValue) {
                        fReachedTarget = true;
                        if (nTotal < nBest) {
                            nBest = nTotal;
                            vfBest = vfIncluded;
                        }

                        nTotal -= vValue[i].first;
                        vfIncluded[i] = false;
                    }
                }
            }
        }
    }
}

bool CWallet::SelectCoinsMinConf(
    const CAmount &nTargetValue, const int nConfMine, const int nConfTheirs,
    const uint64_t nMaxAncestors, std::vector<COutput> vCoins,
    std::set<std::pair<const CWalletTx *, unsigned int>> &setCoinsRet,
    CAmount &nValueRet) const {
    setCoinsRet.clear();
    nValueRet = 0;

    // List of values less than target
    std::pair<CAmount, std::pair<const CWalletTx *, unsigned int>>
        coinLowestLarger;
    coinLowestLarger.first = std::numeric_limits<CAmount>::max();
    coinLowestLarger.second.first = nullptr;
    std::vector<std::pair<CAmount, std::pair<const CWalletTx *, unsigned int>>>
        vValue;
    CAmount nTotalLower = 0;

    random_shuffle(vCoins.begin(), vCoins.end(), GetRandInt);

    for (const COutput &output : vCoins) {
        if (!output.fSpendable) {
            continue;
        }

        const CWalletTx *pcoin = output.tx;

        if (output.nDepth <
            (pcoin->IsFromMe(ISMINE_ALL) ? nConfMine : nConfTheirs)) {
            continue;
        }

        if (!mempool.TransactionWithinChainLimit(pcoin->GetId(),
                                                 nMaxAncestors)) {
            continue;
        }

        int i = output.i;
        CAmount n = pcoin->tx->vout[i].nValue;

        std::pair<CAmount, std::pair<const CWalletTx *, unsigned int>> coin =
            std::make_pair(n, std::make_pair(pcoin, i));

        if (n == nTargetValue) {
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
            return true;
        } else if (n < nTargetValue + MIN_CHANGE) {
            vValue.push_back(coin);
            nTotalLower += n;
        } else if (n < coinLowestLarger.first) {
            coinLowestLarger = coin;
        }
    }

    if (nTotalLower == nTargetValue) {
        for (unsigned int i = 0; i < vValue.size(); ++i) {
            setCoinsRet.insert(vValue[i].second);
            nValueRet += vValue[i].first;
        }

        return true;
    }

    if (nTotalLower < nTargetValue) {
        if (coinLowestLarger.second.first == nullptr) {
            return false;
        }

        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
        return true;
    }

    // Solve subset sum by stochastic approximation
    std::sort(vValue.begin(), vValue.end(), CompareValueOnly());
    std::reverse(vValue.begin(), vValue.end());
    std::vector<char> vfBest;
    CAmount nBest;

    ApproximateBestSubset(vValue, nTotalLower, nTargetValue, vfBest, nBest);
    if (nBest != nTargetValue && nTotalLower >= nTargetValue + MIN_CHANGE) {
        ApproximateBestSubset(vValue, nTotalLower, nTargetValue + MIN_CHANGE,
                              vfBest, nBest);
    }

    // If we have a bigger coin and (either the stochastic approximation didn't
    // find a good solution, or the next bigger coin is closer), return the
    // bigger coin.
    if (coinLowestLarger.second.first &&
        ((nBest != nTargetValue && nBest < nTargetValue + MIN_CHANGE) ||
         coinLowestLarger.first <= nBest)) {
        setCoinsRet.insert(coinLowestLarger.second);
        nValueRet += coinLowestLarger.first;
    } else {
        for (unsigned int i = 0; i < vValue.size(); i++) {
            if (vfBest[i]) {
                setCoinsRet.insert(vValue[i].second);
                nValueRet += vValue[i].first;
            }
        }

        LogPrint("selectcoins", "SelectCoins() best subset: ");
        for (unsigned int i = 0; i < vValue.size(); i++) {
            if (vfBest[i]) {
                LogPrint("selectcoins", "%s ", FormatMoney(vValue[i].first));
            }
        }

        LogPrint("selectcoins", "total %s\n", FormatMoney(nBest));
    }

    return true;
}

bool CWallet::SelectCoins(
    const std::vector<COutput> &vAvailableCoins, const CAmount &nTargetValue,
    std::set<std::pair<const CWalletTx *, unsigned int>> &setCoinsRet,
    CAmount &nValueRet, const CCoinControl *coinControl) const {
    std::vector<COutput> vCoins(vAvailableCoins);

    // coin control -> return all selected outputs (we want all selected to go
    // into the transaction for sure).
    if (coinControl && coinControl->HasSelected() &&
        !coinControl->fAllowOtherInputs) {
        for (const COutput &out : vCoins) {
            if (!out.fSpendable) {
                continue;
            }

            nValueRet += out.tx->tx->vout[out.i].nValue;
            setCoinsRet.insert(std::make_pair(out.tx, out.i));
        }

        return (nValueRet >= nTargetValue);
    }

    // Calculate value from preset inputs and store them.
    std::set<std::pair<const CWalletTx *, uint32_t>> setPresetCoins;
    CAmount nValueFromPresetInputs = 0;

    std::vector<COutPoint> vPresetInputs;
    if (coinControl) {
        coinControl->ListSelected(vPresetInputs);
    }

    for (const COutPoint &outpoint : vPresetInputs) {

        const CWalletTx *pcoin = GetWalletTx(outpoint.utxid);
        if (!pcoin) {
            // TODO: Allow non-wallet inputs
            return false;
        }

        // Clearly invalid input, fail.
        if (pcoin->tx->vout.size() <= outpoint.n) {
            return false;
        }

        nValueFromPresetInputs += pcoin->tx->vout[outpoint.n].nValue;
        setPresetCoins.insert(std::make_pair(pcoin, outpoint.n));
    }

    // Remove preset inputs from vCoins.
    for (std::vector<COutput>::iterator it = vCoins.begin();
         it != vCoins.end() && coinControl && coinControl->HasSelected();) {
        if (setPresetCoins.count(std::make_pair(it->tx, it->i))) {
            it = vCoins.erase(it);
        } else {
            ++it;
        }
    }

    size_t nMaxChainLength =
        std::min(GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT),
                 GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT));
    bool fRejectLongChains = GetBoolArg("-walletrejectlongchains",
                                        DEFAULT_WALLET_REJECT_LONG_CHAINS);

    bool res =
        nTargetValue <= nValueFromPresetInputs ||
        SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 1, 6, 0,
                           vCoins, setCoinsRet, nValueRet) ||
        SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 1, 1, 0,
                           vCoins, setCoinsRet, nValueRet) ||
        (bSpendZeroConfChange &&
         SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 0, 1, 2,
                            vCoins, setCoinsRet, nValueRet)) ||
        (bSpendZeroConfChange &&
         SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 0, 1,
                            std::min((size_t)4, nMaxChainLength / 3), vCoins,
                            setCoinsRet, nValueRet)) ||
        (bSpendZeroConfChange &&
         SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 0, 1,
                            nMaxChainLength / 2, vCoins, setCoinsRet,
                            nValueRet)) ||
        (bSpendZeroConfChange &&
         SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 0, 1,
                            nMaxChainLength, vCoins, setCoinsRet, nValueRet)) ||
        (bSpendZeroConfChange && !fRejectLongChains &&
         SelectCoinsMinConf(nTargetValue - nValueFromPresetInputs, 0, 1,
                            std::numeric_limits<uint64_t>::max(), vCoins,
                            setCoinsRet, nValueRet));

    // Because SelectCoinsMinConf clears the setCoinsRet, we now add the
    // possible inputs to the coinset.
    setCoinsRet.insert(setPresetCoins.begin(), setPresetCoins.end());

    // Add preset inputs to the total value selected.
    nValueRet += nValueFromPresetInputs;

    return res;
}

bool CWallet::FundTransaction(CMutableTransaction &tx, CAmount &nFeeRet,
                              bool overrideEstimatedFeeRate,
                              const CFeeRate &specificFeeRate,
                              int &nChangePosInOut, std::string &strFailReason,
                              bool includeWatching, bool lockUnspents,
                              const std::set<int> &setSubtractFeeFromOutputs,
                              bool keepReserveKey,
                              const CTxDestination &destChange) {
    std::vector<CRecipient> vecSend;

    // Turn the txout set into a CRecipient vector.
    for (size_t idx = 0; idx < tx.vout.size(); idx++) {
        const CTxOut &txOut = tx.vout[idx];
        CRecipient recipient = {txOut.scriptPubKey, txOut.nValue,
                                setSubtractFeeFromOutputs.count(idx) == 1};
        vecSend.push_back(recipient);
    }

    CCoinControl coinControl;
    coinControl.destChange = destChange;
    coinControl.fAllowOtherInputs = true;
    coinControl.fAllowWatchOnly = includeWatching;
    coinControl.fOverrideFeeRate = overrideEstimatedFeeRate;
    coinControl.nFeeRate = specificFeeRate;

    for (const CTxIn &txin : tx.vin) {
        coinControl.Select(txin.prevout);
    }

    CReserveKey reservekey(this);
    CWalletTx wtx;
    if (!CreateTransaction(vecSend, wtx, reservekey, nFeeRet, nChangePosInOut,
                           strFailReason, &coinControl, false)) {
        return false;
    }

    if (nChangePosInOut != -1) {
        tx.vout.insert(tx.vout.begin() + nChangePosInOut,
                       wtx.tx->vout[nChangePosInOut]);
    }

    // Copy output sizes from new transaction; they may have had the fee
    // subtracted from them.
    for (size_t idx = 0; idx < tx.vout.size(); idx++) {
        tx.vout[idx].nValue = wtx.tx->vout[idx].nValue;
    }

    // Add new txins (keeping original txin scriptSig/order)
    for (const CTxIn &txin : wtx.tx->vin) {
        if (!coinControl.IsSelected(txin.prevout)) {
            tx.vin.push_back(txin);

            if (lockUnspents) {
                LOCK2(cs_main, cs_wallet);
                LockCoin(txin.prevout);
            }
        }
    }

    // Optionally keep the change output key.
    if (keepReserveKey) {
        reservekey.KeepKey();
    }

    return true;
}

bool CWallet::CreateTransaction(const std::vector<CRecipient> &vecSend,
                                CWalletTx &wtxNew, CReserveKey &reservekey,
                                CAmount &nFeeRet, int &nChangePosInOut,
                                std::string &strFailReason,
                                const CCoinControl *coinControl, bool sign) {
    CAmount nValue = 0;
    int nChangePosRequest = nChangePosInOut;
    unsigned int nSubtractFeeFromAmount = 0;
    for (const auto &recipient : vecSend) {
        if (nValue < 0 || recipient.nAmount < 0) {
            strFailReason = _("Transaction amounts must not be negative");
            return false;
        }

        nValue += recipient.nAmount;

        if (recipient.fSubtractFeeFromAmount) {
            nSubtractFeeFromAmount++;
        }
    }

    if (vecSend.empty()) {
        strFailReason = _("Transaction must have at least one recipient");
        return false;
    }

    wtxNew.fTimeReceivedIsTxTime = true;
    wtxNew.BindWallet(this);
    CMutableTransaction txNew;

    // Discourage fee sniping.
    //
    // For a large miner the value of the transactions in the best block and the
    // mempool can exceed the cost of deliberately attempting to mine two blocks
    // to orphan the current best block. By setting nLockTime such that only the
    // next block can include the transaction, we discourage this practice as
    // the height restricted and limited blocksize gives miners considering fee
    // sniping fewer options for pulling off this attack.
    //
    // A simple way to think about this is from the wallet's point of view we
    // always want the blockchain to move forward. By setting nLockTime this way
    // we're basically making the statement that we only want this transaction
    // to appear in the next block; we don't want to potentially encourage
    // reorgs by allowing transactions to appear at lower heights than the next
    // block in forks of the best chain.
    //
    // Of course, the subsidy is high enough, and transaction volume low enough,
    // that fee sniping isn't a problem yet, but by implementing a fix now we
    // ensure code won't be written that makes assumptions about nLockTime that
    // preclude a fix later.
    txNew.nLockTime = chainActive.Height();

    // Secondly occasionally randomly pick a nLockTime even further back, so
    // that transactions that are delayed after signing for whatever reason,
    // e.g. high-latency mix networks and some CoinJoin implementations, have
    // better privacy.
    if (GetRandInt(10) == 0) {
        txNew.nLockTime = std::max(0, (int)txNew.nLockTime - GetRandInt(100));
    }

    assert(txNew.nLockTime <= (unsigned int)chainActive.Height());
    assert(txNew.nLockTime < LOCKTIME_THRESHOLD);

    {
        std::set<std::pair<const CWalletTx *, unsigned int>> setCoins;
        LOCK2(cs_main, cs_wallet);

        std::vector<COutput> vAvailableCoins;
        AvailableCoins(vAvailableCoins, true, coinControl);

        nFeeRet = 0;
        // Start with no fee and loop until there is enough fee.
        while (true) {
            nChangePosInOut = nChangePosRequest;
            txNew.vin.clear();
            txNew.vout.clear();
            wtxNew.fFromMe = true;
            bool fFirst = true;

            CAmount nValueToSelect = nValue;
            if (nSubtractFeeFromAmount == 0) {
                nValueToSelect += nFeeRet;
            }

            double dPriority = 0;
            // vouts to the payees
            for (const auto &recipient : vecSend) {
                CTxOut txout(recipient.nAmount, recipient.scriptPubKey);

                if (recipient.fSubtractFeeFromAmount) {
                    // Subtract fee equally from each selected recipient.
                    txout.nValue -= nFeeRet / nSubtractFeeFromAmount;

                    // First receiver pays the remainder not divisible by output
                    // count.
                    if (fFirst) {
                        fFirst = false;
                        txout.nValue -= nFeeRet % nSubtractFeeFromAmount;
                    }
                }

                if (txout.IsDust(dustRelayFee)) {
                    if (recipient.fSubtractFeeFromAmount && nFeeRet > 0) {
                        if (txout.nValue < 0) {
                            strFailReason = _("The transaction amount is "
                                              "too small to pay the fee");
                        } else {
                            strFailReason =
                                _("The transaction amount is too small to "
                                  "send after the fee has been deducted");
                        }
                    } else {
                        strFailReason = _("Transaction amount too small");
                    }

                    return false;
                }

                txNew.vout.push_back(txout);
            }

            // Choose coins to use.
            CAmount nValueIn = 0;
            setCoins.clear();
            if (!SelectCoins(vAvailableCoins, nValueToSelect, setCoins,
                             nValueIn, coinControl)) {
                strFailReason = _("Insufficient funds");
                return false;
            }

            for (const auto &pcoin : setCoins) {
                CAmount nCredit = pcoin.first->tx->vout[pcoin.second].nValue;
                // The coin age after the next block (depth+1) is used instead
                // of the current, reflecting an assumption the user would
                // accept a bit more delay for a chance at a free transaction.
                // But mempool inputs might still be in the mempool, so their
                // age stays 0.
                int age = pcoin.first->GetDepthInMainChain();
                assert(age >= 0);
                if (age != 0) age += 1;
                dPriority += (double)nCredit * age;
            }

            const CAmount nChange = nValueIn - nValueToSelect;
            if (nChange > 0) {
                // Fill a vout to ourself.
                // TODO: pass in scriptChange instead of reservekey so change
                // transaction isn't always pay-to-bitcoin-address.
                CScript scriptChange;

                // Coin control: send change to custom address.
                if (coinControl &&
                    !boost::get<CNoDestination>(&coinControl->destChange)) {
                    scriptChange =
                        GetScriptForDestination(coinControl->destChange);

                    // No coin control: send change to newly generated address.
                } else {
                    // Note: We use a new key here to keep it from being obvious
                    // which side is the change. The drawback is that by not
                    // reusing a previous key, the change may be lost if a
                    // backup is restored, if the backup doesn't have the new
                    // private key for the change. If we reused the old key, it
                    // would be possible to add code to look for and rediscover
                    // unknown transactions that were written with keys of ours
                    // to recover post-backup change.

                    // Reserve a new key pair from key pool.
                    CPubKey vchPubKey;
                    bool ret;
                    ret = reservekey.GetReservedKey(vchPubKey);
                    if (!ret) {
                        strFailReason = _("Keypool ran out, please call "
                                          "keypoolrefill first");
                        return false;
                    }

                    scriptChange = GetScriptForDestination(vchPubKey.GetID());
                }

                CTxOut newTxOut(nChange, scriptChange);

                // We do not move dust-change to fees, because the sender would
                // end up paying more than requested. This would be against the
                // purpose of the all-inclusive feature. So instead we raise the
                // change and deduct from the recipient.
                if (nSubtractFeeFromAmount > 0 &&
                    newTxOut.IsDust(dustRelayFee)) {
                    CAmount nDust = newTxOut.GetDustThreshold(dustRelayFee) -
                                    newTxOut.nValue;
                    // Raise change until no more dust.
                    newTxOut.nValue += nDust;
                    // Subtract from first recipient.
                    for (unsigned int i = 0; i < vecSend.size(); i++) {
                        if (vecSend[i].fSubtractFeeFromAmount) {
                            txNew.vout[i].nValue -= nDust;
                            if (txNew.vout[i].IsDust(dustRelayFee)) {
                                strFailReason =
                                    _("The transaction amount is too small "
                                      "to send after the fee has been "
                                      "deducted");
                                return false;
                            }

                            break;
                        }
                    }
                }

                // Never create dust outputs; if we would, just add the dust to
                // the fee.
                if (newTxOut.IsDust(dustRelayFee)) {
                    nChangePosInOut = -1;
                    nFeeRet += nChange;
                    reservekey.ReturnKey();
                } else {
                    if (nChangePosInOut == -1) {
                        // Insert change txn at random position:
                        nChangePosInOut = GetRandInt(txNew.vout.size() + 1);
                    } else if ((unsigned int)nChangePosInOut >
                               txNew.vout.size()) {
                        strFailReason = _("Change index out of range");
                        return false;
                    }

                    std::vector<CTxOut>::iterator position =
                        txNew.vout.begin() + nChangePosInOut;
                    txNew.vout.insert(position, newTxOut);
                }
            } else {
                reservekey.ReturnKey();
            }

            // Fill vin
            //
            // Note how the sequence number is set to non-maxint so that the
            // nLockTime set above actually works.
            for (const auto &coin : setCoins) {
                txNew.vin.push_back(
                    CTxIn(coin.first->tx->GetUtxid(MALFIX_MODE_LEGACY), coin.second, CScript(),
                          std::numeric_limits<unsigned int>::max() - 1));
            }

            // Fill in dummy signatures for fee calculation.
            if (!DummySignTx(txNew, setCoins)) {
                strFailReason = _("Signing transaction failed");
                return false;
            }

            unsigned int nBytes = GetTransactionSize(txNew);

            CTransaction txNewConst(txNew);
            dPriority = txNewConst.ComputePriority(dPriority, nBytes);

            // Remove scriptSigs to eliminate the fee calculation dummy
            // signatures.
            for (auto &vin : txNew.vin) {
                vin.scriptSig = CScript();
            }

            // Allow to override the default confirmation target over the
            // CoinControl instance.
            int currentConfirmationTarget = nTxConfirmTarget;
            if (coinControl && coinControl->nConfirmTarget > 0) {
                currentConfirmationTarget = coinControl->nConfirmTarget;
            }

            // Can we complete this as a free transaction?
            if (fSendFreeTransactions &&
                nBytes <= MAX_FREE_TRANSACTION_CREATE_SIZE) {
                // Not enough fee: enough priority?
                double dPriorityNeeded =
                    mempool.estimateSmartPriority(currentConfirmationTarget);
                // Require at least hard-coded AllowFree.
                if (dPriority >= dPriorityNeeded && AllowFree(dPriority)) {
                    break;
                }
            }

            CAmount nFeeNeeded =
                GetMinimumFee(nBytes, currentConfirmationTarget, mempool);
            if (coinControl && nFeeNeeded > 0 &&
                coinControl->nMinimumTotalFee > nFeeNeeded) {
                nFeeNeeded = coinControl->nMinimumTotalFee;
            }

            if (coinControl && coinControl->fOverrideFeeRate) {
                nFeeNeeded = coinControl->nFeeRate.GetFee(nBytes);
            }

            // If we made it here and we aren't even able to meet the relay fee
            // on the next pass, give up because we must be at the maximum
            // allowed fee.
            if (nFeeNeeded < ::minRelayTxFee.GetFee(nBytes)) {
                strFailReason = _("Transaction too large for fee policy");
                return false;
            }

            if (nFeeRet >= nFeeNeeded) {
                // Reduce fee to only the needed amount if we have change output
                // to increase. This prevents potential overpayment in fees if
                // the coins selected to meet nFeeNeeded result in a transaction
                // that requires less fee than the prior iteration.
                // TODO: The case where nSubtractFeeFromAmount > 0 remains to be
                // addressed because it requires returning the fee to the payees
                // and not the change output.
                // TODO: The case where there is no change output remains to be
                // addressed so we avoid creating too small an output.
                if (nFeeRet > nFeeNeeded && nChangePosInOut != -1 &&
                    nSubtractFeeFromAmount == 0) {
                    CAmount extraFeePaid = nFeeRet - nFeeNeeded;
                    std::vector<CTxOut>::iterator change_position =
                        txNew.vout.begin() + nChangePosInOut;
                    change_position->nValue += extraFeePaid;
                    nFeeRet -= extraFeePaid;
                }

                // Done, enough fee included.
                break;
            }

            // Try to reduce change to include necessary fee.
            if (nChangePosInOut != -1 && nSubtractFeeFromAmount == 0) {
                CAmount additionalFeeNeeded = nFeeNeeded - nFeeRet;
                std::vector<CTxOut>::iterator change_position =
                    txNew.vout.begin() + nChangePosInOut;
                // Only reduce change if remaining amount is still a large
                // enough output.
                if (change_position->nValue >=
                    MIN_FINAL_CHANGE + additionalFeeNeeded) {
                    change_position->nValue -= additionalFeeNeeded;
                    nFeeRet += additionalFeeNeeded;
                    // Done, able to increase fee from change.
                    break;
                }
            }

            // Include more fee and try again.
            nFeeRet = nFeeNeeded;
            continue;
        }

        if (sign) {
            uint32_t nHashType = SIGHASH_ALL | SIGHASH_FORKID;

            CTransaction txNewConst(txNew);
            int nIn = 0;
            for (const auto &coin : setCoins) {
                const CScript &scriptPubKey =
                    coin.first->tx->vout[coin.second].scriptPubKey;
                SignatureData sigdata;

                if (!ProduceSignature(
                        TransactionSignatureCreator(
                            this, &txNewConst, nIn,
                            coin.first->tx->vout[coin.second].nValue,
                            nHashType),
                        scriptPubKey, sigdata)) {
                    strFailReason = _("Signing transaction failed");
                    return false;
                } else {
                    UpdateTransaction(txNew, nIn, sigdata);
                }

                nIn++;
            }
        }

        // Embed the constructed transaction data in wtxNew.
        wtxNew.SetTx(MakeTransactionRef(std::move(txNew)));

        // Limit size.
        if (GetTransactionSize(wtxNew) >= MAX_STANDARD_TX_SIZE) {
            strFailReason = _("Transaction too large");
            return false;
        }
    }

    if (GetBoolArg("-walletrejectlongchains",
                   DEFAULT_WALLET_REJECT_LONG_CHAINS)) {
        // Lastly, ensure this tx will pass the mempool's chain limits.
        LockPoints lp;
        CTxMemPoolEntry entry(wtxNew.tx, 0, 0, 0, 0, 0, false, 0, lp);
        CTxMemPool::setEntries setAncestors;
        size_t nLimitAncestors =
            GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT);
        size_t nLimitAncestorSize =
            GetArg("-limitancestorsize", DEFAULT_ANCESTOR_SIZE_LIMIT) * 1000;
        size_t nLimitDescendants =
            GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT);
        size_t nLimitDescendantSize =
            GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT) *
            1000;
        std::string errString;
        if (!mempool.CalculateMemPoolAncestors(
                entry, setAncestors, nLimitAncestors, nLimitAncestorSize,
                nLimitDescendants, nLimitDescendantSize, errString)) {
            strFailReason = _("Transaction has too long of a mempool chain");
            return false;
        }
    }

    return true;
}

/**
 * Call after CreateTransaction unless you want to abort
 */
bool CWallet::CommitTransaction(CWalletTx &wtxNew, CReserveKey &reservekey,
                                CConnman *connman, CValidationState &state) {
    LOCK2(cs_main, cs_wallet);
    LogPrintf("CommitTransaction:\n%s", wtxNew.tx->ToString());

    // Take key pair from key pool so it won't be used again.
    reservekey.KeepKey();

    // Add tx to wallet, because if it has change it's also ours, otherwise just
    // for transaction history.
    AddToWallet(wtxNew);

    // Notify that old coins are spent.
    for (const CTxIn &txin : wtxNew.tx->vin) {
        const CWalletTx *prev = GetWalletTx(txin.prevout.utxid);
        assert(prev);
        CWalletTx &coin = mapWallet[prev->GetId()];
        coin.BindWallet(this);
        NotifyTransactionChanged(this, coin.GetId(), CT_UPDATED);
    }

    // Track how many getdata requests our transaction gets.
    mapRequestCount[wtxNew.GetId()] = 0;

    if (fBroadcastTransactions) {
        // Broadcast
        if (!wtxNew.AcceptToMemoryPool(maxTxFee, state)) {
            LogPrintf("CommitTransaction(): Transaction cannot be "
                      "broadcast immediately, %s\n",
                      state.GetRejectReason());
            // TODO: if we expect the failure to be long term or permanent,
            // instead delete wtx from the wallet and return failure.
        } else {
            wtxNew.RelayWalletTransaction(connman);
        }
    }

    return true;
}

void CWallet::ListAccountCreditDebit(const std::string &strAccount,
                                     std::list<CAccountingEntry> &entries) {
    CWalletDB walletdb(strWalletFile);
    return walletdb.ListAccountCreditDebit(strAccount, entries);
}

bool CWallet::AddAccountingEntry(const CAccountingEntry &acentry) {
    CWalletDB walletdb(strWalletFile);

    return AddAccountingEntry(acentry, &walletdb);
}

bool CWallet::AddAccountingEntry(const CAccountingEntry &acentry,
                                 CWalletDB *pwalletdb) {
    if (!pwalletdb->WriteAccountingEntry_Backend(acentry)) {
        return false;
    }

    laccentries.push_back(acentry);
    CAccountingEntry &entry = laccentries.back();
    wtxOrdered.insert(std::make_pair(entry.nOrderPos, TxPair(nullptr, &entry)));

    return true;
}

CAmount CWallet::GetRequiredFee(unsigned int nTxBytes) {
    return std::max(minTxFee.GetFee(nTxBytes),
                    ::minRelayTxFee.GetFee(nTxBytes));
}

CAmount CWallet::GetMinimumFee(unsigned int nTxBytes,
                               unsigned int nConfirmTarget,
                               const CTxMemPool &pool) {
    // payTxFee is the user-set global for desired feerate.
    return GetMinimumFee(nTxBytes, nConfirmTarget, pool,
                         payTxFee.GetFee(nTxBytes));
}

CAmount CWallet::GetMinimumFee(unsigned int nTxBytes,
                               unsigned int nConfirmTarget,
                               const CTxMemPool &pool, CAmount targetFee) {
    CAmount nFeeNeeded = targetFee;
    // User didn't set: use -txconfirmtarget to estimate...
    if (nFeeNeeded == 0) {
        int estimateFoundTarget = nConfirmTarget;
        nFeeNeeded = pool.estimateSmartFee(nConfirmTarget, &estimateFoundTarget)
                         .GetFee(nTxBytes);
        // ... unless we don't have enough mempool data for estimatefee, then
        // use fallbackFee.
        if (nFeeNeeded == 0) {
            nFeeNeeded = fallbackFee.GetFee(nTxBytes);
        }
    }

    // Prevent user from paying a fee below minRelayTxFee or minTxFee.
    nFeeNeeded = std::max(nFeeNeeded, GetRequiredFee(nTxBytes));

    // But always obey the maximum.
    if (nFeeNeeded > maxTxFee) {
        nFeeNeeded = maxTxFee;
    }

    return nFeeNeeded;
}

DBErrors CWallet::LoadWallet(bool &fFirstRunRet) {
    if (!fFileBacked) {
        return DB_LOAD_OK;
    }

    fFirstRunRet = false;
    DBErrors nLoadWalletRet = CWalletDB(strWalletFile, "cr+").LoadWallet(this);
    if (nLoadWalletRet == DB_NEED_REWRITE) {
        if (CDB::Rewrite(strWalletFile, "\x04pool")) {
            LOCK(cs_wallet);
            setKeyPool.clear();
            // Note: can't top-up keypool here, because wallet is locked. User
            // will be prompted to unlock wallet the next operation that
            // requires a new key.
        }
    }

    if (nLoadWalletRet != DB_LOAD_OK) {
        return nLoadWalletRet;
    }

    fFirstRunRet = !vchDefaultKey.IsValid();

    uiInterface.LoadWallet(this);

    return DB_LOAD_OK;
}

DBErrors CWallet::ZapSelectTx(std::vector<txid_t> &vHashIn,
                              std::vector<txid_t> &vHashOut) {
    if (!fFileBacked) {
        return DB_LOAD_OK;
    }

    DBErrors nZapSelectTxRet =
        CWalletDB(strWalletFile, "cr+").ZapSelectTx(this, vHashIn, vHashOut);
    if (nZapSelectTxRet == DB_NEED_REWRITE) {
        if (CDB::Rewrite(strWalletFile, "\x04pool")) {
            LOCK(cs_wallet);
            setKeyPool.clear();
            // Note: can't top-up keypool here, because wallet is locked. User
            // will be prompted to unlock wallet the next operation that
            // requires a new key.
        }
    }

    if (nZapSelectTxRet != DB_LOAD_OK) {
        return nZapSelectTxRet;
    }

    MarkDirty();

    return DB_LOAD_OK;
}

DBErrors CWallet::ZapWalletTx(std::vector<CWalletTx> &vWtx) {
    if (!fFileBacked) {
        return DB_LOAD_OK;
    }

    DBErrors nZapWalletTxRet =
        CWalletDB(strWalletFile, "cr+").ZapWalletTx(this, vWtx);
    if (nZapWalletTxRet == DB_NEED_REWRITE) {
        if (CDB::Rewrite(strWalletFile, "\x04pool")) {
            LOCK(cs_wallet);
            setKeyPool.clear();
            // Note: can't top-up keypool here, because wallet is locked. User
            // will be prompted to unlock wallet the next operation that
            // requires a new key.
        }
    }

    if (nZapWalletTxRet != DB_LOAD_OK) {
        return nZapWalletTxRet;
    }

    return DB_LOAD_OK;
}

bool CWallet::SetAddressBook(const CTxDestination &address,
                             const std::string &strName,
                             const std::string &strPurpose) {
    bool fUpdated = false;
    {
        // mapAddressBook
        LOCK(cs_wallet);
        std::map<CTxDestination, CAddressBookData>::iterator mi =
            mapAddressBook.find(address);
        fUpdated = mi != mapAddressBook.end();
        mapAddressBook[address].name = strName;
        // Update purpose only if requested.
        if (!strPurpose.empty()) {
            mapAddressBook[address].purpose = strPurpose;
        }
    }

    NotifyAddressBookChanged(this, address, strName,
                             ::IsMine(*this, address) != ISMINE_NO, strPurpose,
                             (fUpdated ? CT_UPDATED : CT_NEW));
    if (!fFileBacked) {
        return false;
    }

    if (!strPurpose.empty() &&
        !CWalletDB(strWalletFile)
             .WritePurpose(CBitcoinAddress(address).ToString(), strPurpose)) {
        return false;
    }

    return CWalletDB(strWalletFile)
        .WriteName(CBitcoinAddress(address).ToString(), strName);
}

bool CWallet::DelAddressBook(const CTxDestination &address) {
    {
        // mapAddressBook
        LOCK(cs_wallet);

        if (fFileBacked) {
            // Delete destdata tuples associated with address.
            std::string strAddress = CBitcoinAddress(address).ToString();
            for (const std::pair<std::string, std::string> &item :
                 mapAddressBook[address].destdata) {
                CWalletDB(strWalletFile).EraseDestData(strAddress, item.first);
            }
        }
        mapAddressBook.erase(address);
    }

    NotifyAddressBookChanged(this, address, "",
                             ::IsMine(*this, address) != ISMINE_NO, "",
                             CT_DELETED);

    if (!fFileBacked) {
        return false;
    }

    CWalletDB(strWalletFile).ErasePurpose(CBitcoinAddress(address).ToString());
    return CWalletDB(strWalletFile)
        .EraseName(CBitcoinAddress(address).ToString());
}

bool CWallet::SetDefaultKey(const CPubKey &vchPubKey) {
    if (fFileBacked && !CWalletDB(strWalletFile).WriteDefaultKey(vchPubKey)) {
        return false;
    }

    vchDefaultKey = vchPubKey;
    return true;
}

/**
 * Mark old keypool keys as used, and generate all new keys.
 */
bool CWallet::NewKeyPool() {
    LOCK(cs_wallet);
    CWalletDB walletdb(strWalletFile);
    for (int64_t nIndex : setKeyPool) {
        walletdb.ErasePool(nIndex);
    }
    setKeyPool.clear();

    if (IsLocked()) {
        return false;
    }

    int64_t nKeys =
        std::max(GetArg("-keypool", DEFAULT_KEYPOOL_SIZE), int64_t(0));
    for (int i = 0; i < nKeys; i++) {
        int64_t nIndex = i + 1;
        walletdb.WritePool(nIndex, CKeyPool(GenerateNewKey()));
        setKeyPool.insert(nIndex);
    }

    LogPrintf("CWallet::NewKeyPool wrote %d new keys\n", nKeys);
    return true;
}

bool CWallet::TopUpKeyPool(unsigned int kpSize) {
    LOCK(cs_wallet);

    if (IsLocked()) {
        return false;
    }

    CWalletDB walletdb(strWalletFile);

    // Top up key pool.
    unsigned int nTargetSize;
    if (kpSize > 0) {
        nTargetSize = kpSize;
    } else {
        nTargetSize =
            std::max(GetArg("-keypool", DEFAULT_KEYPOOL_SIZE), int64_t(0));
    }

    while (setKeyPool.size() < (nTargetSize + 1)) {
        int64_t nEnd = 1;
        if (!setKeyPool.empty()) {
            nEnd = *(--setKeyPool.end()) + 1;
        }

        if (!walletdb.WritePool(nEnd, CKeyPool(GenerateNewKey()))) {
            throw std::runtime_error(std::string(__func__) +
                                     ": writing generated key failed");
        }

        setKeyPool.insert(nEnd);
        LogPrintf("keypool added key %d, size=%u\n", nEnd, setKeyPool.size());
    }

    return true;
}

void CWallet::ReserveKeyFromKeyPool(int64_t &nIndex, CKeyPool &keypool) {
    nIndex = -1;
    keypool.vchPubKey = CPubKey();

    LOCK(cs_wallet);

    if (!IsLocked()) {
        TopUpKeyPool();
    }

    // Get the oldest key.
    if (setKeyPool.empty()) {
        return;
    }

    CWalletDB walletdb(strWalletFile);

    nIndex = *(setKeyPool.begin());
    setKeyPool.erase(setKeyPool.begin());
    if (!walletdb.ReadPool(nIndex, keypool)) {
        throw std::runtime_error(std::string(__func__) + ": read failed");
    }

    if (!HaveKey(keypool.vchPubKey.GetID())) {
        throw std::runtime_error(std::string(__func__) +
                                 ": unknown key in key pool");
    }

    assert(keypool.vchPubKey.IsValid());
    LogPrintf("keypool reserve %d\n", nIndex);
}

void CWallet::KeepKey(int64_t nIndex) {
    // Remove from key pool.
    if (fFileBacked) {
        CWalletDB walletdb(strWalletFile);
        walletdb.ErasePool(nIndex);
    }

    LogPrintf("keypool keep %d\n", nIndex);
}

void CWallet::ReturnKey(int64_t nIndex) {
    // Return to key pool.
    {
        LOCK(cs_wallet);
        setKeyPool.insert(nIndex);
    }

    LogPrintf("keypool return %d\n", nIndex);
}

bool CWallet::GetKeyFromPool(CPubKey &result) {
    LOCK(cs_wallet);

    int64_t nIndex = 0;
    CKeyPool keypool;

    ReserveKeyFromKeyPool(nIndex, keypool);
    if (nIndex == -1) {
        if (IsLocked()) {
            return false;
        }

        result = GenerateNewKey();
        return true;
    }

    KeepKey(nIndex);
    result = keypool.vchPubKey;

    return true;
}

int64_t CWallet::GetOldestKeyPoolTime() {
    LOCK(cs_wallet);

    // If the keypool is empty, return <NOW>
    if (setKeyPool.empty()) {
        return GetTime();
    }

    // Load oldest key from keypool, get time and return.
    CKeyPool keypool;
    CWalletDB walletdb(strWalletFile);
    int64_t nIndex = *(setKeyPool.begin());
    if (!walletdb.ReadPool(nIndex, keypool)) {
        throw std::runtime_error(std::string(__func__) +
                                 ": read oldest key in keypool failed");
    }

    assert(keypool.vchPubKey.IsValid());
    return keypool.nTime;
}

std::map<CTxDestination, CAmount> CWallet::GetAddressBalances() {
    std::map<CTxDestination, CAmount> balances;

    LOCK(cs_wallet);
    for (std::pair<txid_t, CWalletTx> walletEntry : mapWallet) {
        CWalletTx *pcoin = &walletEntry.second;
        const utxid_t &utxid = pcoin->tx->GetUtxid(MALFIX_MODE_LEGACY);
        if (!pcoin->IsTrusted()) {
            continue;
        }

        if (pcoin->IsCoinBase() && pcoin->GetBlocksToMaturity() > 0) {
            continue;
        }

        int nDepth = pcoin->GetDepthInMainChain();
        if (nDepth < (pcoin->IsFromMe(ISMINE_ALL) ? 0 : 1)) {
            continue;
        }

        for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
            CTxDestination addr;
            if (!IsMine(pcoin->tx->vout[i])) {
                continue;
            }

            if (!ExtractDestination(pcoin->tx->vout[i].scriptPubKey, addr)) {
                continue;
            }

            CAmount n =
                IsSpent(COutPoint(utxid, i)) ? 0 : pcoin->tx->vout[i].nValue;

            if (!balances.count(addr)) balances[addr] = 0;
            balances[addr] += n;
        }
    }

    return balances;
}

std::set<std::set<CTxDestination>> CWallet::GetAddressGroupings() {
    // mapWallet
    AssertLockHeld(cs_wallet);
    std::set<std::set<CTxDestination>> groupings;
    std::set<CTxDestination> grouping;

    for (std::pair<uint256, CWalletTx> walletEntry : mapWallet) {
        CWalletTx *pcoin = &walletEntry.second;

        if (pcoin->tx->vin.size() > 0) {
            bool any_mine = false;
            // Group all input addresses with each other.
            for (CTxIn txin : pcoin->tx->vin) {
                CTxDestination address;
                // If this input isn't mine, ignore it.
                if (!IsMine(txin)) {
                    continue;
                }

                const CWalletTx *wtx = GetWalletTx(txin.prevout.utxid);
                assert(wtx);
                if (!ExtractDestination(wtx->tx->vout[txin.prevout.n].scriptPubKey,
                                        address)) {
                    continue;
                }

                grouping.insert(address);
                any_mine = true;
            }

            // Group change with input addresses.
            if (any_mine) {
                for (CTxOut txout : pcoin->tx->vout) {
                    if (IsChange(txout)) {
                        CTxDestination txoutAddr;
                        if (!ExtractDestination(txout.scriptPubKey,
                                                txoutAddr)) {
                            continue;
                        }

                        grouping.insert(txoutAddr);
                    }
                }
            }

            if (grouping.size() > 0) {
                groupings.insert(grouping);
                grouping.clear();
            }
        }

        // Group lone addrs by themselves.
        for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++)
            if (IsMine(pcoin->tx->vout[i])) {
                CTxDestination address;
                if (!ExtractDestination(pcoin->tx->vout[i].scriptPubKey,
                                        address)) {
                    continue;
                }

                grouping.insert(address);
                groupings.insert(grouping);
                grouping.clear();
            }
    }

    // A set of pointers to groups of addresses.
    std::set<std::set<CTxDestination> *> uniqueGroupings;
    // Map addresses to the unique group containing it.
    std::map<CTxDestination, std::set<CTxDestination> *> setmap;
    for (std::set<CTxDestination> _grouping : groupings) {
        // Make a set of all the groups hit by this new group.
        std::set<std::set<CTxDestination> *> hits;
        std::map<CTxDestination, std::set<CTxDestination> *>::iterator it;
        for (CTxDestination address : _grouping) {
            if ((it = setmap.find(address)) != setmap.end())
                hits.insert((*it).second);
        }

        // Merge all hit groups into a new single group and delete old groups.
        std::set<CTxDestination> *merged =
            new std::set<CTxDestination>(_grouping);
        for (std::set<CTxDestination> *hit : hits) {
            merged->insert(hit->begin(), hit->end());
            uniqueGroupings.erase(hit);
            delete hit;
        }
        uniqueGroupings.insert(merged);

        // Update setmap.
        for (CTxDestination element : *merged) {
            setmap[element] = merged;
        }
    }

    std::set<std::set<CTxDestination>> ret;
    for (std::set<CTxDestination> *uniqueGrouping : uniqueGroupings) {
        ret.insert(*uniqueGrouping);
        delete uniqueGrouping;
    }

    return ret;
}

CAmount CWallet::GetAccountBalance(const std::string &strAccount, int nMinDepth,
                                   const isminefilter &filter) {
    CWalletDB walletdb(strWalletFile);
    return GetAccountBalance(walletdb, strAccount, nMinDepth, filter);
}

CAmount CWallet::GetAccountBalance(CWalletDB &walletdb,
                                   const std::string &strAccount, int nMinDepth,
                                   const isminefilter &filter) {
    CAmount nBalance = 0;

    // Tally wallet transactions.
    for (std::map<txid_t, CWalletTx>::iterator it = mapWallet.begin();
         it != mapWallet.end(); ++it) {
        const CWalletTx &wtx = (*it).second;
        if (!CheckFinalTx(wtx) || wtx.GetBlocksToMaturity() > 0 ||
            wtx.GetDepthInMainChain() < 0) {
            continue;
        }

        CAmount nReceived, nSent, nFee;
        wtx.GetAccountAmounts(strAccount, nReceived, nSent, nFee, filter);

        if (nReceived != 0 && wtx.GetDepthInMainChain() >= nMinDepth) {
            nBalance += nReceived;
        }

        nBalance -= nSent + nFee;
    }

    // Tally internal accounting entries.
    nBalance += walletdb.GetAccountCreditDebit(strAccount);

    return nBalance;
}

std::set<CTxDestination>
CWallet::GetAccountAddresses(const std::string &strAccount) const {
    LOCK(cs_wallet);
    std::set<CTxDestination> result;
    for (const std::pair<CTxDestination, CAddressBookData> &item :
         mapAddressBook) {
        const CTxDestination &address = item.first;
        const std::string &strName = item.second.name;
        if (strName == strAccount) {
            result.insert(address);
        }
    }

    return result;
}

bool CReserveKey::GetReservedKey(CPubKey &pubkey) {
    if (nIndex == -1) {
        CKeyPool keypool;
        pwallet->ReserveKeyFromKeyPool(nIndex, keypool);
        if (nIndex != -1) {
            vchPubKey = keypool.vchPubKey;
        } else {
            return false;
        }
    }

    assert(vchPubKey.IsValid());
    pubkey = vchPubKey;
    return true;
}

void CReserveKey::KeepKey() {
    if (nIndex != -1) {
        pwallet->KeepKey(nIndex);
    }

    nIndex = -1;
    vchPubKey = CPubKey();
}

void CReserveKey::ReturnKey() {
    if (nIndex != -1) {
        pwallet->ReturnKey(nIndex);
    }

    nIndex = -1;
    vchPubKey = CPubKey();
}

void CWallet::GetAllReserveKeys(std::set<CKeyID> &setAddress) const {
    setAddress.clear();

    CWalletDB walletdb(strWalletFile);

    LOCK2(cs_main, cs_wallet);
    for (const int64_t &id : setKeyPool) {
        CKeyPool keypool;
        if (!walletdb.ReadPool(id, keypool)) {
            throw std::runtime_error(std::string(__func__) + ": read failed");
        }

        assert(keypool.vchPubKey.IsValid());
        CKeyID keyID = keypool.vchPubKey.GetID();
        if (!HaveKey(keyID)) {
            throw std::runtime_error(std::string(__func__) +
                                     ": unknown key in key pool");
        }

        setAddress.insert(keyID);
    }
}

void CWallet::UpdatedTransaction(const txid_t &hashTx) {
    LOCK(cs_wallet);
    // Only notify UI if this transaction is in this wallet.
    const CWalletTx *wtx = GetWalletTx(hashTx);
    if (wtx) {
        NotifyTransactionChanged(this, hashTx, CT_UPDATED);
    }
}

void CWallet::GetScriptForMining(boost::shared_ptr<CReserveScript> &script) {
    boost::shared_ptr<CReserveKey> rKey(new CReserveKey(this));
    CPubKey pubkey;
    if (!rKey->GetReservedKey(pubkey)) {
        return;
    }

    script = rKey;
    script->reserveScript = CScript() << ToByteVector(pubkey) << OP_CHECKSIG;
}

void CWallet::LockCoin(const COutPoint &output) {
    // setLockedCoins
    AssertLockHeld(cs_wallet);
    setLockedCoins.insert(output);
}

void CWallet::UnlockCoin(const COutPoint &output) {
    // setLockedCoins
    AssertLockHeld(cs_wallet);
    setLockedCoins.erase(output);
}

void CWallet::UnlockAllCoins() {
    // setLockedCoins
    AssertLockHeld(cs_wallet);
    setLockedCoins.clear();
}

bool CWallet::IsLockedCoin(const COutPoint &outpoint) const {
    // setLockedCoins
    AssertLockHeld(cs_wallet);

    return setLockedCoins.count(outpoint) > 0;
}

void CWallet::ListLockedCoins(std::vector<COutPoint> &vOutpts) {
    // setLockedCoins
    AssertLockHeld(cs_wallet);
    for (std::set<COutPoint>::iterator it = setLockedCoins.begin();
         it != setLockedCoins.end(); it++) {
        COutPoint outpt = (*it);
        vOutpts.push_back(outpt);
    }
}

/** @} */ // end of Actions

class CAffectedKeysVisitor : public boost::static_visitor<void> {
private:
    const CKeyStore &keystore;
    std::vector<CKeyID> &vKeys;

public:
    CAffectedKeysVisitor(const CKeyStore &keystoreIn,
                         std::vector<CKeyID> &vKeysIn)
        : keystore(keystoreIn), vKeys(vKeysIn) {}

    void Process(const CScript &script) {
        txnouttype type;
        std::vector<CTxDestination> vDest;
        int nRequired;
        if (ExtractDestinations(script, type, vDest, nRequired)) {
            for (const CTxDestination &dest : vDest) {
                boost::apply_visitor(*this, dest);
            }
        }
    }

    void operator()(const CKeyID &keyId) {
        if (keystore.HaveKey(keyId)) {
            vKeys.push_back(keyId);
        }
    }

    void operator()(const CScriptID &scriptId) {
        CScript script;
        if (keystore.GetCScript(scriptId, script)) {
            Process(script);
        }
    }

    void operator()(const CNoDestination &none) {}
};

void CWallet::GetKeyBirthTimes(
    std::map<CTxDestination, int64_t> &mapKeyBirth) const {
    // mapKeyMetadata
    AssertLockHeld(cs_wallet);
    mapKeyBirth.clear();

    // Get birth times for keys with metadata.
    for (const auto &entry : mapKeyMetadata) {
        if (entry.second.nCreateTime) {
            mapKeyBirth[entry.first] = entry.second.nCreateTime;
        }
    }

    // Map in which we'll infer heights of other keys the tip can be
    // reorganized; use a 144-block safety margin.
    CBlockIndex *pindexMax =
        chainActive[std::max(0, chainActive.Height() - 144)];
    std::map<CKeyID, CBlockIndex *> mapKeyFirstBlock;
    std::set<CKeyID> setKeys;
    GetKeys(setKeys);
    for (const CKeyID &keyid : setKeys) {
        if (mapKeyBirth.count(keyid) == 0) {
            mapKeyFirstBlock[keyid] = pindexMax;
        }
    }
    setKeys.clear();

    // If there are no such keys, we're done.
    if (mapKeyFirstBlock.empty()) {
        return;
    }

    // Find first block that affects those keys, if there are any left.
    std::vector<CKeyID> vAffected;
    for (std::map<txid_t, CWalletTx>::const_iterator it = mapWallet.begin();
         it != mapWallet.end(); it++) {
        // Iterate over all wallet transactions...
        const CWalletTx &wtx = (*it).second;
        BlockMap::const_iterator blit = mapBlockIndex.find(wtx.hashBlock);
        if (blit != mapBlockIndex.end() && chainActive.Contains(blit->second)) {
            // ... which are already in a block.
            int nHeight = blit->second->nHeight;
            for (const CTxOut &txout : wtx.tx->vout) {
                // Iterate over all their outputs...
                CAffectedKeysVisitor(*this, vAffected)
                    .Process(txout.scriptPubKey);
                for (const CKeyID &keyid : vAffected) {
                    // ... and all their affected keys.
                    std::map<CKeyID, CBlockIndex *>::iterator rit =
                        mapKeyFirstBlock.find(keyid);
                    if (rit != mapKeyFirstBlock.end() &&
                        nHeight < rit->second->nHeight) {
                        rit->second = blit->second;
                    }
                }
                vAffected.clear();
            }
        }
    }

    // Extract block timestamps for those keys.
    for (std::map<CKeyID, CBlockIndex *>::const_iterator it =
             mapKeyFirstBlock.begin();
         it != mapKeyFirstBlock.end(); it++) {
        // Block times can be 2h off.
        mapKeyBirth[it->first] = it->second->GetBlockTime() - 7200;
    }
}

bool CWallet::AddDestData(const CTxDestination &dest, const std::string &key,
                          const std::string &value) {
    if (boost::get<CNoDestination>(&dest)) {
        return false;
    }

    mapAddressBook[dest].destdata.insert(std::make_pair(key, value));
    if (!fFileBacked) {
        return true;
    }

    return CWalletDB(strWalletFile)
        .WriteDestData(CBitcoinAddress(dest).ToString(), key, value);
}

bool CWallet::EraseDestData(const CTxDestination &dest,
                            const std::string &key) {
    if (!mapAddressBook[dest].destdata.erase(key)) {
        return false;
    }

    if (!fFileBacked) {
        return true;
    }

    return CWalletDB(strWalletFile)
        .EraseDestData(CBitcoinAddress(dest).ToString(), key);
}

bool CWallet::LoadDestData(const CTxDestination &dest, const std::string &key,
                           const std::string &value) {
    mapAddressBook[dest].destdata.insert(std::make_pair(key, value));
    return true;
}

bool CWallet::GetDestData(const CTxDestination &dest, const std::string &key,
                          std::string *value) const {
    std::map<CTxDestination, CAddressBookData>::const_iterator i =
        mapAddressBook.find(dest);
    if (i != mapAddressBook.end()) {
        CAddressBookData::StringMap::const_iterator j =
            i->second.destdata.find(key);
        if (j != i->second.destdata.end()) {
            if (value) {
                *value = j->second;
            }

            return true;
        }
    }

    return false;
}

std::string CWallet::GetWalletHelpString(bool showDebug) {
    std::string strUsage = HelpMessageGroup(_("Wallet options:"));
    strUsage += HelpMessageOpt(
        "-disablewallet",
        _("Do not load the wallet and disable wallet RPC calls"));
    strUsage += HelpMessageOpt(
        "-keypool=<n>", strprintf(_("Set key pool size to <n> (default: %u)"),
                                  DEFAULT_KEYPOOL_SIZE));
    strUsage += HelpMessageOpt(
        "-fallbackfee=<amt>",
        strprintf(_("A fee rate (in %s/kB) that will be used when fee "
                    "estimation has insufficient data (default: %s)"),
                  CURRENCY_UNIT, FormatMoney(DEFAULT_FALLBACK_FEE)));
    strUsage += HelpMessageOpt(
        "-mintxfee=<amt>",
        strprintf(_("Fees (in %s/kB) smaller than this are considered zero fee "
                    "for transaction creation (default: %s)"),
                  CURRENCY_UNIT, FormatMoney(DEFAULT_TRANSACTION_MINFEE)));
    strUsage += HelpMessageOpt(
        "-paytxfee=<amt>",
        strprintf(
            _("Fee (in %s/kB) to add to transactions you send (default: %s)"),
            CURRENCY_UNIT, FormatMoney(payTxFee.GetFeePerK())));
    strUsage += HelpMessageOpt(
        "-rescan",
        _("Rescan the block chain for missing wallet transactions on startup"));
    strUsage += HelpMessageOpt(
        "-salvagewallet",
        _("Attempt to recover private keys from a corrupt wallet on startup"));
    if (showDebug) {
        strUsage += HelpMessageOpt(
            "-sendfreetransactions",
            strprintf(_("Send transactions as zero-fee transactions if "
                        "possible (default: %u)"),
                      DEFAULT_SEND_FREE_TRANSACTIONS));
    }

    strUsage +=
        HelpMessageOpt("-spendzeroconfchange",
                       strprintf(_("Spend unconfirmed change when sending "
                                   "transactions (default: %u)"),
                                 DEFAULT_SPEND_ZEROCONF_CHANGE));
    strUsage +=
        HelpMessageOpt("-txconfirmtarget=<n>",
                       strprintf(_("If paytxfee is not set, include enough fee "
                                   "so transactions begin confirmation on "
                                   "average within n blocks (default: %u)"),
                                 DEFAULT_TX_CONFIRM_TARGET));
    strUsage += HelpMessageOpt(
        "-usehd",
        _("Use hierarchical deterministic key generation (HD) after BIP32. "
          "Only has effect during wallet creation/first start") +
            " " + strprintf(_("(default: %u)"), DEFAULT_USE_HD_WALLET));
    strUsage += HelpMessageOpt("-upgradewallet",
                               _("Upgrade wallet to latest format on startup"));
    strUsage +=
        HelpMessageOpt("-wallet=<file>",
                       _("Specify wallet file (within data directory)") + " " +
                           strprintf(_("(default: %s)"), DEFAULT_WALLET_DAT));
    strUsage += HelpMessageOpt(
        "-walletbroadcast",
        _("Make the wallet broadcast transactions") + " " +
            strprintf(_("(default: %u)"), DEFAULT_WALLETBROADCAST));
    strUsage += HelpMessageOpt("-walletnotify=<cmd>",
                               _("Execute command when a wallet transaction "
                                 "changes (%s in cmd is replaced by TxID)"));
    strUsage += HelpMessageOpt(
        "-zapwallettxes=<mode>",
        _("Delete all wallet transactions and only recover those parts of the "
          "blockchain through -rescan on startup") +
            " " + _("(1 = keep tx meta data e.g. account owner and payment "
                    "request information, 2 = drop tx meta data)"));

    if (showDebug) {
        strUsage += HelpMessageGroup(_("Wallet debugging/testing options:"));

        strUsage += HelpMessageOpt(
            "-dblogsize=<n>",
            strprintf("Flush wallet database activity from memory to disk log "
                      "every <n> megabytes (default: %u)",
                      DEFAULT_WALLET_DBLOGSIZE));
        strUsage += HelpMessageOpt(
            "-flushwallet",
            strprintf("Run a thread to flush wallet periodically (default: %u)",
                      DEFAULT_FLUSHWALLET));
        strUsage += HelpMessageOpt(
            "-privdb", strprintf("Sets the DB_PRIVATE flag in the wallet db "
                                 "environment (default: %u)",
                                 DEFAULT_WALLET_PRIVDB));
        strUsage += HelpMessageOpt(
            "-walletrejectlongchains",
            strprintf(_("Wallet will not create transactions that violate "
                        "mempool chain limits (default: %u)"),
                      DEFAULT_WALLET_REJECT_LONG_CHAINS));
    }

    return strUsage;
}

CWallet *CWallet::CreateWalletFromFile(const std::string walletFile) {
    // Needed to restore wallet transaction meta data after -zapwallettxes
    std::vector<CWalletTx> vWtx;

    if (GetBoolArg("-zapwallettxes", false)) {
        uiInterface.InitMessage(_("Zapping all transactions from wallet..."));

        CWallet *tempWallet = new CWallet(walletFile);
        DBErrors nZapWalletRet = tempWallet->ZapWalletTx(vWtx);
        if (nZapWalletRet != DB_LOAD_OK) {
            InitError(
                strprintf(_("Error loading %s: Wallet corrupted"), walletFile));
            return nullptr;
        }

        delete tempWallet;
        tempWallet = nullptr;
    }

    uiInterface.InitMessage(_("Loading wallet..."));

    int64_t nStart = GetTimeMillis();
    bool fFirstRun = true;
    CWallet *walletInstance = new CWallet(walletFile);
    DBErrors nLoadWalletRet = walletInstance->LoadWallet(fFirstRun);
    if (nLoadWalletRet != DB_LOAD_OK) {
        if (nLoadWalletRet == DB_CORRUPT) {
            InitError(
                strprintf(_("Error loading %s: Wallet corrupted"), walletFile));
            return nullptr;
        }

        if (nLoadWalletRet == DB_NONCRITICAL_ERROR) {
            InitWarning(strprintf(
                _("Error reading %s! All keys read correctly, but transaction "
                  "data"
                  " or address book entries might be missing or incorrect."),
                walletFile));
        } else if (nLoadWalletRet == DB_TOO_NEW) {
            InitError(strprintf(
                _("Error loading %s: Wallet requires newer version of %s"),
                walletFile, _(PACKAGE_NAME)));
            return nullptr;
        } else if (nLoadWalletRet == DB_NEED_REWRITE) {
            InitError(strprintf(
                _("Wallet needed to be rewritten: restart %s to complete"),
                _(PACKAGE_NAME)));
            return nullptr;
        } else {
            InitError(strprintf(_("Error loading %s"), walletFile));
            return nullptr;
        }
    }

    if (GetBoolArg("-upgradewallet", fFirstRun)) {
        int nMaxVersion = GetArg("-upgradewallet", 0);
        // The -upgradewallet without argument case
        if (nMaxVersion == 0) {
            LogPrintf("Performing wallet upgrade to %i\n", FEATURE_LATEST);
            nMaxVersion = CLIENT_VERSION;
            // permanently upgrade the wallet immediately
            walletInstance->SetMinVersion(FEATURE_LATEST);
        } else {
            LogPrintf("Allowing wallet upgrade up to %i\n", nMaxVersion);
        }

        if (nMaxVersion < walletInstance->GetVersion()) {
            InitError(_("Cannot downgrade wallet"));
            return nullptr;
        }

        walletInstance->SetMaxVersion(nMaxVersion);
    }

    if (fFirstRun) {
        // Create new keyUser and set as default key.
        if (GetBoolArg("-usehd", DEFAULT_USE_HD_WALLET) &&
            !walletInstance->IsHDEnabled()) {
            // Generate a new master key.
            CPubKey masterPubKey = walletInstance->GenerateNewHDMasterKey();
            if (!walletInstance->SetHDMasterKey(masterPubKey)) {
                throw std::runtime_error(std::string(__func__) +
                                         ": Storing master key failed");
            }
        }

        CPubKey newDefaultKey;
        if (walletInstance->GetKeyFromPool(newDefaultKey)) {
            walletInstance->SetDefaultKey(newDefaultKey);
            if (!walletInstance->SetAddressBook(
                    walletInstance->vchDefaultKey.GetID(), "", "receive")) {
                InitError(_("Cannot write default address") += "\n");
                return nullptr;
            }
        }

        walletInstance->SetBestChain(chainActive.GetLocator());
    } else if (IsArgSet("-usehd")) {
        bool useHD = GetBoolArg("-usehd", DEFAULT_USE_HD_WALLET);
        if (walletInstance->IsHDEnabled() && !useHD) {
            InitError(strprintf(_("Error loading %s: You can't disable HD on a "
                                  "already existing HD wallet"),
                                walletFile));
            return nullptr;
        }

        if (!walletInstance->IsHDEnabled() && useHD) {
            InitError(strprintf(_("Error loading %s: You can't enable HD on a "
                                  "already existing non-HD wallet"),
                                walletFile));
            return nullptr;
        }
    }

    LogPrintf(" wallet      %15dms\n", GetTimeMillis() - nStart);

    RegisterValidationInterface(walletInstance);

    CBlockIndex *pindexRescan = chainActive.Tip();
    if (GetBoolArg("-rescan", false)) {
        pindexRescan = chainActive.Genesis();
    } else {
        CWalletDB walletdb(walletFile);
        CBlockLocator locator;
        if (walletdb.ReadBestBlock(locator)) {
            pindexRescan = FindForkInGlobalIndex(chainActive, locator);
        } else {
            pindexRescan = chainActive.Genesis();
        }
    }

    if (chainActive.Tip() && chainActive.Tip() != pindexRescan) {
        // We can't rescan beyond non-pruned blocks, stop and throw an error.
        // This might happen if a user uses a old wallet within a pruned node or
        // if he ran -disablewallet for a longer time, then decided to
        // re-enable.
        if (fPruneMode) {
            CBlockIndex *block = chainActive.Tip();
            while (block && block->pprev &&
                   (block->pprev->nStatus & BLOCK_HAVE_DATA) &&
                   block->pprev->nTx > 0 && pindexRescan != block) {
                block = block->pprev;
            }

            if (pindexRescan != block) {
                InitError(_("Prune: last wallet synchronisation goes beyond "
                            "pruned data. You need to -reindex (download the "
                            "whole blockchain again in case of pruned node)"));
                return nullptr;
            }
        }

        uiInterface.InitMessage(_("Rescanning..."));
        LogPrintf("Rescanning last %i blocks (from block %i)...\n",
                  chainActive.Height() - pindexRescan->nHeight,
                  pindexRescan->nHeight);
        nStart = GetTimeMillis();
        walletInstance->ScanForWalletTransactions(pindexRescan, true);
        LogPrintf(" rescan      %15dms\n", GetTimeMillis() - nStart);
        walletInstance->SetBestChain(chainActive.GetLocator());
        CWalletDB::IncrementUpdateCounter();

        // Restore wallet transaction metadata after -zapwallettxes=1
        if (GetBoolArg("-zapwallettxes", false) &&
            GetArg("-zapwallettxes", "1") != "2") {
            CWalletDB walletdb(walletFile);

            for (const CWalletTx &wtxOld : vWtx) {
                txid_t txid = wtxOld.GetId();
                std::map<txid_t, CWalletTx>::iterator mi =
                    walletInstance->mapWallet.find(txid);
                if (mi != walletInstance->mapWallet.end()) {
                    const CWalletTx *copyFrom = &wtxOld;
                    CWalletTx *copyTo = &mi->second;
                    copyTo->mapValue = copyFrom->mapValue;
                    copyTo->vOrderForm = copyFrom->vOrderForm;
                    copyTo->nTimeReceived = copyFrom->nTimeReceived;
                    copyTo->nTimeSmart = copyFrom->nTimeSmart;
                    copyTo->fFromMe = copyFrom->fFromMe;
                    copyTo->strFromAccount = copyFrom->strFromAccount;
                    copyTo->nOrderPos = copyFrom->nOrderPos;
                    walletdb.WriteTx(*copyTo);
                }
            }
        }
    }

    walletInstance->SetBroadcastTransactions(
        GetBoolArg("-walletbroadcast", DEFAULT_WALLETBROADCAST));

    LOCK(walletInstance->cs_wallet);
    LogPrintf("setKeyPool.size() = %u\n", walletInstance->GetKeyPoolSize());
    LogPrintf("mapWallet.size() = %u\n", walletInstance->mapWallet.size());
    LogPrintf("mapAddressBook.size() = %u\n",
              walletInstance->mapAddressBook.size());

    return walletInstance;
}

bool CWallet::InitLoadWallet() {
    if (GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET)) {
        pwalletMain = nullptr;
        LogPrintf("Wallet disabled!\n");
        return true;
    }

    std::string walletFile = GetArg("-wallet", DEFAULT_WALLET_DAT);

    CWallet *const pwallet = CreateWalletFromFile(walletFile);
    if (!pwallet) {
        return false;
    }

    pwalletMain = pwallet;

    return true;
}

std::atomic<bool> CWallet::fFlushThreadRunning(false);

void CWallet::postInitProcess(boost::thread_group &threadGroup) {
    // Add wallet transactions that aren't already in a block to mempool.
    // Do this here as mempool requires genesis block to be loaded.
    ReacceptWalletTransactions();

    // Run a thread to flush wallet periodically.
    if (!CWallet::fFlushThreadRunning.exchange(true)) {
        threadGroup.create_thread(ThreadFlushWalletDB);
    }
}

bool CWallet::ParameterInteraction() {
    if (GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET)) {
        return true;
    }

    if (GetBoolArg("-blocksonly", DEFAULT_BLOCKSONLY) &&
        SoftSetBoolArg("-walletbroadcast", false)) {
        LogPrintf("%s: parameter interaction: -blocksonly=1 -> setting "
                  "-walletbroadcast=0\n",
                  __func__);
    }

    if (GetBoolArg("-salvagewallet", false) &&
        SoftSetBoolArg("-rescan", true)) {
        // Rewrite just private keys: rescan to find transactions
        LogPrintf("%s: parameter interaction: -salvagewallet=1 -> setting "
                  "-rescan=1\n",
                  __func__);
    }

    // -zapwallettx implies a rescan
    if (GetBoolArg("-zapwallettxes", false) &&
        SoftSetBoolArg("-rescan", true)) {
        LogPrintf("%s: parameter interaction: -zapwallettxes=<mode> -> setting "
                  "-rescan=1\n",
                  __func__);
    }

    if (GetBoolArg("-sysperms", false)) {
        return InitError("-sysperms is not allowed in combination with enabled "
                         "wallet functionality");
    }

    if (GetArg("-prune", 0) && GetBoolArg("-rescan", false)) {
        return InitError(
            _("Rescans are not possible in pruned mode. You will need to use "
              "-reindex which will download the whole blockchain again."));
    }

    if (::minRelayTxFee.GetFeePerK() > HIGH_TX_FEE_PER_KB) {
        InitWarning(
            AmountHighWarn("-minrelaytxfee") + " " +
            _("The wallet will avoid paying less than the minimum relay fee."));
    }

    if (IsArgSet("-mintxfee")) {
        CAmount n = 0;
        if (!ParseMoney(GetArg("-mintxfee", ""), n) || 0 == n) {
            return InitError(AmountErrMsg("mintxfee", GetArg("-mintxfee", "")));
        }

        if (n > HIGH_TX_FEE_PER_KB) {
            InitWarning(AmountHighWarn("-mintxfee") + " " +
                        _("This is the minimum transaction fee you pay on "
                          "every transaction."));
        }

        CWallet::minTxFee = CFeeRate(n);
    }

    if (IsArgSet("-fallbackfee")) {
        CAmount nFeePerK = 0;
        if (!ParseMoney(GetArg("-fallbackfee", ""), nFeePerK)) {
            return InitError(
                strprintf(_("Invalid amount for -fallbackfee=<amount>: '%s'"),
                          GetArg("-fallbackfee", "")));
        }

        if (nFeePerK > HIGH_TX_FEE_PER_KB) {
            InitWarning(AmountHighWarn("-fallbackfee") + " " +
                        _("This is the transaction fee you may pay when fee "
                          "estimates are not available."));
        }

        CWallet::fallbackFee = CFeeRate(nFeePerK);
    }

    if (IsArgSet("-paytxfee")) {
        CAmount nFeePerK = 0;
        if (!ParseMoney(GetArg("-paytxfee", ""), nFeePerK)) {
            return InitError(AmountErrMsg("paytxfee", GetArg("-paytxfee", "")));
        }

        if (nFeePerK > HIGH_TX_FEE_PER_KB) {
            InitWarning(AmountHighWarn("-paytxfee") + " " +
                        _("This is the transaction fee you will pay if you "
                          "send a transaction."));
        }

        payTxFee = CFeeRate(nFeePerK, 1000);
        if (payTxFee < ::minRelayTxFee) {
            return InitError(
                strprintf(_("Invalid amount for -paytxfee=<amount>: '%s' (must "
                            "be at least %s)"),
                          GetArg("-paytxfee", ""), ::minRelayTxFee.ToString()));
        }
    }

    if (IsArgSet("-maxtxfee")) {
        CAmount nMaxFee = 0;
        if (!ParseMoney(GetArg("-maxtxfee", ""), nMaxFee)) {
            return InitError(AmountErrMsg("maxtxfee", GetArg("-maxtxfee", "")));
        }

        if (nMaxFee > HIGH_MAX_TX_FEE) {
            InitWarning(_("-maxtxfee is set very high! Fees this large could "
                          "be paid on a single transaction."));
        }

        maxTxFee = nMaxFee;
        if (CFeeRate(maxTxFee, 1000) < ::minRelayTxFee) {
            return InitError(
                strprintf(_("Invalid amount for -maxtxfee=<amount>: '%s' (must "
                            "be at least the minrelay fee of %s to prevent "
                            "stuck transactions)"),
                          GetArg("-maxtxfee", ""), ::minRelayTxFee.ToString()));
        }
    }

    nTxConfirmTarget = GetArg("-txconfirmtarget", DEFAULT_TX_CONFIRM_TARGET);
    bSpendZeroConfChange =
        GetBoolArg("-spendzeroconfchange", DEFAULT_SPEND_ZEROCONF_CHANGE);
    fSendFreeTransactions =
        GetBoolArg("-sendfreetransactions", DEFAULT_SEND_FREE_TRANSACTIONS);

    if (fSendFreeTransactions &&
        GetArg("-limitfreerelay", DEFAULT_LIMITFREERELAY) <= 0) {
        return InitError("Creation of free transactions with their relay "
                         "disabled is not supported.");
    }

    return true;
}

bool CWallet::BackupWallet(const std::string &strDest) {
    if (!fFileBacked) {
        return false;
    }

    while (true) {
        {
            LOCK(bitdb.cs_db);
            if (!bitdb.mapFileUseCount.count(strWalletFile) ||
                bitdb.mapFileUseCount[strWalletFile] == 0) {
                // Flush log data to the dat file.
                bitdb.CloseDb(strWalletFile);
                bitdb.CheckpointLSN(strWalletFile);
                bitdb.mapFileUseCount.erase(strWalletFile);

                // Copy wallet file.
                boost::filesystem::path pathSrc = GetDataDir() / strWalletFile;
                boost::filesystem::path pathDest(strDest);
                if (boost::filesystem::is_directory(pathDest)) {
                    pathDest /= strWalletFile;
                }

                try {
#if BOOST_VERSION >= 104000
                    boost::filesystem::copy_file(
                        pathSrc, pathDest,
                        boost::filesystem::copy_option::overwrite_if_exists);
#else
                    boost::filesystem::copy_file(pathSrc, pathDest);
#endif
                    LogPrintf("copied %s to %s\n", strWalletFile,
                              pathDest.string());
                    return true;
                } catch (const boost::filesystem::filesystem_error &e) {
                    LogPrintf("error copying %s to %s - %s\n", strWalletFile,
                              pathDest.string(), e.what());
                    return false;
                }
            }
        }

        MilliSleep(100);
    }

    return false;
}

CKeyPool::CKeyPool() {
    nTime = GetTime();
}

CKeyPool::CKeyPool(const CPubKey &vchPubKeyIn) {
    nTime = GetTime();
    vchPubKey = vchPubKeyIn;
}

CWalletKey::CWalletKey(int64_t nExpires) {
    nTimeCreated = (nExpires ? GetTime() : 0);
    nTimeExpires = nExpires;
}

void CMerkleTx::SetMerkleBranch(const CBlockIndex *pindex, int posInBlock) {
    // Update the tx's hashBlock
    hashBlock = pindex->GetBlockHash();

    // Set the position of the transaction in the block.
    nIndex = posInBlock;
}

int CMerkleTx::GetDepthInMainChain(const CBlockIndex *&pindexRet) const {
    if (hashUnset()) {
        return 0;
    }

    AssertLockHeld(cs_main);

    // Find the block it claims to be in.
    BlockMap::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end()) {
        return 0;
    }

    CBlockIndex *pindex = (*mi).second;
    if (!pindex || !chainActive.Contains(pindex)) {
        return 0;
    }

    pindexRet = pindex;
    return ((nIndex == -1) ? (-1) : 1) *
           (chainActive.Height() - pindex->nHeight + 1);
}

int CMerkleTx::GetBlocksToMaturity() const {
    if (!IsCoinBase()) {
        return 0;
    }

    return std::max(0, (COINBASE_MATURITY + 1) - GetDepthInMainChain());
}

bool CMerkleTx::AcceptToMemoryPool(const CAmount &nAbsurdFee,
                                   CValidationState &state) {
    return ::AcceptToMemoryPool(GetConfig(), mempool, state, tx, true, nullptr,
                                nullptr, false, nAbsurdFee);
}
