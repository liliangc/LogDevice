/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "GOSSIP_Message.h"

#include <memory>

#include <folly/small_vector.h>

#include "logdevice/common/Processor.h"
#include "logdevice/common/Worker.h"
#include "logdevice/common/debug.h"
#include "logdevice/common/protocol/ProtocolReader.h"
#include "logdevice/common/protocol/ProtocolWriter.h"
#include "logdevice/common/stats/ServerHistograms.h"
#include "logdevice/common/stats/Stats.h"

namespace facebook { namespace logdevice {

GOSSIP_Message::GOSSIP_Message(NodeID this_node,
                               gossip_list_t gossip_list,
                               std::chrono::milliseconds instance_id,
                               std::chrono::milliseconds sent_time,
                               gossip_ts_t gossip_ts_list,
                               failover_list_t failover_list,
                               suspect_matrix_t suspect_matrix,
                               boycott_list_t boycott_list,
                               boycott_durations_list_t boycott_durations,
                               starting_list_t starting_list,
                               GOSSIP_Message::GOSSIP_flags_t flags,
                               uint64_t msg_id)
    : Message(MessageType::GOSSIP, TrafficClass::FAILURE_DETECTOR),
      num_nodes_(gossip_list.size()),
      gossip_node_(this_node),
      flags_(flags),
      gossip_list_(std::move(gossip_list)),
      instance_id_(instance_id),
      sent_time_(sent_time),
      gossip_ts_(std::move(gossip_ts_list)),
      failover_list_(failover_list),
      suspect_matrix_(std::move(suspect_matrix)),
      num_boycotts_(boycott_list.size()),
      boycott_list_(std::move(boycott_list)),
      boycott_durations_list_(std::move(boycott_durations)),
      num_starting_(starting_list.size()),
      starting_list_(starting_list),
      msg_id_(msg_id) {
  ld_check(gossip_list_.size() <=
           std::numeric_limits<decltype(num_nodes_)>::max());
  ld_check(suspect_matrix_.size() == 0 ||
           gossip_list_.size() == suspect_matrix_.size());
  if (flags_ & HAS_FAILOVER_LIST_FLAG) {
    ld_check(failover_list_.size() == gossip_list_.size());
  }
}

Message::Disposition GOSSIP_Message::onReceived(const Address& /*from*/) {
  // Receipt handler lives in server/GOSSIP_onReceived.cpp; this should
  // never get called.
  std::abort();
}

void GOSSIP_Message::serialize(ProtocolWriter& writer) const {
  ld_check(gossip_list_.size() == num_nodes_);
  ld_check(gossip_ts_.size() == num_nodes_);

  auto flags = flags_;

  if (writer.proto() < Compatibility::ProtocolVersion::STARTING_STATE_SUPPORT) {
    /* remove starting list */
    flags &= ~HAS_STARTING_LIST_FLAG;
  }

  writer.write(num_nodes_);
  writer.write(gossip_node_);
  writer.write(flags);
  writer.writeVector(gossip_list_);
  writer.write(instance_id_);
  writer.write(sent_time_);
  writer.writeVector(gossip_ts_);

  if (flags & HAS_FAILOVER_LIST_FLAG) {
    ld_check(failover_list_.size() == num_nodes_);
    writer.writeVector(failover_list_);
  }

  writeBoycottList(writer);

  if (writer.proto() >=
      Compatibility::ProtocolVersion::ADAPTIVE_BOYCOTT_DURATION) {
    writeBoycottDurations(writer);
  }

  if (flags & HAS_STARTING_LIST_FLAG) {
    writeStartingList(writer);
  }

  writeSuspectMatrix(writer);
}

MessageReadResult GOSSIP_Message::deserialize(ProtocolReader& reader) {
  std::unique_ptr<GOSSIP_Message> msg(new GOSSIP_Message());

  reader.read(&msg->num_nodes_);
  reader.read(&msg->gossip_node_);
  reader.read(&msg->flags_);
  reader.readVector(&msg->gossip_list_, msg->num_nodes_);
  reader.read(&msg->instance_id_);
  reader.read(&msg->sent_time_);
  reader.readVector(&msg->gossip_ts_, msg->num_nodes_);

  if (reader.ok() && (msg->flags_ & HAS_FAILOVER_LIST_FLAG)) {
    reader.readVector(&msg->failover_list_, msg->num_nodes_);
  }

  msg->readBoycottList(reader);

  if (reader.proto() >=
      Compatibility::ProtocolVersion::ADAPTIVE_BOYCOTT_DURATION) {
    msg->readBoycottDurations(reader);
  }

  if (msg->flags_ & HAS_STARTING_LIST_FLAG) {
    msg->readStartingList(reader);
  }

  if (reader.ok() && reader.bytesRemaining() > 0) {
    msg->readSuspectMatrix(reader);
  }

  return reader.resultMsg(std::move(msg));
}

void GOSSIP_Message::writeSuspectMatrix(ProtocolWriter& writer) const {
  if (!suspect_matrix_.size()) {
    return;
  }

  // Since suspect matrix contains only {0, 1}, this method packs a single row
  // of the matrix into row_chunks 32-bit integers.
  // TODO (#4214621): size of the on-the-wire representation can be further
  //                  reduced using varint encoding.
  size_t row_chunks = (num_nodes_ + 31) / 32;
  folly::small_vector<uint32_t, 2> row;
  for (size_t i = 0; i < num_nodes_; ++i) {
    row.assign(row_chunks, 0);
    for (size_t j = 0; j < num_nodes_; ++j) {
      if (suspect_matrix_[i][j]) {
        row[j >> 5] |= (1UL << (j & 31));
      }
    }
    writer.writeVector(row);
  }
}

void GOSSIP_Message::readSuspectMatrix(ProtocolReader& reader) {
  size_t row_chunks = (num_nodes_ + 31) / 32;

  suspect_matrix_.resize(num_nodes_);

  folly::small_vector<uint32_t, 2> row(row_chunks);
  for (size_t i = 0; i < num_nodes_; ++i) {
    reader.readVector(&row);
    suspect_matrix_[i].resize(num_nodes_);
    for (size_t j = 0; j < num_nodes_; ++j) {
      suspect_matrix_[i][j] = ((row[j >> 5] >> (j & 31)) & 1);
    }
  }
}

void GOSSIP_Message::onSent(Status /*st*/, const Address& /*to*/) const {
  // Receipt handler lives in server/GOSSIP_onSent.cpp; this should
  // never get called.
  std::abort();
}

void GOSSIP_Message::writeBoycottList(ProtocolWriter& writer) const {
  ld_check(boycott_list_.size() == num_boycotts_);
  writer.write(num_boycotts_);

  for (auto& boycott : boycott_list_) {
    writer.write(boycott.node_index);
    writer.write(boycott.boycott_in_effect_time);
    if (writer.proto() >=
        Compatibility::ProtocolVersion::ADAPTIVE_BOYCOTT_DURATION) {
      writer.write(boycott.boycott_duration);
    }
    writer.write(boycott.reset);
  }
}

void GOSSIP_Message::readBoycottList(ProtocolReader& reader) {
  reader.read(&num_boycotts_);

  boycott_list_.resize(num_boycotts_);

  for (auto& boycott : boycott_list_) {
    reader.read(&boycott.node_index);
    reader.read(&boycott.boycott_in_effect_time);
    if (reader.proto() >=
        Compatibility::ProtocolVersion::ADAPTIVE_BOYCOTT_DURATION) {
      reader.read(&boycott.boycott_duration);
    } else {
      boycott.boycott_duration = getDefaultBoycottDuration();
    }
    reader.read(&boycott.reset);
  }
}

std::chrono::milliseconds GOSSIP_Message::getDefaultBoycottDuration() const {
  return Worker::settings().sequencer_boycotting.node_stats_boycott_duration;
}

// flattens the boycott durations and write them
void GOSSIP_Message::writeBoycottDurations(ProtocolWriter& writer) const {
  writer.write(boycott_durations_list_.size());
  for (const auto& d : boycott_durations_list_) {
    d.serialize(writer);
  }
}

// reads the flattened boycott durations and unflatten them
void GOSSIP_Message::readBoycottDurations(ProtocolReader& reader) {
  size_t list_size;
  reader.read(&list_size);
  boycott_durations_list_.resize(list_size);
  for (auto& d : boycott_durations_list_) {
    d.deserialize(reader);
  }
}

void GOSSIP_Message::writeStartingList(ProtocolWriter& writer) const {
  ld_check(starting_list_.size() == num_starting_);
  writer.write(num_starting_);

  for (auto& node : starting_list_) {
    writer.write(node.index());
  }
}

void GOSSIP_Message::readStartingList(ProtocolReader& reader) {
  reader.read(&num_starting_);

  starting_list_.resize(num_starting_);

  for (auto& node_id : starting_list_) {
    node_index_t nidx;
    reader.read(&nidx);
    node_id = NodeID(nidx);
  }
}

}} // namespace facebook::logdevice
