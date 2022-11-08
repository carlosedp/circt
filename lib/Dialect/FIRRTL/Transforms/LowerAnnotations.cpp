//===- LowerAnnotations.cpp - Lower Annotations -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the LowerAnnotations pass.  This pass processes FIRRTL
// annotations, rewriting them, scattering them, and dealing with non-local
// annotations.
//
//===----------------------------------------------------------------------===//

#include "PassDetails.h"
#include "circt/Dialect/FIRRTL/AnnotationDetails.h"
#include "circt/Dialect/FIRRTL/CHIRRTLDialect.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotationHelper.h"
#include "circt/Dialect/FIRRTL/FIRRTLAnnotations.h"
#include "circt/Dialect/FIRRTL/FIRRTLAttributes.h"
#include "circt/Dialect/FIRRTL/FIRRTLInstanceGraph.h"
#include "circt/Dialect/FIRRTL/FIRRTLOps.h"
#include "circt/Dialect/FIRRTL/FIRRTLTypes.h"
#include "circt/Dialect/FIRRTL/FIRRTLVisitors.h"
#include "circt/Dialect/FIRRTL/Namespace.h"
#include "circt/Dialect/FIRRTL/Passes.h"
#include "circt/Dialect/HW/HWAttributes.h"
#include "mlir/IR/Diagnostics.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "lower-annos"

using namespace circt;
using namespace firrtl;
using namespace chirrtl;

/// Get annotations or an empty set of annotations.
static ArrayAttr getAnnotationsFrom(Operation *op) {
  if (auto annots = op->getAttrOfType<ArrayAttr>(getAnnotationAttrName()))
    return annots;
  return ArrayAttr::get(op->getContext(), {});
}

/// Construct the annotation array with a new thing appended.
static ArrayAttr appendArrayAttr(ArrayAttr array, Attribute a) {
  if (!array)
    return ArrayAttr::get(a.getContext(), ArrayRef<Attribute>{a});
  SmallVector<Attribute> old(array.begin(), array.end());
  old.push_back(a);
  return ArrayAttr::get(a.getContext(), old);
}

/// Update an ArrayAttribute by replacing one entry.
static ArrayAttr replaceArrayAttrElement(ArrayAttr array, size_t elem,
                                         Attribute newVal) {
  SmallVector<Attribute> old(array.begin(), array.end());
  old[elem] = newVal;
  return ArrayAttr::get(array.getContext(), old);
}

/// Apply a new annotation to a resolved target.  This handles ports,
/// aggregates, modules, wires, etc.
static void addAnnotation(AnnoTarget ref, unsigned fieldIdx,
                          ArrayRef<NamedAttribute> anno) {
  auto *context = ref.getOp()->getContext();
  DictionaryAttr annotation;
  if (fieldIdx) {
    SmallVector<NamedAttribute> annoField(anno.begin(), anno.end());
    annoField.emplace_back(
        StringAttr::get(context, "circt.fieldID"),
        IntegerAttr::get(IntegerType::get(context, 32, IntegerType::Signless),
                         fieldIdx));
    annotation = DictionaryAttr::get(context, annoField);
  } else {
    annotation = DictionaryAttr::get(context, anno);
  }

  if (ref.isa<OpAnnoTarget>()) {
    auto newAnno = appendArrayAttr(getAnnotationsFrom(ref.getOp()), annotation);
    ref.getOp()->setAttr(getAnnotationAttrName(), newAnno);
    return;
  }

  auto portRef = ref.cast<PortAnnoTarget>();
  auto portAnnoRaw = ref.getOp()->getAttr(getPortAnnotationAttrName());
  ArrayAttr portAnno = portAnnoRaw.dyn_cast_or_null<ArrayAttr>();
  if (!portAnno || portAnno.size() != getNumPorts(ref.getOp())) {
    SmallVector<Attribute> emptyPortAttr(
        getNumPorts(ref.getOp()),
        ArrayAttr::get(ref.getOp()->getContext(), {}));
    portAnno = ArrayAttr::get(ref.getOp()->getContext(), emptyPortAttr);
  }
  portAnno = replaceArrayAttrElement(
      portAnno, portRef.getPortNo(),
      appendArrayAttr(portAnno[portRef.getPortNo()].dyn_cast<ArrayAttr>(),
                      annotation));
  ref.getOp()->setAttr("portAnnotations", portAnno);
}

