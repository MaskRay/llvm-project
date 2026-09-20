//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The command line options of LLVMPasses, stored in struct PassesOptions
// (llvm/Passes/PassesOptions.h).
//
//===----------------------------------------------------------------------===//

include "llvm/Option/OptParser.td"

def PassesOptions : OptionsStruct;

// llvm/Analysis/InlineAdvisor.h
def InliningAdvisorMode : OptionEnum<"InliningAdvisorMode", [
  EnumMember<"Default", "default">,
  EnumMember<"Release", "release">,
  EnumMember<"Development", "development">]>;

// llvm/Passes/PassesOptions.h
def AttributorRunOption : OptionEnum<"AttributorRunOption", [
  EnumMember<"NONE", "none">,
  EnumMember<"MODULE", "module">,
  EnumMember<"CGSCC", "cgscc">,
  EnumMember<"MODULE_LIGHT", "module-light">,
  EnumMember<"CGSCC_LIGHT", "cgscc-light">,
  EnumMember<"FULL", "full">,
  EnumMember<"LIGHT", "light">]>;

def pipeline_group : OptionGroup<"pipeline">, HelpText<"PASS PIPELINE OPTIONS">;

let Group = pipeline_group in {
defm AttributorRun : EnumField<"attributor-enable", AttributorRunOption,
    "AttributorRunOption::NONE",
    "Enable the attributor inter-procedural deduction pass">;
defm DisablePreInliner : BoolField<"disable-preinline", "0",
    "Disable pre-instrumentation inliner">;
defm EagerlyInvalidateAnalyses : BoolField<"eagerly-invalidate-analyses", "1",
    "Eagerly invalidate more analyses in default pipelines">;
// This option is used in simplifying testing SampleFDO optimizations for
// profile loading.
defm EnableCHR : BoolField<"enable-chr", "1",
    "Enable control height reduction optimization (CHR)">;
defm EnableConstraintElimination : BoolField<"enable-constraint-elimination",
    "1", "Enable pass to eliminate conditions based on linear constraints">;
// TODO: There is a similar flag in WPD pass, we should consolidate them by
// parsing the option only once in PassBuilder and share it across both places.
defm DevirtualizeSpeculatively : BoolField<"enable-devirtualize-speculatively",
    "0", "Enable speculative devirtualization optimization">;
defm EnableDFAJumpThreading : BoolField<"enable-dfa-jump-thread", "1",
    "Enable DFA jump threading">;
defm EnableGlobalAnalyses : BoolField<"enable-global-analyses", "1",
    "Enable inter-procedural analyses">;
defm EnableGVNHoist : BoolField<"enable-gvn-hoist", "0",
    "Enable the GVN hoisting pass (default = off)">;
defm EnableGVNSink : BoolField<"enable-gvn-sink", "0",
    "Enable the GVN sinking pass (default = off)">;
defm EnableInstrumentor : BoolField<"enable-instrumentor", "0",
    "Enable the Instrumentor Pass">;
defm EnableJumpTableToSwitch : BoolField<"enable-jump-table-to-switch", "1",
    "Enable JumpTableToSwitch pass (default = true)">;
defm EnableLoopFlatten : BoolField<"enable-loop-flatten", "0",
    "Enable the LoopFlatten Pass">;
defm UseLoopVersioningLICM : BoolField<"enable-loop-versioning-licm", "0",
    "Enable the experimental Loop Versioning LICM pass">;
defm LoopInterchange : BoolField<"enable-loopinterchange", "1",
    "Enable the LoopInterchange Pass">;
defm EnableMatrix : BoolField<"enable-matrix", "0",
    "Enable lowering of the matrix intrinsics">;
defm MergeFunctions : BoolField<"enable-merge-functions", "0",
    "Enable function merging as part of the optimization pipeline">;
defm EnableMergeICmps : BoolField<"enable-mergeicmps", "1",
    "Enable MergeICmps pass in the optimization pipeline">;
defm UseInlineAdvisor : EnumField<"enable-ml-inliner", InliningAdvisorMode,
    "InliningAdvisorMode::Default",
    "Enable ML policy for inliner. Currently trained for -Oz only">;
defm EnableModuleInliner : BoolField<"enable-module-inliner", "0",
    "Enable module inliner">;
defm RunNewGVN : BoolField<"enable-newgvn", "0", "Run the NewGVN pass">;
defm EnablePGOInlineDeferral : BoolField<"enable-npm-pgo-inline-deferral", "1",
    "Enable inline deferral during PGO">;
defm RunPartialInlining : BoolField<"enable-partial-inlining", "0",
    "Run Partial inlining pass">;
defm EnablePostPGOLoopRotation : BoolField<"enable-post-pgo-loop-rotation", "1",
    "Run the loop rotation transformation after PGO instrumentation">;
defm EnableSampledInstr : BoolField<"enable-sampled-instrumentation", "0",
    "Enable profile instrumentation sampling (default = off)">;
defm EnableUnrollAndJam : BoolField<"enable-unroll-and-jam", "0",
    "Enable Unroll And Jam Pass">;
defm ExtraVectorizerPasses : BoolField<"extra-vectorizer-passes", "0",
    "Run cleanup optimization passes after vectorization">;
defm FlattenedProfileUsed : BoolField<"flattened-profile-used", "0",
    "Indicate the sample profile being used is flattened, i.e., no inline "
    "hierarchy exists in the profile">;
defm EnableHotColdSplit : BoolField<"hot-cold-split", "0",
    "Enable hot-cold splitting pass">;
defm PerformMandatoryInliningsFirst : BoolField<"mandatory-inlining-first", "0",
    "Perform mandatory inlinings module-wide, before performing inlining">;
defm TriggerCrash : BoolField<"opt-pipeline-trigger-crash", "0",
    "Trigger crash in optimization pipeline">;
defm PreInlineThreshold : ValueField<"preinline-threshold", "int", "75",
    "Control the amount of inlining in pre-instrumentation inliner (default = "
    "75)">;
defm InstrumentColdFuncOnlyPath
    : ValueField<"instrument-cold-function-only-path", "std::string", "",
    "File path for cold function only instrumentation(requires use with --pgo-"
    "instrument-cold-function-only)">;
}
