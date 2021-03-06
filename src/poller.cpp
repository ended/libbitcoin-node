/*
 * Copyright (c) 2011-2014 libbitcoin developers (see AUTHORS)
 *
 * This file is part of libbitcoin-node.
 *
 * libbitcoin-node is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Affero General Public License with
 * additional permissions to the one published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version. For more information see LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <bitcoin/node/poller.hpp>

#include <bitcoin/blockchain.hpp>

namespace libbitcoin {
namespace node {

using std::placeholders::_1;
using std::placeholders::_2;
using boost::asio::io_service;

poller::poller(threadpool& pool, chain::blockchain& chain)
  : strand_(pool), chain_(chain)
{
}

void poller::query(network::channel_ptr node)
{
    fetch_block_locator(chain_,
        std::bind(&poller::initial_ask_blocks,
            this, _1, _2, node));
}

void poller::monitor(network::channel_ptr node)
{
    node->subscribe_inventory(
        strand_.wrap(&poller::receive_inv, this, _1, _2, node));
    node->subscribe_block(
        std::bind(&poller::receive_block,
            this, _1, _2, node));
}

void poller::initial_ask_blocks(const std::error_code& ec,
    const block_locator_type& locator, network::channel_ptr node)
{
    if (ec)
    {
        log_error(LOG_POLLER)
            << "Fetching initial block locator: " << ec.message();
        return;
    }
    strand_.randomly_queue(
        &poller::ask_blocks, this, ec, locator, null_hash, node);
}

void handle_send_packet(const std::error_code& ec)
{
    if (ec)
        log_error(LOG_POLLER) << "Send problem: " << ec.message();
}

void poller::receive_inv(const std::error_code& ec,
    const inventory_type& packet, network::channel_ptr node)
{
    if (ec)
    {
        log_warning(LOG_POLLER) << "Received bad inventory: " << ec.message();
        return;
    }
    // Filter out only block inventories
    get_data_type getdata;
    for (const inventory_vector_type& ivv: packet.inventories)
    {
        if (ivv.type != inventory_type_id::block)
            continue;
        // Already got this block
        if (ivv.hash == last_block_hash_)
            continue;
        getdata.inventories.push_back(ivv);
    }
    if (!getdata.inventories.empty())
    {
        last_block_hash_ = getdata.inventories.back().hash;
        node->send(getdata, handle_send_packet);
    }
    node->subscribe_inventory(
        strand_.wrap(&poller::receive_inv, this, _1, _2, node));
}

void poller::receive_block(const std::error_code& ec,
    const block_type& blk, network::channel_ptr node)
{
    if (ec)
    {
        log_warning(LOG_POLLER) << "Received bad block: " << ec.message();
        return;
    }
    chain_.store(blk,
        strand_.wrap(&poller::handle_store,
            this, _1, _2, hash_block_header(blk.header), node));
    node->subscribe_block(
        std::bind(&poller::receive_block,
            this, _1, _2, node));
}

void poller::handle_store(const std::error_code& ec, block_info info,
    const hash_digest& block_hash, network::channel_ptr node)
{
    // We need orphan blocks so we can do the next getblocks round
    if (ec && info.status != block_status::orphan)
    {
        log_warning(LOG_POLLER)
            << "Storing block " << encode_hash(block_hash)
            << ": " << ec.message();
        return;
    }
    switch (info.status)
    {
        case block_status::orphan:
            log_warning(LOG_POLLER) << "Orphan block "
                << encode_hash(block_hash);
            // TODO: Make more efficient by storing block hash
            // and next time do not download orphan block again.
            // Remember to remove from list once block is no longer orphan
            fetch_block_locator(chain_, strand_.wrap(
                &poller::ask_blocks, this, _1, _2, block_hash, node));
            break;

        case block_status::rejected:
            log_warning(LOG_POLLER) << "Rejected block "
                << encode_hash(block_hash);
            break;

        case block_status::confirmed:
            log_info(LOG_POLLER)
                << "Block #" << info.height << " " << encode_hash(block_hash);
            break;
    }
}

void poller::ask_blocks(const std::error_code& ec,
    const block_locator_type& locator,
    const hash_digest& hash_stop, network::channel_ptr node)
{
    if (ec)
    {
        log_error(LOG_POLLER) << "Ask for blocks: " << ec.message();
        return;
    }
    if (last_locator_begin_ == locator.front() &&
        last_hash_stop_ == hash_stop && last_requested_node_ == node.get())
    {
        log_debug(LOG_POLLER) << "Skipping duplicate ask blocks: "
            << encode_hash(locator.front());
        return;
    }
    // Send get_blocks request.
    get_blocks_type packet;
    packet.start_hashes = locator;
    packet.hash_stop = hash_stop;
    node->send(packet, std::bind(&handle_send_packet, _1));
    // Update last values.
    last_locator_begin_ = locator.front();
    last_hash_stop_ = hash_stop;
    last_requested_node_ = node.get();
}

} // namespace node
} // namespace libbitcoin

