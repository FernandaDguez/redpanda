#pragma once

#include "model/adl_serde.h"
#include "model/fundamental.h"
#include "model/metadata.h"
#include "model/record.h"
#include "model/record_batch_reader.h"
#include "model/timeout_clock.h"
#include "reflection/async_adl.h"
#include "utils/named_type.h"

#include <seastar/net/socket_defs.hh>
#include <seastar/util/bool_class.hh>

#include <boost/range/irange.hpp>
#include <boost/range/join.hpp>

#include <cstdint>
#include <exception>

namespace raft {
using clock_type = ss::lowres_clock;
using duration_type = typename clock_type::duration;
using timer_type = ss::timer<clock_type>;
static constexpr clock_type::time_point no_timeout
  = clock_type::time_point::max();

using group_id = named_type<int64_t, struct raft_group_id_type>;

static constexpr const model::record_batch_type configuration_batch_type{2};
static constexpr const model::record_batch_type data_batch_type{1};

struct protocol_metadata {
    group_id group;
    model::offset commit_index;
    model::term_id term;
    model::offset prev_log_index;
    model::term_id prev_log_term;
};

struct group_configuration {
    group_configuration() noexcept = default;
    ~group_configuration() noexcept = default;
    group_configuration(const group_configuration&) = default;
    group_configuration& operator=(const group_configuration&) = delete;
    group_configuration(group_configuration&&) noexcept = default;
    group_configuration& operator=(group_configuration&&) noexcept = default;

    using brokers_t = std::vector<model::broker>;
    using iterator = brokers_t::iterator;
    using const_iterator = brokers_t::const_iterator;

    bool has_voters() const { return !nodes.empty(); }
    bool has_learners() const { return !learners.empty(); }
    size_t majority() const { return (nodes.size() / 2) + 1; }
    iterator find_in_nodes(model::node_id id);
    const_iterator find_in_nodes(model::node_id id) const;
    iterator find_in_learners(model::node_id id);
    const_iterator find_in_learners(model::node_id id) const;
    bool contains_broker(model::node_id id) const;

    template<typename Func>
    void for_each(Func&& f) const {
        auto joined_range = boost::join(nodes, learners);
        std::for_each(
          std::cbegin(joined_range),
          std::cend(joined_range),
          std::forward<Func>(f));
    }

    // data
    brokers_t nodes;
    brokers_t learners;
};

// The sequence used to track the order of follower append entries request
using follower_req_seq = named_type<uint64_t, struct follower_req_seq_tag>;

struct follower_index_metadata {
    explicit follower_index_metadata(model::node_id node)
      : node_id(node) {}
    model::node_id node_id;
    // index of last known log for this follower
    model::offset last_committed_log_index;
    // index of last not flushed offset
    model::offset last_dirty_log_index;
    // index of log for which leader and follower logs matches
    model::offset match_index;
    // Used to establish index persistently replicated by majority
    constexpr model::offset match_committed_index() const {
        return std::min(last_committed_log_index, match_index);
    }
    // next index to send to this follower
    model::offset next_index;
    // timestamp of last append_entries_rpc call
    clock_type::time_point last_hbeat_timestamp;
    uint64_t failed_appends{0};
    // The pair of sequences used to track append entries requests sent and
    // received by the follower. Every time append entries request is created
    // the `last_sent_seq` is incremented before accessing raft protocol state
    // and dispatching an RPC and its value is passed to the response
    // processing continuation. When follower append entries replies are
    // received if the sequence bound with reply is greater than or equal to
    // `last_received_seq` the `last_received_seq` field is updated with
    // received sequence and reply is treated as valid. If received sequence is
    // smaller than `last_received_seq` requests were reordered.

    /// Using `follower_req_seq` argument to track the follower replies
    /// reordering
    ///
    ///                                                                    Time
    ///                                                        Follower     +
    ///                                                           +         |
    ///                      +--------------+                     |         |
    ///                      | Req [seq: 1] +-------------------->+         |
    ///                      +--------------+                     |         |
    ///                           +--------------+                |         |
    ///                           | Req [seq: 2] +--------------->+         |
    ///                           +--------------+                |         |
    ///                                +--------------+           |         |
    ///                                | Req [seq: 3] +---------->+         |
    ///                                +--------------+           |         |
    ///                                                           |         |
    ///                                                           |         |
    ///                                                           |         |
    ///                                        Reply [seq: 1]     |         |
    /// last_received_seq = 1;    <-------------------------------+         |
    ///                                                           |         |
    ///                                                           |         |
    ///                                        Reply [seq: 3]     |         |
    /// last_received_seq = 3;    <-------------------------------+         |
    ///                                                           |         |
    ///                                                           |         |
    ///                                        Reply [seq: 2]     |         |
    /// reordered 2 < last_rec    <-------------------------------+         |
    ///                                                           |         |
    ///                                                           |         |
    ///                                                           |         |
    ///                                                           |         |
    ///                                                           |         |
    ///                                                           +         |
    ///                                                                     v

    follower_req_seq last_sent_seq{0};
    follower_req_seq last_received_seq{0};
    bool is_learner = false;
    bool is_recovering = false;
};

struct append_entries_request {
    using flush_after_append = ss::bool_class<struct flush_after_append_tag>;

