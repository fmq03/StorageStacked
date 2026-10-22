// Copyright 2025 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//-----------------------------------------------------------------------------------
// Class: coralnpu_rvvi_agent
//-----------------------------------------------------------------------------------
class coralnpu_rvvi_agent extends uvm_agent;
  `uvm_component_utils(coralnpu_rvvi_agent)
  coralnpu_rvvi_monitor monitor;
  coralnpu_cov          cov;
  virtual rvviTrace #(
    .ILEN(`ILEN), .XLEN(`XLEN), .FLEN(`FLEN), .VLEN(`VLEN), .NHART(`NHART),
    .RETIRE(`RETIRE)
  ) rvvi_vif;

  function new(string name = "coralnpu_rvvi_agent",
                uvm_component parent = null);
    super.new(name, parent);
  endfunction

  virtual function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    //is_active = UVM_PASSIVE;
    if (!uvm_config_db#(
        virtual rvviTrace #(.ILEN(`ILEN), .XLEN(`XLEN), .FLEN(`FLEN),
                            .VLEN(`VLEN), .NHART(`NHART), .RETIRE(`RETIRE))
        )::get(this, "", "rvvi_vif", rvvi_vif)) begin
        `uvm_fatal(get_type_name(), "RVVI virtual interface not found!")
    end
    monitor = coralnpu_rvvi_monitor::type_id::create("monitor", this);
    cov     = coralnpu_cov::type_id::create("cov",this);
    uvm_config_db#(
      virtual rvviTrace #(.ILEN(`ILEN), .XLEN(`XLEN), .FLEN(`FLEN),
                          .VLEN(`VLEN), .NHART(`NHART), .RETIRE(`RETIRE))
      )::set(this, "monitor*", "rvvi_vif", rvvi_vif);
  endfunction

  // **************************************************************
  // connect phase
  // **************************************************************
  virtual function void connect_phase(uvm_phase phase);
    monitor.rv32i_item_collected_port.connect(cov.imp_rv32i);
    monitor.rv32v_item_collected_port.connect(cov.imp_rv32v);
    monitor.rv32m_item_collected_port.connect(cov.imp_rv32m);
    monitor.rv32f_item_collected_port.connect(cov.imp_rv32f);
    monitor.rv32zicsr_item_collected_port.connect(cov.imp_rv32zicsr);
    monitor.rv32zbb_item_collected_port.connect(cov.imp_rv32zbb);
    monitor.rv32zifencei_item_collected_port.connect(cov.imp_rv32zifencei);
    monitor.custom_item_collected_port.connect(cov.imp_custom);
  endfunction
endclass : coralnpu_rvvi_agent
