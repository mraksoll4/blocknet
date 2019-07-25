// Copyright (c) 2019 The Blocknet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BLOCKNET_SERVICENODE_H
#define BLOCKNET_SERVICENODE_H

#include <amount.h>
#include <base58.h>
#include <chainparams.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/standard.h>
#include <streams.h>
#include <sync.h>
#include <timedata.h>

#include <set>
#include <vector>

class LegacyXBridgePacket {
public:
    void CopyFrom(std::vector<unsigned char> packet) {
        unsigned int offset{20+8}; // ignore packet address (uint160) & timestamp (uint64_t)
        version   = *static_cast<uint32_t*>(static_cast<void*>(&packet[0]+offset)); offset += sizeof(uint32_t);
        command   = *static_cast<uint32_t*>(static_cast<void*>(&packet[0]+offset)); offset += sizeof(uint32_t);
        timestamp = *static_cast<uint32_t*>(static_cast<void*>(&packet[0]+offset)); offset += sizeof(uint32_t);
        bodysize  = *static_cast<uint32_t*>(static_cast<void*>(&packet[0]+offset)); offset += sizeof(uint32_t);
        pubkey    = CPubKey(packet.begin()+offset, packet.begin()+offset+CPubKey::COMPRESSED_PUBLIC_KEY_SIZE); offset += CPubKey::COMPRESSED_PUBLIC_KEY_SIZE;
        signature = std::vector<unsigned char>(packet.begin()+offset, packet.begin()+offset+64); offset += 64;
        body      = std::vector<unsigned char>(packet.begin()+offset, packet.end());
    }

public:
    uint32_t version;
    uint32_t command;
    uint32_t timestamp;
    uint32_t bodysize;
    CPubKey pubkey;
    std::vector<unsigned char> signature;
    std::vector<unsigned char> body;
};

class ServiceNode {
public:
    static const CAmount COLLATERAL_SPV = 5000 * COIN;
    enum Tier : uint32_t {
        OPEN = 0,
        SPV = 50,
    };

public:
    explicit ServiceNode() : snodePubKey(std::vector<unsigned char>()),
                    tier(Tier::OPEN),
                    collateral(std::vector<COutPoint>()),
                    bestBlock(0),
                    bestBlockHash(uint256()),
                    signature(std::vector<unsigned char>()),
                    regtime(GetAdjustedTime()),
                    pingtime(0)
                    {}

    friend inline bool operator==(const ServiceNode & a, const ServiceNode & b) { return a.snodePubKey == b.snodePubKey; }
    friend inline bool operator!=(const ServiceNode & a, const ServiceNode & b) { return a.snodePubKey != b.snodePubKey; }
    friend inline bool operator<(const ServiceNode & a, const ServiceNode & b) { return a.regtime < b.regtime; }

    bool isNull() const {
        return snodePubKey.empty();
    }

    CPubKey getSnodePubKey() const {
        return CPubKey(snodePubKey);
    }

    Tier getTier() const {
        return static_cast<Tier>(tier);
    }

    const std::vector<COutPoint>& getCollateral() const {
        return collateral;
    }

    const std::vector<unsigned char>& getSignature() const {
        return signature;
    }

    int64_t getRegTime() const {
        return regtime;
    }

    void updatePing() {
        pingtime = GetAdjustedTime();
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(snodePubKey);
        READWRITE(tier);
        READWRITE(collateral);
        READWRITE(bestBlock);
        READWRITE(bestBlockHash);
        READWRITE(signature);
    }

    static uint256 CreateSigHash(const std::vector<unsigned char> & snodePubKey, const Tier & tier,
            const std::vector<COutPoint> & collateral, const uint32_t & bestBlock=0, const uint256 & bestBlockHash=uint256())
    {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << snodePubKey << static_cast<uint32_t>(tier) << collateral << bestBlock << bestBlockHash;
        return ss.GetHash();
    }

    uint256 sigHash() const {
        return CreateSigHash(snodePubKey, static_cast<Tier>(tier), collateral, bestBlock, bestBlockHash);
    }

    uint256 getHash() const {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << snodePubKey << static_cast<uint32_t>(tier) << collateral << bestBlock << bestBlockHash
           << signature << regtime;
        return ss.GetHash();
    }

    bool isValid(const std::function<CTransactionRef(const COutPoint & out)> & getTxFunc,
                 const std::function<bool(const uint32_t & blockNumber, const uint256 & blockHash)> & isBlockValid) const
    {
        // Block reported by snode must be ancestor of our chain tip
        if (!isBlockValid(bestBlock, bestBlockHash))
            return false;

        // Validate the snode pubkey
        {
            CPubKey pubkey(snodePubKey);
            if (!pubkey.IsFullyValid())
                return false;
        }

        // If open tier, the signature should be generated from the snode pubkey
        if (tier == Tier::OPEN) {
            const auto & sighash = sigHash();
            CPubKey pubkey2;
            if (!pubkey2.RecoverCompact(sighash, signature))
                return false; // not valid if bad sig
            return CPubKey(snodePubKey).GetID() == pubkey2.GetID();
        }

        // If not on the open tier, check collateral
        if (collateral.empty())
            return false; // not valid if no collateral

        const auto & sighash = sigHash();
        CAmount total{0}; // Track the total collateral amount
        std::set<COutPoint> unique_collateral(collateral.begin(), collateral.end()); // prevent duplicates

        // Determine if all collateral utxos validate the sig
        for (const auto & op : unique_collateral) {
            CTransactionRef tx = getTxFunc(op);
            if (!tx)
                return false; // not valid if no transaction found or utxo is already spent

            if (tx->vout.size() <= op.n)
                return false; // not valid if bad vout index

            const auto & out = tx->vout[op.n];
            total += out.nValue;

            CTxDestination address;
            if (!ExtractDestination(out.scriptPubKey, address))
                return false; // not valid if bad address

            CKeyID *keyid = boost::get<CKeyID>(&address);
            CPubKey pubkey;
            if (!pubkey.RecoverCompact(sighash, signature))
                return false; // not valid if bad sig
            if (pubkey.GetID() != *keyid)
                return false; // fail if pubkeys don't match
        }

        if (tier == Tier::SPV && total >= COLLATERAL_SPV)
            return true;

        return false;
    }

protected: // included in network serialization
    std::vector<unsigned char> snodePubKey;
    uint32_t tier;
    std::vector<COutPoint> collateral;
    uint32_t bestBlock;
    uint256 bestBlockHash;
    std::vector<unsigned char> signature;

protected: // in-memory only
    int64_t regtime;
    int64_t pingtime;
};

typedef std::shared_ptr<ServiceNode> ServiceNodePtr;

class ServiceNodePing {
public:
    ServiceNodePing() : snodePubKey(std::vector<unsigned char>()),
                        signature(std::vector<unsigned char>()) {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(snodePubKey);
        READWRITE(signature);
    }

    CPubKey getSnodePubKey() const {
        return CPubKey(snodePubKey);
    }

    const std::vector<unsigned char>& getSignature() const {
        return signature;
    }

    uint256 getHash() const {
        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << snodePubKey << signature;
        return ss.GetHash();
    }

protected:
    std::vector<unsigned char> snodePubKey;
    std::vector<unsigned char> signature;
};


#endif //BLOCKNET_SERVICENODE_H