    append_entries_request(
      model::node_id i,
      protocol_metadata m,
      model::record_batch_reader r,
      flush_after_append f = flush_after_append::yes) noexcept
      : node_id(i)
      , meta(m)
      , batches(std::move(r))
      , flush(f){};
    ~append_entries_request() noexcept = default;
    append_entries_request(const append_entries_request&) = delete;
    append_entries_request& operator=(const append_entries_request&) = delete;
    append_entries_request(append_entries_request&&) noexcept = default;
    append_entries_request& operator=(append_entries_request&&) noexcept
      = default;

    raft::group_id target_group() const { return meta.group; }

    model::node_id node_id;
    protocol_metadata meta;
    model::record_batch_reader batches;
    flush_after_append flush;
    static append_entries_request make_foreign(append_entries_request&& req) {
        return append_entries_request(
          req.node_id,
          std::move(req.meta),
          model::make_foreign_record_batch_reader(std::move(req.batches)),
          req.flush);
    }
};

struct append_entries_reply {
    enum class status : uint8_t { success, failure, group_unavailable };
    /// \brief callee's node_id; work-around for batched heartbeats
    model::node_id node_id;
    group_id group;
    /// \brief callee's term, for the caller to upate itself
    model::term_id term;
    /// \brief The recipient's last log index after it applied changes to
    /// the log. This is used to speed up finding the correct value for the
    /// nextIndex with a follower that is far behind a leader
    model::offset last_committed_log_index;
    model::offset last_dirty_log_index;
    /// \brief did the rpc succeed or not
    status result = status::failure;
};

/// \brief this is our _biggest_ modification to how raft works
/// to accomodate for millions of raft groups in a cluster.
/// internally, the receiving side will simply iterate and dispatch one
/// at a time, as well as the receiving side will trigger the
/// individual raft responses one at a time - for example to start replaying the
/// log at some offset
struct heartbeat_request {
    model::node_id node_id;
    std::vector<protocol_metadata> meta;
};
struct heartbeat_reply {
    std::vector<append_entries_reply> meta;
};

struct vote_request {
    model::node_id node_id;
    group_id group;
    /// \brief current term
    model::term_id term;
    /// \brief used to compare completeness
    model::offset prev_log_index;
    model::term_id prev_log_term;
    raft::group_id target_group() const { return group; }
};

struct vote_reply {
    /// \brief callee's term, for the caller to upate itself
    model::term_id term;

    /// True if the follower granted the candidate it's vote, false otherwise
    bool granted = false;

    /// set to true if the caller's log is as up to date as the recipient's
    /// - extension on raft. see Diego's phd dissertation, section 9.6
    /// - "Preventing disruptions when a server rejoins the cluster"
    bool log_ok = false;
};

/// This structure is used by consensus to notify other systems about group
/// leadership changes.
struct leadership_status {
    // Current term
    model::term_id term;
    // Group for which leader have changed
    group_id group;
    // Empty when there is no known leader in the group
    std::optional<model::node_id> current_leader;
};

struct replicate_result {
    /// used by the kafka API to produce a kafka reply to produce request.
    /// see produce_request.cc
    model::offset last_offset;
};

enum class consistency_level { quorum_ack, leader_ack, no_ack };

struct replicate_options {
    explicit replicate_options(consistency_level l)
      : consistency(l) {}

    consistency_level consistency;
};

std::ostream& operator<<(std::ostream& o, const consistency_level& l);
std::ostream& operator<<(std::ostream& o, const protocol_metadata& m);
std::ostream& operator<<(std::ostream& o, const vote_reply& r);
std::ostream& operator<<(std::ostream& o, const append_entries_reply::status&);
std::ostream& operator<<(std::ostream& o, const append_entries_reply& r);
std::ostream& operator<<(std::ostream& o, const vote_request& r);
std::ostream& operator<<(std::ostream& o, const follower_index_metadata& i);
std::ostream& operator<<(std::ostream& o, const heartbeat_request& r);
std::ostream& operator<<(std::ostream& o, const heartbeat_reply& r);
std::ostream& operator<<(std::ostream& o, const group_configuration& c);
} // namespace raft

namespace reflection {
template<>
struct async_adl<raft::append_entries_request> {
    ss::future<> to(iobuf& out, raft::append_entries_request&& request);
    ss::future<raft::append_entries_request> from(iobuf_parser& in);
};
template<>
struct adl<raft::protocol_metadata> {
    void to(iobuf& out, raft::protocol_metadata request);
    raft::protocol_metadata from(iobuf_parser& in);
};
template<>
struct async_adl<raft::heartbeat_request> {
    ss::future<> to(iobuf& out, raft::heartbeat_request&& request);
    ss::future<raft::heartbeat_request> from(iobuf_parser& in);
};

template<>
struct async_adl<raft::heartbeat_reply> {
    ss::future<> to(iobuf& out, raft::heartbeat_reply&& request);
    ss::future<raft::heartbeat_reply> from(iobuf_parser& in);
};
} // namespace reflection
