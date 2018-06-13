//===- MemoryAllocation.cpp -----------------------------------------------===//
//
//                             The ONNC Project
//
// See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
#include <onnc/Analysis/LivenessAnalysis.h>
#include <onnc/Analysis/MemoryAllocation.h>
#include <onnc/Analysis/NodeIRScheduler.h>
#include <onnc/Analysis/SplitNode.h>
#include <onnc/Analysis/UpdateGraphOutputSize.h>
#include <onnc/Core/InitializePasses.h>
#include <onnc/Core/AnalysisResolver.h>
#include <onnc/Core/AnalysisUsage.h>
#include <onnc/Core/PassAnalysisSupport.h>
#include <onnc/Support/IOStream.h>
#include <onnc/Target/TargetTransformInfo.h>

using namespace onnc;

using TensorSizes = std::vector<onnx::Dimension>;

//===----------------------------------------------------------------------===//
// MemoryAllocation
//===----------------------------------------------------------------------===//
MemoryAllocation::MemoryAllocation(DLATargetBackend* pDLATB)
  : ModulePass(ID), m_MemAllocList(), m_DLATB(pDLATB) {
}

MemoryAllocation::~MemoryAllocation()
{
  clear();
}

struct MemRegion {
  size_t start, size;
  MemRegion(size_t pStart, size_t pSize)
    : start(pStart), size(pSize) {
  }
  MemRegion() {}
};

using MemRegionList = std::vector<MemRegion>;

static MemRegionList GetUsedMemRegions(const MemAllocList& pAllocs,
                                       const LiveInterval &pIntrvl)
{
  MemRegionList regions;
  for (auto entry : pAllocs) {
    const LiveInterval& liveIntrvl = entry->liveIntrvl;

    if (!liveIntrvl.intersect(pIntrvl))
      continue;

    regions.emplace_back(entry->startAddr, entry->size);
  }

  // sort by starting address
  std::sort(regions.begin(), regions.end(),
            [] (const MemRegion& ra, const MemRegion& rb) {
              return ra.start < rb.start;
            });
  return regions;
}

static bool HasConflict(size_t pStartA, size_t pSizeA,
                        size_t pStartB, size_t pSizeB)
{
  size_t endA = pStartA + pSizeA,
         endB = pStartB + pSizeB;

  return !(endA <= pStartB || endB <= pStartA);
}

size_t MemoryAllocation::allocByLiveness(ValMemSizeMap &pValMemSizeMap)
{
  clear();

  GraphLivenessAnalysis *liveAnaly = getAnalysis<GraphLivenessAnalysis>();

  // by liverange analysis, we can get minimun requirement memory size.
  size_t minSize = 0;

  // allocate memory considering liveness.
  auto &livesInfo = liveAnaly->getLiveIntervals();
  for (const LiveInterval* li : livesInfo) {
    auto v = &li->getValue();
    if (!pValMemSizeMap.count(v))
      continue;

    size_t required = pValMemSizeMap[v].size,
           startAddr = 0;

    MemRegionList conflicts = GetUsedMemRegions(m_MemAllocList, *li);

    // Note: conflicts has been sorted by starting address in GetUsedMemRegions.
    for (const MemRegion &reg : conflicts) {
      if (!HasConflict(reg.start, reg.size, startAddr, required))
        break;
      startAddr = reg.start + reg.size;
    }

    // Allocate new memory region.
    m_MemAllocList.push_back(new MemAllocEntry(startAddr,
                                               required, *li));
    minSize = std::max(minSize, startAddr + required);
  }
  return minSize;
}

Pass::ReturnType MemoryAllocation::runOnModule(Module& pModule)
{
  if (!m_DLATB) {
    errs() << "No backend infomation that is needed for memory allcation.\n";
    return kPassFailure;
  }

  GraphLivenessAnalysis *liveAnaly = getAnalysis<GraphLivenessAnalysis>();
  NodeIRScheduler *scheduler = getAnalysis<NodeIRScheduler>();

  scheduler->runOnModule(pModule);
  liveAnaly->runOnModule(pModule);

  clear();

  SplitGraphManager sgMgr(*pModule.getGraph(), *m_DLATB);

  const uint64_t localMemSize = m_DLATB->getMemInfo()
                                       ->getLocalMemSize();
  std::vector<SplitGraph*> worklist(sgMgr.getSplitGraphs());
  while (!worklist.empty()) {
    SplitGraph *spGraph = worklist.back();
    worklist.pop_back();

    // per graph
    int64_t prevMinSize = 0;
    const float threshold = 0.9f; // 90% splitting threshold.
    errs() << "allocate or shrink size: ";
    while (true) {
      ValMemSizeMap valMemSMap;
      spGraph->getMemUsage(valMemSMap);

      // get maximum requirements.
      uint64_t maxSize = 0;
      for (auto & req : valMemSMap)
        maxSize += req.second.size;

      // Try to allocate.
      uint64_t minSize = allocByLiveness(valMemSMap);
      if (minSize < localMemSize)
        break;

      // If new allocation is not smaller enough than previous allocation, then
      // create new sub graph
      if (prevMinSize && (float)minSize / prevMinSize > threshold) {
        prevMinSize = 0;
        spGraph->resetToOrigSize();

        SplitGraph *newSpGraph = sgMgr.splitNewSubGraph(*spGraph);

        if (newSpGraph) {
          worklist.push_back(newSpGraph);
          continue;
        }
        else {
          errs() << "[MemoryAllocation] Unable to allocate memory for group.\n";
          break;
        }
      }
      prevMinSize = minSize;
      errs() << " -> " << (float)prevMinSize / 1024.f << " kb";

      spGraph->shrinkSize();
    }
    outs() << "\n";
  }
  sgMgr.dump();
  return kModuleNoChanged;
}

void MemoryAllocation::getAnalysisUsage(AnalysisUsage& pUsage) const
{
  pUsage.addRequiredID(NodeIRScheduler::ID);
  pUsage.addRequiredID(GraphLivenessAnalysis::ID);
  pUsage.addRequiredID(UpdateGraphOutputSize::ID);
}

void MemoryAllocation::print(OStream& pOS) const
{
  for (const MemAllocEntry *e : m_MemAllocList) {
    const LiveInterval &li = e->liveIntrvl;
    pOS << li.getValue().uniqueName() << ": \t"
        << "[" << e->startAddr << ", " << e->startAddr + e->size << ")\t"
        << "(total: " << e->size << ")\t"
        << " [" << li.getStart() << ", " << li.getEnd() << "]"
        << "\n";
  }
}

void MemoryAllocation::clear()
{
  for (MemAllocEntry* entry: m_MemAllocList) {
    delete entry;
  }
  m_MemAllocList.clear();
}

//===----------------------------------------------------------------------===//
// Factory method
//===----------------------------------------------------------------------===//
char MemoryAllocation::ID = 0;

namespace onnc
{
  INITIALIZE_DLA_PASS(MemoryAllocation, "MemoryAllocation")
}

MemoryAllocation* onnc::CreateMemoryAllocationPass(DLATargetBackend* pDLATB)
{
  return new MemoryAllocation(pDLATB);
}
