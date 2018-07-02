// Copyright (c) 2018 The Merit Foundation developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pog2/select.h"
#include "base58.h"
#include "clientversion.h"
#include "util.h"

#include <algorithm>
#include <iterator>
#include "referrals.h"

namespace pog2
{
    namespace
    {
        enum PoolType {
            CGS,
            NEW,
            ANY,
        };

        struct InvitePool
        {
            PoolType type;
            double probability;
        };

        using InvitePoolDitributions = std::vector<InvitePool>;

        const InvitePoolDitributions INVITE_POOLS = {
            {CGS, 0.5},
            {NEW, 0.4},
            {ANY, 0.1}
        };
    }

    bool IsValidAmbassadorDestination(char type)
    {
        //KeyID or ScriptID
        return type == 1 || type == 2;
    }

    /**
     * CgsDistribution uses Inverse Transform Sampling. Computing the
     * CDF is trivial for the CGS discrete distribution by simply sorting and
     * adding up all the CGSs of the addresss provided.
     *
     * Scaling to probabilities is unnecessary because we will use a hash function
     * to sample into the range between 0-MaxCgs. Since the hash is already
     * a uniform distribution then it provides a good way to sample into
     * the distribution of CGSs where those with a bigger CGS are sampled more often.
     *
     * The most expensive part of creating the distribution is sorting the CGSs.
     * However, since the number of CGSs is fixed no matter how large the
     * blockchain gets, then there should be no issue handling growth.
     */
    CgsDistribution::CgsDistribution(pog2::Entrants cgses) :
        m_inverted(cgses.size())
    {
        //index cgses by address id for convenience.
        std::transform(
                std::begin(cgses),
                std::end(cgses),
                std::inserter(m_cgses, std::begin(m_cgses)),

                [](const pog2::Entrant& v) {
                    assert(v.cgs >= 0);
                    return std::make_pair(v.address, v);
                });

        assert(m_cgses.size() == cgses.size());

        std::sort(std::begin(cgses), std::end(cgses),
                [](const pog2::Entrant& a, const pog2::Entrant& b) {
                    return a.cgs == b.cgs ? a.address < b.address : a.cgs < b.cgs;
                });

        assert(m_inverted.size() == cgses.size());

        //compute CDF by adding up all the CGSs
        CAmount previous_cgs = 0;
        std::transform(std::begin(cgses), std::end(cgses), std::begin(m_inverted),
                [&previous_cgs](pog2::Entrant w) {
                    w.cgs += previous_cgs;
                    previous_cgs = w.cgs;
                    return w;
                });

        //back will always return because we assume m_cgses is non-empty
        if(!m_inverted.empty()) m_max_cgs = m_inverted.back().cgs;

        assert(m_max_cgs >= 0);
    }

    const pog2::Entrant& CgsDistribution::Sample(const uint256& hash) const
    {
        //It doesn't make sense to sample from an empty distribution.
        assert(m_inverted.empty() == false);

        const auto selected_cgs = SipHashUint256(0, 0, hash) % m_max_cgs;

        auto pos = std::lower_bound(std::begin(m_inverted), std::end(m_inverted),
                selected_cgs,
                [](const pog2::Entrant& a, CAmount selected) {
                    return a.cgs < selected;
                });

        assert(m_max_cgs >= 0);
        assert(selected_cgs < static_cast<uint64_t>(m_max_cgs));
        assert(pos != std::end(m_inverted)); //it should be impossible to not find an cgs
                                             //because selected_cgs must be less than max
        auto selected_address = m_cgses.find(pos->address);

        assert(selected_address != std::end(m_cgses)); //all cgses in m_inverted must be in
                                                    //our index
        return selected_address->second;
    }

    size_t CgsDistribution::Size() const {
        return m_inverted.size();
    }

    AddressSelector::AddressSelector(
            int height,
            const pog2::Entrants& entrants,
            const Consensus::Params& params)
    {
        m_old_distribution.reset(new CgsDistribution{entrants});

        pog2::Entrants new_entrants;
        std::copy_if(entrants.begin(), entrants.end(), std::back_inserter(new_entrants),
                [height,&params](const Entrant& e) {
                    assert(height >= e.beacon_height);
                    const double age = height - e.beacon_height;
                    return age <= params.pog2_new_distribution_age;

                });
        m_new_distribution.reset(new CgsDistribution{new_entrants});

        assert(m_old_distribution);
        assert(m_new_distribution);
    }

