//===- FIRRTLInstanceGraph.cpp - Instance Graph -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/FIRRTL/FIRRTLInstanceGraph.h"
#include "mlir/IR/BuiltinOps.h"

using namespace circt;
using namespace firrtl;

static CircuitOp findCircuitOp(Operation *operation) {
  if (auto mod = dyn_cast<mlir::ModuleOp>(operation))
    for (auto &op : *mod.getBody())
      if (auto circuit = dyn_cast<CircuitOp>(&op))
        return circuit;
  return cast<CircuitOp>(operation);
}

InstanceGraph::InstanceGraph(Operation *operation)
    : InstanceGraphBase(findCircuitOp(operation)) {
  topLevelNode = lookup(cast<CircuitOp>(getParent()).getNameAttr());
}

ArrayRef<InstancePath> InstancePathCache::getAbsolutePaths(Operation *op) {
  assert(isa<FModuleLike>(op));

  // If we have reached the circuit root, we're done.
  if (op == instanceGraph.getTopLevelNode()->getModule()) {
    static InstancePath empty{};
    return empty; // array with single empty path
  }

  // Fast path: hit the cache.
  auto cached = absolutePathsCache.find(op);
  if (cached != absolutePathsCache.end())
    return cached->second;

  // For each instance, collect the instance paths to its parent and append the
  // instance itself to each.
  SmallVector<InstancePath, 8> extendedPaths;
  for (auto *inst : instanceGraph[op]->uses()) {
    auto instPaths = getAbsolutePaths(inst->getParent()->getModule());
    extendedPaths.reserve(instPaths.size());
    for (auto path : instPaths) {
      extendedPaths.push_back(
          appendInstance(path, cast<InstanceOp>(*inst->getInstance())));
    }
  }

  // Move the list of paths into the bump allocator for later quick retrieval.
  ArrayRef<InstancePath> pathList;
  if (!extendedPaths.empty()) {
    auto *paths = allocator.Allocate<InstancePath>(extendedPaths.size());
    std::copy(extendedPaths.begin(), extendedPaths.end(), paths);
    pathList = ArrayRef<InstancePath>(paths, extendedPaths.size());
  }

  absolutePathsCache.insert({op, pathList});
  return pathList;
}

InstancePath InstancePathCache::appendInstance(InstancePath path,
                                               InstanceOp inst) {
  size_t n = path.size() + 1;
  auto *newPath = allocator.Allocate<InstanceOp>(n);
  std::copy(path.begin(), path.end(), newPath);
  newPath[path.size()] = inst;
  return InstancePath(newPath, n);
}

void InstancePathCache::replaceInstance(InstanceOp oldOp, InstanceOp newOp) {

  instanceGraph.replaceInstance(oldOp, newOp);

  // Iterate over all the paths, and search for the old InstanceOp. If found,
  // then replace it with the new InstanceOp, and create a new copy of the paths
  // and update the cache.
  auto instanceExists = [&](const ArrayRef<InstancePath> &paths) -> bool {
    return llvm::any_of(
        paths, [&](InstancePath p) { return llvm::is_contained(p, oldOp); });
  };

  for (auto &iter : absolutePathsCache) {
    if (!instanceExists(iter.getSecond()))
      continue;
    SmallVector<InstancePath, 8> updatedPaths;
    for (auto path : iter.getSecond()) {
      const auto *iter = llvm::find(path, oldOp);
      if (iter == path.end()) {
        // path does not contain the oldOp, just copy it as is.
        updatedPaths.push_back(path);
        continue;
      }
      auto *newPath = allocator.Allocate<InstanceOp>(path.size());
      llvm::copy(path, newPath);
      newPath[iter - path.begin()] = newOp;
      updatedPaths.push_back(InstancePath(newPath, path.size()));
    }
    // Move the list of paths into the bump allocator for later quick
    // retrieval.
    auto *paths = allocator.Allocate<InstancePath>(updatedPaths.size());
    llvm::copy(updatedPaths, paths);
    iter.getSecond() = ArrayRef<InstancePath>(paths, updatedPaths.size());
  }
}

bool circt::firrtl::allUnder(ArrayRef<InstanceRecord *> nodes,
                             InstanceGraphNode *top) {
  DenseSet<InstanceGraphNode *> seen;
  SmallVector<InstanceGraphNode *> worklist;
  worklist.reserve(nodes.size());
  seen.reserve(nodes.size());
  seen.insert(top);
  for (auto *n : nodes) {
    auto *mod = n->getParent();
    if (seen.insert(mod).second)
      worklist.push_back(mod);
  }

  while (!worklist.empty()) {
    auto *node = worklist.back();
    worklist.pop_back();

    assert(node != top);

    // If reach top-level node we're not covered by 'top', return.
    if (node->noUses())
      return false;

    // Otherwise, walk upwards.
    for (auto *use : node->uses()) {
      auto *mod = use->getParent();
      if (seen.insert(mod).second)
        worklist.push_back(mod);
    }
  }
  return true;
}
