/**
 * Copyright (c) 2011-2017 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/blockchain/populate/populate_chain_state.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/define.hpp>
#include <bitcoin/blockchain/interface/fast_chain.hpp>
#include <bitcoin/blockchain/pools/header_branch.hpp>
#include <bitcoin/blockchain/settings.hpp>

namespace libbitcoin {
namespace blockchain {

using namespace bc::chain;

// This value should never be read, but may be useful in debugging.
static constexpr uint32_t unspecified_timestamp = max_uint32;
static constexpr uint32_t hour_seconds = 3600u;

// Database access is limited to { top, hash, bits, version, timestamp }.

populate_chain_state::populate_chain_state(const fast_chain& chain,
    const settings& settings)
  : checkpoints_(config::checkpoint::sort(settings.checkpoints)),
    forks_(settings.enabled_forks()),
    stale_seconds_(settings.notify_limit_hours * hour_seconds),
    fast_chain_(chain)
{
}

bool populate_chain_state::get_bits(uint32_t& bits, size_t height,
    header_branch::const_ptr branch, bool block) const
{
    return branch->get_bits(bits, height) ||
        fast_chain_.get_bits(bits, height, block);
}

bool populate_chain_state::get_version(uint32_t& version, size_t height,
    header_branch::const_ptr branch, bool block) const
{
    return branch->get_version(version, height) ||
        fast_chain_.get_version(version, height, block);
}

bool populate_chain_state::get_timestamp(uint32_t& time, size_t height,
    header_branch::const_ptr branch, bool block) const
{
    return branch->get_timestamp(time, height) ||
        fast_chain_.get_timestamp(time, height, block);
}

bool populate_chain_state::get_block_hash(hash_digest& hash, size_t height,
    header_branch::const_ptr branch, bool block) const
{
    return branch->get_block_hash(hash, height) ||
        fast_chain_.get_block_hash(hash, height, block);
}

bool populate_chain_state::populate_bits(chain_state::data& data,
    const chain_state::map& map, header_branch::const_ptr branch,
    bool block) const
{
    auto& bits = data.bits.ordered;
    bits.resize(map.bits.count);
    auto height = map.bits.high - map.bits.count;

    for (auto& bit: bits)
        if (!get_bits(bit, ++height, branch, block))
            return false;

    return get_bits(data.bits.self, map.bits_self, branch, block);
}

bool populate_chain_state::populate_versions(chain_state::data& data,
    const chain_state::map& map, header_branch::const_ptr branch,
    bool block) const
{
    auto& versions = data.version.ordered;
    versions.resize(map.version.count);
    auto height = map.version.high - map.version.count;

    for (auto& version: versions)
        if (!get_version(version, ++height, branch, block))
            return false;

    return get_version(data.version.self, map.version_self, branch, block);
}

bool populate_chain_state::populate_timestamps(chain_state::data& data,
    const chain_state::map& map, header_branch::const_ptr branch,
    bool block) const
{
    data.timestamp.retarget = unspecified_timestamp;
    auto& timestamps = data.timestamp.ordered;
    timestamps.resize(map.timestamp.count);
    auto height = map.timestamp.high - map.timestamp.count;

    for (auto& timestamp: timestamps)
        if (!get_timestamp(timestamp, ++height, branch, block))
            return false;

    // Retarget is required if timestamp_retarget is not unrequested.
    if (map.timestamp_retarget != chain_state::map::unrequested &&
        !get_timestamp(data.timestamp.retarget, map.timestamp_retarget, branch,
            block))
    {
        return false;
    }

    return get_timestamp(data.timestamp.self, map.timestamp_self, branch,
        block);
}

bool populate_chain_state::populate_checkpoint(chain_state::data& data,
    const chain_state::map& map, header_branch::const_ptr branch,
    bool block) const
{
    if (map.allow_collisions_height == chain_state::map::unrequested)
    {
        data.allow_collisions_hash = null_hash;
        return true;
    }

    return get_block_hash(data.allow_collisions_hash,
        map.allow_collisions_height, branch, block);
}

bool populate_chain_state::populate_all(chain_state::data& data,
    header_branch::const_ptr branch, bool block) const
{
    // Construct a chain state map to inform chain state data population.
    // Map heights are relative to the branch of the block of interest.
    const auto map = chain_state::get_map(data.height, checkpoints_, forks_);

    return
        populate_bits(data, map, branch, block) &&
        populate_versions(data, map, branch, block) &&
        populate_timestamps(data, map, branch, block) &&
        populate_checkpoint(data, map, branch, block);
}

// Get chain state for top block|header.
chain_state::ptr populate_chain_state::populate(bool block_index) const
{
    size_t height;
    if (!fast_chain_.get_block_height(height, block_index))
        return{};

    hash_digest hash;
    if (!fast_chain_.get_block_hash(hash, height, block_index))
        return{};

    chain_state::data data;
    data.hash = hash;
    data.height = height;

    // There is no branch in the startup sceanrio.
    const auto branch = std::make_shared<const header_branch>();

    if (!populate_all(data, branch, block_index))
        return nullptr;

    return std::make_shared<chain_state>(std::move(data), checkpoints_, forks_,
        stale_seconds_);
}

// Get chain state for top block of the given header branch.
chain::chain_state::ptr populate_chain_state::populate(
    header_branch::const_ptr branch) const
{
    // An index chain state query must provide a non-empty branch.
    if (branch->empty())
        return{};

    const auto top_header = branch->top();
    const auto top_parent = branch->top_parent();

    // Promote from immediate parent state if avialable (most common and fast).
    if (top_parent && top_parent->validation.state)
    {
        const auto state = std::make_shared<chain::chain_state>(
            *top_parent->validation.state, *top_header);
        top_header->validation.state = state;
        return state;
    }

    chain_state::data data;
    data.hash = top_header->hash();
    data.height = branch->top_height();

    if (!populate_all(data, branch, false))
        return nullptr;

    return std::make_shared<chain_state>(std::move(data), checkpoints_, forks_,
        stale_seconds_);
}

} // namespace blockchain
} // namespace libbitcoin
