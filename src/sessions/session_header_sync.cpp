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
#include <bitcoin/node/sessions/session_header_sync.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <bitcoin/blockchain.hpp>
#include <bitcoin/network.hpp>
#include <bitcoin/node/define.hpp>
#include <bitcoin/node/protocols/protocol_header_sync.hpp>
#include <bitcoin/node/full_node.hpp>
#include <bitcoin/node/settings.hpp>
#include <bitcoin/node/utility/header_queue.hpp>

namespace libbitcoin {
namespace node {

#define CLASS session_header_sync

using namespace bc::blockchain;
using namespace bc::chain;
using namespace bc::config;
using namespace bc::network;
using namespace std::placeholders;

// The minimum rate back off factor, must be < 1.0.
static constexpr float back_off_factor = 0.75f;

// The starting minimum header download rate, exponentially backs off.
static constexpr uint32_t headers_per_second = 10000;

// Sort is required here but not in configuration settings.
session_header_sync::session_header_sync(full_node& network,
    header_queue& hashes, fast_chain& blockchain,
    const checkpoint::list& checkpoints)
  : session<network::session_outbound>(network, false),
    hashes_(hashes),
    minimum_rate_(headers_per_second),
    chain_(blockchain),
    checkpoints_(checkpoint::sort(checkpoints)),
    CONSTRUCT_TRACK(session_header_sync)
{
    static_assert(back_off_factor < 1.0, "invalid back-off factor");
}

// Start sequence.
// ----------------------------------------------------------------------------

void session_header_sync::start(result_handler handler)
{
    session::start(CONCURRENT2(handle_started, _1, handler));
}

void session_header_sync::handle_started(const code& ec,
    result_handler handler)
{
    if (ec)
    {
        handler(ec);
        return;
    }

    if (!initialize(handler))
        return;

    // This is the end of the start sequence.
    new_connection(create_connector(), handler);
}

// Header sync sequence.
// ----------------------------------------------------------------------------

void session_header_sync::new_connection(connector::ptr connect,
    result_handler handler)
{
    if (stopped())
    {
        LOG_DEBUG(LOG_NODE)
            << "Suspending header sync session.";
        return;
    }

    // HEADER SYNC CONNECT
    this->connect(connect, BIND4(handle_connect, _1, _2, connect, handler));
}

void session_header_sync::handle_connect(const code& ec, channel::ptr channel,
    connector::ptr connect, result_handler handler)
{
    if (ec)
    {
        LOG_DEBUG(LOG_NODE)
            << "Failure connecting header sync channel: " << ec.message();
        new_connection(connect, handler);
        return;
    }

    LOG_DEBUG(LOG_NODE)
        << "Connected to header sync channel [" << channel->authority() << "]";

    register_channel(channel,
        BIND4(handle_channel_start, _1, connect, channel, handler),
        BIND1(handle_channel_stop, _1));
}

void session_header_sync::attach_handshake_protocols(channel::ptr channel,
    result_handler handle_started)
{
    // Don't use configured services, relay or min version for header sync.
    const auto relay = false;
    const auto own_version = settings_.protocol_maximum;
    const auto own_services = message::version::service::none;
    const auto minimum_version = message::version::level::headers;
    const auto minimum_services = message::version::service::node_network;

    // The negotiated_version is initialized to the configured maximum.
    if (channel->negotiated_version() >= message::version::level::bip61)
        attach<protocol_version_70002>(channel, own_version, own_services,
            minimum_version, minimum_services, relay)->start(handle_started);
    else
        attach<protocol_version_31402>(channel, own_version, own_services,
            minimum_version, minimum_services)->start(handle_started);
}

void session_header_sync::handle_channel_start(const code& ec,
    connector::ptr connect, channel::ptr channel, result_handler handler)
{
    // Treat a start failure just like a completion failure.
    if (ec)
    {
        handle_complete(ec, connect, handler);
        return;
    }

    attach_protocols(channel, connect, handler);
}

void session_header_sync::attach_protocols(channel::ptr channel,
    connector::ptr connect, result_handler handler)
{
    BITCOIN_ASSERT(channel->negotiated_version() >=
        message::version::level::headers);

    if (channel->negotiated_version() >= message::version::level::bip31)
        attach<protocol_ping_60001>(channel)->start();
    else
        attach<protocol_ping_31402>(channel)->start();

    attach<protocol_address_31402>(channel)->start();
    attach<protocol_header_sync>(channel, hashes_, minimum_rate_, last_)
        ->start(BIND3(handle_complete, _1, connect, handler));
}

void session_header_sync::handle_complete(const code& ec,
    network::connector::ptr connect, result_handler handler)
{
    if (!ec)
    {
        // This is the end of the header sync sequence.
        handler(ec);
        return;
    }

    // Reduce the rate minimum so that we don't get hung up.
    minimum_rate_ = static_cast<uint32_t>(minimum_rate_ * back_off_factor);

    // There is no failure scenario, we ignore the result code here.
    new_connection(connect, handler);
}

void session_header_sync::handle_channel_stop(const code& ec)
{
    LOG_DEBUG(LOG_NODE)
        << "Header sync channel stopped: " << ec.message();
}

// Utility.
// ----------------------------------------------------------------------------

bool session_header_sync::initialize(result_handler handler)
{
    if (!hashes_.empty())
    {
        LOG_ERROR(LOG_NODE)
            << "Header hash list must not be initialized.";
        handler(error::operation_failed);
        return false;
    }

    checkpoint seed;
    const auto ec = get_range(seed, last_);

    if (ec)
    {
        LOG_ERROR(LOG_NODE)
            << "Error getting header sync range: " << ec.message();
        handler(ec);
        return false;
    }

    if (seed == last_)
    {
        handler(error::success);
        return false;
    }

    // The stop is either a block or a checkpoint, so it may be downloaded.
    const auto stop_height = last_.height();

    // The seed is a block that we already have, so it will not be downloaded.
    const auto first_height = seed.height() + 1;

    LOG_INFO(LOG_NODE)
        << "Getting headers " << first_height << "-" << stop_height << ".";

    hashes_.initialize(seed);
    return true;
}

// Get the block hashes that bracket the range to download.
code session_header_sync::get_range(checkpoint& out_seed, checkpoint& out_stop)
{
    size_t last_height;

    if (!chain_.get_last_height(last_height))
        return error::operation_failed;

    size_t last_gap;
    size_t first_gap;
    auto first_height = last_height;

    if (chain_.get_gap_range(first_gap, last_gap))
    {
        last_height = last_gap + 1;
        first_height = first_gap - 1;
    }

    header first_header;

    if (!chain_.get_header(first_header, first_height))
        return error::not_found;
    
    if (!checkpoints_.empty() && checkpoints_.back().height() > last_height)
    {
        out_stop = checkpoints_.back();
    }
    else if (first_height == last_height)
    {
        out_stop = std::move(checkpoint{ first_header.hash(), first_height });
    }
    else
    {
        header last_header;

        if (!chain_.get_header(last_header, last_height))
            return error::not_found;

        out_stop = std::move(checkpoint{ last_header.hash(), last_height });
    }

    out_seed = std::move(checkpoint{ first_header.hash(), first_height });
    return error::success;
}

} // namespace node
} // namespace libbitcoin
