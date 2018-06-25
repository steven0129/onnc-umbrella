//===- Not.cpp ------------------------------------------------===//
//
//                             The ONNC Project
//
// See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include <onnc/IR/Compute/Not.h>

using namespace onnc;

//===----------------------------------------------------------------------===//
// Not
//===----------------------------------------------------------------------===//
Not::Not()
  : ComputeOperator("Not") {
}



void Not::print(std::ostream& pOS) const
{
}