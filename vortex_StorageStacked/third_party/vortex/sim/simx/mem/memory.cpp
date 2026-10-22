// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <VX_types.h>
#include "memory.h"
#include <algorithm>
#include <vector>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <iostream>
#include <stdlib.h>
#include <dram_sim.h>

#include "mem_block_pool.h"
#include "constants.h"
#include "types.h"
#include "debug.h"
#include "VX_config.h"

using namespace vortex;

class Memory::Impl {
private:
	Memory*   simobject_;
	Config    config_;
	MemCrossBar::Ptr mem_xbar_;
	DramSim   dram_sim_;
	RAM*      ram_;
	Memory::PreSendHook pre_send_hook_;
	Memory::ExternalIssueHook external_issue_hook_;
	uint64_t next_external_token_ = 1;
	struct ExternalPending {
		MemReq request;
		uint32_t bank_id;
		std::shared_ptr<mem_block_t> rsp_data;
		bool complete = false;
	};
	std::unordered_map<uint64_t, ExternalPending> external_pending_;
	std::queue<uint64_t> external_completed_;
	mutable PerfStats perf_stats_;
	struct DramCallbackArgs {
		Memory::Impl* memsim;
		MemReq request;
		uint32_t bank_id;
		std::shared_ptr<mem_block_t> rsp_data;  // captured at request time for reads
	};

public:
	Impl(Memory* simobject, const Config& config)
		: simobject_(simobject)
		, config_(config)
		, dram_sim_(config.num_banks, config.block_size, config.clock_ratio)
		, ram_(nullptr)
	{
		char sname[100];
		snprintf(sname, 100, "%s-xbar", simobject->name().c_str());
		mem_xbar_ = MemCrossBar::Create(sname, ArbiterType::RoundRobin, config.num_ports, config.num_banks,
			[lg2_block_size = log2ceil(config.block_size), num_banks = config.num_banks](const MemCrossBar::ReqType& req) {
    	// Custom logic to calculate the output index using bank interleaving
			return (uint32_t)((req.addr >> lg2_block_size) & (num_banks-1));
		});
		for (uint32_t i = 0; i < config.num_ports; ++i) {
			simobject->mem_req_in.at(i).bind(&mem_xbar_->ReqIn.at(i));
			mem_xbar_->RspOut.at(i).bind(&simobject->mem_rsp_out.at(i));
		}
	}

	~Impl() {}

	const PerfStats& perf_stats() const {
		perf_stats_.bank_stalls = mem_xbar_->collisions();
		return perf_stats_;
	}

	void reset() {
		dram_sim_.reset();
		external_pending_.clear();
		while (!external_completed_.empty()) external_completed_.pop();
	}

