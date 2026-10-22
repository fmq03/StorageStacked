// Copyright 2026 Google LLC
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

// Register address definitions from yocto_isp_register_address.h
// fixed values
localparam logic [31:0] VI_CCL = 32'h00000000;
localparam logic [31:0] VI_ICCL = 32'h00000010;
localparam logic [31:0] VI_IRCL = 32'h00000014;
localparam logic [31:0] ISP_CTRL = 32'h00000400;
localparam logic [31:0] ISP_ACQ_PROP = 32'h00000404;
localparam logic [31:0] ISP_ACQ_H_OFFS = 32'h00000408;
localparam logic [31:0] ISP_ACQ_V_OFFS = 32'h0000040c;
localparam logic [31:0] ISP_ACQ_H_SIZE = 32'h00000410;
localparam logic [31:0] ISP_ACQ_V_SIZE = 32'h00000414;
localparam logic [31:0] ISP_TPG_CTRL = 32'h00000500;
localparam logic [31:0] ISP_TPG_TOTAL_IN = 32'h00000504;
localparam logic [31:0] ISP_TPG_ACT_IN = 32'h00000508;
localparam logic [31:0] ISP_TPG_FP_IN = 32'h0000050c;
localparam logic [31:0] ISP_TPG_BP_IN = 32'h00000510;
localparam logic [31:0] ISP_TPG_W_IN = 32'h00000514;
localparam logic [31:0] ISP_TPG_GAP_IN = 32'h00000518;
localparam logic [31:0] ISP_TPG_GAP_STD_IN = 32'h0000051c;
localparam logic [31:0] ISP_TPG_RANDOM_SEED = 32'h00000520;
localparam logic [31:0] ISP_TPG_FRAME_NUM = 32'h00000524;
localparam logic [31:0] ISP_FRAME_RATE = 32'h00000600;
localparam logic [31:0] ISP_OUT_H_OFFS = 32'h00000604;
localparam logic [31:0] ISP_OUT_V_OFFS = 32'h00000608;
localparam logic [31:0] ISP_OUT_H_SIZE = 32'h0000060c;
localparam logic [31:0] ISP_OUT_V_SIZE = 32'h00000610;
localparam logic [31:0] ISP_BINNING_BASE = 32'h00000624;
localparam logic [31:0] ISP_BINNING_NUM = 32'h00000628;
localparam logic [31:0] ISP_BLS_CTRL = 32'h00000700;
localparam logic [31:0] ISP_BLS_A_FIXED = 32'h00000704;
localparam logic [31:0] ISP_BLS_B_FIXED = 32'h00000708;
localparam logic [31:0] ISP_BLS_C_FIXED = 32'h0000070c;
localparam logic [31:0] ISP_BLS_D_FIXED = 32'h00000710;
localparam logic [31:0] ISP_EXP_CONF = 32'h00000720;
localparam logic [31:0] ISP_EXP_H_OFFSET = 32'h00000724;
localparam logic [31:0] ISP_EXP_V_OFFSET = 32'h00000728;
localparam logic [31:0] ISP_EXP_H_SIZE = 32'h0000072c;
localparam logic [31:0] ISP_EXP_V_SIZE = 32'h00000730;
localparam logic [31:0] ISP_DGAIN_RB = 32'h00000800;
localparam logic [31:0] ISP_DGAIN_G = 32'h00000804;
localparam logic [31:0] ISP_DEMOSAIC = 32'h00000810;
localparam logic [31:0] ISP_FILT_MODE = 32'h00000814;
localparam logic [31:0] ISP_FILT_THRES_BL0 = 32'h00000818;
localparam logic [31:0] ISP_FILT_THRES_BL1 = 32'h0000081c;
localparam logic [31:0] ISP_FILT_THRES_SH0 = 32'h00000820;
localparam logic [31:0] ISP_FILT_THRES_SH1 = 32'h00000824;
localparam logic [31:0] ISP_FILT_LUM_WEIGHT = 32'h00000828;
localparam logic [31:0] ISP_FILT_FAC_SH1 = 32'h0000082c;
localparam logic [31:0] ISP_FILT_FAC_SH0 = 32'h00000830;
localparam logic [31:0] ISP_FILT_FAC_MID = 32'h00000834;
localparam logic [31:0] ISP_FILT_FAC_BL0 = 32'h00000838;
localparam logic [31:0] ISP_FILT_FAC_BL1 = 32'h0000083c;
localparam logic [31:0] ISP_CAC_CTRL = 32'h00000870;
localparam logic [31:0] ISP_CAC_COUNT_START = 32'h00000874;
localparam logic [31:0] ISP_CAC_A = 32'h00000878;
localparam logic [31:0] ISP_CAC_B = 32'h0000087c;
localparam logic [31:0] ISP_CAC_C = 32'h00000880;
localparam logic [31:0] ISP_CAC_X_NORM = 32'h00000884;
localparam logic [31:0] ISP_CAC_Y_NORM = 32'h00000888;
localparam logic [31:0] ISP_GAMMA_OUT_MODE = 32'h00000900;
localparam logic [31:0] ISP_GAMMA_OUT_Y_0 = 32'h00000904;
localparam logic [31:0] ISP_GAMMA_OUT_Y_1 = 32'h00000908;
localparam logic [31:0] ISP_GAMMA_OUT_Y_2 = 32'h0000090c;
localparam logic [31:0] ISP_GAMMA_OUT_Y_3 = 32'h00000910;
localparam logic [31:0] ISP_GAMMA_OUT_Y_4 = 32'h00000914;
localparam logic [31:0] ISP_GAMMA_OUT_Y_5 = 32'h00000918;
localparam logic [31:0] ISP_GAMMA_OUT_Y_6 = 32'h0000091c;
localparam logic [31:0] ISP_GAMMA_OUT_Y_7 = 32'h00000920;
localparam logic [31:0] ISP_GAMMA_OUT_Y_8 = 32'h00000924;
localparam logic [31:0] ISP_GAMMA_OUT_Y_9 = 32'h00000928;
localparam logic [31:0] ISP_GAMMA_OUT_Y_10 = 32'h0000092c;
localparam logic [31:0] ISP_GAMMA_OUT_Y_11 = 32'h00000930;
localparam logic [31:0] ISP_GAMMA_OUT_Y_12 = 32'h00000934;
localparam logic [31:0] ISP_GAMMA_OUT_Y_13 = 32'h00000938;
localparam logic [31:0] ISP_GAMMA_OUT_Y_14 = 32'h0000093c;
localparam logic [31:0] ISP_GAMMA_OUT_Y_15 = 32'h00000940;
localparam logic [31:0] ISP_GAMMA_OUT_Y_16 = 32'h00000944;
localparam logic [31:0] ISP_AWB_PROP = 32'h00000950;
localparam logic [31:0] ISP_AWB_H_OFFS = 32'h00000954;
localparam logic [31:0] ISP_AWB_V_OFFS = 32'h00000958;
localparam logic [31:0] ISP_AWB_H_SIZE = 32'h0000095c;
localparam logic [31:0] ISP_AWB_V_SIZE = 32'h00000960;
localparam logic [31:0] ISP_AWB_FRAMES = 32'h00000964;
localparam logic [31:0] ISP_AWB_REF = 32'h00000968;
localparam logic [31:0] ISP_AWB_THRESH = 32'h0000096c;
localparam logic [31:0] ISP_CC_COEFF_0 = 32'h00000a00;
localparam logic [31:0] ISP_CC_COEFF_1 = 32'h00000a04;
localparam logic [31:0] ISP_CC_COEFF_2 = 32'h00000a08;
localparam logic [31:0] ISP_CC_COEFF_3 = 32'h00000a0c;
localparam logic [31:0] ISP_CC_COEFF_4 = 32'h00000a10;
localparam logic [31:0] ISP_CC_COEFF_5 = 32'h00000a14;
localparam logic [31:0] ISP_CC_COEFF_6 = 32'h00000a18;
localparam logic [31:0] ISP_CC_COEFF_7 = 32'h00000a1c;
localparam logic [31:0] ISP_CC_COEFF_8 = 32'h00000a20;
localparam logic [31:0] FORMAT_CONV_CTRL = 32'h00000a24;
localparam logic [31:0] ISP_CT_COEFF_0 = 32'h00000a30;
localparam logic [31:0] ISP_CT_COEFF_1 = 32'h00000a34;
localparam logic [31:0] ISP_CT_COEFF_2 = 32'h00000a38;
localparam logic [31:0] ISP_CT_COEFF_3 = 32'h00000a3c;
localparam logic [31:0] ISP_CT_COEFF_4 = 32'h00000a40;
localparam logic [31:0] ISP_CT_COEFF_5 = 32'h00000a44;
localparam logic [31:0] ISP_CT_COEFF_6 = 32'h00000a48;
localparam logic [31:0] ISP_CT_COEFF_7 = 32'h00000a4c;
localparam logic [31:0] ISP_CT_COEFF_8 = 32'h00000a50;
localparam logic [31:0] ISP_CT_OFFSET_R = 32'h00000a54;
localparam logic [31:0] ISP_CT_OFFSET_G = 32'h00000a58;
localparam logic [31:0] ISP_CT_OFFSET_B = 32'h00000a5c;
localparam logic [31:0] ISP_IMSC = 32'h00000b00;
localparam logic [31:0] ISP_RIS = 32'h00000b04;
localparam logic [31:0] ISP_MIS = 32'h00000b08;
localparam logic [31:0] ISP_ICR = 32'h00000b0c;
localparam logic [31:0] ISP_ISR = 32'h00000b10;
localparam logic [31:0] ISP_ERR_CLR = 32'h00000b18;
localparam logic [31:0] ISP_MRSZ_BASE = 32'h00000c00;
localparam logic [31:0] ISP_MRSZ_SCALE_HY = 32'h00000c04;
localparam logic [31:0] ISP_MRSZ_SCALE_HCB = 32'h00000c08;
localparam logic [31:0] ISP_MRSZ_SCALE_HCR = 32'h00000c0C;
localparam logic [31:0] ISP_MRSZ_SCALE_VY = 32'h00000c10;
localparam logic [31:0] ISP_MRSZ_SCALE_VC = 32'h00000c14;
localparam logic [31:0] ISP_MRSZ_PHASE_HY = 32'h00000c18;
localparam logic [31:0] ISP_MRSZ_PHASE_HC = 32'h00000c1C;
localparam logic [31:0] ISP_MRSZ_PHASE_VY = 32'h00000c20;
localparam logic [31:0] ISP_MRSZ_PHASE_VC = 32'h00000c24;
localparam logic [31:0] ISP_MRSZ_FORMAT_CONV_CTRL = 32'h00000c6c;
localparam logic [31:0] MI_CTRL = 32'h00000e00;
localparam logic [31:0] MI_INIT = 32'h00000e04;
localparam logic [31:0] MI_MP_Y_BASE_AD_INIT = 32'h00000e08;
localparam logic [31:0] MI_MP_Y_SIZE_INIT = 32'h00000e0c;
localparam logic [31:0] MI_MP_Y_OFFS_CNT_INIT = 32'h00000e10;
localparam logic [31:0] MI_MP_Y_IRQ_OFFS_INIT = 32'h00000e18;
localparam logic [31:0] MI_MP_CB_BASE_AD_INIT = 32'h00000e1c;
localparam logic [31:0] MI_MP_CB_SIZE_INIT = 32'h00000e20;
localparam logic [31:0] MI_MP_CB_OFFS_CNT_INIT = 32'h00000e24;
localparam logic [31:0] MI_MP_CR_BASE_AD_INIT = 32'h00000e2c;
localparam logic [31:0] MI_MP_CR_SIZE_INIT = 32'h00000e30;
localparam logic [31:0] MI_MP_CR_OFFS_CNT_INIT = 32'h00000e34;
localparam logic [31:0] MI_SP_Y_BASE_AD_INIT = 32'h00000e3c;
localparam logic [31:0] MI_SP_Y_SIZE_INIT = 32'h00000e40;
localparam logic [31:0] MI_SP_Y_OFFS_CNT_INIT = 32'h00000e44;
localparam logic [31:0] MI_SP_Y_LLENGTH = 32'h00000e4c;
localparam logic [31:0] MI_SP_CB_BASE_AD_INIT = 32'h00000e50;
localparam logic [31:0] MI_SP_CB_SIZE_INIT = 32'h00000e54;
localparam logic [31:0] MI_SP_CB_OFFS_CNT_INIT = 32'h00000e58;
localparam logic [31:0] MI_SP_CR_BASE_AD_INIT = 32'h00000e60;
localparam logic [31:0] MI_SP_CR_SIZE_INIT = 32'h00000e64;
localparam logic [31:0] MI_SP_CR_OFFS_CNT_INIT = 32'h00000e68;
localparam logic [31:0] MI_IMSC = 32'h00000ef8;
localparam logic [31:0] MI_RIS = 32'h00000efc;
localparam logic [31:0] MI_MIS = 32'h00000f00;
localparam logic [31:0] MI_ICR = 32'h00000f04;
localparam logic [31:0] MI_ISR = 32'h00000f08;
localparam logic [31:0] MI_STATUS_CLR = 32'h00000f10;
localparam logic [31:0] MI_SP_Y_PIC_WIDTH = 32'h00000f14;
localparam logic [31:0] MI_SP_Y_PIC_HEIGHT = 32'h00000f18;
localparam logic [31:0] MI_SP_Y_PIC_SIZE = 32'h00000f1c;
localparam logic [31:0] MI_MP_Y_BASE_AD_INIT2 = 32'h00000f30;
localparam logic [31:0] MI_MP_CB_BASE_AD_INIT2 = 32'h00000f34;
localparam logic [31:0] MI_MP_CR_BASE_AD_INIT2 = 32'h00000f38;
localparam logic [31:0] MI_SP_Y_BASE_AD_INIT2 = 32'h00000f3c;
localparam logic [31:0] MI_SP_CB_BASE_AD_INIT2 = 32'h00000f40;
localparam logic [31:0] MI_SP_CR_BASE_AD_INIT2 = 32'h00000f44;
localparam logic [31:0] MI_MP_Y_LLENGTH = 32'h00000f50;
localparam logic [31:0] MI_OUTPUT_ALIGN_FORMAT = 32'h00000f5c;
localparam logic [31:0] MI_MP_OUTPUT_FIFO_SIZE = 32'h00000f60;
localparam logic [31:0] MI_MP_Y_PIC_WIDTH = 32'h00000f64;
localparam logic [31:0] MI_MP_Y_PIC_HEIGHT = 32'h00000f68;
localparam logic [31:0] MI_MP_Y_PIC_SIZE = 32'h00000f6c;
localparam logic [31:0] SRSZ_CTRL = 32'h00001000;
localparam logic [31:0] SRSZ_SCALE_HY = 32'h00001004;
localparam logic [31:0] SRSZ_SCALE_HCB = 32'h00001008;
localparam logic [31:0] SRSZ_SCALE_HCR = 32'h0000100c;
localparam logic [31:0] SRSZ_SCALE_VY = 32'h00001010;
localparam logic [31:0] SRSZ_SCALE_VC = 32'h00001014;
localparam logic [31:0] SRSZ_PHASE_HY = 32'h00001018;
localparam logic [31:0] SRSZ_PHASE_HC = 32'h0000101c;
localparam logic [31:0] SRSZ_PHASE_VY = 32'h00001020;
localparam logic [31:0] SRSZ_PHASE_VC = 32'h00001024;
localparam logic [31:0] SRSZ_FORMAT_CONV_CTRL = 32'h0000106c;
// END Register address definitions from yocto_isp_register_address.h
// fixed values

