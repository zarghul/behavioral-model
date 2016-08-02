/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Antonin Bas (antonin@barefootnetworks.com)
 *
 */

#include <bm/bm_sim/packet.h>
#include <bm/bm_sim/parser.h>
#include <bm/bm_sim/tables.h>
#include <bm/bm_sim/switch.h>
#include <bm/bm_sim/event_logger.h>

#include <bm/bm_runtime/bm_runtime.h>

#include <unistd.h>

#include <iostream>
#include <memory>
#include <thread>
#include <fstream>
#include <string>
#include <chrono>

#include <iostream>
#include <iomanip>

#include <cstdio>

#define LOCKFREE_QUEUE 1

#if LOCKFREE_QUEUE
#include <bm/bm_sim/spsc_queue.h>
#include <bm/bm_sim/lockless_queueing.h>
template<typename T>
using Queue = bm::SPSCQueue<T>;
#else
#include <bm/bm_sim/queue.h>
using bm::Queue;
#endif

using bm::Switch;
using bm::Packet;
using bm::PHV;
using bm::Parser;
using bm::Deparser;
using bm::Pipeline;


#define num_queues 1

struct WorkerMapper {
  WorkerMapper(size_t nb_workers)
      : nb_workers(nb_workers) { }

  size_t operator()(size_t queue_id) const {
    return queue_id % nb_workers;
  }

  size_t nb_workers;
};

class FastSwitch : public Switch {
 public:
  FastSwitch()
    : Switch(true),  // enable_swap = false  
    input_buffer(num_queues,1,1024,WorkerMapper(1)),
    process_buffer(512), output_buffer(512) { }

  int receive(int port_num, const char *buffer, int len, uint64_t flags) {
    //static int pkt_id = 0;

    (void)flags;
    (void)buffer;
    (void)len;
    (void)port_num;
    //if (this->do_swap() == 0)  // a swap took place
    //  swap_happened = true;

    //auto packet = new_packet_ptr(port_num, pkt_id++, len,
    //                             bm::PacketBuffer(len+512, buffer, len));

    //BMELOG(packet_in, *packet);

    packet_count_in++;
    //Parser *parser = this->get_parser("parser");
    //parser->parse(packet.get());

    //input_buffer.push_front(0, flags==0);
    input_buffer.push_front(packet_count_in%num_queues, 0);
    return 0;
  }

  void start_and_return() {
    std::thread ti(&FastSwitch::ingress_thread, this);
    ti.detach();
    //for (size_t i = 0; i < nb_egress_threads; i++) {
    //  std::thread te(&FastSwitch::egress_thread, this, i);
    //  te.detach();
    //}
    //std::thread tt(&FastSwitch::transmit_thread, this);
    //tt.detach();
    std::thread ts(&FastSwitch::stats_thread, this);
    ts.detach();
  }

 private:
  void ingress_thread();
  void egress_thread(size_t i);
  void transmit_thread();

  void stats_thread();

 private:
  //Queue<std::unique_ptr<Packet>> input_buffer;
  bm::QueueingLogicLL<std::unique_ptr<Packet>,WorkerMapper> input_buffer;
  Queue<std::unique_ptr<Packet>> process_buffer;
  Queue<std::unique_ptr<Packet>> output_buffer;
  //bool swap_happened{false};

  static constexpr size_t nb_egress_threads = 1u;

  // XXX variables for stat printing
  uint64_t packet_count_in{0};
  uint64_t packet_count_out{0};
  uint64_t avg_latency{0};
  uint64_t max_latency{0};

};

