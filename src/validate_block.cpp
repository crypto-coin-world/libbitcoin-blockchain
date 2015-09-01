/**
 * Copyright (c) 2011-2015 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin.
 *
 * libbitcoin is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/blockchain/validate_block.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <vector>
#include <boost/date_time.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/block.hpp>
#include <bitcoin/blockchain/checkpoint.hpp>
#include <bitcoin/blockchain/validate_transaction.hpp>

#ifdef WITH_CONSENSUS
#include <bitcoin/consensus.hpp>
#endif

namespace libbitcoin {
namespace blockchain {

using std::placeholders::_1;
using boost::posix_time::ptime;
using boost::posix_time::from_time_t;
using boost::posix_time::second_clock;
using boost::posix_time::hours;

// Max block size is 1,000,000.
constexpr uint32_t max_block_size = 1000000;

// Maximum signature operations per block is 20,000.
constexpr uint32_t max_block_script_sig_operations = max_block_size / 50;

// The maximum height of version 1 blocks.
// Is this correct, looks like it should be 227835?
// see: github.com/bitcoin/bips/blob/master/bip-0034.mediawiki#result
constexpr uint64_t max_version1_height = 237370;

// BIP30 exception blocks.
// see: github.com/bitcoin/bips/blob/master/bip-0030.mediawiki#specification
constexpr uint64_t bip30_exception_block1 = 91842;
constexpr uint64_t bip30_exception_block2 = 91880;

// Target readjustment every 2 weeks (in seconds).
constexpr uint64_t target_timespan = 2 * 7 * 24 * 60 * 60;

// Aim for blocks every 10 mins (in seconds).
constexpr uint64_t target_spacing = 10 * 60;

// Two weeks worth of blocks (count of blocks).
constexpr uint64_t readjustment_interval = target_timespan / target_spacing;

// To improve readability.
#define RETURN_IF_STOPPED(callback) \
if (stopped()) \
{ \
    callback(error::service_stopped); \
    return; \
}

// The nullptr option is for backward compatibility only.
validate_block::validate_block(threadpool& pool, size_t height, 
    const chain::block& block, const config::checkpoint::list& checks,
    stopped_callback callback)
  : height_(height),
    current_block_(block),
    checkpoints_(checks),
    stop_callback_(callback == nullptr ? [](){ return false; } : callback),
    dispatch_(pool)
{
}

bool validate_block::stopped() const
{
    return stop_callback_();
}

void validate_block::check_block(handler complete)
{
    // These are checks that are independent of the blockchain
    // that can be validated before saving an orphan block.

    const auto& transactions = current_block_.transactions;
    if (transactions.empty() || transactions.size() > max_block_size ||
        current_block_.serialized_size() > max_block_size)
    {
        complete(error::size_limits);
        return;
    }

    const auto& header = current_block_.header;
    const auto hash = header.hash();
    if (!is_valid_proof_of_work(hash, header.bits))
    {
        complete(error::proof_of_work);
        return;
    }

    RETURN_IF_STOPPED(complete);
    if (!is_valid_time_stamp(header.timestamp))
    {
        complete(error::futuristic_timestamp);
        return;
    }

    RETURN_IF_STOPPED(complete);
    if (!transactions.front().is_coinbase())
    {
        complete(error::first_not_coinbase);
        return;
    }

    for (auto it = ++transactions.begin(); it != transactions.end(); ++it)
    {
        RETURN_IF_STOPPED(complete);
        if (it->is_coinbase())
        {
            complete(error::extra_coinbases);
            return;
        }
    }

    for (const auto& tx: transactions)
    {
        RETURN_IF_STOPPED(complete);
        const auto ec = validate_transaction::check_transaction(tx);
        if (ec)
        {
            complete(ec);
            return;
        }
    }

    RETURN_IF_STOPPED(complete);
    if (!is_distinct_tx_set(transactions))
    {
        complete(error::duplicate);
        return;
    }

    RETURN_IF_STOPPED(complete);
    const auto sigops = legacy_sigops_count(transactions);
    if (sigops > max_block_script_sig_operations)
    {
        complete(error::too_many_sigs);
        return;
    }

    RETURN_IF_STOPPED(complete);
    if (header.merkle != chain::block::generate_merkle_root(transactions))
    {
        complete(error::merkle_mismatch);
        return;
    }

    complete(error::success);
}

bool validate_block::is_distinct_tx_set(const chain::transaction::list& txs)
{
    // We test distinctness by transaction hash.
    const auto hasher = [](const chain::transaction& transaction)
    {
        return transaction.hash();
    };

    std::vector<hash_digest> hashes(txs.size());
    std::transform(txs.begin(), txs.end(), hashes.begin(), hasher);
    auto distinct_end = std::unique(hashes.begin(), hashes.end());
    return distinct_end == hashes.end();
}

ptime validate_block::current_time() const
{
    return second_clock::universal_time();
}

bool validate_block::is_valid_time_stamp(uint32_t timestamp) const
{
    const auto two_hour_future = current_time() + hours(2);
    const auto block_time = from_time_t(timestamp);
    return block_time <= two_hour_future;
}

bool validate_block::is_valid_proof_of_work(hash_digest hash, uint32_t bits)
{
    hash_number target;
    if (!target.set_compact(bits))
        return false;

    if (target <= 0 || target > max_target())
        return false;

    hash_number our_value;
    our_value.set_hash(hash);
    return (our_value <= target);
}

inline bool within_op_n(chain::opcode code)
{
    const auto raw_code = static_cast<uint8_t>(code);
    constexpr auto op_1 = static_cast<uint8_t>(chain::opcode::op_1);
    constexpr auto op_16 = static_cast<uint8_t>(chain::opcode::op_16);
    return op_1 <= raw_code && raw_code <= op_16;
}

inline uint8_t decode_op_n(chain::opcode code)
{
    const auto raw_code = static_cast<uint8_t>(code);
    BITCOIN_ASSERT(within_op_n(code));

    // Add 1 because we minus opcode::op_1, not the value before.
    constexpr auto op_1 = static_cast<uint8_t>(chain::opcode::op_1);
    return raw_code - op_1 + 1;
}

inline size_t count_script_sigops(const chain::operation::stack& operations,
    bool accurate)
{
    size_t total_sigs = 0;
    chain::opcode last_opcode = chain::opcode::bad_operation;
    for (const auto& op: operations)
    {
        if (op.code == chain::opcode::checksig ||
            op.code == chain::opcode::checksigverify)
        {
            total_sigs++;
        }
        else if (op.code == chain::opcode::checkmultisig ||
            op.code == chain::opcode::checkmultisigverify)
        {
            if (accurate && within_op_n(last_opcode))
                total_sigs += decode_op_n(last_opcode);
            else
                total_sigs += 20;
        }

        last_opcode = op.code;
    }

    return total_sigs;
}

size_t validate_block::legacy_sigops_count(const chain::transaction& tx)
{
    size_t total_sigs = 0;
    for (const auto& input: tx.inputs)
    {
        const auto& operations = input.script.operations;
        total_sigs += count_script_sigops(operations, false);
    }

    for (const auto& output: tx.outputs)
    {
        const auto& operations = output.script.operations;
        total_sigs += count_script_sigops(operations, false);
    }

    return total_sigs;
}

size_t validate_block::legacy_sigops_count(const chain::transaction::list& txs)
{
    size_t total_sigs = 0;
    for (const auto& tx: txs)
        total_sigs += legacy_sigops_count(tx);

    return total_sigs;
}

void validate_block::accept_block(handler complete) 
{
    const auto& header = current_block_.header;
    if (header.bits != work_required())
    {
        complete(error::incorrect_proof_of_work);
        return;
    }

    RETURN_IF_STOPPED(complete);
    if (header.timestamp <= median_time_past())
    {
        complete(error::timestamp_too_early);
        return;
    }

    RETURN_IF_STOPPED(complete);
    for (const auto& tx: current_block_.transactions)
    {
        // Txs should be final when included in a block.
        if (!tx.is_final(height_, header.timestamp))
        {
            complete(error::non_final_transaction);
            return;
        }

        RETURN_IF_STOPPED(complete);
    }

    // Ensure that the block passes checkpoints.
    // This is both DOS protection and performance optimization for sync.
    const auto block_hash = header.hash();
    if (!checkpoint::validate(block_hash, height_, checkpoints_))
    {
        complete(error::checkpoints_failed);
        return;
    }

    RETURN_IF_STOPPED(complete);
    if (header.version < 2 && height_ > max_version1_height)
    {
        // Reject version=1 blocks after switchover point.
        complete(error::old_version_block);
        return;
    }

    RETURN_IF_STOPPED(complete);
    if (header.version >= 2 && 
        !is_valid_coinbase_height(height_, current_block_))
    {
        // Enforce version=2 rule that coinbase starts with serialized height.
        complete(error::coinbase_height_mismatch);
        return;
    }

    complete(error::success);
}

uint32_t validate_block::work_required() const
{
#ifdef ENABLE_TESTNET
    const auto last_non_special_bits = [this]()
    {
        // Return the last non-special block
        chain::header previous_block;
        auto previous_height = height_;

        // Loop backwards until we find a difficulty change point,
        // or we find a block which does not have max_bits (is not special).
        while (true)
        {
            if (previous_height % readjustment_interval == 0)
               break;

            --previous_height;
            previous_block = fetch_block(previous_height);
            if (previous_block.bits != max_work_bits)
               break;
        }

        return previous_block.bits;
    };
#endif

    if (height_ == 0)
        return max_work_bits;

    if (height_ % readjustment_interval != 0)
    {
#ifdef ENABLE_TESTNET
        uint32_t max_time_gap =
            fetch_block(height_ - 1).timestamp + 2 * target_spacing;
        if (current_block_.header.timestamp > max_time_gap)
            return max_work_bits;

        return last_non_special_bits();
#else
        return previous_block_bits();
#endif
    }

    // This is the total time it took for the last 2016 blocks.
    const auto actual = actual_timespan(readjustment_interval);

    // Now constrain the time between an upper and lower bound.
    const auto constrained = range_constrain(actual,
        target_timespan / 4, target_timespan * 4);

    hash_number retarget;
    retarget.set_compact(previous_block_bits());
    retarget *= constrained;
    retarget /= target_timespan;
    if (retarget > max_target())
        retarget = max_target();

    return retarget.compact();
}

bool validate_block::is_valid_coinbase_height(size_t height, 
    const chain::block& block)
{
    // There are old blocks with version incorrectly set to 2. Ignore them.
    if (height < max_version1_height)
        return true;

    // Checks whether the block height is in the coinbase tx input script.
    // Version 2 blocks and onwards.
    if (block.header.version < 2 || block.transactions.empty() || 
        block.transactions.front().inputs.empty())
        return false;

    // First get the serialized coinbase input script as a series of bytes.
    const auto& coinbase_tx = block.transactions.front();
    const auto& coinbase_script = coinbase_tx.inputs.front().script;
    const auto raw_coinbase = coinbase_script.to_data(false);

    // Try to recreate the expected bytes.
    chain::script expect_coinbase;
    script_number expect_number(height);
    expect_coinbase.operations.push_back(
        { chain::opcode::special, expect_number.data() });

    // Save the expected coinbase script.
    const auto expect = expect_coinbase.to_data(false);

    // Perform comparison of the first bytes with raw_coinbase.
    if (expect.size() > raw_coinbase.size())
        return false;

    return std::equal(expect.begin(), expect.end(), raw_coinbase.begin());
}

void validate_block::connect_block(handler complete)
{
    RETURN_IF_STOPPED(complete);

    auto next_step = dispatch_.ordered_delegate(
        &validate_block::connect_block1, this, _1, complete);

    const auto skip_bip30_rule =
        height_ == bip30_exception_block1 ||
        height_ == bip30_exception_block2;

    if (skip_bip30_rule)
    {
        next_step(error::success, complete);
        return;
    }

    dispatch_.parallel(current_block_.transactions, "spent_tx", next_step,
        &validate_block::check_spent_duplicate, this);
}

void validate_block::connect_block1(const code& ec, handler complete)
{
    RETURN_IF_STOPPED(complete);
    complete(error::unknown);
}

// BIP 30:
// Blocks are not allowed to contain a transaction whose identifier matches
// that of an earlier, not-fully-spent transaction in the same chain.
void validate_block::check_spent_duplicate(const chain::transaction& tx,
    handler complete)
{
    RETURN_IF_STOPPED(complete);

    const auto hash = tx.hash();
    if (!transaction_exists(hash))
    {
        // There is no matching previous tx.
        complete(error::success);
        return;
    }

    // Convert collection to outpoints for parallel iteration.
    size_t index = 0;
    std::vector<chain::output_point> points;
    for (const auto& output: tx.outputs)
        points.push_back({ hash, index++ });

    dispatch_.parallel(points, "spent_output", complete,
        &validate_block::check_output_spent, this);
}

void validate_block::check_output_spent(const chain::output_point& point,
    handler complete)
{
    RETURN_IF_STOPPED(complete);

    // If not all if its outputs are spent then a duplicate tx is not allowed.
    const auto spent = is_output_spent(point);
    complete(spent ? error::success : error::duplicate_or_spent);
}

////void validate_block::check_sigops_limit(const code& ec, handler complete) const
////{
////    RETURN_IF_STOPPED(complete);
////
////    if (ec)
////    {
////        complete(ec);
////        return;
////    }
////
////    size_t tx_index = 0;
////    size_t total_sigops = 0;
////    const auto& transactions = current_block_.transactions;
////    const auto count = transactions.size();
////
////    ////////////// TODO: parallelize. //////////////
////    for (const auto& tx: transactions)
////    {
////        ++tx_index;
////        uint64_t value_in = 0;
////        total_sigops += legacy_sigops_count(tx);
////        if (total_sigops > max_block_script_sig_operations)
////        {
////            // It appears that this is also checked in check_block().
////            complete(error::too_many_sigs);
////            return;
////        }
////    }
////    ////////////////////////////////////////////////
////
////    uint64_t fees = 0;
////    // TODO: check fees
////
////    RETURN_IF_STOPPED(complete);
////    const auto& coinbase = transactions.front();
////    const auto coinbase_value = coinbase.total_output_value();
////    if (coinbase_value > block_value(height_) + fees)
////    {
////        complete(error::validate_inputs_failed);
////        return;
////    }
////
////    complete(error::success);
////}
////
////void validate_block::check_fee_tally(const code& ec, handler complete) const
////{
////    for (const auto& tx: transactions)
////    {
////        RETURN_IF_STOPPED(complete);
////        if (!validate_transaction::tally_fees(tx, value_in, fees))
////        {
////            complete(error::validate_inputs_failed);
////            return;
////        }
////    }
////}
////
////void validate_block::check_signed_inputs(const code& ec, handler complete) const
////{
////    for (const auto& tx: transactions)
////    {
////        ++tx_index;
////
////        RETURN_IF_STOPPED(complete);
////        if (!validate_inputs(tx, tx_index - 1, value_in, total_sigops))
////        {
////            // libbitcoin-consensus checks here.
////            complete(error::validate_inputs_failed);
////            return;
////        }
////    }
////}

bool validate_block::validate_inputs(const chain::transaction& tx,
    size_t index_in_parent, uint64_t& value_in, size_t& total_sigops) const
{
    BITCOIN_ASSERT(!tx.is_coinbase());

    ////////////// TODO: parallelize. //////////////
    for (size_t input_index = 0; input_index < tx.inputs.size(); ++input_index)
    {
        if (!connect_input(index_in_parent, tx, input_index, value_in,
            total_sigops))
        {
            log_warning(LOG_VALIDATE) << "Invalid input ["
                << encode_hash(tx.hash()) << ":"
                << input_index << "]";
            return false;
        }
    }
    ////////////////////////////////////////////////

    return true;
}

size_t script_hash_signature_operations_count(
    const chain::script& output_script, const chain::script& input_script)
{
    if (output_script.type() != chain::payment_type::script_hash)
        return 0;

    if (input_script.operations.empty())
        return 0;

    const auto& last_data = input_script.operations.back().data;

    // ignore possible failure?
    chain::script eval_script;

    if (!eval_script.from_data(last_data, false,
        chain::script::parse_mode::strict))
        throw end_of_stream();

    return count_script_sigops(eval_script.operations, true);
}

bool validate_block::connect_input(size_t index_in_parent,
    const chain::transaction& current_tx, size_t input_index,
    uint64_t& value_in, size_t& total_sigops) const
{
    BITCOIN_ASSERT(input_index < current_tx.inputs.size());

    // Lookup previous output
    size_t previous_height;
    chain::transaction previous_tx;
    const auto& input = current_tx.inputs[input_index];
    const auto& previous_output = input.previous_output;

    if (!fetch_transaction(previous_tx, previous_height, previous_output.hash))
    {
        log_warning(LOG_VALIDATE) << "Failure fetching input transaction.";
        return false;
    }

    const auto& previous_tx_out = previous_tx.outputs[previous_output.index];

    // Signature operations count if script_hash payment type.
    try
    {
        total_sigops += script_hash_signature_operations_count(
            previous_tx_out.script, input.script);
    }
    catch (end_of_stream)
    {
        log_warning(LOG_VALIDATE) << "Invalid eval script.";
        return false;
    }

    if (total_sigops > max_block_script_sig_operations)
    {
        log_warning(LOG_VALIDATE) << "Total sigops exceeds block maximum.";
        return false;
    }

    // Get output amount
    const auto output_value = previous_tx_out.value;
    if (output_value > max_money())
    {
        log_warning(LOG_VALIDATE) << "Output money exceeds 21 million.";
        return false;
    }

    // Check coinbase maturity has been reached
    if (previous_tx.is_coinbase())
    {
        BITCOIN_ASSERT(previous_height <= height_);
        const auto height_difference = height_ - previous_height;
        if (height_difference < coinbase_maturity)
        {
            log_warning(LOG_VALIDATE) << "Immature coinbase spend attempt.";
            return false;
        }
    }

    if (!validate_transaction::validate_consensus(previous_tx_out.script,
        current_tx, input_index, current_block_.header, height_))
    {
        log_warning(LOG_VALIDATE) << "Input script invalid consensus.";
        return false;
    }

    // Search for double spends.
    if (is_output_spent(previous_output, index_in_parent, input_index))
    {
        log_warning(LOG_VALIDATE) << "Double spend attempt.";
        return false;
    }

    // Increase value_in by this output's value
    value_in += output_value;
    if (value_in > max_money())
    {
        log_warning(LOG_VALIDATE) << "Input money exceeds 21 million.";
        return false;
    }

    return true;
}

#undef RETURN_IF_STOPPED

} // namespace blockchain
} // namespace libbitcoin