	void tick() {
		// Completion callbacks arrive on the embedding simulator's event queue.
		// Deliver them only from Memory::tick(), keeping SimChannel mutations on
		// the ordinary Vortex cycle boundary.
		while (!external_completed_.empty()) {
			const uint64_t token = external_completed_.front();
			auto found = external_pending_.find(token);
			if (found == external_pending_.end()) {
				external_completed_.pop();
				continue;
			}
			auto& pending = found->second;
			if (!pending.request.is_write()) {
				MemRsp mem_rsp{pending.request.tag, pending.request.hart_id,
				               pending.request.uuid};
				mem_rsp.data = pending.rsp_data;
				if (!mem_xbar_->RspIn.at(pending.bank_id).try_send(mem_rsp))
					break;
			}
			external_pending_.erase(found);
			external_completed_.pop();
		}

		dram_sim_.tick();

		for (uint32_t i = 0; i < config_.num_banks; ++i) {
			if (mem_xbar_->ReqOut.at(i).empty())
				continue;

			auto& mem_req = mem_xbar_->ReqOut.at(i).peek();

			if (external_issue_hook_) {
				// The local RAM applies writes before subsequent reads. Preserve
				// that order when independent external read/write channels overlap.
				const auto block = mem_req.addr / config_.block_size;
				bool conflict = false;
				for (const auto& entry : external_pending_) {
					const auto& older = entry.second.request;
					if (older.addr / config_.block_size == block &&
					    (older.is_write() || mem_req.is_write())) {
						conflict = true;
						break;
					}
				}
				if (conflict) continue;
				const uint64_t token = next_external_token_++;
				std::shared_ptr<mem_block_t> rsp_data;
				if (!mem_req.is_write()) rsp_data = make_mem_block();
				external_pending_.emplace(
					token, ExternalPending{mem_req, i, rsp_data, false});
				const uint8_t* write_data =
					(mem_req.is_write() && mem_req.data)
						? mem_req.data->data() : nullptr;
				if (!external_issue_hook_(token, mem_req, write_data,
				                          mem_req.byteen, config_.block_size)) {
					external_pending_.erase(token);
					continue;
				}
				if (pre_send_hook_) pre_send_hook_(mem_req);
				mem_xbar_->ReqOut.at(i).pop();
				continue;
			}

			std::shared_ptr<mem_block_t> rsp_data;
			if (ram_) {
				uint64_t line_addr = mem_req.addr & ~uint64_t(VX_CFG_MEM_BLOCK_SIZE - 1);
				// Cache fills/writebacks are simulator-internal traffic and
				// don't carry the kernel's intent (e.g. a write-back cache
				// reads a write-only buffer to fill the line on write-miss,
				// since memory buses lack per-region read/write permissions).
				// Suppress ACL for the duration; ACL still guards upload/download.
				ram_->enable_acl(false);
				if (mem_req.is_write()) {
					// Apply byte-enabled write to RAM at request arrival.
					// IO_COUT-range bytes are tapped to the print buffer and
					// not stored in RAM.
					if (mem_req.data) {
						for (uint32_t b = 0; b < VX_CFG_MEM_BLOCK_SIZE; ++b) {
							if (mem_req.byteen & (1ull << b)) {
								uint8_t value = (*mem_req.data)[b];
								ram_->write(&value, line_addr + b, 1);
							}
						}
					}
				} else {
					// Capture the line at request time; response carries it back.
					rsp_data = make_mem_block();
					ram_->read(rsp_data->data(), line_addr, VX_CFG_MEM_BLOCK_SIZE);
				}
				ram_->enable_acl(true);
			}

			if (pre_send_hook_) {
				pre_send_hook_(mem_req);
			}

			// enqueue the request to the memory system
			auto req_args = new DramCallbackArgs{this, mem_req, i, rsp_data};
			dram_sim_.send_request(
				mem_req.addr,
				mem_req.is_write(),
				[](void* arg)->bool {
					auto rsp_args = reinterpret_cast<const DramCallbackArgs*>(arg);
					if (rsp_args->request.is_write()) {
						delete rsp_args;
						return true;
					} else {
								MemRsp mem_rsp{rsp_args->request.tag, rsp_args->request.hart_id, rsp_args->request.uuid};
						mem_rsp.data = rsp_args->rsp_data;
						if (rsp_args->memsim->mem_xbar_->RspIn.at(rsp_args->bank_id).try_send(mem_rsp)) {
							DT(3, rsp_args->memsim->simobject_->name() << " mem-rsp" << rsp_args->bank_id << ": " << mem_rsp);
							delete rsp_args;
							return true;
						}
					}
					return false; // stall
				},
				req_args
			);

			DT(3, simobject_->name() << " mem-req" << i << ": " << mem_req);
			mem_xbar_->ReqOut.at(i).pop();
		}
	}

	void attach_ram(RAM* ram) {
		ram_ = ram;
	}

	void set_pre_send_hook(Memory::PreSendHook hook) {
		pre_send_hook_ = std::move(hook);
	}

	void set_external_timing_hook(Memory::ExternalIssueHook hook) {
		external_issue_hook_ = std::move(hook);
	}

	void complete_external(uint64_t token, const uint8_t* read_data,
	                       uint32_t size) {
		auto found = external_pending_.find(token);
		if (found == external_pending_.end() || found->second.complete)
			return;
		auto& pending = found->second;
		if (!pending.request.is_write() && pending.rsp_data) {
			std::fill(pending.rsp_data->begin(), pending.rsp_data->end(), 0);
			if (read_data != nullptr) {
				std::copy_n(read_data,
				            std::min<uint32_t>(size, config_.block_size),
				            pending.rsp_data->begin());
			}
		}
		pending.complete = true;
		external_completed_.push(token);
	}

	bool has_external_pending() const {
		return !external_pending_.empty();
	}
};

///////////////////////////////////////////////////////////////////////////////

Memory::Memory(const SimContext& ctx, const char* name, const Config& config)
	: SimObject<Memory>(ctx, name)
	, mem_req_in(config.num_ports, this)
	, mem_rsp_out(config.num_ports, this)
	, impl_(new Impl(this, config))
{}

Memory::~Memory() {
  delete impl_;
}

void Memory::on_reset() {
  impl_->reset();
}

void Memory::on_tick() {
  impl_->tick();
}

void Memory::attach_ram(RAM* ram) {
  impl_->attach_ram(ram);
}

void Memory::set_pre_send_hook(PreSendHook hook) {
  impl_->set_pre_send_hook(std::move(hook));
}

void Memory::set_external_timing_hook(ExternalIssueHook hook) {
  impl_->set_external_timing_hook(std::move(hook));
}

void Memory::complete_external(uint64_t token, const uint8_t* read_data,
                               uint32_t size) {
  impl_->complete_external(token, read_data, size);
}

bool Memory::has_external_pending() const {
	return impl_->has_external_pending();
}

const Memory::PerfStats &Memory::perf_stats() const {
	return impl_->perf_stats();
}
