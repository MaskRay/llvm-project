//===- PassesOptions.td - LLVMPasses' command line options -*- tablegen -*-===//
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

// Declared in llvm/Analysis/InlineAdvisor.h.
def InliningAdvisorMode : ExternalEnum<"InliningAdvisorMode", [
  EnumMember<"Default", "default">,
  EnumMember<"Release", "release">,
  EnumMember<"Development", "development">]>;

// A bitmask; FULL and LIGHT combine the others.
def AttributorRunOption : OptionEnum<"AttributorRunOption", [
  EnumMember<"NONE", "none", 0>,
  EnumMember<"MODULE", "module", 1>,
  EnumMember<"CGSCC", "cgscc", 2>,
  EnumMember<"MODULE_LIGHT", "module-light", 4>,
  EnumMember<"CGSCC_LIGHT", "cgscc-light", 8>,
  EnumMember<"FULL", "full", 3>,
  EnumMember<"LIGHT", "light", 12>]>;

def pipeline_group : OptionGroup<"pipeline">, HelpText<"PASS PIPELINE OPTIONS">;

let Group = pipeline_group in {

defm enable_ml_inliner : EnumField<"enable-ml-inliner", InliningAdvisorMode,
    "UseInlineAdvisor", "InliningAdvisorMode::Default",
    "Enable ML policy for inliner. Currently trained for -Oz only",
    [HelpHidden]>;
defm enable_npm_pgo_inline_deferral
    : BoolField<"enable-npm-pgo-inline-deferral", "EnablePGOInlineDeferral",
                "true", "Enable inline deferral during PGO", [HelpHidden]>;
defm enable_module_inliner : BoolField<"enable-module-inliner",
    "EnableModuleInliner", "false", "Enable module inliner", [HelpHidden]>;
defm mandatory_inlining_first : BoolField<"mandatory-inlining-first",
    "PerformMandatoryInliningsFirst", "false",
    "Perform mandatory inlinings module-wide, before performing inlining",
    [HelpHidden]>;
defm eagerly_invalidate_analyses : BoolField<"eagerly-invalidate-analyses",
    "EagerlyInvalidateAnalyses", "true",
    "Eagerly invalidate more analyses in default pipelines", [HelpHidden]>;
defm enable_merge_functions : BoolField<"enable-merge-functions",
    "MergeFunctions", "false",
    "Enable function merging as part of the optimization pipeline",
    [HelpHidden]>;
defm enable_post_pgo_loop_rotation : BoolField<"enable-post-pgo-loop-rotation",
    "EnablePostPGOLoopRotation", "true",
    "Run the loop rotation transformation after PGO instrumentation",
    [HelpHidden]>;
defm opt_pipeline_trigger_crash : BoolField<"opt-pipeline-trigger-crash",
    "TriggerCrash", "false", "Trigger crash in optimization pipeline",
    [HelpHidden]>;
defm enable_global_analyses : BoolField<"enable-global-analyses",
    "EnableGlobalAnalyses", "true", "Enable inter-procedural analyses",
    [HelpHidden]>;
defm enable_partial_inlining : BoolField<"enable-partial-inlining",
    "RunPartialInlining", "false", "Run Partial inlining pass", [HelpHidden]>;
defm extra_vectorizer_passes : BoolField<"extra-vectorizer-passes",
    "ExtraVectorizerPasses", "false",
    "Run cleanup optimization passes after vectorization", [HelpHidden]>;
defm enable_newgvn : BoolField<"enable-newgvn", "RunNewGVN", "false",
    "Run the NewGVN pass", [HelpHidden]>;
defm enable_loopinterchange : BoolField<"enable-loopinterchange",
    "LoopInterchange", "true", "Enable the LoopInterchange Pass", [HelpHidden]>;
defm enable_unroll_and_jam : BoolField<"enable-unroll-and-jam",
    "EnableUnrollAndJam", "false", "Enable Unroll And Jam Pass", [HelpHidden]>;
defm enable_loop_flatten : BoolField<"enable-loop-flatten",
    "EnableLoopFlatten", "false", "Enable the LoopFlatten Pass", [HelpHidden]>;
defm enable_instrumentor : BoolField<"enable-instrumentor",
    "EnableInstrumentor", "false", "Enable the Instrumentor Pass",
    [HelpHidden]>;
defm enable_dfa_jump_thread : BoolField<"enable-dfa-jump-thread",
    "EnableDFAJumpThreading", "true", "Enable DFA jump threading",
    [HelpHidden]>;
defm hot_cold_split : BoolField<"hot-cold-split", "EnableHotColdSplit",
    "false", "Enable hot-cold splitting pass">;
defm disable_preinline : BoolField<"disable-preinline", "DisablePreInliner",
    "false", "Disable pre-instrumentation inliner", [HelpHidden]>;
defm preinline_threshold : ValueField<"preinline-threshold", "int",
    "PreInlineThreshold", "75",
    "Control the amount of inlining in pre-instrumentation inliner (default = "
    "75)", [HelpHidden]>;
defm enable_gvn_hoist : BoolField<"enable-gvn-hoist", "EnableGVNHoist",
    "false", "Enable the GVN hoisting pass (default = off)">;
defm enable_gvn_sink : BoolField<"enable-gvn-sink", "EnableGVNSink", "false",
    "Enable the GVN sinking pass (default = off)">;
defm enable_jump_table_to_switch : BoolField<"enable-jump-table-to-switch",
    "EnableJumpTableToSwitch", "true",
    "Enable JumpTableToSwitch pass (default = true)">;
defm enable_chr : BoolField<"enable-chr", "EnableCHR", "true",
    "Enable control height reduction optimization (CHR)", [HelpHidden]>;
defm flattened_profile_used : BoolField<"flattened-profile-used",
    "FlattenedProfileUsed", "false",
    "Indicate the sample profile being used is flattened, i.e., no inline "
    "hierarchy exists in the profile", [HelpHidden]>;
defm enable_matrix : BoolField<"enable-matrix", "EnableMatrix", "false",
    "Enable lowering of the matrix intrinsics", [HelpHidden]>;
defm enable_mergeicmps : BoolField<"enable-mergeicmps", "EnableMergeICmps",
    "true", "Enable MergeICmps pass in the optimization pipeline",
    [HelpHidden]>;
defm enable_constraint_elimination : BoolField<"enable-constraint-elimination",
    "EnableConstraintElimination", "true",
    "Enable pass to eliminate conditions based on linear constraints",
    [HelpHidden]>;
defm attributor_enable : EnumField<"attributor-enable", AttributorRunOption,
    "AttributorRun", "AttributorRunOption::NONE",
    "Enable the attributor inter-procedural deduction pass", [HelpHidden]>;
defm enable_sampled_instrumentation
    : BoolField<"enable-sampled-instrumentation", "EnableSampledInstr", "false",
                "Enable profile instrumentation sampling (default = off)",
                [HelpHidden]>;
defm enable_loop_versioning_licm : BoolField<"enable-loop-versioning-licm",
    "UseLoopVersioningLICM", "false",
    "Enable the experimental Loop Versioning LICM pass", [HelpHidden]>;
defm instrument_cold_function_only_path
    : ValueField<"instrument-cold-function-only-path", "std::string",
                 "InstrumentColdFuncOnlyPath", "",
                 "File path for cold function only instrumentation(requires "
                 "use with --pgo-instrument-cold-function-only)",
                 [HelpHidden]>;
defm enable_devirtualize_speculatively
    : BoolField<"enable-devirtualize-speculatively",
                "DevirtualizeSpeculatively", "false",
                "Enable speculative devirtualization optimization">;
}