`ifdef CAM_MODEL
localparam logic [31:0] frame_h_size = 32'h00000140;
localparam logic [31:0] frame_v_size = 32'h000000f0;
localparam logic [31:0] frame_h_offs = 32'h00000002;
localparam logic [31:0] frame_v_offs = 32'h00000002;
localparam logic [31:0] y_base_ad_init_addr = 32'h5a300000;
localparam logic [31:0] cb_base_ad_init_addr = 32'h5a320000;
localparam logic [31:0] cr_base_ad_init_addr = 32'h5a320000;
localparam logic [31:0] y_base_ad_init2_addr = 32'h5a340000;
localparam logic [31:0] cb_base_ad_init2_addr = 32'h5a360000;
localparam logic [31:0] cr_base_ad_init2_addr = 32'h5a360000;

//    localparam logic [31:0] mi_mp_y_size_init = frame_h_size * frame_v_size;
// DEBUG avoid triggering mi_wrap_mp_y_int
localparam logic [31:0] mi_mp_y_size_init = (frame_h_size * frame_v_size) + 1024;
localparam logic [31:0] mi_mp_y_pic_size = frame_h_size * frame_v_size;
`else
localparam logic [31:0] y_base_ad_init_addr_tpg = 32'h5a300000;
localparam logic [31:0] cb_base_ad_init_addr_tpg = 32'h5a320000;
localparam logic [31:0] cr_base_ad_init_addr_tpg = 32'h5a320000;
localparam logic [31:0] y_base_ad_init2_addr_tpg = 32'h5a340000;
localparam logic [31:0] cb_base_ad_init2_addr_tpg = 32'h5a360000;
localparam logic [31:0] cr_base_ad_init2_addr_tpg = 32'h5a360000;
`endif


task automatic configure_isp();
`ifdef CAM_MODEL
  // HACKERY to see dbg_mi_line_cnt (viv_s9) increment
  // Program MI_MP_HANDSHAKE (0x14C)
  // Bits [12:5] = slice_size (set to 0xFF = 255)
  // Bit 0 = handshake_en (set to 0)
  tl_write(32'h0000014c, 32'h00001FE0);

  $display("Configuring ISP for Camera Model 320x240 (Matching HPS Demo)...");
  // ---------------------------------------------------------
  // CAMERA MODEL MODE (320x240) - MATCHING HPS demo exactly
  // manually copied because AI likes to hallucinate register addresses
  // ---------------------------------------------------------
  tl_write(ISP_IMSC, 32'hffffffff);  // ISP_IMSC
  tl_write(MI_IMSC, 32'hffffffff);  // MI_IMSC
  tl_write(VI_CCL, 32'h00000002);  // VI_CCL
  tl_write(VI_ICCL, 32'h00000059);  // VI_ICCL
  tl_write(VI_IRCL, 32'h00000000);  // VI_IRCL
  tl_write(ISP_ACQ_H_OFFS, frame_h_offs);
  tl_write(ISP_ACQ_V_OFFS, frame_v_offs);
  tl_write(ISP_ACQ_H_SIZE, frame_h_size);
  tl_write(ISP_ACQ_V_SIZE, frame_v_size);
  tl_write(ISP_TPG_TOTAL_IN, 32'h011d8518);
  tl_write(ISP_TPG_ACT_IN, 32'h00a001e0);
  tl_write(ISP_TPG_FP_IN, 32'h001c8126);
  tl_write(ISP_TPG_BP_IN, 32'h00454129);
  tl_write(ISP_TPG_W_IN, 32'h001bc0e9);
  tl_write(ISP_TPG_GAP_IN, 32'h003540a0);
  tl_write(ISP_TPG_GAP_STD_IN, 32'h00000050);
  tl_write(ISP_TPG_RANDOM_SEED, 32'he30ce95f);
  tl_write(ISP_TPG_FRAME_NUM, 32'h00000003);
  tl_write(ISP_OUT_H_OFFS, 32'h00000000);
  tl_write(ISP_OUT_V_OFFS, 32'h00000000);
  tl_write(ISP_OUT_H_SIZE, frame_h_size);
  tl_write(ISP_OUT_V_SIZE, frame_v_size);
  tl_write(ISP_BLS_CTRL, 32'h00000000);
  tl_write(ISP_BLS_A_FIXED, 32'h00000000);
  tl_write(ISP_BLS_B_FIXED, 32'h00000000);
  tl_write(ISP_BLS_C_FIXED, 32'h00000000);
  tl_write(ISP_BLS_D_FIXED, 32'h00000000);
  tl_write(ISP_EXP_CONF, 32'h00000001);
  tl_write(ISP_EXP_H_OFFSET, 32'h000000d3);
  tl_write(ISP_EXP_V_OFFSET, 32'h00000017);
  tl_write(ISP_EXP_H_SIZE, 32'h00000043);
  tl_write(ISP_EXP_V_SIZE, 32'h00000048);
  tl_write(ISP_DGAIN_RB, 32'h01a200d7);
  tl_write(ISP_DGAIN_G, 32'h00f9011e);
  tl_write(ISP_DEMOSAIC, 32'h000004f6);
  tl_write(ISP_FILT_MODE, 32'h00000810);
  tl_write(ISP_FILT_THRES_BL0, 32'h00000230);
  tl_write(ISP_FILT_THRES_BL1, 32'h00000018);
  tl_write(ISP_FILT_THRES_SH0, 32'h00000287);
  tl_write(ISP_FILT_THRES_SH1, 32'h000003f2);
  tl_write(ISP_FILT_LUM_WEIGHT, 32'h00061fdc);
  tl_write(ISP_FILT_FAC_SH1, 32'h00000031);
  tl_write(ISP_FILT_FAC_SH0, 32'h00000031);
  tl_write(ISP_FILT_FAC_MID, 32'h0000002c);
  tl_write(ISP_FILT_FAC_BL0, 32'h0000001a);
  tl_write(ISP_FILT_FAC_BL1, 32'h00000016);
  tl_write(ISP_CAC_CTRL, 32'h00000004);
  tl_write(ISP_CAC_COUNT_START, 32'h00f00140);
  tl_write(ISP_CAC_A, 32'h01160187);
  tl_write(ISP_CAC_B, 32'h005f004c);
  tl_write(ISP_CAC_C, 32'h0138017a);
  tl_write(ISP_CAC_X_NORM, 32'h00020001);
  tl_write(ISP_CAC_Y_NORM, 32'h00060015);
  tl_write(ISP_GAMMA_OUT_MODE, 32'h00000001);
  tl_write(ISP_GAMMA_OUT_Y_0, 32'h00000170);
  tl_write(ISP_GAMMA_OUT_Y_1, 32'h00000178);
  tl_write(ISP_GAMMA_OUT_Y_2, 32'h00000181);
  tl_write(ISP_GAMMA_OUT_Y_3, 32'h0000018e);
  tl_write(ISP_GAMMA_OUT_Y_4, 32'h00000194);
  tl_write(ISP_GAMMA_OUT_Y_5, 32'h000001b4);
  tl_write(ISP_GAMMA_OUT_Y_6, 32'h000001b7);
  tl_write(ISP_GAMMA_OUT_Y_7, 32'h000001b9);
  tl_write(ISP_GAMMA_OUT_Y_8, 32'h000001ba);
  tl_write(ISP_GAMMA_OUT_Y_9, 32'h000001ba);
  tl_write(ISP_GAMMA_OUT_Y_10, 32'h000001ba);
  tl_write(ISP_GAMMA_OUT_Y_11, 32'h000001ba);
  tl_write(ISP_GAMMA_OUT_Y_12, 32'h000001ba);
  tl_write(ISP_GAMMA_OUT_Y_13, 32'h000001ba);
  tl_write(ISP_GAMMA_OUT_Y_14, 32'h000001ba);
  tl_write(ISP_GAMMA_OUT_Y_15, 32'h000001ba);
  tl_write(ISP_GAMMA_OUT_Y_16, 32'h00000303);
  tl_write(ISP_AWB_PROP, 32'h00000002);
  tl_write(ISP_AWB_H_OFFS, 32'h00000052);
  tl_write(ISP_AWB_V_OFFS, 32'h0000016f);
  tl_write(ISP_AWB_H_SIZE, 32'h000001be);
  tl_write(ISP_AWB_V_SIZE, 32'h00000061);
  tl_write(ISP_AWB_FRAMES, 32'h00000001);
  tl_write(ISP_AWB_REF, 32'h0000b0c6);
  tl_write(ISP_AWB_THRESH, 32'h0b145491);
  tl_write(ISP_CC_COEFF_0, 32'h0000008d);
  tl_write(ISP_CC_COEFF_1, 32'h000000b1);
  tl_write(ISP_CC_COEFF_2, 32'h000000a2);
  tl_write(ISP_CC_COEFF_3, 32'h00000078);
  tl_write(ISP_CC_COEFF_4, 32'h00000169);
  tl_write(ISP_CC_COEFF_5, 32'h000000ba);
  tl_write(ISP_CC_COEFF_6, 32'h0000015c);
  tl_write(ISP_CC_COEFF_7, 32'h000000f8);
  tl_write(ISP_CC_COEFF_8, 32'h0000017c);
  tl_write(FORMAT_CONV_CTRL, 32'h00000000);
  tl_write(ISP_CT_COEFF_0, 32'h000005ff);
  tl_write(ISP_CT_COEFF_1, 32'h00000707);
  tl_write(ISP_CT_COEFF_2, 32'h00000464);
  tl_write(ISP_CT_COEFF_3, 32'h00000008);
  tl_write(ISP_CT_COEFF_4, 32'h00000058);
  tl_write(ISP_CT_COEFF_5, 32'h000007de);
  tl_write(ISP_CT_COEFF_6, 32'h00000241);
  tl_write(ISP_CT_COEFF_7, 32'h000006ff);
  tl_write(ISP_CT_COEFF_8, 32'h0000007d);
  tl_write(ISP_CT_OFFSET_R, 32'h000005ec);
  tl_write(ISP_CT_OFFSET_G, 32'h00000d7e);
  tl_write(ISP_CT_OFFSET_B, 32'h00000dd7);
  tl_write(ISP_IMSC, 32'h000400fc);
  tl_write(ISP_ERR_CLR, 32'h00000000);
  tl_write(MI_MP_Y_BASE_AD_INIT, y_base_ad_init_addr);
  tl_write(MI_MP_Y_SIZE_INIT, mi_mp_y_size_init);
  tl_write(MI_MP_Y_OFFS_CNT_INIT, 32'h00000000);
  tl_write(MI_MP_Y_IRQ_OFFS_INIT, 32'h00000000);
  tl_write(MI_MP_CB_BASE_AD_INIT, cb_base_ad_init_addr);
  tl_write(MI_MP_CB_SIZE_INIT, 32'h00000000);
  tl_write(MI_MP_CB_OFFS_CNT_INIT, 32'h00000000);
  tl_write(MI_MP_CR_BASE_AD_INIT, cr_base_ad_init_addr);
  tl_write(MI_MP_CR_SIZE_INIT, 32'h00000000);
  tl_write(MI_MP_CR_OFFS_CNT_INIT, 32'h00000000);
  tl_write(MI_IMSC, 32'h000003ff);
  tl_write(MI_STATUS_CLR, 32'h00000077);
  tl_write(MI_MP_Y_BASE_AD_INIT2, y_base_ad_init2_addr);
  tl_write(MI_MP_CB_BASE_AD_INIT2, cb_base_ad_init2_addr);
  tl_write(MI_MP_CR_BASE_AD_INIT2, cr_base_ad_init2_addr);
  tl_write(MI_MP_Y_LLENGTH, frame_h_size);
  tl_write(MI_OUTPUT_ALIGN_FORMAT, 32'h00000401);
  tl_write(MI_MP_OUTPUT_FIFO_SIZE, 32'h00000000);
  tl_write(MI_MP_Y_PIC_WIDTH, frame_h_size);
  tl_write(MI_MP_Y_PIC_HEIGHT, frame_v_size);
  tl_write(MI_MP_Y_PIC_SIZE, mi_mp_y_pic_size);
  tl_write(SRSZ_CTRL, 32'h000001c0);
  tl_write(SRSZ_SCALE_HY, 32'h0000546d);
  tl_write(SRSZ_SCALE_HCB, 32'h0000db23);
  tl_write(SRSZ_SCALE_HCR, 32'h000071b7);
  tl_write(SRSZ_SCALE_VY, 32'h00007803);
  tl_write(SRSZ_SCALE_VC, 32'h00005628);
  tl_write(SRSZ_PHASE_HY, 32'h00000000);
  tl_write(SRSZ_PHASE_HC, 32'h00000000);
  tl_write(SRSZ_PHASE_VY, 32'h00000000);
  tl_write(SRSZ_PHASE_VC, 32'h00000000);
  tl_write(SRSZ_FORMAT_CONV_CTRL, 32'h000000fa);
  tl_write(MI_SP_Y_BASE_AD_INIT, 32'h50000000);
  tl_write(MI_SP_Y_SIZE_INIT, 32'h0006c1c0);
  tl_write(MI_SP_Y_OFFS_CNT_INIT, 32'h00000000);
  tl_write(MI_SP_Y_LLENGTH, 32'h00000128);
  tl_write(MI_SP_CB_BASE_AD_INIT, 32'h5006c800);
  tl_write(MI_SP_CB_SIZE_INIT, 32'h00000000);
  tl_write(MI_SP_CB_OFFS_CNT_INIT, 32'h00000000);
  tl_write(MI_SP_CR_BASE_AD_INIT, 32'h5006c800);
  tl_write(MI_SP_CR_SIZE_INIT, 32'h00000000);
  tl_write(MI_SP_CR_OFFS_CNT_INIT, 32'h00000000);
  tl_write(MI_SP_Y_PIC_WIDTH, 32'h00000124);
  tl_write(MI_SP_Y_PIC_HEIGHT, 32'h00000176);
  tl_write(MI_SP_Y_PIC_SIZE, 32'h0001b070);
  tl_write(MI_SP_Y_BASE_AD_INIT2, 32'h500d9000);
  tl_write(MI_SP_CB_BASE_AD_INIT2, 32'h50145800);
  tl_write(MI_SP_CR_BASE_AD_INIT2, 32'h50145800);
  tl_write(ISP_ACQ_PROP, 32'h40011);
  tl_write(SRSZ_CTRL, 32'h000001c0);
  tl_write(ISP_CTRL, 32'h00207211);
  tl_write(ISP_TPG_CTRL, 32'h00000d76);
  tl_write(MI_CTRL, 32'h68352808);
  tl_write(MI_INIT, 32'h000000b0);

`else
  $display("Configuring ISP for TPG 128x64 Gray Bar...");
  // ---------------------------------------------------------
  // TPG MODE (128x64)
  // ---------------------------------------------------------
  tl_write(ISP_IMSC, 32'hffffffff);  // ISP_IMSC
  tl_write(MI_IMSC, 32'hffffffff);  // MI_IMSC
  tl_write(VI_CCL, 32'h00000002);  // VI_CCL
  tl_write(VI_ICCL, 32'h00000059);  // VI_ICCL
  tl_write(VI_IRCL, 32'h00000000);  // VI_IRCL
  tl_write(ISP_ACQ_H_OFFS, 32'h00000000);  // ISP_ACQ_H_OFFS
  tl_write(ISP_ACQ_V_OFFS, 32'h00000000);  // ISP_ACQ_V_OFFS
  tl_write(ISP_ACQ_H_SIZE, 32'h00000080);  // ISP_ACQ_H_SIZE
  tl_write(ISP_ACQ_V_SIZE, 32'h00000040);  // ISP_ACQ_V_SIZE
  tl_write(ISP_TPG_TOTAL_IN, 32'h00bfc25f);  // ISP_TPG_TOTAL_IN
  tl_write(ISP_TPG_ACT_IN, 32'h00200040);  // ISP_TPG_ACT_IN
  tl_write(ISP_TPG_FP_IN, 32'h002bc11b);  // ISP_TPG_FP_IN
  tl_write(ISP_TPG_BP_IN, 32'h0039c0a7);  // ISP_TPG_BP_IN
  tl_write(ISP_TPG_W_IN, 32'h003a405d);  // ISP_TPG_W_IN
  tl_write(ISP_TPG_GAP_IN, 32'h000a8015);  // ISP_TPG_GAP_IN
  tl_write(ISP_TPG_GAP_STD_IN, 32'h00000010);  // ISP_TPG_GAP_STD_IN
  tl_write(ISP_TPG_RANDOM_SEED, 32'hb6ccff1c);  // ISP_TPG_RANDOM_SEED
  tl_write(ISP_TPG_FRAME_NUM,
           32'h00000002);  // ISP_TPG_FRAME_NUM (2 frames for Ping Pong buffer test)
  tl_write(ISP_OUT_H_OFFS, 32'h00000000);  // ISP_OUT_H_OFFS
  tl_write(ISP_OUT_V_OFFS, 32'h00000000);  // ISP_OUT_V_OFFS
  tl_write(ISP_OUT_H_SIZE, 32'h00000080);  // ISP_OUT_H_SIZE
  tl_write(ISP_OUT_V_SIZE, 32'h00000040);  // ISP_OUT_V_SIZE
  tl_write(ISP_BLS_CTRL, 32'h00000001);  // ISP_BLS_CTRL
  tl_write(ISP_BLS_A_FIXED, 32'h00000000);  // ISP_BLS_A_FIXED
  tl_write(ISP_BLS_B_FIXED, 32'h00000000);  // ISP_BLS_B_FIXED
  tl_write(ISP_BLS_C_FIXED, 32'h00000000);  // ISP_BLS_C_FIXED
  tl_write(ISP_BLS_D_FIXED, 32'h00000000);  // ISP_BLS_D_FIXED
  tl_write(ISP_EXP_CONF, 32'h00000000);  // ISP_EXP_CONF
  tl_write(ISP_EXP_H_OFFSET, 32'h00000012);  // ISP_EXP_H_OFFSET
  tl_write(ISP_EXP_V_OFFSET, 32'h0000001f);  // ISP_EXP_V_OFFSET
  tl_write(ISP_EXP_H_SIZE, 32'h00000012);  // ISP_EXP_H_SIZE
  tl_write(ISP_EXP_V_SIZE, 32'h00000004);  // ISP_EXP_V_SIZE
  tl_write(ISP_DGAIN_RB, 32'h006600ba);  // ISP_DGAIN_RB
  tl_write(ISP_DGAIN_G, 32'h00dd00e3);  // ISP_DGAIN_G
  tl_write(ISP_DEMOSAIC, 32'h00000462);  // ISP_DEMOSAIC
  tl_write(ISP_FILT_MODE, 32'h00000a12);  // ISP_FILT_MODE
  tl_write(ISP_FILT_THRES_BL0, 32'h0000036d);  // ISP_FILT_THRES_BL0
  tl_write(ISP_FILT_THRES_BL1, 32'h0000028e);  // ISP_FILT_THRES_BL1
  tl_write(ISP_FILT_THRES_SH0, 32'h000003c8);  // ISP_FILT_THRES_SH0
  tl_write(ISP_FILT_THRES_SH1, 32'h000003e2);  // ISP_FILT_THRES_SH1
  tl_write(ISP_FILT_LUM_WEIGHT, 32'h000259b5);  // ISP_FILT_LUM_WEIGHT
  tl_write(ISP_FILT_FAC_SH1, 32'h0000003e);  // ISP_FILT_FAC_SH1
  tl_write(ISP_FILT_FAC_SH0, 32'h0000003d);  // ISP_FILT_FAC_SH0
  tl_write(ISP_FILT_FAC_MID, 32'h0000002c);  // ISP_FILT_FAC_MID
  tl_write(ISP_FILT_FAC_BL0, 32'h00000021);  // ISP_FILT_FAC_BL0
  tl_write(ISP_FILT_FAC_BL1, 32'h0000000c);  // ISP_FILT_FAC_BL1
  tl_write(ISP_CAC_CTRL, 32'h0000000a);  // ISP_CAC_CTRL
  tl_write(ISP_CAC_COUNT_START, 32'h00200040);  // ISP_CAC_COUNT_START
  tl_write(ISP_CAC_A, 32'h01f4018c);  // ISP_CAC_A
  tl_write(ISP_CAC_B, 32'h012e01ca);  // ISP_CAC_B
  tl_write(ISP_CAC_C, 32'h003401a4);  // ISP_CAC_C
  tl_write(ISP_CAC_X_NORM, 32'h00030008);  // ISP_CAC_X_NORM
  tl_write(ISP_CAC_Y_NORM, 32'h00020015);  // ISP_CAC_Y_NORM
  tl_write(ISP_GAMMA_OUT_MODE, 32'h00000001);  // ISP_GAMMA_OUT_MODE
  tl_write(ISP_GAMMA_OUT_Y_0, 32'h000001ae);  // ISP_GAMMA_OUT_Y_0
  tl_write(ISP_GAMMA_OUT_Y_1, 32'h000002b0);  // ISP_GAMMA_OUT_Y_1
  tl_write(ISP_GAMMA_OUT_Y_2, 32'h000002db);  // ISP_GAMMA_OUT_Y_2
  tl_write(ISP_GAMMA_OUT_Y_3, 32'h000002f9);  // ISP_GAMMA_OUT_Y_3
  tl_write(ISP_GAMMA_OUT_Y_4, 32'h000002fd);  // ISP_GAMMA_OUT_Y_4
  tl_write(ISP_GAMMA_OUT_Y_5, 32'h000002fe);  // ISP_GAMMA_OUT_Y_5
  tl_write(ISP_GAMMA_OUT_Y_6, 32'h000002fe);  // ISP_GAMMA_OUT_Y_6
  tl_write(ISP_GAMMA_OUT_Y_7, 32'h000002fe);  // ISP_GAMMA_OUT_Y_7
  tl_write(ISP_GAMMA_OUT_Y_8, 32'h000002fe);  // ISP_GAMMA_OUT_Y_8
  tl_write(ISP_GAMMA_OUT_Y_9, 32'h000002fe);  // ISP_GAMMA_OUT_Y_9
  tl_write(ISP_GAMMA_OUT_Y_10, 32'h000002fe);  // ISP_GAMMA_OUT_Y_10
  tl_write(ISP_GAMMA_OUT_Y_11, 32'h000002fe);  // ISP_GAMMA_OUT_Y_11
  tl_write(ISP_GAMMA_OUT_Y_12, 32'h000002fe);  // ISP_GAMMA_OUT_Y_12
  tl_write(ISP_GAMMA_OUT_Y_13, 32'h000002fe);  // ISP_GAMMA_OUT_Y_13
  tl_write(ISP_GAMMA_OUT_Y_14, 32'h000002fe);  // ISP_GAMMA_OUT_Y_14
  tl_write(ISP_GAMMA_OUT_Y_15, 32'h000002fe);  // ISP_GAMMA_OUT_Y_15
  tl_write(ISP_GAMMA_OUT_Y_16, 32'h000002fe);  // ISP_GAMMA_OUT_Y_16
  tl_write(ISP_AWB_PROP, 32'h80000007);  // ISP_AWB_PROP
  tl_write(ISP_AWB_H_OFFS, 32'h0000005b);  // ISP_AWB_H_OFFS
  tl_write(ISP_AWB_V_OFFS, 32'h00000022);  // ISP_AWB_V_OFFS
  tl_write(ISP_AWB_H_SIZE, 32'h00000005);  // ISP_AWB_H_SIZE
  tl_write(ISP_AWB_V_SIZE, 32'h00000009);  // ISP_AWB_V_SIZE
  tl_write(ISP_AWB_FRAMES, 32'h00000000);  // ISP_AWB_FRAMES
  tl_write(ISP_AWB_REF, 32'h00006b55);  // ISP_AWB_REF
  tl_write(ISP_AWB_THRESH, 32'heaef51fa);  // ISP_AWB_THRESH
  tl_write(ISP_CC_COEFF_0, 32'h00000004);  // ISP_CC_COEFF_0
  tl_write(ISP_CC_COEFF_1, 32'h0000011b);  // ISP_CC_COEFF_1
  tl_write(ISP_CC_COEFF_2, 32'h000001af);  // ISP_CC_COEFF_2
  tl_write(ISP_CC_COEFF_3, 32'h000001e0);  // ISP_CC_COEFF_3
  tl_write(ISP_CC_COEFF_4, 32'h00000020);  // ISP_CC_COEFF_4
  tl_write(ISP_CC_COEFF_5, 32'h00000140);  // ISP_CC_COEFF_5
  tl_write(ISP_CC_COEFF_6, 32'h00000053);  // ISP_CC_COEFF_6
  tl_write(ISP_CC_COEFF_7, 32'h0000002e);  // ISP_CC_COEFF_7
  tl_write(ISP_CC_COEFF_8, 32'h000001b7);  // ISP_CC_COEFF_8
  tl_write(FORMAT_CONV_CTRL, 32'h00000001);  // FORMAT_CONV_CTRL
  tl_write(ISP_CT_COEFF_0, 32'h00000588);  // ISP_CT_COEFF_0
  tl_write(ISP_CT_COEFF_1, 32'h000005d9);  // ISP_CT_COEFF_1
  tl_write(ISP_CT_COEFF_2, 32'h000004ea);  // ISP_CT_COEFF_2
  tl_write(ISP_CT_COEFF_3, 32'h000004f9);  // ISP_CT_COEFF_3
  tl_write(ISP_CT_COEFF_4, 32'h0000031e);  // ISP_CT_COEFF_4
  tl_write(ISP_CT_COEFF_5, 32'h000006db);  // ISP_CT_COEFF_5
  tl_write(ISP_CT_COEFF_6, 32'h000004f6);  // ISP_CT_COEFF_6
  tl_write(ISP_CT_COEFF_7, 32'h00000307);  // ISP_CT_COEFF_7
  tl_write(ISP_CT_COEFF_8, 32'h00000085);  // ISP_CT_COEFF_8
  tl_write(ISP_CT_OFFSET_R, 32'h00000310);  // ISP_CT_OFFSET_R
  tl_write(ISP_CT_OFFSET_G, 32'h00000592);  // ISP_CT_OFFSET_G
  tl_write(ISP_CT_OFFSET_B, 32'h00000d58);  // ISP_CT_OFFSET_B
  tl_write(ISP_IMSC, 32'h000400fc);  // ISP_IMSC
  tl_write(ISP_ERR_CLR, 32'h00000001);  // ISP_ERR_CLR
  tl_write(MI_MP_Y_BASE_AD_INIT, y_base_ad_init_addr_tpg);  // MI_MP_Y_BASE_AD_INIT
  tl_write(MI_MP_Y_SIZE_INIT, 32'h0000a800);  // MI_MP_Y_SIZE_INIT
  tl_write(MI_MP_Y_OFFS_CNT_INIT, 32'h00000000);  // MI_MP_Y_OFFS_CNT_INIT
  tl_write(MI_MP_Y_IRQ_OFFS_INIT, 32'h00000000);  // MI_MP_Y_IRQ_OFFS_INIT
  tl_write(MI_MP_CB_BASE_AD_INIT, cb_base_ad_init_addr_tpg);  // MI_MP_CB_BASE_AD_INIT
  tl_write(MI_MP_CB_SIZE_INIT, 32'h00005400);  // MI_MP_CB_SIZE_INIT
  tl_write(MI_MP_CB_OFFS_CNT_INIT, 32'h00000000);  // MI_MP_CB_OFFS_CNT_INIT
  tl_write(MI_MP_CR_BASE_AD_INIT, cr_base_ad_init_addr_tpg);  // MI_MP_CR_BASE_AD_INIT
  tl_write(MI_MP_CR_SIZE_INIT, 32'h00005400);  // MI_MP_CR_SIZE_INIT
  tl_write(MI_MP_CR_OFFS_CNT_INIT, 32'h00000000);  // MI_MP_CR_OFFS_CNT_INIT
  tl_write(MI_IMSC, 32'h000003ff);  // MI_IMSC
  tl_write(MI_STATUS_CLR, 32'h00000053);  // MI_STATUS_CLR
  tl_write(MI_MP_Y_BASE_AD_INIT2, y_base_ad_init2_addr_tpg);  // MI_MP_Y_BASE_AD_INIT2
  tl_write(MI_MP_CB_BASE_AD_INIT2, cb_base_ad_init2_addr_tpg);  // MI_MP_CB_BASE_AD_INIT2
  tl_write(MI_MP_CR_BASE_AD_INIT2, cr_base_ad_init2_addr_tpg);  // MI_MP_CR_BASE_AD_INIT2
  tl_write(MI_MP_Y_LLENGTH, 32'h00000080);  // MI_MP_Y_LLENGTH
  tl_write(MI_OUTPUT_ALIGN_FORMAT, 32'h00000401);  // MI_OUTPUT_ALIGN_FORMAT
  tl_write(MI_MP_OUTPUT_FIFO_SIZE, 32'h00000000);  // MI_MP_OUTPUT_FIFO_SIZE
  tl_write(MI_MP_Y_PIC_WIDTH, 32'h00000080);  // MI_MP_Y_PIC_WIDTH
  tl_write(MI_MP_Y_PIC_HEIGHT, 32'h00000040);  // MI_MP_Y_PIC_HEIGHT
  tl_write(MI_MP_Y_PIC_SIZE, 32'h00002000);  // MI_MP_Y_PIC_SIZE
  tl_write(SRSZ_CTRL, 32'h0000011c);  // SRSZ_CTRL
  tl_write(SRSZ_SCALE_HY, 32'h0000c177);  // SRSZ_SCALE_HY
  tl_write(SRSZ_SCALE_HCB, 32'h0000ec82);  // SRSZ_SCALE_HCB
  tl_write(SRSZ_SCALE_HCR, 32'h00000057);  // SRSZ_SCALE_HCR
  tl_write(SRSZ_SCALE_VY, 32'h000075d8);  // SRSZ_SCALE_VY
  tl_write(SRSZ_SCALE_VC, 32'h000075d8);  // SRSZ_SCALE_VC
  tl_write(SRSZ_PHASE_HY, 32'h00000000);  // SRSZ_PHASE_HY
  tl_write(SRSZ_PHASE_HC, 32'h00000000);  // SRSZ_PHASE_HC
  tl_write(SRSZ_PHASE_VY, 32'h00000000);  // SRSZ_PHASE_VY
  tl_write(SRSZ_PHASE_VC, 32'h00000000);  // SRSZ_PHASE_VC
  tl_write(SRSZ_FORMAT_CONV_CTRL, 32'h000000fa);  // SRSZ_FORMAT_CONV_CTRL
  tl_write(MI_SP_Y_BASE_AD_INIT, 32'h2354d5f8);  // MI_SP_Y_BASE_AD_INIT
  tl_write(MI_SP_Y_SIZE_INIT, 32'h00008000);  // MI_SP_Y_SIZE_INIT
  tl_write(MI_SP_Y_OFFS_CNT_INIT, 32'h00000000);  // MI_SP_Y_OFFS_CNT_INIT
  tl_write(MI_SP_Y_LLENGTH, 32'h00000080);  // MI_SP_Y_LLENGTH
  tl_write(MI_SP_CB_BASE_AD_INIT, 32'h9fff2d98);  // MI_SP_CB_BASE_AD_INIT
  tl_write(MI_SP_CB_SIZE_INIT, 32'h01673858);  // MI_SP_CB_SIZE_INIT
  tl_write(MI_SP_CB_OFFS_CNT_INIT, 32'h00000000);  // MI_SP_CB_OFFS_CNT_INIT
  tl_write(MI_SP_CR_BASE_AD_INIT, 32'hd3bbca68);  // MI_SP_CR_BASE_AD_INIT
  tl_write(MI_SP_CR_SIZE_INIT, 32'h0f8bf230);  // MI_SP_CR_SIZE_INIT
  tl_write(MI_SP_CR_OFFS_CNT_INIT, 32'h00000000);  // MI_SP_CR_OFFS_CNT_INIT
  tl_write(MI_SP_Y_PIC_WIDTH, 32'h00000080);  // MI_SP_Y_PIC_WIDTH
  tl_write(MI_SP_Y_PIC_HEIGHT, 32'h00000040);  // MI_SP_Y_PIC_HEIGHT
  tl_write(MI_SP_Y_PIC_SIZE, 32'h00002000);  // MI_SP_Y_PIC_SIZE
  tl_write(MI_SP_Y_BASE_AD_INIT2, 32'h1939f6d8);  // MI_SP_Y_BASE_AD_INIT2
  tl_write(MI_SP_CB_BASE_AD_INIT2, 32'hf9f91948);  // MI_SP_CB_BASE_AD_INIT2
  tl_write(MI_SP_CR_BASE_AD_INIT2, 32'h8335a0b0);  // MI_SP_CR_BASE_AD_INIT2
  tl_write(ISP_ACQ_PROP, 32'h00000089);  // ISP_ACQ_PROP
  tl_write(SRSZ_CTRL, 32'h0000011c);  // SRSZ_CTRL
  tl_write(ISP_CTRL, 32'h80007250);  // ISP_CTRL
  tl_write(ISP_TPG_CTRL, 32'h00000d95);  // ISP_TPG_CTRL
  tl_write(MI_CTRL, 32'h68372808);  // MI_CTRL
  tl_write(MI_INIT, 32'h000000b0);  // MI_INIT
`endif

endtask