/// Make an anchor for a non-local annotation.  Use the expanded path to build
/// the module and name list in the anchor.
static FlatSymbolRefAttr buildNLA(const AnnoPathValue &target,
                                  ApplyState &state) {
  OpBuilder b(state.circuit.getBodyRegion());
  SmallVector<Attribute> insts;
  for (auto inst : target.instances) {
    insts.push_back(OpAnnoTarget(inst).getNLAReference(
        state.getNamespace(inst->getParentOfType<FModuleLike>())));
  }

  insts.push_back(
      FlatSymbolRefAttr::get(target.ref.getModule().moduleNameAttr()));

  auto instAttr = ArrayAttr::get(state.circuit.getContext(), insts);

  // Re-use NLA for this path if already created.
  auto it = state.instPathToNLAMap.find(instAttr);
  if (it != state.instPathToNLAMap.end()) {
    ++state.numReusedHierPaths;
    return it->second;
  }

  // Create the NLA
  auto nla = b.create<HierPathOp>(state.circuit.getLoc(), "nla", instAttr);
  state.symTbl.insert(nla);
  nla.setVisibility(SymbolTable::Visibility::Private);
  auto sym = FlatSymbolRefAttr::get(nla);
  state.instPathToNLAMap.insert({instAttr, sym});
  return sym;
}

/// Scatter breadcrumb annotations corresponding to non-local annotations
/// along the instance path.  Returns symbol name used to anchor annotations to
/// path.
// FIXME: uniq annotation chain links
static FlatSymbolRefAttr scatterNonLocalPath(const AnnoPathValue &target,
                                             ApplyState &state) {

  FlatSymbolRefAttr sym = buildNLA(target, state);
  return sym;
}

//===----------------------------------------------------------------------===//
// Standard Utility Resolvers
//===----------------------------------------------------------------------===//

/// Always resolve to the circuit, ignoring the annotation.
static Optional<AnnoPathValue> noResolve(DictionaryAttr anno,
                                         ApplyState &state) {
  return AnnoPathValue(state.circuit);
}

/// Implementation of standard resolution.  First parses the target path, then
/// resolves it.
static Optional<AnnoPathValue> stdResolveImpl(StringRef rawPath,
                                              ApplyState &state) {
  auto pathStr = canonicalizeTarget(rawPath);
  StringRef path{pathStr};

  auto tokens = tokenizePath(path);
  if (!tokens) {
    mlir::emitError(state.circuit.getLoc())
        << "Cannot tokenize annotation path " << rawPath;
    return {};
  }

  return resolveEntities(*tokens, state.circuit, state.symTbl,
                         state.targetCaches);
}

/// (SFC) FIRRTL SingleTargetAnnotation resolver.  Uses the 'target' field of
/// the annotation with standard parsing to resolve the path.  This requires
/// 'target' to exist and be normalized (per docs/FIRRTLAnnotations.md).
static Optional<AnnoPathValue> stdResolve(DictionaryAttr anno,
                                          ApplyState &state) {
  auto target = anno.getNamed("target");
  if (!target) {
    mlir::emitError(state.circuit.getLoc())
        << "No target field in annotation " << anno;
    return {};
  }
  if (!target->getValue().isa<StringAttr>()) {
    mlir::emitError(state.circuit.getLoc())
        << "Target field in annotation doesn't contain string " << anno;
    return {};
  }
  return stdResolveImpl(target->getValue().cast<StringAttr>().getValue(),
                        state);
}

/// Resolves with target, if it exists.  If not, resolves to the circuit.
static Optional<AnnoPathValue> tryResolve(DictionaryAttr anno,
                                          ApplyState &state) {
  auto target = anno.getNamed("target");
  if (target)
    return stdResolveImpl(target->getValue().cast<StringAttr>().getValue(),
                          state);
  return AnnoPathValue(state.circuit);
}

//===----------------------------------------------------------------------===//
// Standard Utility Appliers
//===----------------------------------------------------------------------===//

