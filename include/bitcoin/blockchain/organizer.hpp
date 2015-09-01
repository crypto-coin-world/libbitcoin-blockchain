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
#ifndef LIBBITCOIN_BLOCKCHAIN_ORGANIZER_HPP
#define LIBBITCOIN_BLOCKCHAIN_ORGANIZER_HPP

#include <memory>
#include <boost/circular_buffer.hpp>
#include <bitcoin/bitcoin.hpp>
#include <bitcoin/blockchain/define.hpp>
#include <bitcoin/blockchain/block_detail.hpp>
#include <bitcoin/blockchain/block_info.hpp>
#include <bitcoin/blockchain/blockchain.hpp>
#include <bitcoin/blockchain/orphans_pool.hpp>
#include <bitcoin/blockchain/simple_chain.hpp>

namespace libbitcoin {
namespace blockchain {

/**
 * Dependency graph:
 *                   ___________
 *                  |           |
 *             -----| organizer |----
 *            /     |___________|    \
 *           /                        \
 *  ________/_____                 ____\_________
 * |              |               |              |
 * | orphans_pool |               | simple_chain |
 * |______________|               |______________|
 *
 * And both implementations of the organizer and simple_chain
 * depend on blockchain_common.
 *   ________________          ________
 *  [_organizer_impl_]------->[        ]
 *   ___________________      [ common ]
 *  [_simple_chain_impl_]---->[________]
 *
 * All these components are managed and kept inside blockchain_impl.
 */

// Structure which organises the blocks from the orphan pool to the blockchain.
class BCB_API organizer
{
public:
    organizer(threadpool& pool, orphans_pool& orphans, simple_chain& chain);

    bool start();
    bool stop();
    void subscribe_reorganize(blockchain::reorganize_handler handle_reorganize);

    typedef subscriber<const code&, uint64_t, 
        const blockchain::block_list&, const blockchain::block_list&> 
        reorganize_subscriber;

protected:
    bool stopped();
    virtual code verify(size_t fork_index,
        const block_detail_list& orphan_chain, size_t orphan_index) = 0;

private:

    void process(block_detail_ptr process_block);
    void replace_chain(size_t fork_index, block_detail_list& orphan_chain);
    void clip_orphans(block_detail_list& orphan_chain, size_t orphan_index,
        const code& invalid_reason);
    void notify_reorganize(size_t fork_point,
        const block_detail_list& orphan_chain,
        const block_detail_list& replaced_chain);
    void notify_stop();

    orphans_pool& orphans_;
    simple_chain& chain_;
    reorganize_subscriber::ptr reorganize_subscriber_;
    block_detail_list process_queue_;
    bool stopped_;
};

// TODO: define in organizer (compat break).
typedef std::shared_ptr<organizer> organizer_ptr;

} // namespace blockchain
} // namespace libbitcoin

#endif
