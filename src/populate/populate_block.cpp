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
#include <bitcoin/blockchain/populate/populate_block.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/interface/fast_chain.hpp>
#include <bitcoin/blockchain/pools/header_branch.hpp>

namespace libbitcoin {
namespace blockchain {

using namespace bc::chain;
using namespace bc::machine;

#define NAME "populate_block"

populate_block::populate_block(dispatcher& dispatch, const fast_chain& chain)
  : populate_base(dispatch, chain)
{
}

// Fork height is the last confirmed block considered, or the block that this
// block's indexed header branch connects to. Confirmed transactions in blocks
// above the fork point are considered pool blocks for validation purposes.
void populate_block::populate(block_const_ptr block, size_t fork_height,
    result_handler&& handler) const
{
    // The block class has no population method, so set timer externally.
    block->validation.start_populate = asio::steady_clock::now();

    // TODO: utilize last-validated-block cache so this can be promoted.
    const auto state = fast_chain_.chain_state(block);
    block->header().validation.state = state;

    if (!state)
    {
        handler(error::operation_failed);
        return;
    }

    // Return if this block is under a checkpoint, block state not requried.
    if (state->is_under_checkpoint())
    {
        handler(error::success);
        return;
    }

    // Handle the coinbase as a special case tx.
    populate_coinbase(block, fork_height);

    const auto non_coinbase_inputs = block->total_non_coinbase_inputs();

    // Return if there are no non-coinbase inputs to validate.
    if (non_coinbase_inputs == 0)
    {
        handler(error::success);
        return;
    }

    const auto buckets = std::min(dispatch_.size(), non_coinbase_inputs);
    const auto join_handler = synchronize(std::move(handler), buckets, NAME);
    BITCOIN_ASSERT(buckets != 0);

    for (size_t bucket = 0; bucket < buckets; ++bucket)
        dispatch_.concurrent(&populate_block::populate_transactions,
            this, block, fork_height, bucket, buckets, join_handler);
}

// Initialize the coinbase input for subsequent validation.
void populate_block::populate_coinbase(block_const_ptr block,
    size_t fork_height) const
{
    const auto& txs = block->transactions();
    BITCOIN_ASSERT(!txs.empty());

    const auto& coinbase = txs.front();
    BITCOIN_ASSERT(coinbase.is_coinbase());

    // A coinbase tx guarantees exactly one input.
    auto& prevout = coinbase.inputs().front().previous_output().validation;

    // A coinbase input cannot be a double spend since it originates coin.
    prevout.spent = false;

    // A coinbase is only valid within a block and input is confirmed if valid.
    prevout.confirmed = true;

    // A coinbase input has no previous output.
    prevout.cache = chain::output{};

    // A coinbase input does not spend an output so is itself always mature.
    prevout.coinbase_height = output_point::validation::unspecified;

    const auto forks = block->header().validation.state->enabled_forks();
    fast_chain_.populate_transaction(coinbase, forks, fork_height);
}

void populate_block::populate_transactions(block_const_ptr block,
    size_t fork_height, size_t bucket, size_t buckets,
    result_handler handler) const
{
    BITCOIN_ASSERT(bucket < buckets);
    const auto& txs = block->transactions();
    const auto state = block->header().validation.state;
    size_t input_position = 0;

    // Must skip coinbase here as it is already accounted for.
    const auto first = bucket == 0 ? buckets : bucket;

    // If collisions allowed then don't need to test for collisions.
    // If stale then don't bother checking for the pool optimization.
    if (!fast_chain_.is_blocks_stale() || 
        !state->is_enabled(rule_fork::allow_collisions))
    {
        const auto forks = state->enabled_forks();

        for (auto position = first; position < txs.size();
            position = ceiling_add(position, buckets))
        {
            const auto& tx = txs[position];
            fast_chain_.populate_transaction(tx, forks, fork_height);
        }
    }

    // Must skip coinbase here as it is already accounted for.
    for (auto tx = txs.begin() + 1; tx != txs.end(); ++tx)
    {
        const auto& inputs = tx->inputs();

        for (size_t input_index = 0; input_index < inputs.size();
            ++input_index, ++input_position)
        {
            if (input_position % buckets != bucket)
                continue;

            const auto& input = inputs[input_index];
            const auto& prevout = input.previous_output();
            fast_chain_.populate_output(prevout, fork_height);
        }
    }

    handler(error::success);
}

} // namespace blockchain
} // namespace libbitcoin