/// An applier which puts the annotation on the target and drops the 'target'
/// field from the annotation.  Optionally handles non-local annotations.
static LogicalResult applyWithoutTargetImpl(const AnnoPathValue &target,
                                            DictionaryAttr anno,
                                            ApplyState &state,
                                            bool allowNonLocal) {
  if (!allowNonLocal && !target.isLocal()) {
    Annotation annotation(anno);
    auto diag = mlir::emitError(target.ref.getOp()->getLoc())
                << "is targeted by a non-local annotation \""
                << annotation.getClass() << "\" with target "
                << annotation.getMember("target")
                << ", but this annotation cannot be non-local";
    diag.attachNote() << "see current annotation: " << anno << "\n";
    return failure();
  }
  SmallVector<NamedAttribute> newAnnoAttrs;
  for (auto &na : anno) {
    if (na.getName().getValue() != "target") {
      newAnnoAttrs.push_back(na);
    } else if (!target.isLocal()) {
      auto sym = scatterNonLocalPath(target, state);
      newAnnoAttrs.push_back(
          {StringAttr::get(anno.getContext(), "circt.nonlocal"), sym});
    }
  }
  addAnnotation(target.ref, target.fieldIdx, newAnnoAttrs);
  return success();
}

/// An applier which puts the annotation on the target and drops the 'target'
/// field from the annotaiton.  Optionally handles non-local annotations.
/// Ensures the target resolves to an expected type of operation.
template <bool allowNonLocal, bool allowPortAnnoTarget, typename T,
          typename... Tr>
static LogicalResult applyWithoutTarget(const AnnoPathValue &target,
                                        DictionaryAttr anno,
                                        ApplyState &state) {
  if (target.ref.isa<PortAnnoTarget>()) {
    if (!allowPortAnnoTarget)
      return failure();
  } else if (!target.isOpOfType<T, Tr...>())
    return failure();

  return applyWithoutTargetImpl(target, anno, state, allowNonLocal);
}

template <bool allowNonLocal, typename T, typename... Tr>
static LogicalResult applyWithoutTarget(const AnnoPathValue &target,
                                        DictionaryAttr anno,
                                        ApplyState &state) {
  return applyWithoutTarget<allowNonLocal, false, T, Tr...>(target, anno,
                                                            state);
}

/// An applier which puts the annotation on the target and drops the 'target'
/// field from the annotaiton.  Optionally handles non-local annotations.
template <bool allowNonLocal = false>
static LogicalResult applyWithoutTarget(const AnnoPathValue &target,
                                        DictionaryAttr anno,
                                        ApplyState &state) {
  return applyWithoutTargetImpl(target, anno, state, allowNonLocal);
}

/// Just drop the annotation.  This is intended for Annotations which are known,
/// but can be safely ignored.
static LogicalResult drop(const AnnoPathValue &target, DictionaryAttr anno,
                          ApplyState &state) {
  return success();
}

//===----------------------------------------------------------------------===//
// Driving table
//===----------------------------------------------------------------------===//

namespace {
struct AnnoRecord {
  llvm::function_ref<Optional<AnnoPathValue>(DictionaryAttr, ApplyState &)>
      resolver;
  llvm::function_ref<LogicalResult(const AnnoPathValue &, DictionaryAttr,
                                   ApplyState &)>
      applier;
};

/// Resolution and application of a "firrtl.annotations.NoTargetAnnotation".
/// This should be used for any Annotation which does not apply to anything in
/// the FIRRTL Circuit, i.e., an Annotation which has no target.  Historically,
/// NoTargetAnnotations were used to control the Scala FIRRTL Compiler (SFC) or
/// its passes, e.g., to set the output directory or to turn on a pass.
/// Examplesof these in the SFC are "firrtl.options.TargetDirAnnotation" to set
/// the output directory or "firrtl.stage.RunFIRRTLTransformAnnotation" to
/// casuse the SFC to schedule a specified pass.  Instead of leaving these
/// floating or attaching them to the top-level MLIR module (which is a purer
/// interpretation of "no target"), we choose to attach them to the Circuit even
/// they do not "apply" to the Circuit.  This gives later passes a common place,
/// the Circuit, to search for these control Annotations.
static AnnoRecord NoTargetAnnotation = {noResolve,
                                        applyWithoutTarget<false, CircuitOp>};

} // end anonymous namespace