    /**
     * Selecting winners from the distribution is deterministic and will return the same
     * N samples given the same input hash.
     */
    pog2::Entrants AddressSelector::Select(
            const referral::ReferralsViewCache& referrals,
            uint256 hash,
            size_t n,
            const CgsDistribution& distribution)
    {
        assert(n <= Size());
        pog2::Entrants samples;

        auto max_tries = std::min(std::max(n, distribution.Size() / 2), distribution.Size());
        while(n-- && max_tries--) {
            const auto& sampled = distribution.Sample(hash);

            //combine hashes and hash to get next sampling value
            CHashWriter hasher{SER_DISK, CLIENT_VERSION};
            hasher << hash << sampled.address;
            hash = hasher.GetHash();

            const bool not_sampled = m_sampled.count(sampled.address) == 0;

            if( not_sampled && referrals.IsConfirmed(sampled.address)) {
                m_sampled.insert(sampled.address);
                samples.push_back(sampled);
            } else {
                n++;
            }
        }

        return samples;
    }

    pog2::Entrants AddressSelector::SelectOld(
            const referral::ReferralsViewCache& referrals,
            uint256 hash,
            size_t n)
    {
        assert(m_old_distribution);
        return Select( referrals, hash, n, *m_old_distribution);
    }
        
    pog2::Entrants AddressSelector::SelectNew(
            const referral::ReferralsViewCache& referrals,
            uint256 hash,
            size_t n)
    {
        assert(m_new_distribution);
        return Select( referrals, hash, n, *m_new_distribution);
    }

    size_t AddressSelector::Size() const
    {
        assert(m_old_distribution);
        assert(m_new_distribution);

        return m_old_distribution->Size();
    }

    using Novite = std::pair<referral::MaybeConfirmedAddress,uint64_t>;

    Novite FindNextNovite(const referral::ReferralsViewCache& db, uint64_t idx) {
        const auto total = db.GetTotalConfirmations();
        idx++;
        for(; idx < total; idx++) {
            auto c = db.GetConfirmation(idx);
            if(c && c->invites == 1) {
                return {c, idx};
            }
        }
        return {{}, 0};
    }

    Novite FindPrevNovite(const referral::ReferralsViewCache& db, uint64_t idx) {
        if(idx == 0) { 
            return {{}, 0};
        }

        idx--;
        for(; idx >= 0; idx--) {
            auto c = db.GetConfirmation(idx);
            if(c && c->invites == 1) {
                return {c, idx};
            }
        }
        return {{}, 0};
    }

    Novite SelectInviteAddressFromNewPool(const referral::ReferralsViewCache& db, uint64_t idx)
    {
        return FindNextNovite(db, idx);
    }

    referral::MaybeConfirmedAddress SelectInviteAddressFromCgsPool(
            const referral::ReferralsViewCache& db,
            AddressSelector& cgs_selector,
            uint256 hash)
    {
        const auto sampled = cgs_selector.SelectOld(db, hash, 1);
        if(sampled.empty()) {
            return {};
        }
        assert(sampled.size() == 1);

        const auto& entrant = sampled[0];

        return db.GetConfirmation(entrant.address_type, entrant.address);
    }

    referral::MaybeConfirmedAddress SelectInviteAddressFromAnyPool(
            const referral::ReferralsViewCache& db,
            uint64_t total_beacons,
            uint256 hash)
    {
        const auto selected_idx = SipHashUint256(0, 0, hash) % total_beacons;
        return db.GetConfirmation(selected_idx);
    }


