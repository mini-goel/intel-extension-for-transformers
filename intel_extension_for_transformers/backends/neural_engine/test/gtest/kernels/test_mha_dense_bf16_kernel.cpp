//  Copyright (c) 2021 Intel Corporation
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#include <map>
#include <string>
#include <cmath>

#include "amx_utils.hpp"
#include "cpu_isa.hpp"
#include "gtest/gtest.h"
#include "kernels/mha_dense_ref.hpp"
#include "unit_test_utils.hpp"

namespace jd {
using dt = data_type;
struct test_params_t {
  dim_t bs;
  dim_t sl_m;
  dim_t sl_n;
  dim_t head_num;
  dim_t head_size;
  bool has_pmask;
  bool has_badd;
  int nthr;
  bool expect_to_fail;
};

struct test_data_t {
  operator_desc op_desc;
  std::vector<const void*> rt_data_kern;
  std::vector<const void*> rt_data_ref;
};

static std::mt19937 rand_gen(1);

inline static std::string TestParam2str(testing::TestParamInfo<test_params_t> tpi) {
  auto&& p = tpi.param;
  std::vector<std::string> params;
  params.push_back("c" + std::to_string(tpi.param.nthr));
  params.push_back(std::to_string(p.bs));         // bs
  params.push_back(std::to_string(p.sl_m));       // sl_m
  params.push_back(std::to_string(p.sl_n));       // sl_n
  params.push_back(std::to_string(p.head_num));   // head_num
  params.push_back(std::to_string(p.head_size));  // head_size
  if (p.has_pmask) params.push_back("pmask");     // has_pmask
  if (p.has_badd) params.push_back("badd");       // has_badd
  return join_str(params, "_");
}

bool check_result(const int nthr, const bool expect_to_fail, const test_data_t& d) {
  try {
    std::shared_ptr<const kernel_desc_t> mha_dense_ref_desc;
    kernel_desc_t::create<mha_dense_ref_kd_t>(mha_dense_ref_desc, d.op_desc);
    std::shared_ptr<const kernel_t> mha_dense_ref_kernel;
    kernel_t::create<mha_dense_ref_k_t, mha_dense_ref_kd_t>(mha_dense_ref_kernel, mha_dense_ref_desc);
    mha_dense_ref_kernel->execute(d.rt_data_ref);

    n_thread_t with_n_thread(nthr);
    mha_dense_desc mha_dense_desc(d.op_desc);
    mha_dense mha_dense_kernel(mha_dense_desc);
    const auto tmp_p = std::shared_ptr<char>(aligned_allocator_t<char>::allocate(mha_dense_kernel.get_workspace_size()),
                                             [](char* ptr) { aligned_allocator_t<char>::deallocate(ptr); });
    auto data_p(d.rt_data_kern);
    data_p[mha_dense_io::WORKSPACE] = tmp_p.get();
    mha_dense_kernel.execute(data_p);
  } catch (const std::exception& e) {
    SPARSE_LOG(ERROR) << e.what();
    return expect_to_fail;
  }

  if (!expect_to_fail) {
    auto buf1 = d.rt_data_kern[mha_dense_io::DST];
    auto dst_size = d.op_desc.tensor_descs()[mha_dense_io::DST].size();
    auto buf2 = d.rt_data_ref[mha_dense_io::DST];
    // Should compare buffer with different addresses
    EXPECT_NE(buf1, buf2);
    switch (d.op_desc.tensor_descs()[mha_dense_io::DST].dtype()) {
      case dt::bf16:
        return compare_data<bfloat16_t>(buf1, dst_size, buf2, dst_size, 5e-2);
      default:
        SPARSE_LOG(ERROR) << "Unexpected dst type";
    }
  }
  return false;
}

std::pair<const void*, const void*> make_tensor_obj(const tensor_desc& ts_desc, float min_value, float max_value) {
  int64_t elem_num = std::accumulate(ts_desc.shape().begin(), ts_desc.shape().end(), 1LL, std::multiplies<int64_t>());
  int bytes_size = elem_num * type_size[ts_desc.dtype()];
  void* data_ptr = nullptr;
  if (min_value == 0.f && max_value == 0.f) {
    data_ptr = new uint8_t[bytes_size];
    memset(data_ptr, 0, bytes_size);
  } else {
    const auto seed = std::uniform_int_distribution<>()(rand_gen);
    if (ts_desc.dtype() == dt::fp32) {
      data_ptr = new float[elem_num];
      init_vector(static_cast<float*>(data_ptr), elem_num, min_value, max_value, seed);
    } else if (ts_desc.dtype() == dt::bf16) {
      data_ptr = new bfloat16_t[elem_num];
      init_vector(static_cast<bfloat16_t*>(data_ptr), elem_num, min_value, max_value, seed);
    } else if (ts_desc.dtype() == dt::s32) {
      data_ptr = new int32_t[elem_num];
      init_vector(static_cast<int32_t*>(data_ptr), elem_num, min_value, max_value, seed);
    } else if (ts_desc.dtype() == dt::u8) {
      data_ptr = new uint8_t[elem_num];
      init_vector(static_cast<uint8_t*>(data_ptr), elem_num, min_value, max_value, seed);
    } else if (ts_desc.dtype() == dt::s8) {
      data_ptr = new int8_t[elem_num];
      init_vector(static_cast<int8_t*>(data_ptr), elem_num, min_value, max_value, seed);
    }
  }

  void* data_ptr_copy = new uint8_t[bytes_size];
  memcpy(data_ptr_copy, data_ptr, bytes_size);
  return std::pair<const void*, const void*>{data_ptr, data_ptr_copy};
}

test_data_t gen_data(const test_params_t& p) {
  n_thread_t with_nthr(p.nthr);
  std::vector<tensor_desc> ts_descs(mha_dense_io::mha_dense_io_MAX + 1,
                                    tensor_desc{{}, data_type::undef, format_type::undef});
  ts_descs[mha_dense_io::SRC_Q] = {{p.bs, p.sl_m, p.head_num, p.head_size}, data_type::bf16, format_type::abcd};
  ts_descs[mha_dense_io::SRC_K] = {{p.bs, p.sl_n, p.head_num, p.head_size}, data_type::bf16, format_type::abcd};
  ts_descs[mha_dense_io::SRC_V] = {{p.bs, p.sl_n, p.head_num, p.head_size}, data_type::bf16, format_type::abcd};
  ts_descs[mha_dense_io::DST] = {{p.bs, p.sl_m, p.head_num, p.head_size}, data_type::bf16, format_type::abcd};
  ts_descs[mha_dense_io::ATT_SCALE] = {{1}, data_type::fp32, format_type::a};
  // TODO(Yi): enable broadcasting
  if (p.has_badd) ts_descs[mha_dense_io::BINARY_ADD] = {{1, 1, p.sl_m, p.sl_n}, data_type::fp32, format_type::abcd};
  if (p.has_pmask) ts_descs[mha_dense_io::MASK] = {{p.bs}, data_type::s32, format_type::a};

  // Step 1.1: Construct Operator config obj
  std::unordered_map<std::string, std::string> attr_map;
  attr_map["approx_exp"] = "True";
  attr_map["stable_softmax"] = "False";

  // Step 2: Construct Tensor ptr
  const auto att_scale_val = 1.f / std::sqrt(p.sl_n);
  const std::pair<const void*, const void*> empty_tensor_data{};
  auto Qs = make_tensor_obj(ts_descs[mha_dense_io::SRC_Q], -1, 1);
  auto Ks = make_tensor_obj(ts_descs[mha_dense_io::SRC_K], -1, 1);
  auto Vs = make_tensor_obj(ts_descs[mha_dense_io::SRC_V], -1, 1);
  auto dsts = make_tensor_obj(ts_descs[mha_dense_io::DST], 0, 0);
  auto att_scales = make_tensor_obj(ts_descs[mha_dense_io::ATT_SCALE], att_scale_val, att_scale_val);
  auto badds = p.has_badd ? make_tensor_obj(ts_descs[mha_dense_io::BINARY_ADD], -1.f, 1.f) : empty_tensor_data;
  auto pmasks = p.has_pmask ? make_tensor_obj(ts_descs[mha_dense_io::MASK], 1, p.sl_n) : empty_tensor_data;

  std::vector<const void*> data_p(mha_dense_io::mha_dense_io_MAX + 1, nullptr);
  data_p[mha_dense_io::SRC_Q] = Qs.first;
  data_p[mha_dense_io::SRC_K] = Ks.first;
  data_p[mha_dense_io::SRC_V] = Vs.first;
  data_p[mha_dense_io::DST] = dsts.first;
  data_p[mha_dense_io::ATT_SCALE] = att_scales.first;
  if (p.has_badd) data_p[mha_dense_io::BINARY_ADD] = badds.first;
  if (p.has_pmask) data_p[mha_dense_io::MASK] = pmasks.first;

  std::vector<const void*> data_q(mha_dense_io::mha_dense_io_MAX + 1, nullptr);
  data_q[mha_dense_io::SRC_Q] = Qs.second;
  data_q[mha_dense_io::SRC_K] = Ks.second;
  data_q[mha_dense_io::SRC_V] = Vs.second;
  data_q[mha_dense_io::DST] = dsts.second;
  data_q[mha_dense_io::ATT_SCALE] = att_scales.second;
  if (p.has_badd) data_q[mha_dense_io::BINARY_ADD] = badds.second;
  if (p.has_pmask) data_q[mha_dense_io::MASK] = pmasks.second;

  operator_desc op_desc(kernel_kind::mha_dense, kernel_prop::forward_inference, engine_kind::cpu, ts_descs, attr_map);
  return {op_desc, data_p, data_q};
}

static auto case_func = []() {
  std::vector<test_params_t> cases;

  // case param: bs sl_m sl_n head_num head_size has_pmask has_badd nthr expect_to_fail
  cases.push_back(test_params_t{1, 64, 64, 1, 32, false, true, 1, false});
  cases.push_back(test_params_t{2, 64, 64, 1, 32, false, true, 1, false});
  cases.push_back(test_params_t{2, 1024, 1024, 1, 40, false, true, 1, false});
  cases.push_back(test_params_t{2, 1024, 1024, 1, 80, false, true, 1, false});
  cases.push_back(test_params_t{2, 256, 256, 1, 160, false, true, 1, false});

  cases.push_back(test_params_t{1, 64, 32, 1, 32, false, true, 1, false});
  cases.push_back(test_params_t{1, 64, 33, 1, 32, false, true, 1, false});
  cases.push_back(test_params_t{1, 64, 61, 1, 32, false, true, 1, false});
  cases.push_back(test_params_t{1, 1, 61, 1, 32, false, true, 1, false});
  cases.push_back(test_params_t{1, 1, 61, 1, 32, true, true, 1, false});
  cases.push_back(test_params_t{1, 1, 35, 1, 64, true, true, 1, false});
  cases.push_back(test_params_t{2, 1, 42, 1, 64, false, true, 1, false});
  cases.push_back(test_params_t{1, 64, 33, 1, 32, true, true, 3, false});
  cases.push_back(test_params_t{1, 64, 33, 1, 32, true, true, 0, false});

  // cases.push_back({2, 1024, 77, 1, 40, false, true, 1, false});  // TODO(Yi): fix
  // cases.push_back({2, 1024, 77, 1, 80, false, true, 1, false});  // TODO(Yi): fix
  // cases.push_back({2, 256, 77, 1, 160, false, true, 1, false});  // TODO(Yi): fix

  return ::testing::ValuesIn(cases);
};

class MhaDenseBf16KernTest : public testing::TestWithParam<test_params_t> {
 protected:
  MhaDenseBf16KernTest() {}
  ~MhaDenseBf16KernTest() {}
  void SetUp() override {}
  void TearDown() override {}
};

TEST_P(MhaDenseBf16KernTest, ) {
  const test_params_t& p = testing::TestWithParam<test_params_t>::GetParam();
  const auto d = gen_data(p);
  EXPECT_TRUE(check_result(p.nthr, p.expect_to_fail, d));

  for (auto data : {d.rt_data_kern, d.rt_data_ref})
    for (auto p : data)
      if (p != nullptr) delete[] reinterpret_cast<const char*>(p);
}
INSTANTIATE_TEST_SUITE_P(SparseLib, MhaDenseBf16KernTest, case_func(), TestParam2str);
}  // namespace jd