static const llvm::StringMap<AnnoRecord> annotationRecords{{

    // Testing Annotation
    {"circt.test", {stdResolve, applyWithoutTarget<true>}},
    {"circt.testLocalOnly", {stdResolve, applyWithoutTarget<>}},
    {"circt.testNT", {noResolve, applyWithoutTarget<>}},
    {"circt.missing", {tryResolve, applyWithoutTarget<true>}},
    // Grand Central Views/Interfaces Annotations
    {extractGrandCentralClass, NoTargetAnnotation},
    {grandCentralHierarchyFileAnnoClass, NoTargetAnnotation},
    {serializedViewAnnoClass, {noResolve, applyGCTView}},
    {viewAnnoClass, {noResolve, applyGCTView}},
    {companionAnnoClass, {stdResolve, applyWithoutTarget<>}},
    {parentAnnoClass, {stdResolve, applyWithoutTarget<>}},
    {augmentedGroundTypeClass, {stdResolve, applyWithoutTarget<true>}},
    // Grand Central Data Tap Annotations
    {dataTapsClass, {noResolve, applyGCTDataTaps}},
    {dataTapsBlackboxClass, {stdResolve, applyWithoutTarget<true>}},
    {referenceKeySourceClass, {stdResolve, applyWithoutTarget<true>}},
    {referenceKeyPortClass, {stdResolve, applyWithoutTarget<true>}},
    {internalKeySourceClass, {stdResolve, applyWithoutTarget<true>}},
    {internalKeyPortClass, {stdResolve, applyWithoutTarget<true>}},
    {deletedKeyClass, {stdResolve, applyWithoutTarget<true>}},
    {literalKeyClass, {stdResolve, applyWithoutTarget<true>}},
    // Grand Central Mem Tap Annotations
    {memTapClass, {noResolve, applyGCTMemTaps}},
    {memTapSourceClass, {stdResolve, applyWithoutTarget<true>}},
    {memTapPortClass, {stdResolve, applyWithoutTarget<true>}},
    {memTapBlackboxClass, {stdResolve, applyWithoutTarget<true>}},
    // Grand Central Signal Mapping Annotations
    {signalDriverAnnoClass, {noResolve, applyGCTSignalMappings}},
    {signalDriverTargetAnnoClass, {stdResolve, applyWithoutTarget<true>}},
    {signalDriverModuleAnnoClass, {stdResolve, applyWithoutTarget<true>}},
    // OMIR Annotations
    {omirAnnoClass, {noResolve, applyOMIR}},
    {omirTrackerAnnoClass, {stdResolve, applyWithoutTarget<true>}},
    {omirFileAnnoClass, NoTargetAnnotation},
    // Miscellaneous Annotations
    {dontTouchAnnoClass,
     {stdResolve, applyWithoutTarget<true, true, WireOp, NodeOp, RegOp,
                                     RegResetOp, InstanceOp, MemOp, CombMemOp,
                                     MemoryPortOp, SeqMemOp>}},
    {prefixModulesAnnoClass,
     {stdResolve,
      applyWithoutTarget<true, FModuleOp, FExtModuleOp, InstanceOp>}},
    {dutAnnoClass, {stdResolve, applyWithoutTarget<false, FModuleOp>}},
    {extractSeqMemsAnnoClass, NoTargetAnnotation},
    {injectDUTHierarchyAnnoClass, NoTargetAnnotation},
    {convertMemToRegOfVecAnnoClass, NoTargetAnnotation},
    {excludeMemToRegAnnoClass,
     {stdResolve, applyWithoutTarget<true, MemOp, CombMemOp>}},
    {sitestBlackBoxAnnoClass, NoTargetAnnotation},
    {enumComponentAnnoClass, {noResolve, drop}},
    {enumDefAnnoClass, {noResolve, drop}},
    {enumVecAnnoClass, {noResolve, drop}},
    {forceNameAnnoClass,
     {stdResolve, applyWithoutTarget<true, FModuleOp, FExtModuleOp>}},
    {flattenAnnoClass, {stdResolve, applyWithoutTarget<false, FModuleOp>}},
    {inlineAnnoClass, {stdResolve, applyWithoutTarget<false, FModuleOp>}},
    {noDedupAnnoClass,
     {stdResolve, applyWithoutTarget<false, FModuleOp, FExtModuleOp>}},
    {blackBoxInlineAnnoClass,
     {stdResolve, applyWithoutTarget<false, FExtModuleOp>}},
    {dontObfuscateModuleAnnoClass,
     {stdResolve, applyWithoutTarget<false, FModuleOp>}},
    {verifBlackBoxAnnoClass,
     {stdResolve, applyWithoutTarget<false, FExtModuleOp>}},
    {elaborationArtefactsDirectoryAnnoClass, NoTargetAnnotation},
    {subCircuitsTargetDirectoryAnnoClass, NoTargetAnnotation},
    {retimeModulesFileAnnoClass, NoTargetAnnotation},
    {retimeModuleAnnoClass,
     {stdResolve, applyWithoutTarget<false, FModuleOp, FExtModuleOp>}},
    {metadataDirectoryAttrName, NoTargetAnnotation},
    {moduleHierAnnoClass, NoTargetAnnotation},
    {sitestTestHarnessBlackBoxAnnoClass, NoTargetAnnotation},
    {testBenchDirAnnoClass, NoTargetAnnotation},
    {testHarnessHierAnnoClass, NoTargetAnnotation},
    {testHarnessPathAnnoClass, NoTargetAnnotation},
    {prefixInterfacesAnnoClass, NoTargetAnnotation},
    {subCircuitDirAnnotation, NoTargetAnnotation},
    {extractAssertAnnoClass, NoTargetAnnotation},
    {extractAssumeAnnoClass, NoTargetAnnotation},
    {extractCoverageAnnoClass, NoTargetAnnotation},
    {dftTestModeEnableAnnoClass, {stdResolve, applyWithoutTarget<true>}},
    {runFIRRTLTransformAnnoClass, {noResolve, drop}},
    {mustDedupAnnoClass, NoTargetAnnotation},
    {addSeqMemPortAnnoClass, NoTargetAnnotation},
    {addSeqMemPortsFileAnnoClass, NoTargetAnnotation},
    {extractClockGatesAnnoClass, NoTargetAnnotation},
    {fullAsyncResetAnnoClass, {stdResolve, applyWithoutTarget<true>}},
    {ignoreFullAsyncResetAnnoClass,
     {stdResolve, applyWithoutTarget<true, FModuleOp>}},
    {decodeTableAnnotation, {noResolve, drop}},
    {blackBoxTargetDirAnnoClass, NoTargetAnnotation},
    {traceNameAnnoClass, {stdResolve, applyTraceName}},
    {traceAnnoClass, {stdResolve, applyWithoutTarget<true>}},

}};

