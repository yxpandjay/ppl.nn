// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "ppl/nn/engines/cuda/optimizer/algos/algo_gemm.h"

#include <chrono>

#include "ppl/common/cuda/cuda_types.h"
#include "cudakernel/nn/conv/conv_fp16.h"
#include "cudakernel/gemm/gemm.h"
#include "ppl/nn/common/logger.h"
#include "ppl/nn/utils/utils.h"

using namespace ppl::common;

namespace ppl { namespace nn { namespace cuda {

void GemmAlgorithm::DeleteAttrParam(void*& param) {
    delete (CudaGemmParam*)param;
    return;
}

void GemmAlgorithm::GetAttrParam(void*& param) const {
    if (param == nullptr) {
        param = new CudaGemmParam();
    }
    *(CudaGemmParam*)param = attr_param_;
    return;
}

double GemmAlgorithm::ExcuteTimer(const ir::Node* node, OptKernelOptions& options) {
    this->attr_param_ = *(reinterpret_cast<CudaGemmParam*>(options.param));
    options.compile_set->emplace(node->GetId());
    if (node->GetInputCount() == 3) {
        attr_param_.param.bias_term = 1;
    }

    auto shape_in0 = options.tensors->find(node->GetInput(0))->second->GetShape();
    auto shape_in1 = options.tensors->find(node->GetInput(1))->second->GetShape();
    auto shape_in2 = TensorShape();
    auto shape_out = options.tensors->find(node->GetOutput(0))->second->GetShape();
    auto align_size = ppl::common::cuda::GetDataFormatChannelAlignment(shape_in0.GetDataFormat());

    // illegal gemm input
    if (shape_in0.GetDim(!attr_param_.param.transA) != shape_in1.GetDim(attr_param_.param.transB)) {
        shape_in0.SetDim(!attr_param_.param.transA, shape_in1.GetDim(attr_param_.param.transB));
    }

    // input N is too small
    if (shape_in0.GetDim(attr_param_.param.transA) < 1) {
        shape_in0.SetDim(attr_param_.param.transA, 1);
        shape_out.SetDim(0, 1);
    }

    conv_param_t temp_conv_param;
    fuse_param_t temp_fuse_param;
    temp_conv_param.in_num = attr_param_.param.transA ? shape_in0.GetDim(1) : shape_in0.GetDim(0);
    temp_conv_param.num_chl = attr_param_.param.transB ? shape_in1.GetDim(1) : shape_in1.GetDim(0);
    temp_conv_param.num_flt = attr_param_.param.transB ? shape_in1.GetDim(0) : shape_in1.GetDim(1);
    temp_conv_param.in_height = 1;
    temp_conv_param.in_width = 1;
    temp_conv_param.flt_height = 1;
    temp_conv_param.flt_width = 1;
    temp_conv_param.out_height = 1;
    temp_conv_param.out_width = 1;
    temp_conv_param.pad_height = 1;
    temp_conv_param.pad_width = 1;
    temp_conv_param.stride_height = 1;
    temp_conv_param.stride_width = 1;
    temp_conv_param.hole_height = 1;
    temp_conv_param.hole_width = 1;
    temp_conv_param.num_grp = 1;

    std::string key_str = node->GetName();
    auto algo_info = options.algos->find(key_str);
    if (algo_info != options.algos->end()) {
        attr_param_.extra_param.algo_info.algo_name = algo_info->second.kname;
        attr_param_.extra_param.algo_info.kid = algo_info->second.kid;
        attr_param_.extra_param.algo_info.splitk = algo_info->second.splitk;
        attr_param_.extra_param.algo_info.splitf = algo_info->second.splitf;
        PPLCUDAConvolutionLoadAlgoParam(attr_param_.extra_param.algo_info);
        return 0.0f;
    } else { // Give the default kernel
        attr_param_.extra_param.algo_info.algo_name = "nv2spkConv_hmma1688_nhwc_f1_b128x128_w64x64_k32_s32_buf1";
        attr_param_.extra_param.algo_info.kid = 0;
        attr_param_.extra_param.algo_info.splitk = 1;
        attr_param_.extra_param.algo_info.splitf = 1;
        PPLCUDAConvolutionLoadAlgoParam(attr_param_.extra_param.algo_info);
    }

    if (options.args->quick_select) {
        return 0.0f;
    }

    // Padding
    shape_in1.SetDim(0, (shape_in1.GetDim(0) + align_size - 1) / align_size * align_size);
    shape_in1.SetDim(1, (shape_in1.GetDim(1) + align_size - 1) / align_size * align_size);
    if (attr_param_.param.bias_term) {
        shape_in2 = options.tensors->find(node->GetInput(2))->second->GetShape();
        shape_in2.SetDim(0, (shape_in2.GetDim(0) + align_size - 1) / align_size * align_size);
    }

    RetCode status;
    ALLOC_BUFFERF_FOR_ALGO_SELECT(input_buffer, shape_in0.GetBytesIncludingPadding(), ALGO_MAX_TIME)
    ALLOC_BUFFERF_FOR_ALGO_SELECT(weight_buffer, shape_in1.GetBytesIncludingPadding(), ALGO_MAX_TIME)
    ALLOC_BUFFERF_FOR_ALGO_SELECT(bias_buffer, shape_in2.GetBytesIncludingPadding(), ALGO_MAX_TIME)
    ALLOC_BUFFERF_FOR_ALGO_SELECT(output_buffer, shape_out.GetBytesIncludingPadding(), ALGO_MAX_TIME)

    uint64_t size = PPLGemmCUDAGetBufSize(&shape_in0, attr_param_.param.transA);
    ALLOC_BUFFERF_FOR_ALGO_SELECT(temp_buffer, size, ALGO_MAX_TIME)

    auto stream = options.device->GetStream();

#ifdef PPLNN_ENABLE_CUDA_JIT
    // Do select
    LOG(INFO) << "Compiling " << node->GetName();
    int device_id = options.device->GetDeviceId();
    PPLCUDAConvolutionPredictKernel(attr_param_.extra_param.algo_info, temp_conv_param);
    auto timer = PPLCUDAGemmJITSelectKernel(device_id, stream, shape_in0.GetDataType(), &shape_in0, input_buffer.addr, &shape_in1,
                                            weight_buffer.addr, bias_buffer.addr, &shape_out, output_buffer.addr,
                                            temp_buffer.addr, temp_conv_param, temp_fuse_param,
                                            attr_param_.extra_param.algo_info);
#else
    // Do Select
    auto timer = PPLCUDAGemmSelectKernel(stream, &shape_in0, input_buffer.addr, &shape_in1, weight_buffer.addr,
                                         bias_buffer.addr, &shape_out, output_buffer.addr, temp_buffer.addr,
                                         attr_param_.param, temp_fuse_param, attr_param_.extra_param.algo_info);
#endif
    CudaArgs::AlgoSelects algo_select;
    algo_select.kname  = attr_param_.extra_param.algo_info.algo_name;
    algo_select.kid    = attr_param_.extra_param.algo_info.kid;
    algo_select.splitk = attr_param_.extra_param.algo_info.splitk;
    algo_select.splitf = attr_param_.extra_param.algo_info.splitf;
    options.algos->emplace(key_str, std::move(algo_select));
    LoadAlgoInfo(options.args->save_algo_path, attr_param_.extra_param.algo_info, key_str);
    return timer;
}

RetCode GemmAlgorithm::ModifyParam(const ir::Node* node, OptKernelOptions& options) {
    this->attr_param_ = *(reinterpret_cast<CudaGemmParam*>(options.param));
    auto topo = options.graph->topo.get();
    auto data = options.graph->data.get();
    auto weight_edge = topo->GetEdgeById(node->GetInput(1));
    auto weight_node = topo->GetNodeById(weight_edge->GetProducer());

    auto shape_in0 = options.tensors->find(node->GetInput(0))->second->GetShape();
    auto shape_in1 = options.tensors->find(node->GetInput(1))->second->GetShape();
    auto shape_out = options.tensors->find(node->GetOutput(0))->second->GetShape();
    auto align_size = ppl::common::cuda::GetDataFormatChannelAlignment(shape_in0.GetDataFormat());

    // Transpose weight if needed
    auto stream = options.device->GetStream();
    auto weight_iter = data->constants.find(weight_node->GetInput(0));
    if (weight_iter != data->constants.end() && // is a constant tensor and has not be loaded
        options.info->constants.find(weight_node->GetInput(0)) == options.info->constants.end()) {
        auto preedge_id = weight_node->GetInput(0);
        auto postedge_id = node->GetInput(1);
        auto preshape = options.tensors->find(preedge_id)->second->GetShape();
        auto postshape = options.tensors->find(postedge_id)->second->GetShape();
        auto newshape = postshape;

        if (!attr_param_.param.transB) {
            postshape.SetDim(0, newshape.GetDim(1));
            postshape.SetDim(1, newshape.GetDim(0));
        }

        newshape.SetDim(0, (postshape.GetDim(0) + align_size - 1) / align_size * align_size);
        newshape.SetDim(1, (postshape.GetDim(1) + align_size - 1) / align_size * align_size);
        newshape.SetPadding0(0, 0);
        newshape.SetPadding0(1, 0);

        RuntimeConstantInfo weight_constat_info;
        {
            BufferDesc buffer;
            auto status = options.device->Realloc(newshape, &buffer);
            if (status != RC_SUCCESS) {
                LOG(ERROR) << "alloc buffer for constant failed: " << GetRetCodeStr(status);
                return status;
            }

            weight_constat_info.Reshape(postshape);
            weight_constat_info.SetBuffer(buffer, options.device, true);
        }

        BufferDesc temp_weight_buffer;
        auto status = options.device->Realloc(newshape, &temp_weight_buffer);
        if (status != RC_SUCCESS) {
            LOG(ERROR) << "alloc buffer for constant failed: " << GetRetCodeStr(status);
            return status;
        }
        BufferDescGuard __device_src_guard__(&temp_weight_buffer, [&options](BufferDesc* buffer) {
            options.device->Free(buffer);
        });

        status = options.device->GetDataConverter()->ConvertFromHost(&weight_constat_info.GetBufferDesc(), postshape,
                                                                     weight_iter->second.data.data(), preshape);
        if (status != RC_SUCCESS) {
            LOG(ERROR) << node->GetName() << " copy constant failed: " << GetRetCodeStr(status);
            return status;
        }

        status = PPLCUDAGemmModifyWeights(stream, &newshape, weight_constat_info.GetBufferPtr(),
                                          temp_weight_buffer.addr, &attr_param_.param);

        reinterpret_cast<CudaGemmParam*>(options.param)->param.transB = 1;
        options.info->constants.emplace(preedge_id, std::move(weight_constat_info));
        options.tensors->find(preedge_id)->second->GetShape() = postshape;
        options.quants->at(preedge_id).format = postshape.GetDataFormat();
        options.quants->at(preedge_id).type = postshape.GetDataType();
    } else {
        reinterpret_cast<CudaGemmParam*>(options.param)->extra_param.is_initializer_weight = 0;
    }

    if (attr_param_.param.bias_term == 0) {
        return RC_SUCCESS;
    }

    // scale bias
    auto bias_edge = topo->GetEdgeById(node->GetInput(2));
    auto bias_node = topo->GetNodeById(bias_edge->GetProducer());
    auto bias_iter = data->constants.find(bias_node->GetInput(0));

    if (bias_iter != data->constants.end() && // is a constant tensor and has not be loaded
        options.info->constants.find(bias_node->GetInput(0)) == options.info->constants.end()) {
        auto preedge_id = bias_node->GetInput(0);
        auto postedge_id = node->GetInput(2);
        auto preshape = options.tensors->find(preedge_id)->second->GetShape();
        auto postshape = options.tensors->find(postedge_id)->second->GetShape();
        auto newshape = postshape;

        newshape.SetDim(0, (postshape.GetDim(0) + align_size - 1) / align_size * align_size);

        RuntimeConstantInfo bias_constat_info;
        {
            BufferDesc buffer;
            auto status = options.device->Realloc(newshape, &buffer);
            if (status != RC_SUCCESS) {
                LOG(ERROR) << "alloc buffer for constant failed: " << GetRetCodeStr(status);
                return status;
            }

            bias_constat_info.Reshape(postshape); // give the init shape, but the actual shape is padded
            bias_constat_info.SetBuffer(buffer, options.device, true);
        }

        auto status = options.device->GetDataConverter()->ConvertFromHost(&bias_constat_info.GetBufferDesc(), postshape,
                                                                          bias_iter->second.data.data(), preshape);
        if (status != RC_SUCCESS) {
            LOG(ERROR) << "copy constant failed: " << GetRetCodeStr(status);
            return status;
        }

        status = PPLCUDAGemmModifyBias(stream, &newshape, bias_constat_info.GetBufferDesc().addr, &attr_param_.param);

        options.info->constants.emplace(preedge_id, std::move(bias_constat_info));
        options.tensors->find(preedge_id)->second->GetShape() = postshape;
        options.quants->at(preedge_id).format = postshape.GetDataFormat();
        options.quants->at(preedge_id).type = postshape.GetDataType();
    }

    return RC_SUCCESS;
}

void GemmAlgorithm::ReshapeOnEdges(const ir::Node* node, std::map<edgeid_t, std::unique_ptr<TensorImpl>>* tensors,
                                   dataformat_t input_format, dataformat_t output_format) {
    for (uint32_t i = 0; i < node->GetInputCount(); ++i) { // only reset formats of input0 and weight
        auto edge_id = node->GetInput(i);
        if (edge_id == INVALID_EDGEID) {
            continue;
        }
        auto shape = &tensors->find(edge_id)->second->GetShape();
        if (shape->GetDimCount() > 1) {
            shape->SetDataFormat(input_format);
        } else {
            shape->SetDataFormat(DATAFORMAT_NDARRAY);
        }
    }

    for (uint32_t i = 0; i < node->GetOutputCount(); ++i) {
        auto edge_id = node->GetOutput(i);
        auto shape = &tensors->find(edge_id)->second->GetShape();
        shape->SetDataFormat(output_format);
    }
    return;
}

}}} // namespace ppl::nn::cuda
