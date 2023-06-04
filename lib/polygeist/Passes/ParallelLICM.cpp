#include "PassDetails.h"

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/OpenMP/OpenMPDialect.h"
#include "mlir/Dialect/SCF/Passes.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "mlir/IR/Dominance.h"
#include "mlir/IR/Matchers.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "polygeist/Passes/Passes.h"
#include <mlir/Dialect/Arithmetic/IR/Arithmetic.h>

#define DEBUG_TYPE "parallel-licm"

using namespace mlir;
using namespace mlir::func;
using namespace mlir::arith;
using namespace polygeist;

namespace {
struct ParallelLICM : public ParallelLICMBase<ParallelLICM> {
  void runOnOperation() override;
};
} // namespace

bool isReadOnly(Operation *);

static bool canBeParallelHoisted(Operation *op, Operation *scope,
                                 SmallPtrSetImpl<Operation *> &willBeMoved) {
  // Helper to check whether an operation is loop invariant wrt. SSA properties.
  auto definedOutside = [&](Value value) {
    if (auto BA = value.dyn_cast<BlockArgument>())
      if (willBeMoved.count(BA.getOwner()->getParentOp()))
        return true;
    auto *definingOp = value.getDefiningOp();
    if ((definingOp && !!willBeMoved.count(definingOp)) ||
        cast<LoopLikeOpInterface>(scope).isDefinedOutsideOfLoop(value))
      return true;
    return false;
  };

  // Check that dependencies are defined outside of loop.
  if (!llvm::all_of(op->getOperands(), definedOutside))
    return false;

  if (auto memEffect = dyn_cast<MemoryEffectOpInterface>(op)) {
    SmallVector<MemoryEffects::EffectInstance, 1> effects;
    memEffect.getEffects(effects);

    SmallVector<SideEffects::Resource *> readResources;
    SmallVector<SideEffects::Resource *> writeResources;
    SmallVector<SideEffects::Resource *> freeResources;
    for (auto effect : effects) {
      if (isa<MemoryEffects::Allocate>(effect.getEffect()))
        return false;
      if (isa<MemoryEffects::Read>(effect.getEffect()))
        readResources.push_back(effect.getResource());
      if (isa<MemoryEffects::Write>(effect.getEffect()))
        writeResources.push_back(effect.getResource());
      if (isa<MemoryEffects::Free>(effect.getEffect()))
        freeResources.push_back(effect.getResource());
    }

    std::function<bool(Operation *)> conflicting = [&](Operation *b) {
      if (willBeMoved.count(b))
        return false;

      if (b->hasTrait<OpTrait::HasRecursiveSideEffects>()) {

        for (auto &region : b->getRegions()) {
          for (auto &block : region) {
            for (auto &innerOp : block)
              if (conflicting(&innerOp))
                return true;
          }
        }
        return false;
      }

      auto memEffect = dyn_cast<MemoryEffectOpInterface>(b);
      if (!memEffect)
        return true;
      for (auto res : readResources) {
        SmallVector<MemoryEffects::EffectInstance> effects;
        memEffect.getEffectsOnResource(res, effects);
        for (auto effect : effects) {
          if (isa<MemoryEffects::Allocate>(effect.getEffect()))
            return true;
          if (isa<MemoryEffects::Write>(effect.getEffect()))
            return true;
        }
      }
      for (auto res : writeResources) {
        SmallVector<MemoryEffects::EffectInstance> effects;
        memEffect.getEffectsOnResource(res, effects);
        for (auto effect : effects) {
          if (isa<MemoryEffects::Allocate>(effect.getEffect()))
            return true;
          if (isa<MemoryEffects::Read>(effect.getEffect()))
            return true;
        }
      }
      for (auto res : freeResources) {
        SmallVector<MemoryEffects::EffectInstance> effects;
        memEffect.getEffectsOnResource(res, effects);
        for (auto effect : effects) {
          if (isa<MemoryEffects::Allocate>(effect.getEffect()))
            return true;
          if (isa<MemoryEffects::Write>(effect.getEffect()))
            return true;
          if (isa<MemoryEffects::Read>(effect.getEffect()))
            return true;
        }
      }
      return false;
    };

    std::function<bool(Operation *)> hasConflictBefore = [&](Operation *b) {
      for (Operation *it = b->getPrevNode(); it != nullptr;
           it = it->getPrevNode()) {
        if (conflicting(it)) {
          return true;
        }
      }

      if (b->getParentOp() == scope)
        return false;
      if (hasConflictBefore(b->getParentOp()))
        return true;

      bool conflict = false;
      // If the parent operation is not guaranteed to execute its (single-block)
      // region once, walk the block.
      if (!isa<scf::IfOp, AffineIfOp, memref::AllocaScopeOp>(b))
        b->walk([&](Operation *in) {
          if (conflict)
            return WalkResult::interrupt();
          if (conflicting(in)) {
            conflict = true;
            return WalkResult::interrupt();
          }
          return WalkResult::advance();
        });

      return conflict;
    };
    if ((readResources.size() || writeResources.size() ||
         freeResources.size()) &&
        hasConflictBefore(op))
      return false;
  } else if (!op->hasTrait<OpTrait::HasRecursiveSideEffects>())
    return false;

  // Recurse into the regions for this op and check whether the contained ops
  // can be hoisted.
  // We can inductively assume that this op will have its block args available
  // outside the loop
  SmallPtrSet<Operation *, 2> willBeMoved2(willBeMoved.begin(),
                                           willBeMoved.end());
  willBeMoved2.insert(op);
  /*
  for (auto &region : op->getRegions())
    for (auto &block : region)
       for (auto arg : block.getArguments())
           willBeMoved2.insert(&arg);
           */

  for (auto &region : op->getRegions()) {
    for (auto &block : region) {
      for (auto &innerOp : block)
        if (!canBeParallelHoisted(&innerOp, scope, willBeMoved2)) {
          return false;
        } else
          willBeMoved2.insert(&innerOp);
    }
  }
  return true;
}