    referral::ConfirmedAddresses SelectInviteAddresses(
            referral::NoviteRange& novite_range,
            AddressSelector& cgs_selector,
            int height,
            const referral::ReferralsViewCache& db,
            uint256 hash,
            const uint160& genesis_address,
            size_t n,
            const std::set<referral::Address> &unconfirmed_invites,
            int max_outstanding_invites)
    {
        assert(n > 0);
        assert(max_outstanding_invites > 0);

        auto requested = n;

        const auto total_beacons = db.GetTotalConfirmations();
        auto max_tries = std::min(std::max(static_cast<uint64_t>(n), total_beacons / 10), total_beacons);

        referral::ConfirmedAddresses addresses;
        uint64_t novite_idx = db.GetMaxNoviteIdx();
        novite_range.first = novite_idx;

        LogPrint(BCLog::POG, "%s: Selecting %d: Max: %d Out of: %d noviteidx: %d\n", __func__, n, max_tries, total_beacons, novite_idx);

        while(n-- && max_tries--) {
            const auto selected_idx = SipHashUint256(0, 0, hash) % total_beacons;
            const double rand_val = static_cast<double>(selected_idx) / static_cast<double>(total_beacons);

            CHashWriter hasher_a{SER_DISK, CLIENT_VERSION};
            hasher_a << hash << hash;
            hash = hasher_a.GetHash();

            const auto& selected_pool = INVITE_POOLS[SipHashUint256(0, 0, hash) % INVITE_POOLS.size()];

            LogPrint(BCLog::POG, "%s: \tsampling pool: %d randval: %d poolprob: %d n: %d maxtries: %d\n",
                    __func__,
                    static_cast<int>(selected_pool.type),
                    rand_val,
                    selected_pool.probability,
                    n,
                    max_tries);

            if(rand_val < selected_pool.probability) {
                referral::MaybeConfirmedAddress maybe_address;
                switch(selected_pool.type) {
                    case PoolType::CGS: 
                        maybe_address = SelectInviteAddressFromCgsPool(
                                db,
                                cgs_selector,
                                hash);
                        break;
                    case PoolType::NEW: 
                        {
                            auto novite = SelectInviteAddressFromNewPool(db, novite_idx);
                            novite_idx = std::max(novite_idx, novite.second);
                            maybe_address = novite.first;
                        }
                        break;
                    case PoolType::ANY: 
                        maybe_address = SelectInviteAddressFromAnyPool(
                                db,
                                total_beacons,
                                hash);
                        break;
                }

                if(maybe_address) {
                    LogPrint(BCLog::POG, "%s: \t%d %s invites: %d\n",
                            __func__,
                            static_cast<int>(selected_pool.type),
                            CMeritAddress{maybe_address->address_type, maybe_address->address}.ToString(),
                            maybe_address->invites);

                }

                if (!maybe_address) {
                    LogPrint(BCLog::POG, "%s: \tskipping no address: %d\n",
                            __func__,
                            static_cast<int>(selected_pool.type));
                    n++;
                } else if (!IsValidAmbassadorDestination(maybe_address->address_type)) {
                    LogPrint(BCLog::POG, "%s: \tskipping invalid address: %d %s invites: %d\n",
                            __func__,
                            static_cast<int>(selected_pool.type),
                            CMeritAddress{maybe_address->address_type, maybe_address->address}.ToString(),
                            maybe_address->invites);
                    n++;
                } else if (maybe_address->invites > max_outstanding_invites) {
                    LogPrint(BCLog::POG, "%s: \tskipping max invites: %d %s invites: %d\n",
                            __func__,
                            static_cast<int>(selected_pool.type),
                            CMeritAddress{maybe_address->address_type, maybe_address->address}.ToString(),
                            maybe_address->invites);
                    n++;
                } else if (maybe_address->address == genesis_address) {
                    LogPrint(BCLog::POG, "%s: \tskipping genesis: %d %s invites: %d\n",
                            __func__,
                            static_cast<int>(selected_pool.type),
                            CMeritAddress{maybe_address->address_type, maybe_address->address}.ToString(),
                            maybe_address->invites);
                    n++;
                } else if ( unconfirmed_invites.count(maybe_address->address)) {
                    LogPrint(BCLog::POG, "%s: \tskipping unconfirmed: %d %s invites: %d\n",
                            __func__,
                            static_cast<int>(selected_pool.type),
                            CMeritAddress{maybe_address->address_type, maybe_address->address}.ToString(),
                            maybe_address->invites);
                    n++;
                } else {
                    addresses.push_back(*maybe_address);
                }
            } else {
                n++;
                max_tries++;
            }

            CHashWriter hasher_b{SER_DISK, CLIENT_VERSION};
            hasher_b << hash << hash;
            hash = hasher_b.GetHash();
        }

        LogPrint(BCLog::POG, "%s: Selected %d: noviteidx: %d\n", __func__, addresses.size(), novite_idx);
        novite_range.second = novite_idx;

        if(requested > 0) {
            LogPrintf("Selected %d addresses (requested %d) for the invite lottery from a pool of %d\n", addresses.size(), requested, total_beacons);
        }

        assert(addresses.size() <= requested);
        return addresses;
    }

} // namespace pog2