/// Lookup a record for a given annotation class.  Optionally, returns the
/// record for "circuit.missing" if the record doesn't exist.
static const AnnoRecord *getAnnotationHandler(StringRef annoStr,
                                              bool ignoreUnhandledAnno) {
  auto ii = annotationRecords.find(annoStr);
  if (ii != annotationRecords.end())
    return &ii->second;
  if (ignoreUnhandledAnno)
    return &annotationRecords.find("circt.missing")->second;
  return nullptr;
}

bool firrtl::isAnnoClassLowered(StringRef className) {
  return annotationRecords.count(className);
}

//===----------------------------------------------------------------------===//
// Pass Infrastructure
//===----------------------------------------------------------------------===//

namespace {
struct LowerAnnotationsPass
    : public LowerFIRRTLAnnotationsBase<LowerAnnotationsPass> {
  void runOnOperation() override;
  LogicalResult applyAnnotation(DictionaryAttr anno, ApplyState &state);

  bool ignoreUnhandledAnno = false;
  bool ignoreClasslessAnno = false;
  SmallVector<DictionaryAttr> worklistAttrs;
};
} // end anonymous namespace

LogicalResult LowerAnnotationsPass::applyAnnotation(DictionaryAttr anno,
                                                    ApplyState &state) {
  LLVM_DEBUG(llvm::dbgs() << "  - anno: " << anno << "\n";);

  // Lookup the class
  StringRef annoClassVal;
  if (auto annoClass = anno.getNamed("class"))
    annoClassVal = annoClass->getValue().cast<StringAttr>().getValue();
  else if (ignoreClasslessAnno)
    annoClassVal = "circt.missing";
  else
    return mlir::emitError(state.circuit.getLoc())
           << "Annotation without a class: " << anno;

  // See if we handle the class
  auto *record = getAnnotationHandler(annoClassVal, false);
  if (!record) {
    ++numUnhandled;
    if (!ignoreUnhandledAnno)
      return mlir::emitError(state.circuit.getLoc())
             << "Unhandled annotation: " << anno;

    // Try again, requesting the fallback handler.
    record = getAnnotationHandler(annoClassVal, ignoreUnhandledAnno);
    assert(record);
  }

  // Try to apply the annotation
  auto target = record->resolver(anno, state);
  if (!target)
    return mlir::emitError(state.circuit.getLoc())
           << "Unable to resolve target of annotation: " << anno;
  if (record->applier(*target, anno, state).failed())
    return mlir::emitError(state.circuit.getLoc())
           << "Unable to apply annotation: " << anno;
  return success();
}