LogicalResult moveParallelLoopInvariantCode(scf::ParallelOp looplike) {
  auto &loopBody = looplike.getLoopBody();

  // We use two collections here as we need to preserve the order for insertion
  // and this is easiest.
  SmallPtrSet<Operation *, 8> willBeMovedSet;
  SmallVector<Operation *, 8> opsToMove;

  // Do not use walk here, as we do not want to go into nested regions and hoist
  // operations from there. These regions might have semantics unknown to this
  // rewriting. If the nested regions are loops, they will have been processed.
  for (auto &block : loopBody) {
    for (auto &op : block.without_terminator()) {
      if (canBeParallelHoisted(&op, looplike, willBeMovedSet)) {
        opsToMove.push_back(&op);
        willBeMovedSet.insert(&op);
      }
    }
  }

  // For all instructions that we found to be invariant, move outside of the
  // loop.
  if (opsToMove.size()) {
    OpBuilder b(looplike);
    Value cond = nullptr;
    for (auto pair :
         llvm::zip(looplike.getLowerBound(), looplike.getUpperBound())) {
      auto val = b.create<arith::CmpIOp>(looplike.getLoc(), CmpIPredicate::slt,
                                         std::get<0>(pair), std::get<1>(pair));
      if (cond == nullptr)
        cond = val;
      else
        cond = b.create<arith::AndIOp>(looplike.getLoc(), cond, val);
    }
    auto ifOp = b.create<scf::IfOp>(looplike.getLoc(), TypeRange(), cond);
    looplike->moveBefore(ifOp.thenYield());
  }
  LogicalResult result = looplike.moveOutOfLoop(opsToMove);
  LLVM_DEBUG(looplike.print(llvm::dbgs() << "\n\nModified loop:\n"));
  return result;
}

// TODO affine parallel licm
#if 0
 LogicalResult moveParallelLoopInvariantCode(AffineParallelOp looplike) {
   auto &loopBody = looplike.getLoopBody();
 
   // We use two collections here as we need to preserve the order for insertion
   // and this is easiest.
   SmallPtrSet<Operation *, 8> willBeMovedSet;
   SmallVector<Operation *, 8> opsToMove;
  
   // Do not use walk here, as we do not want to go into nested regions and hoist
   // operations from there. These regions might have semantics unknown to this
   // rewriting. If the nested regions are loops, they will have been processed.
   for (auto &block : loopBody) {
     for (auto &op : block.without_terminator()) {
       if (canBeParallelHoisted(&op, looplike, willBeMovedSet)) {
         opsToMove.push_back(&op);
         willBeMovedSet.insert(&op);
       }
     }
   }
 
   // For all instructions that we found to be invariant, move outside of the
   // loop.
   if (opsToMove.size()) {
      OpBuilder b(looplike);
    
	SmallVector<Value, 4> lbOperands(looplike.getLowerBoundOperands());
    SmallVector<Value, 4> ubOperands(looplike.getUpperBoundOperands());

    auto lbMap = looplike.getLowerBoundMap();
    auto ubMap = looplike.getUpperBoundMap();


	// TODO properly fill exprs and eqflags
    SmallVector<AffineExpr, 2> exprs;
    SmallVector<bool, 2> eqflags;

      auto iset = IntegerSet::get(/*dim*/ lbMap.getNumDims() + ubMap.getNumDims(), /*symbol*/ lbMap.getNumSymbols() + ubMap.getNumSymbols(), exprs, eqflags);
      auto ifOp = b.create<AffineIfOp>(looplike.getLoc(), TypeRange(), iset, values);
      looplike->moveBefore(ifOp.thenYield());
   }
   LogicalResult result = looplike.moveOutOfLoop(opsToMove);
   LLVM_DEBUG(looplike.print(llvm::dbgs() << "\n\nModified loop:\n"));
   return result;
 }
#endif

void ParallelLICM::runOnOperation() {
  getOperation()->walk([&](LoopLikeOpInterface loopLike) {
    LLVM_DEBUG(loopLike.print(llvm::dbgs() << "\nOriginal loop:\n"));
    if (failed(moveLoopInvariantCode(loopLike)))
      signalPassFailure();
    if (auto par = dyn_cast<scf::ParallelOp>((Operation *)loopLike)) {
      if (failed(moveParallelLoopInvariantCode(par)))
        signalPassFailure();
    }
  });
}

std::unique_ptr<Pass> mlir::polygeist::createParallelLICMPass() {
  return std::make_unique<ParallelLICM>();
}
