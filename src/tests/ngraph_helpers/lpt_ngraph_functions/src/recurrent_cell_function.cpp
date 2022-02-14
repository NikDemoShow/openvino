// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "lpt_ngraph_functions/recurrent_cell_function.hpp"

#include <ngraph/opsets/opset1.hpp>
#include "ngraph_ops/type_relaxed.hpp"
#include "low_precision/network_helper.hpp"
#include "low_precision/rt_info/precision_preserved_attribute.hpp"
#include "low_precision/rt_info/intervals_alignment_attribute.hpp"
#include "low_precision/rt_info/quantization_alignment_attribute.hpp"

#include "ngraph_functions/builders.hpp"
#include "lpt_ngraph_functions/common/builders.hpp"
#include "lpt_ngraph_functions/common/fake_quantize_on_data.hpp"
#include "lpt_ngraph_functions/common/dequantization_operations.hpp"
#include "lpt_ngraph_functions/common/builders.hpp"

namespace ngraph {
namespace builder {
namespace subgraph {

using namespace ngraph::pass;

std::shared_ptr<ngraph::Function> RecurrentCellFunction::get(
    const ngraph::element::Type inputPrecision,
    const std::vector<ngraph::PartialShape>& inputActivationsShapes,
    const std::vector<ngraph::Shape>& inputWeightsShapes,
    const RNNType type,
    const std::vector<FakeQuantizeOnDataWithConstant>& fqOnDatas,
    const std::vector<DequantizationOperations::Convert>& converts,
    const std::vector<DequantizationOperations>& dequantizations,
    const std::vector<ov::Any>& concatAttributes,
    const ngraph::element::Type precisionAfterOperation,
    const DequantizationOperations& dequantizationAfter) {
    auto X = std::make_shared<opset5::Parameter>(inputPrecision, inputActivationsShapes[0]);
    X->set_friendly_name("X");
    std::shared_ptr<Node> parent_X = makeQuantizationAndDequantization(X,
                                                                       inputPrecision,
                                                                       X->get_friendly_name(),
                                                                       fqOnDatas[0],
                                                                       converts[0],
                                                                       dequantizations[0]);
    auto H = std::make_shared<opset5::Parameter>(inputPrecision, inputActivationsShapes[1]);
    H->set_friendly_name("H");
    std::shared_ptr<Node> parent_H = makeQuantizationAndDequantization(H,
                                                                       inputPrecision,
                                                                       H->get_friendly_name(),
                                                                       fqOnDatas[1],
                                                                       converts[1],
                                                                       dequantizations[1]);
    auto C = std::make_shared<opset5::Parameter>(inputPrecision, inputActivationsShapes[2]);
    C->set_friendly_name("C");

    auto W = ngraph::opset5::Constant::create(fqOnDatas[2].empty() ? ngraph::element::u8 : inputPrecision,
                                              inputWeightsShapes[0],
                                              {1});
    std::shared_ptr<Node> parent_W = makeQuantizationAndDequantization(W,
                                                                       inputPrecision,
                                                                       W->get_friendly_name(),
                                                                       fqOnDatas[2],
                                                                       converts[2],
                                                                       dequantizations[2]);
    auto R = ngraph::opset5::Constant::create(fqOnDatas[2].empty() ? ngraph::element::u8 : inputPrecision,
                                              inputWeightsShapes[1],
                                              {1});
    std::shared_ptr<Node> parent_R = makeQuantizationAndDequantization(R,
                                                                       inputPrecision,
                                                                       R->get_friendly_name(),
                                                                       fqOnDatas[3],
                                                                       converts[3],
                                                                       dequantizations[3]);
    auto B = ngraph::opset5::Constant::create(inputPrecision, inputWeightsShapes[2], {1});
    auto seq_lengths = ngraph::opset5::Constant::create(element::i32, Shape{1}, {3});

    std::shared_ptr<ov::op::util::RNNCellBase> rnn_layer;
    switch (type) {
    case RNNType::LSTMCell:
        rnn_layer = std::make_shared<opset5::LSTMCell>(parent_X,
                                                       parent_H,
                                                       C,
                                                       parent_W,
                                                       parent_R,
                                                       B,
                                                       128);
        rnn_layer->set_friendly_name("lstm_cell");
        break;
    case RNNType::LSTMSequence:
        rnn_layer = std::make_shared<opset5::LSTMSequence>(parent_X,
                                                           parent_H,
                                                           C,
                                                           seq_lengths,
                                                           parent_W,
                                                           parent_R,
                                                           B,
                                                           128,
                                                           op::RecurrentSequenceDirection::FORWARD);
        rnn_layer->set_friendly_name("lstm_sequense");
        break;
    case RNNType::GRU:
        rnn_layer = std::make_shared<opset4::GRUCell>(parent_X,
                                                      parent_H,
                                                      parent_W,
                                                      parent_R,
                                                      3);
        rnn_layer->set_friendly_name("gru_cell");
        break;
    case RNNType::RNNCell:
        rnn_layer = std::make_shared<opset4::RNNCell>(parent_X,
                                                      parent_H,
                                                      parent_W,
                                                      parent_R,
                                                      3);
        rnn_layer->set_friendly_name("rnn_layer");
        break;
    default:
        break;
    }

    auto& rtInfo = rnn_layer->get_rt_info();
    bool is_lstm = type == RNNType::LSTMCell || type == RNNType::LSTMSequence;
    rtInfo["Variant::std::string"] = "rnn_layer";

    auto rnn_layer_res_1 = std::make_shared<opset5::Result>(rnn_layer->output(0));
    rnn_layer_res_1->set_friendly_name("output_1");
    std::shared_ptr<ov::op::v0::Result> rnn_layer_res_2 = {};
    if (is_lstm) {
        rnn_layer_res_2 = std::make_shared<opset5::Result>(rnn_layer->output(1));
        rnn_layer_res_2->set_friendly_name("output_2");
    }

    ngraph::ResultVector results{rnn_layer_res_2 ? rnn_layer_res_1, rnn_layer_res_2 : rnn_layer_res_1};
    std::shared_ptr<ngraph::Function> function = std::make_shared<ngraph::Function>(
        results,
        is_lstm ? ngraph::ParameterVector{X, H, C} : ngraph::ParameterVector{X, H},
        "LSTMTransformation");

    return function;
}

std::shared_ptr<Node> makeQuantizationAndDequantization(const std::shared_ptr<Node> input,
                                                        const ngraph::element::Type inputPrecision,
                                                        const std::string friendly_name,
                                                        const FakeQuantizeOnDataWithConstant& fqOnData,
                                                        const DequantizationOperations::Convert& convert,
                                                        const DequantizationOperations& dequantization) {
    std::shared_ptr<Node> parent;
    if (fqOnData.empty()) {
        parent = input;
    } else {
        std::shared_ptr<Node> fakeQuantize1 = makeFakeQuantizeTypeRelaxed(input, inputPrecision, fqOnData);
        fakeQuantize1->set_friendly_name("fakeQuantize_" + friendly_name);
        parent = fakeQuantize1;
    }
    if (!convert.empty()) {
        parent = std::make_shared<opset1::Convert>(parent, convert.outPrecision);
    }
    if (!dequantization.empty()) {
        parent = makeDequantization(parent, dequantization);
    }
    return parent;
}

}  // namespace subgraph
}  // namespace builder
}  // namespace ngraph
