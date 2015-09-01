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
#ifndef LIBBITCOIN_BLOCKCHAIN_VALIDATE_HPP
#define LIBBITCOIN_BLOCKCHAIN_VALIDATE_HPP

#include <cstddef>
#include <cstdint>
#include <system_error>
#include <boost/date_time.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/checkpoint.hpp>
#include <bitcoin/blockchain/define.hpp>

namespace libbitcoin {
namespace blockchain {

class BCB_API validate_block
{
public:
    typedef std::function<void(const code& ec)> handler;

    void check_block(handler complete);
    void accept_block(handler complete);
    void connect_block(handler complete);

protected:
    typedef std::function<bool()> stopped_callback;

    validate_block(threadpool& pool, size_t height,
        const chain::block& block, const config::checkpoint::list& checks,
        stopped_callback stop_callback=nullptr);

    virtual uint64_t actual_timespan(size_t interval) const = 0;
    virtual chain::header fetch_block(size_t fetch_height) const = 0;
    virtual bool fetch_transaction(chain::transaction& tx,
        size_t& previous_height, const hash_digest& tx_hash) const = 0;
    virtual bool is_output_spent(const chain::output_point& outpoint) const = 0;
    virtual bool is_output_spent(const chain::output_point& previous_output,
        size_t index_in_parent, size_t input_index) const = 0;
    virtual uint64_t median_time_past() const = 0;
    virtual uint32_t previous_block_bits() const = 0;
    virtual bool transaction_exists(const hash_digest& tx_hash) const = 0;

    // These have default implementations that can be overriden.
    virtual bool connect_input(size_t index_in_parent,
        const chain::transaction& current_tx, size_t input_index,
        uint64_t& value_in, size_t& total_sigops) const;
    virtual bool validate_inputs(const chain::transaction& tx,
        size_t index_in_parent, uint64_t& value_in,
        size_t& total_sigops) const;

    // These are protected virtual for testability.
    virtual boost::posix_time::ptime current_time() const;
    virtual bool stopped() const;

    // connect_block
    virtual void check_spent_duplicate(const chain::transaction& tx,
        handler complete);
    virtual void check_output_spent(const chain::output_point& point,
        handler complete);
    virtual void connect_block1(const code& ec, handler complete);

    virtual bool is_valid_time_stamp(uint32_t timestamp) const;
    virtual uint32_t work_required() const;

    static bool is_distinct_tx_set(const chain::transaction::list& txs);
    static bool is_valid_proof_of_work(hash_digest hash, uint32_t bits);
    static bool is_valid_coinbase_height(size_t height,
        const chain::block& block);
    static size_t legacy_sigops_count(const chain::transaction& tx);
    static size_t legacy_sigops_count(const chain::transaction::list& txs);

private:
    const size_t height_;
    const chain::block& current_block_;
    const config::checkpoint::list& checkpoints_;
    const stopped_callback stop_callback_;
    dispatcher dispatch_;
};

} // namespace blockchain
} // namespace libbitcoin

#endif