void FastSwitch::stats_thread() {
  uint64_t old_packet_count_in=0;
  uint64_t old_packet_count_out=0;
  uint64_t old_prod_notified=0;
  uint64_t old_cons_notified=0;

  int period = 200;
  while(true) {
    float delta_t_in = period*1000000.0/(packet_count_in-old_packet_count_in);
    float delta_t_out = period*1000000.0/(packet_count_out-old_packet_count_out);

    printf("-- IN  ns_pkt  %5.1f pkt_s %1.3e prod_notified %6.0f\n"
           "   OUT ns_pkt  %5.1f pkt_s %1.3e cons_notified %6.0f\n"
           ,//"       ms_avg_lat %1.3e ms_max_lat %1.3e\n",
           delta_t_in, 1000000000.0/delta_t_in,
           (input_buffer.queues[0]->prod_notified-old_prod_notified)*1000.0/period,
           delta_t_out, 1000000000.0/delta_t_out,
           (input_buffer.queues[0]->cons_notified-old_cons_notified)*1000.0/period
        );//(0.000001*avg_latency)/packet_count_out, 0.000001*max_latency);

    old_packet_count_in=packet_count_in;
    old_packet_count_out=packet_count_out;
    old_cons_notified = input_buffer.queues[0]->cons_notified;
    old_prod_notified = input_buffer.queues[0]->prod_notified;

    std::this_thread::sleep_for(std::chrono::milliseconds(period));
  }
}

void FastSwitch::transmit_thread() {
  Deparser *deparser = this->get_deparser("deparser");
  while (1) {
    std::vector<std::unique_ptr<Packet>> packets;
    output_buffer.pop_back(&packets);
    for (auto& packet : packets) {
      deparser->deparse(packet.get());
      BMELOG(packet_out, *packet);
      BMLOG_DEBUG_PKT(*packet, "Transmitting packet of size {} out of port {}",
                      packet->get_data_size(), packet->get_egress_port());
      auto ingress_ts = packet->get_ingress_ts();
      std::chrono::nanoseconds delta = std::chrono::system_clock::now() - ingress_ts;
      if (uint64_t(delta.count()) > max_latency) max_latency = delta.count();
      avg_latency+= delta.count();
      packet_count_out++;

      int egress_port = packet->get_egress_port();
      if (egress_port == 511) {
        BMLOG_DEBUG_PKT(*packet, "Dropping packet");
      } else {
        transmit_fn(egress_port, packet->data(), packet->get_data_size());
      }
    }
  }
}

void FastSwitch::ingress_thread() {
  //Pipeline *ingress_mau = this->get_pipeline("ingress");
  while (1) {
    std::unique_ptr<Packet> packet;
    size_t port;
    input_buffer.pop_back(0,&port,&packet);
    packet_count_out++;
    //input_buffer.pop_back(&packet);

#if 0
    for (auto& packet : packets) {
      continue;
      int ingress_port = packet->get_ingress_port();
      (void) ingress_port;
      BMLOG_DEBUG_PKT(*packet, "Processing packet received on port {}",
                      ingress_port);


      ingress_mau->apply(packet.get());

      process_buffer.push_front(std::move(packet));
    }
#endif
  }
}

void FastSwitch::egress_thread(size_t i) {
  (void)i;

  Pipeline *egress_mau = this->get_pipeline("egress");
  PHV *phv;

  while (1) {
    std::vector<std::unique_ptr<Packet>> packets;
    process_buffer.pop_back(&packets);
    for (auto& packet : packets) {
      //continue;

      phv = packet->get_phv();
      int egress_port = phv->get_field("standard_metadata.egress_spec").get_int();
      BMLOG_DEBUG_PKT(*packet, "Egress port is {}", egress_port);

      packet->set_egress_port(egress_port);
      egress_mau->apply(packet.get());
      output_buffer.push_front(std::move(packet));
    }
  }
}

/* Switch instance */

static FastSwitch *fast_switch;

int
main(int argc, char* argv[]) {
  fast_switch = new FastSwitch();
  int status = fast_switch->init_from_command_line_options(argc, argv);
  if (status != 0) std::exit(status);

  // should this be done by the call to init_from_command_line_options
  int thrift_port = fast_switch->get_runtime_port();
  bm_runtime::start_server(fast_switch, thrift_port);

  fast_switch->start_and_return();

  while (true) std::this_thread::sleep_for(std::chrono::seconds(100));

  return 0;
}
