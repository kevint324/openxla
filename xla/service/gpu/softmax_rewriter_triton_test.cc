/* Copyright 2023 The TensorFlow Authors. All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "xla/service/gpu/softmax_rewriter_triton.h"

#include <memory>
#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/base/optimization.h"
#include "absl/log/log.h"
#include "absl/strings/substitute.h"
#include "xla/primitive_util.h"
#include "xla/service/pattern_matcher.h"
#include "xla/service/pattern_matcher_gmock.h"
#include "xla/tests/hlo_test_base.h"

namespace xla {
namespace gpu {
namespace {

namespace m = ::xla::match;

class SoftmaxRewriterTritonTest
    : public HloTestBase,
      public ::testing::WithParamInterface<PrimitiveType> {
 public:
  void SetUp() override {
    gpu_version_ = GpuVersion{
        se::CudaComputeCapability{se::CudaComputeCapability::AMPERE, 0}};
  }

 protected:
  GpuVersion gpu_version_;
};

TEST_P(SoftmaxRewriterTritonTest, CanFuseExactSoftmax) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
add_computation {
  arg_0.1 = $0[] parameter(0)
  arg_1.1 = $0[] parameter(1)
  ROOT add = $0[] add(arg_0.1, arg_1.1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
  exponential = $0[127,125]{1,0} exponential(subtract)
  constant_zero = $0[] constant(0)
  second_reduce = $0[127]{0} reduce(exponential, constant_zero), dimensions={1}, to_apply=add_computation
  second_broadcast = $0[127,125]{1,0} broadcast(second_reduce), dimensions={0}
  ROOT divide = $0[127,125]{1,0} divide(exponential, second_broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);

  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  VLOG(2) << module->ToString();

  switch (data_type) {
    case F32:
    case BF16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Fusion(m::Parameter())));
      break;
    case F16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Divide(m::Exp(), m::Broadcast())));
      break;
    default:
      ABSL_UNREACHABLE();
  }
}

TEST_P(SoftmaxRewriterTritonTest, CanFuseFirstSoftmaxDiamond) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  ROOT subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  VLOG(2) << module->ToString();
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Parameter())));
}

TEST_F(SoftmaxRewriterTritonTest, CanNotFuseExactSoftmaxF64) {
  const std::string hlo_string = R"(
HloModule softmax
max_computation {
  arg_0 = f64[] parameter(0)
  arg_1 = f64[] parameter(1)
  ROOT maximum = f64[] maximum(arg_0, arg_1)
}
add_computation {
  arg_0.1 = f64[] parameter(0)
  arg_1.1 = f64[] parameter(1)
  ROOT add = f64[] add(arg_0.1, arg_1.1)
}
ENTRY main {
  param_0 = f64[127,125]{1,0} parameter(0)
  constant_neg_inf = f64[] constant(-inf)
  reduce = f64[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = f64[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = f64[127,125]{1,0} subtract(param_0, broadcast)
  exponential = f64[127,125]{1,0} exponential(subtract)
  constant_zero = f64[] constant(0)
  second_reduce = f64[127]{0} reduce(exponential, constant_zero), dimensions={1}, to_apply=add_computation
  second_broadcast = f64[127,125]{1,0} broadcast(second_reduce), dimensions={0}
  ROOT divide = f64[127,125]{1,0} divide(exponential, second_broadcast)
}
)";

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

TEST_F(SoftmaxRewriterTritonTest, CanNotFuseExactSoftmaxBF16) {
  const std::string hlo_string = R"(
HloModule softmax
max_computation {
  arg_0 = bf16[] parameter(0)
  arg_1 = bf16[] parameter(1)
  ROOT maximum = bf16[] maximum(arg_0, arg_1)
}
add_computation {
  arg_0.1 = bf16[] parameter(0)
  arg_1.1 = bf16[] parameter(1)
  ROOT add = bf16[] add(arg_0.1, arg_1.1)
}
ENTRY main {
  param_0 = bf16[127,125]{1,0} parameter(0)
  constant_neg_inf = bf16[] constant(-inf)
  reduce = bf16[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = bf16[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = bf16[127,125]{1,0} subtract(param_0, broadcast)
  exponential = bf16[127,125]{1,0} exponential(subtract)
  constant_zero = bf16[] constant(0)
  second_reduce = bf16[127]{0} reduce(exponential, constant_zero), dimensions={1}, to_apply=add_computation
  second_broadcast = bf16[127,125]{1,0} broadcast(second_reduce), dimensions={0}
  ROOT divide = bf16[127,125]{1,0} divide(exponential, second_broadcast)
}
)";

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

TEST_P(SoftmaxRewriterTritonTest,
       CanFuseSoftmaxWithBatchDimMergingAndSplittingBitcastsOnEveryEdge) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
add_computation {
  arg_0.1 = $0[] parameter(0)
  arg_1.1 = $0[] parameter(1)
  ROOT add = $0[] add(arg_0.1, arg_1.1)
}
ENTRY main {
  param_0 = $0[130,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  bitcasted_param_0 = $0[65,2,125] bitcast(param_0)
  reduce = $0[65,2]{1,0} reduce(bitcasted_param_0, constant_neg_inf), dimensions={2}, to_apply=max_computation
  bitcasted_reduce = $0[130] bitcast(reduce)
  broadcast = $0[130,125]{1,0} broadcast(bitcasted_reduce), dimensions={0}
  bitcasted_broadcast = $0[65,2,125] bitcast(broadcast)
  subtract = $0[65,2,125]{2,1,0} subtract(bitcasted_param_0, bitcasted_broadcast)
  bitcasted_subtract = $0[130,125] bitcast(subtract)
  exponential = $0[130,125]{1,0} exponential(bitcasted_subtract)
  constant_zero = $0[] constant(0)
  bitcasted_exponential = $0[2,65,125] bitcast(exponential)
  second_reduce = $0[2,65]{1,0} reduce(bitcasted_exponential, constant_zero), dimensions={2}, to_apply=add_computation
  second_bitcasted_reduce = $0[130] bitcast(second_reduce)
  second_broadcast = $0[130,125]{1,0} broadcast(second_bitcasted_reduce), dimensions={0}
  second_bitcasted_broadcast = $0[2,65,125] bitcast(second_broadcast)
  divide = $0[2,65,125]{2,1,0} divide(bitcasted_exponential, second_bitcasted_broadcast)
  ROOT bitcasted_divide = $0[130,125] bitcast(divide)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);

  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());

  switch (data_type) {
    case F32:
    case BF16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Fusion(m::Parameter())));
      break;
    case F16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Bitcast(m::Divide())));
      break;
    default:
      ABSL_UNREACHABLE();
  }
}

TEST_P(SoftmaxRewriterTritonTest, CanNotFuseSoftmaxDiamondWithWrongLayout) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[127,125]{0,1} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  ROOT subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

TEST_P(SoftmaxRewriterTritonTest,
       CanNotFuseSoftmaxDiamondWithWrongReduceDimension) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[125]{0} reduce(param_0, constant_neg_inf), dimensions={0}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={1}
  ROOT subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

TEST_P(SoftmaxRewriterTritonTest,
       CanNotFuseSoftmaxDiamondWithWrongBroadcastDimension) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[125,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[125]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[125,125]{1,0} broadcast(reduce), dimensions={1}
  ROOT subtract = $0[125,125]{1,0} subtract(param_0, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

// TODO(bchetioui): expand so this can be supported?
TEST_P(SoftmaxRewriterTritonTest,
       CanNotFuseSoftmaxDiamondWithExtraBroadcastUsage) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
  ROOT multiply = $0[127,125]{1,0} multiply(broadcast, subtract)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

TEST_P(SoftmaxRewriterTritonTest,
       CanFuseSoftmaxWithIntermediateUnaryElementwise) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
add_computation {
  arg_0.1 = $0[] parameter(0)
  arg_1.1 = $0[] parameter(1)
  ROOT add = $0[] add(arg_0.1, arg_1.1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
  abs = $0[127,125]{1,0} abs(subtract)
  exponential = $0[127,125]{1,0} exponential(abs)
  constant_zero = $0[] constant(0)
  second_reduce = $0[127]{0} reduce(exponential, constant_zero), dimensions={1}, to_apply=add_computation
  second_broadcast = $0[127,125]{1,0} broadcast(second_reduce), dimensions={0}
  ROOT divide = $0[127,125]{1,0} divide(exponential, second_broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);

  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());

  switch (data_type) {
    case F32:
    case BF16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Fusion(m::Parameter())));
      break;
    case F16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Divide()));
      break;
    default:
      ABSL_UNREACHABLE();
  }
}

TEST_P(SoftmaxRewriterTritonTest,
       CanFuseTwoDiamondsWithSecondDiamondProducerEqualToFirstDiamondRoot) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
add_computation {
  arg_0.1 = $0[] parameter(0)
  arg_1.1 = $0[] parameter(1)
  ROOT add = $0[] add(arg_0.1, arg_1.1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
  constant_zero = $0[] constant(0)
  second_reduce = $0[127]{0} reduce(subtract, constant_zero), dimensions={1}, to_apply=add_computation
  second_broadcast = $0[127,125]{1,0} broadcast(second_reduce), dimensions={0}
  ROOT divide = $0[127,125]{1,0} divide(subtract, second_broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);

  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());

  switch (data_type) {
    case F32:
    case BF16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Fusion(m::Parameter())));
      break;
    case F16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Divide()));
      break;
    default:
      ABSL_UNREACHABLE();
  }
}

TEST_P(SoftmaxRewriterTritonTest,
       CanFuseDiamondWithTrailingUnaryElementwiseAtTheRoot) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
  ROOT abs = $0[127,125]{1,0} abs(subtract)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Parameter())));
}

TEST_P(SoftmaxRewriterTritonTest, CanFuseDiamondWithUnaryElementwisePrefix) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  abs = $0[127,125]{1,0} abs(param_0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(abs, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  ROOT subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Parameter())));
}

TEST_P(SoftmaxRewriterTritonTest,
       CanFuseDiamondWithMultipleBroadcastDimensions) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[1,3,125,125]{3,2,1,0} parameter(0)
  bitcast = $0[3,125,125]{2,1,0} bitcast($0[1,3,125,125]{3,2,1,0} param_0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[3,125]{1,0} reduce($0[3,125,125]{2,1,0} bitcast, $0[] constant_neg_inf), dimensions={2}, to_apply=max_computation
  broadcast = $0[1,3,125,125]{3,2,1,0} broadcast($0[3,125]{1,0} reduce), dimensions={1,2}
  ROOT subtract = $0[1,3,125,125]{3,2,1,0} subtract($0[1,3,125,125]{3,2,1,0} param_0, $0[1,3,125,125]{3,2,1,0} broadcast)
})";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Parameter())));
}

TEST_P(SoftmaxRewriterTritonTest,
       CanNotFuseSoftmaxDiamondWithNonConstantReducerIdentity) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}

ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  identity = $0[] parameter(1)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, identity), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  ROOT subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

TEST_P(SoftmaxRewriterTritonTest,
       CanNotFuseSoftmaxDiamondWithTritonIncompatibleRoot) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  divide = $0[127,125]{1,0} divide(param_0, broadcast)
  ROOT remainder = $0[127,125]{1,0} remainder(divide, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

TEST_P(SoftmaxRewriterTritonTest,
       CanNotFuseSoftmaxDiamondWithTritonIncompatibleReducer) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  floor_0 = $0[] floor(arg_0)
  ROOT maximum = $0[] maximum(floor_0, arg_1)
}

ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  ROOT subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

TEST_P(SoftmaxRewriterTritonTest,
       CanFuseSoftmaxDiamondWithLastDimensionBitcastAfterReduce) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}

ENTRY main {
  param_0 = $0[3,127,125]{2,1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[3,127]{1,0} reduce(param_0, constant_neg_inf), dimensions={2}, to_apply=max_computation
  bitcasted_reduce = $0[381]{0} bitcast(reduce)
  broadcast = $0[381,125]{1,0} broadcast(bitcasted_reduce), dimensions={0}
  bitcasted_broadcast = $0[3,127,125]{2,1,0} bitcast(broadcast)
  ROOT subtract = $0[3,127,125]{2,1,0} subtract(param_0, bitcasted_broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Parameter())));
}

TEST_P(SoftmaxRewriterTritonTest,
       CanNotFuseSoftmaxDiamondWithTransposeBitcast) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}

ENTRY main {
  param_0 = $0[1,127,125]{2,1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  bitcasted_param_0 = $0[127,1,125]{2,0,1} bitcast(param_0)
  reduce = $0[127,1]{0,1} reduce(bitcasted_param_0, constant_neg_inf), dimensions={2}, to_apply=max_computation
  broadcast = $0[127,1,125]{2,0,1} broadcast(reduce), dimensions={0,1}
  bitcasted_broadcast = $0[1,127,125]{2,1,0} bitcast(broadcast)
  ROOT subtract = $0[1,127,125]{2,1,0} subtract(param_0, bitcasted_broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

TEST_P(SoftmaxRewriterTritonTest,
       CanNotFuseTwoDiamondsWithDifferentReductionAxisSizeTogether) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
add_computation {
  arg_0.1 = $0[] parameter(0)
  arg_1.1 = $0[] parameter(1)
  ROOT add = $0[] add(arg_0.1, arg_1.1)
}
ENTRY main {
  param_0 = $0[127,625]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,625]{1,0} broadcast(reduce), dimensions={0}
  subtract = $0[127,625]{1,0} subtract(param_0, broadcast)
  bitcasted_subtract = $0[127,5,125] bitcast(subtract)
  exponential = $0[127,5,125] exponential(bitcasted_subtract)
  constant_zero = $0[] constant(0)
  second_reduce = $0[127,5] reduce(exponential, constant_zero), dimensions={2}, to_apply=add_computation
  second_broadcast = $0[127,5,125] broadcast(second_reduce), dimensions={0,1}
  ROOT divide = $0[127,5,125] divide(exponential, second_broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);

  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());

  switch (data_type) {
    case F32:
    case BF16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Fusion(m::Bitcast(m::Fusion(m::Parameter())))));
      break;
    case F16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Divide(m::Exp(), m::Broadcast())));
      break;
    default:
      ABSL_UNREACHABLE();
  }
}

TEST_P(SoftmaxRewriterTritonTest,
       CanNotFuseTwoDiamondsWithExtraUsageForFirstDiamondRoot) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
add_computation {
  arg_0.1 = $0[] parameter(0)
  arg_1.1 = $0[] parameter(1)
  ROOT add = $0[] add(arg_0.1, arg_1.1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
  exponential = $0[127,125]{1,0} exponential(subtract)
  constant_zero = $0[] constant(0)
  second_reduce = $0[127]{0} reduce(exponential, constant_zero), dimensions={1}, to_apply=add_computation
  second_broadcast = $0[127,125]{1,0} broadcast(second_reduce), dimensions={0}
  divide = $0[127,125]{1,0} divide(exponential, second_broadcast)
  ROOT tuple = ($0[127,125]{1,0}, $0[127,125]{1,0}) tuple(divide, subtract)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);

  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());

  switch (data_type) {
    case F32:
    case BF16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Tuple(m::Fusion(m::Fusion()),
                                      m::Fusion(m::Parameter()))));
      break;
    case F16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Tuple(m::Divide(), m::Fusion(m::Parameter()))));
      break;
    default:
      ABSL_UNREACHABLE();
  }
}

TEST_P(SoftmaxRewriterTritonTest,
       CanNotFuseTwoDiamondsWithExtraUsageForSecondDiamondProducer) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
add_computation {
  arg_0.1 = $0[] parameter(0)
  arg_1.1 = $0[] parameter(1)
  ROOT add = $0[] add(arg_0.1, arg_1.1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
  exponential = $0[127,125]{1,0} exponential(subtract)
  constant_zero = $0[] constant(0)
  second_reduce = $0[127]{0} reduce(exponential, constant_zero), dimensions={1}, to_apply=add_computation
  second_broadcast = $0[127,125]{1,0} broadcast(second_reduce), dimensions={0}
  divide = $0[127,125]{1,0} divide(exponential, second_broadcast)
  ROOT tuple = ($0[127,125]{1,0}, $0[127,125]{1,0}) tuple(divide, exponential)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);

  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());

  switch (data_type) {
    case F32:
    case BF16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Tuple(m::Fusion(m::Fusion()),
                                      m::Fusion(m::Parameter()))));
      break;
    case F16:
      EXPECT_THAT(module->entry_computation()->root_instruction(),
                  GmockMatch(m::Tuple(m::Divide(), m::Exp())));
      break;
    default:
      ABSL_UNREACHABLE();
  }
}

TEST_P(SoftmaxRewriterTritonTest,
       CanFuseSoftmaxDiamondWithTritonIncompatibleProducer) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}

ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  floor_0 = $0[127,125] floor(param_0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(floor_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  ROOT subtract = $0[127,125]{1,0} subtract(floor_0, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  VLOG(2) << module->ToString();
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Floor(m::Parameter()))));
}

TEST_P(SoftmaxRewriterTritonTest,
       CanNotFuseSoftmaxDiamondWithNonFusibleBitcastBetweenReduceAndProducer) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax

max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}

ENTRY main {
  param_0 = $0[1,127,5,25]{3,2,1,0} parameter(0)
  bitcast_0 = $0[127,125] bitcast(param_0)
  bitcast_1 = $0[127,125] bitcast(param_0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(bitcast_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  ROOT subtract = $0[127,125]{1,0} subtract(bitcast_1, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

TEST_P(SoftmaxRewriterTritonTest,
       CanFuseSoftmaxDiamondWithBitcastProducerFollowedByBitcastsOnEachUse) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax

max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}

ENTRY main {
  param_0 = $0[1,1,127,125]{3,2,1,0} parameter(0)
  bitcast_parent = $0[127,125]{1,0} bitcast(param_0)
  bitcast_0 = $0[127,125]{1,0} bitcast(bitcast_parent)
  bitcast_1 = $0[127,125]{1,0} bitcast(bitcast_parent)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(bitcast_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  ROOT subtract = $0[127,125]{1,0} subtract(bitcast_1, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  VLOG(2) << module->ToString();
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Parameter())));
}

TEST_P(
    SoftmaxRewriterTritonTest,
    CanNotFuseSoftmaxDiamondWithBitcastProducerFollowedByThreeBitcastsOnTheLeftIncludingTwoNonFusibleOnes) {  // NOLINT(whitespace/line_length)
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax

max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}

ENTRY main {
  param_0 = $0[1,1,127,125]{3,2,1,0} parameter(0)
  bitcast_parent = $0[127,125] bitcast(param_0)
  bitcast_0 = $0[127,5,25] bitcast(bitcast_parent)
  bitcast_1 = $0[1,127,125] bitcast(bitcast_0)
  bitcast_2 = $0[127,125] bitcast(bitcast_1)
  bitcast_3 = $0[127,125] bitcast(bitcast_parent)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(bitcast_3, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  ROOT subtract = $0[127,125]{1,0} subtract(bitcast_2, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

TEST_P(SoftmaxRewriterTritonTest, DoNotFuseSoftmaxWithSmallRows) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[127,50]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,50]{1,0} broadcast(reduce), dimensions={0}
  ROOT subtract = $0[127,50]{1,0} subtract(param_0, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

TEST_P(
    SoftmaxRewriterTritonTest,
    CanOnlyFuseConvertInvolvingBF16InputIntoSoftmaxDiamondWithAtLeastAmpereComputeCapability) {  // NOLINT(whitespace/line_length)
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = bf16[127,125]{1,0} parameter(0)
  param_0_$0 = $0[127,125]{1,0} convert(param_0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0_$0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  ROOT subtract = $0[127,125]{1,0} subtract(param_0_$0, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto ampere_module = ParseAndReturnVerifiedModule(hlo_string).value();
  auto volta_module = ampere_module->Clone();

  // Ampere
  SoftmaxRewriterTriton fusion_rewriter_ampere(
      se::CudaComputeCapability{se::CudaComputeCapability::AMPERE, 0});
  EXPECT_TRUE(fusion_rewriter_ampere.Run(ampere_module.get()).value());
  EXPECT_TRUE(verifier().Run(ampere_module.get()).status().ok());
  VLOG(2) << ampere_module->ToString();
  EXPECT_THAT(ampere_module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Parameter())));

  // Volta (pre-Ampere)
  SoftmaxRewriterTriton fusion_rewriter_volta(
      se::CudaComputeCapability{se::CudaComputeCapability::VOLTA, 0});
  EXPECT_TRUE(fusion_rewriter_volta.Run(volta_module.get()).value());
  EXPECT_TRUE(verifier().Run(volta_module.get()).status().ok());
  VLOG(2) << volta_module->ToString();
  EXPECT_THAT(volta_module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Convert(m::Parameter()))));
}

TEST_P(SoftmaxRewriterTritonTest, DoesNotFuseConvertWithC64DataType) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
  ROOT convert = c64[127,125]{1,0} convert(subtract)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  VLOG(2) << module->ToString();
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Convert(m::Fusion(m::Parameter()))));
}

TEST_P(SoftmaxRewriterTritonTest, DoesNotFuseConvertWithC128DataType) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
  ROOT convert = c128[127,125]{1,0} convert(subtract)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  VLOG(2) << module->ToString();
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Convert(m::Fusion(m::Parameter()))));
}

TEST_P(SoftmaxRewriterTritonTest,
       CanFuseBinaryElementwiseProducerIntoDiamondWhenBothOperandsAreTheSame) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule fusible_diamond
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  multiply =  $0[127,125]{1,0} multiply(param_0, param_0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(multiply, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  ROOT subtract = $0[127,125]{1,0} subtract(multiply, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  VLOG(2) << module->ToString();
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Parameter())));
}

TEST_P(
    SoftmaxRewriterTritonTest,
    CanFuseIntermediateBinaryElementwiseWithinDiamondWhenBothOperandsAreTheSame) {  // NOLINT(whitespace/line_length)
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule fusible_diamond
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  multiply =  $0[127]{0} multiply(reduce, reduce)
  broadcast = $0[127,125]{1,0} broadcast(multiply), dimensions={0}
  ROOT subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  VLOG(2) << module->ToString();
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Parameter())));
}

TEST_P(SoftmaxRewriterTritonTest,
       CanFuseBinaryElementwiseWhenBothOperandsAreTheSameBetweenDiamonds) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule fusible_diamonds
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
add_computation {
  arg_0.1 = $0[] parameter(0)
  arg_1.1 = $0[] parameter(1)
  ROOT add = $0[] add(arg_0.1, arg_1.1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
  multiply = $0[127,125]{1,0} multiply(subtract, subtract)
  constant_zero = $0[] constant(0)
  second_reduce = $0[127]{0} reduce(multiply, constant_zero), dimensions={1}, to_apply=add_computation
  second_broadcast = $0[127,125]{1,0} broadcast(second_reduce), dimensions={0}
  ROOT subtract_second = $0[127,125]{1,0} subtract(multiply, second_broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  VLOG(2) << module->ToString();
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Parameter())));
}

TEST_P(SoftmaxRewriterTritonTest,
       CanFuseBinaryElementwiseConsumerWhereBothOperandsAreTheSameIntoDiamond) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule fusible_diamond
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
add_computation {
  arg_0.1 = $0[] parameter(0)
  arg_1.1 = $0[] parameter(1)
  ROOT add = $0[] add(arg_0.1, arg_1.1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
  ROOT multiply = $0[127,125]{1,0} multiply(subtract, subtract)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  VLOG(2) << module->ToString();
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Parameter())));
}

TEST_P(
    SoftmaxRewriterTritonTest,
    DoesNotFuseIntermediateBinaryElementwiseWhereBothOperandsAreTheSameIntoDiamondWithoutTritonSupport) {  // NOLINT(whitespace/line_length)
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule softmax
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  remainder = $0[127,125]{1,0} remainder(param_0, param_0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(remainder, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  ROOT subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_FALSE(fusion_rewriter.Run(module.get()).value());
}

TEST_P(SoftmaxRewriterTritonTest,
       CanFuseTwoBinaryElementwiseWhereBothOperandsAreTheSameBetweenDiamonds) {
  PrimitiveType data_type = GetParam();
  const std::string hlo_string_template = R"(
HloModule fusible_diamonds
max_computation {
  arg_0 = $0[] parameter(0)
  arg_1 = $0[] parameter(1)
  ROOT maximum = $0[] maximum(arg_0, arg_1)
}
add_computation {
  arg_0.1 = $0[] parameter(0)
  arg_1.1 = $0[] parameter(1)
  ROOT add = $0[] add(arg_0.1, arg_1.1)
}
ENTRY main {
  param_0 = $0[127,125]{1,0} parameter(0)
  constant_neg_inf = $0[] constant(-inf)
  reduce = $0[127]{0} reduce(param_0, constant_neg_inf), dimensions={1}, to_apply=max_computation
  broadcast = $0[127,125]{1,0} broadcast(reduce), dimensions={0}
  subtract = $0[127,125]{1,0} subtract(param_0, broadcast)
  add = $0[127,125]{1,0} add(subtract, subtract)
  multiply = $0[127,125]{1,0} multiply(add, add)
  constant_zero = $0[] constant(0)
  second_reduce = $0[127]{0} reduce(multiply, constant_zero), dimensions={1}, to_apply=add_computation
  second_broadcast = $0[127,125]{1,0} broadcast(second_reduce), dimensions={0}
  ROOT subtract_second = $0[127,125]{1,0} subtract(multiply, second_broadcast)
}
)";
  const std::string hlo_string =
      absl::Substitute(hlo_string_template,
                       primitive_util::LowercasePrimitiveTypeName(data_type));

  auto module = ParseAndReturnVerifiedModule(hlo_string).value();
  SoftmaxRewriterTriton fusion_rewriter(gpu_version_);
  EXPECT_TRUE(fusion_rewriter.Run(module.get()).value());
  EXPECT_TRUE(verifier().Run(module.get()).status().ok());
  VLOG(2) << module->ToString();
  EXPECT_THAT(module->entry_computation()->root_instruction(),
              GmockMatch(m::Fusion(m::Parameter())));
}

INSTANTIATE_TEST_SUITE_P(SoftmaxRewriterTritonTestSuite,
                         SoftmaxRewriterTritonTest,
                         ::testing::Values(F32, F16));

}  // anonymous namespace
}  // namespace gpu
}  // namespace xla
