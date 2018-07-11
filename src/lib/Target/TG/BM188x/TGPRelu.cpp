#define DEBUG_TYPE "tg_prelu"
#include "TGPRelu.h"
#include "BM188xCodeEmitter.h"
#include <bmnet/targets/plat-bm188x/bmkernel/bmkernel_api.h>
#include <onnc/Support/Debug.h>

namespace onnc {
namespace BM188X {

TGPRelu::TGPRelu(const ::onnx::Node &pNode)
    : BM188xComputeOperator(pNode, std::string("PRelu"))
{
  const std::vector< ::onnx::Dimension> inDim = pNode.inputs()[0]->sizes();
  if (inDim.size() == 4) {
    m_N = inDim[0].dim;
    m_C = inDim[1].dim;
    m_H = inDim[2].dim;
    m_W = inDim[3].dim;
  } else if (inDim.size() == 2) {
    m_N = inDim[0].dim;
    m_C = 1;
    m_H = inDim[1].dim;
    m_W = 1;
  } else {
    assert(0 && "inDim.size() != 4 & !=2");
  }
}

TGPRelu *TGPRelu::addMemOperands(MemOperand *pInput, MemOperand *pSlope,
                                 MemOperand *pOutput)
{
  m_MemOperands.push_back(pInput);
  m_MemOperands.push_back(pSlope);
  m_MemOperands.push_back(pOutput);

  return this;
}

void TGPRelu::print(OStream &pOS) const
{
  pOS << *m_MemOperands[2] << " = PReLU <Slope:" << m_Slope << ", N:" << m_N
      << ", C:" << m_C << ", H:" << m_H << ", W:" << m_W << "> ("
      << *m_MemOperands[1] << ")\n";
}

void TGPRelu::emit() const
{
  DEBUG(print(dbgs()));

  bmnet::bmnet_asm::bmnet_prelu_fixed_forward_bmkernel(
      *bm1880_kernel::getInstance().m_CTX, // bmnet context
      m_MemOperands[0]->m_Addr,            // input_gaddr
      m_MemOperands[1]->m_Addr,            // slope_gaddr
      m_MemOperands[2]->m_Addr,            // output_gaddr
      m_ChannelShared,                     // channel_shared
      m_Slope,                             // slope
      m_N,                                 // input_n
      m_C,                                 // input_c
      m_H,                                 // input_h
      m_W,                                 // input_w
      m_GTScale,                           // GT_scale
      m_GTRShiftWidth,                     // GT_right_shift_width
      m_LERShiftWidth);                    // LE_right_shift_width
}

void TGPRelu::update(const tg::bm1880::LayerCalibrationParameter *pLayerCtable)
{
  const auto &prelu = pLayerCtable->prelu_param();

  m_GTRShiftWidth = prelu.gt_right_shift_width();
  m_LERShiftWidth = prelu.le_right_shift_width();
  m_GTScale = prelu.gt_scale();

  // get slope tensor to determine ChannelShared and Slope
  const ::onnx::Value *value = m_MemOperands[1]->m_Value;
  const ::onnx::Tensor &tensor =
      ::onnc::getTensor(value->uniqueName(), *value->owningGraph());
  assert(tensor.elem_type() == ::onnx::TensorProto_DataType_INT8);

  m_ChannelShared = (tensor.raw().size() == 1);
  m_Slope = tensor.raw()[0];
}

} // namespace BM188X
} // namespace onnc