// This is the main entrypoint for the lowering pass.
void LowerAnnotationsPass::runOnOperation() {
  CircuitOp circuit = getOperation();
  SymbolTable modules(circuit);

  LLVM_DEBUG(llvm::dbgs() << "===- Running LowerAnnotations Pass "
                             "------------------------------------------===\n");

  // Grab the annotations from a non-standard attribute called "rawAnnotations".
  // This is a temporary location for all annotations that are earmarked for
  // processing by this pass as we migrate annotations from being handled by
  // FIRAnnotations/FIRParser into this pass.  While we do this, this pass is
  // not supposed to touch _other_ annotations to enable this pass to be run
  // after FIRAnnotations/FIRParser.
  auto annotations = circuit->getAttrOfType<ArrayAttr>(rawAnnotations);
  if (!annotations)
    return;
  circuit->removeAttr(rawAnnotations);

  // Populate the worklist in reverse order.  This has the effect of causing
  // annotations to be processed in the order in which they appear in the
  // original JSON.
  for (auto anno : llvm::reverse(annotations.getValue()))
    worklistAttrs.push_back(anno.cast<DictionaryAttr>());

  size_t numFailures = 0;
  size_t numAdded = 0;
  auto addToWorklist = [&](DictionaryAttr anno) {
    ++numAdded;
    worklistAttrs.push_back(anno);
  };
  InstancePathCache instancePathCache(getAnalysis<InstanceGraph>());
  ApplyState state{circuit, modules, addToWorklist, instancePathCache};
  LLVM_DEBUG(llvm::dbgs() << "Processing annotations:\n");
  while (!worklistAttrs.empty()) {
    auto attr = worklistAttrs.pop_back_val();
    if (applyAnnotation(attr, state).failed())
      ++numFailures;
  }

  LLVM_DEBUG({
    llvm::dbgs() << "WiringProblems:\n";
    for (auto tuple : llvm::enumerate(state.wiringProblems)) {
      auto problem = tuple.value();
      llvm::dbgs() << "  - id: " << tuple.index() << "\n";
      llvm::dbgs() << "    source:\n";
      llvm::dbgs() << "      module: "
                   << problem.source.getDefiningOp()
                          ->getParentOfType<FModuleOp>()
                          .moduleName()
                   << "\n";
      llvm::dbgs() << "      value: " << problem.source << "\n";
      llvm::dbgs() << "    sink:\n";
      llvm::dbgs() << "      module: "
                   << problem.sink.getDefiningOp()
                          ->getParentOfType<FModuleOp>()
                          .moduleName()
                   << "\n";
      llvm::dbgs() << "      value: " << problem.sink << "\n";
      llvm::dbgs() << "    isRefType: " << (problem.isRefType ? "yes" : "no")
                   << "\n";
    }
  });

  // For all discovered Wiring Problems, record pending modifications to
  // modules.
  DenseMap<FModuleLike, ModuleModifications> moduleModifications;
  LLVM_DEBUG({ llvm::dbgs() << "Grouping WiringProblem by-module\n"; });
  for (auto &tuple : llvm::enumerate(state.wiringProblems)) {
    auto problem = tuple.value();
    auto index = tuple.index();
    // Compute LCA between source and sink.
    auto source = problem.source;
    auto sink = problem.sink;
    llvm::dbgs() << "  - index: " << index << "\n";

    // Pre-populate source/sink module modifications connection values.
    auto sourceModule = source.getDefiningOp()->getParentOfType<FModuleOp>();
    moduleModifications[sourceModule].connectionMap[index] = source;
    llvm::dbgs() << "    initial source:\n"
                 << "      module: " << sourceModule.moduleName() << "\n"
                 << "      value: " << source << "\n";
    auto sinkModule = sink.getDefiningOp()->getParentOfType<FModuleOp>();
    moduleModifications[sinkModule].connectionMap[index] = sink;
    llvm::dbgs() << "    initial sink:\n"
                 << "      module: " << sinkModule.moduleName() << "\n"
                 << "      value: " << sink << "\n";

    auto sourcePaths = instancePathCache.getAbsolutePaths(sourceModule);
    assert(sourcePaths.size() == 1);

    auto sinkPaths = instancePathCache.getAbsolutePaths(sinkModule);
    assert(sinkPaths.size() == 1);

    llvm::dbgs() << "    sourcePaths:\n";
    for (auto inst : sourcePaths[0])
      llvm::errs() << "      - " << inst.instanceName() << " of "
                   << inst.referencedModuleName() << "\n";

    llvm::dbgs() << "    sinkPaths:\n";
    for (auto inst : sinkPaths[0])
      llvm::errs() << "      - " << inst.instanceName() << " of "
                   << inst.referencedModuleName() << "\n";

    FModuleOp lca = cast<FModuleOp>(
        instancePathCache.instanceGraph.getTopLevelNode()->getModule());
    auto sources = sourcePaths[0];
    auto sinks = sinkPaths[0];
    while (!sources.empty() || sinks.empty()) {
      if (sources[0] != sinks[0])
        break;
      auto newLCA = sources[0];
      lca = cast<FModuleOp>(newLCA.getReferencedModule());
      sources = sources.drop_front();
      sinks = sinks.drop_front();
    }

    llvm::errs() << "    LCA: " << lca.moduleName() << "\n";

    // Record ports to add from LCA to source, LCA to sink, and create the
    // U-turn wire in the LCA.
    for (auto &source : sources) {
      auto foo = source;
      auto mod = cast<FModuleOp>(foo.getReferencedModule());
      auto tpe =
          problem.isRefType
              ? RefType::get(cast<FIRRTLBaseType>(problem.source.getType()))
              : problem.source.getType();
      moduleModifications[mod].portsToAdd.push_back(
          {{StringAttr::get(mod.getContext(), state.getNamespace(mod).newName(
                                                  problem.newNameHint)),
            tpe, Direction::Out},
           index});
    }

    for (auto &sink : sinks) {
      auto foo = sink;
      auto mod = cast<FModuleOp>(foo.getReferencedModule());
      auto tpe =
          problem.isRefType
              ? RefType::get(cast<FIRRTLBaseType>(problem.source.getType()))
              : problem.source.getType();
      moduleModifications[mod].portsToAdd.push_back(
          {{StringAttr::get(mod.getContext(), state.getNamespace(mod).newName(
                                                  problem.newNameHint)),
            tpe, Direction::In},
           index});
    }
  }

  // Iterate over modules from leaves to roots, adding ports and connections.
  LLVM_DEBUG({ llvm::dbgs() << "Updating modules\n"; });
  for (auto *op :
       llvm::post_order(instancePathCache.instanceGraph.getTopLevelNode())) {
    auto fmodule = cast<FModuleOp>(op->getModule());
    if (!moduleModifications.count(fmodule))
      continue;
    auto modifications = moduleModifications[fmodule];
    LLVM_DEBUG({
      llvm::dbgs() << "  - module: " << fmodule.moduleName() << "\n";
      llvm::dbgs() << "    ports:\n";
      for (auto tuple : modifications.portsToAdd) {
        auto port = std::get<0>(tuple);
        auto index = std::get<1>(tuple);
        llvm::dbgs() << "      - name: " << port.getName() << "\n"
                     << "        id: " << index << "\n"
                     << "        type: " << port.type << "\n"
                     << "        direction: "
                     << (port.direction == Direction::In ? "in" : "out")
                     << "\n";
      }
    });
    SmallVector<std::pair<unsigned, PortInfo>> newPorts;
    SmallVector<unsigned> problemIndex;
    for (auto tuple : modifications.portsToAdd) {
      auto portInfo = std::get<0>(tuple);
      auto index = std::get<1>(tuple);

      // Create the port.
      newPorts.push_back({fmodule.getNumPorts(), portInfo});
      problemIndex.push_back(index);
    }

    auto builder = ImplicitLocOpBuilder::atBlockEnd(
        UnknownLoc::get(fmodule.getContext()), fmodule.getBodyBlock());
    auto originalNumPorts = fmodule.getNumPorts();
    auto portIdx = fmodule.getNumPorts();
    fmodule.insertPorts(newPorts);
    for (auto tuple : llvm::zip(newPorts, problemIndex)) {
      auto portPair = std::get<0>(tuple);
      auto index = std::get<1>(tuple);
      // Wire up the port.
      Value src = moduleModifications[fmodule].connectionMap[index];
      Value dest = fmodule.getArgument(portIdx++);
      assert(src && "need to have an actual value");
      if (portPair.second.direction == Direction::In)
        std::swap(src, dest);
      // Create RefSend/RefResolve if necessary.
      if (dest.getType().isa<RefType>() && !src.getType().isa<RefType>()) {
        src = builder.create<RefSendOp>(src);
      } else if (!dest.getType().isa<RefType>() &&
                 src.getType().isa<RefType>()) {
        src = builder.create<RefResolveOp>(src);
      }
      builder.create<StrictConnectOp>(dest, src);
    }

    for (auto &uturn : moduleModifications[fmodule].uturns) {
      Value src = std::get<0>(uturn);
      int index = std::get<1>(uturn);
      Value dest = moduleModifications[fmodule].connectionMap[index];
      builder.create<StrictConnectOp>(src, dest);
    }

    for (auto *inst : instancePathCache.instanceGraph.lookup(fmodule)->uses()) {
      InstanceOp useInst = cast<InstanceOp>(inst->getInstance());
      auto enclosingModule = useInst->getParentOfType<FModuleOp>();
      auto clonedInst = useInst.cloneAndInsertPorts(newPorts);
      instancePathCache.replaceInstance(useInst, clonedInst);
      // RAUW needs to have the same number of output results for the instance.
      useInst->replaceAllUsesWith(
          clonedInst.getResults().drop_back(newPorts.size()));
      useInst->erase();
      // Record information in the moduleModifications strucutre for the module
      // _where this is instantiated_.  This is done so that when that module is
      // visited later, there will be information available for it to find ports
      // it needs to wire up.
      for (auto &pair : llvm::enumerate(problemIndex)) {
        if (moduleModifications[enclosingModule].connectionMap.count(
                pair.value())) {
          moduleModifications[enclosingModule].uturns.push_back(
              {clonedInst.getResult(pair.index() + originalNumPorts),
               pair.value()});
        } else {
          moduleModifications[enclosingModule].connectionMap[pair.value()] =
            clonedInst.getResult(pair.index() + originalNumPorts);
        }
      }
    }
  }

  // Update statistics
  numRawAnnotations += annotations.size();
  numAddedAnnos += numAdded;
  numAnnos += numAdded + annotations.size();
  numReusedHierPathOps += state.numReusedHierPaths;

  if (numFailures)
    signalPassFailure();
}

/// This is the pass constructor.
std::unique_ptr<mlir::Pass> circt::firrtl::createLowerFIRRTLAnnotationsPass(
    bool ignoreUnhandledAnnotations, bool ignoreClasslessAnnotations) {
  auto pass = std::make_unique<LowerAnnotationsPass>();
  pass->ignoreUnhandledAnno = ignoreUnhandledAnnotations;
  pass->ignoreClasslessAnno = ignoreClasslessAnnotations;
  return pass;
}
