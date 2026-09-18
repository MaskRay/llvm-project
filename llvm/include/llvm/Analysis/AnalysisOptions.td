//===- AnalysisOptions.td - Options of LLVMAnalysis --------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

include "llvm/Option/OptParser.td"

// -name=<v> or -name <v>, stored in AnalysisOptions::member.
multiclass Val<string name, code type, code member, code default, string help> {
  def NAME#_EQ : Joined<["-", "--"], name#"=">, HelpText<help>,
                 Field<type, member, default>;
  def NAME : Separate<["-", "--"], name>, Alias<!cast<Option>(NAME#"_EQ")>;
}

// -name or -name=<bool>, stored in AnalysisOptions::member.
multiclass Bool<string name, code member, code default, string help,
                code type = "bool"> {
  def NAME#_EQ : Joined<["-", "--"], name#"=">, Field<type, member, default>;
  def NAME : Flag<["-", "--"], name>, HelpText<help>,
             Alias<!cast<Option>(NAME#"_EQ")>, AliasArgs<["true"]>;
}

// InlineCost.cpp
defm inlinedefault_threshold : Val<"inlinedefault-threshold", "int",
    "DefaultThreshold", "225", "Default amount of inlining to perform">;
defm ignore_tti_inline_compatible : Bool<"ignore-tti-inline-compatible",
    "IgnoreTTIInlineCompatible", "false",
    "Ignore TTI attributes compatibility check between callee/caller during "
    "inline cost calculation">;
defm print_instruction_comments : Bool<"print-instruction-comments",
    "PrintInstructionComments", "false",
    "Prints comments for instruction based on inline cost analysis">;
defm inline_threshold : Val<"inline-threshold", "std::optional<int>",
    "InlineThreshold", "",
    "Control the amount of inlining to perform (default = 225)">;
defm inlinehint_threshold : Val<"inlinehint-threshold", "int", "HintThreshold",
    "325", "Threshold for inlining functions with inline hint">;
defm inline_cold_callsite_threshold : Val<"inline-cold-callsite-threshold",
    "int", "ColdCallSiteThreshold", "45",
    "Threshold for inlining cold callsites">;
defm inline_enable_cost_benefit_analysis : Bool<
    "inline-enable-cost-benefit-analysis", "InlineEnableCostBenefitAnalysis",
    "", "Enable the cost-benefit analysis for the inliner",
    "std::optional<bool>">;
defm inline_savings_multiplier : Val<"inline-savings-multiplier",
    "std::optional<int>", "InlineSavingsMultiplier", "",
    "Multiplier to multiply cycle savings by during inlining">;
defm inline_savings_profitable_multiplier : Val<
    "inline-savings-profitable-multiplier", "std::optional<int>",
    "InlineSavingsProfitableMultiplier", "",
    "A multiplier on top of cycle savings to decide whether the savings won't "
    "justify the cost">;
defm inline_size_allowance : Val<"inline-size-allowance", "int",
    "InlineSizeAllowance", "100",
    "The maximum size of a callee that get's inlined without sufficient cycle "
    "savings">;
defm inlinecold_threshold : Val<"inlinecold-threshold", "std::optional<int>",
    "ColdThreshold", "", "Threshold for inlining functions with cold attribute">;
defm hot_callsite_threshold : Val<"hot-callsite-threshold", "int",
    "HotCallSiteThreshold", "3000", "Threshold for hot callsites ">;
defm locally_hot_callsite_threshold : Val<"locally-hot-callsite-threshold",
    "std::optional<int>", "LocallyHotCallSiteThreshold", "",
    "Threshold for locally hot callsites ">;
defm cold_callsite_rel_freq : Val<"cold-callsite-rel-freq", "int",
    "ColdCallSiteRelFreq", "2",
    "Maximum block frequency, expressed as a percentage of caller's entry "
    "frequency, for a callsite to be cold in the absence of profile "
    "information.">;
defm hot_callsite_rel_freq : Val<"hot-callsite-rel-freq", "uint64_t",
    "HotCallSiteRelFreq", "60",
    "Minimum block frequency, expressed as a multiple of caller's entry "
    "frequency, for a callsite to be hot in the absence of profile "
    "information.">;
defm inline_instr_cost : Val<"inline-instr-cost", "int", "InstrCost", "5",
    "Cost of a single instruction when inlining">;
defm inline_asm_instr_cost : Val<"inline-asm-instr-cost", "int",
    "InlineAsmInstrCost", "0",
    "Cost of a single inline asm instruction when inlining">;
defm inline_memaccess_cost : Val<"inline-memaccess-cost", "int",
    "MemAccessCost", "0", "Cost of load/store instruction when inlining">;
defm inline_call_penalty : Val<"inline-call-penalty", "int", "CallPenalty",
    "25", "Call penalty that is applied per callsite when inlining">;
defm inline_max_stacksize : Val<"inline-max-stacksize", "std::optional<size_t>",
    "StackSizeThreshold", "",
    "Do not inline functions with a stack size that exceeds the specified "
    "limit">;
defm recursive_inline_max_stacksize : Val<"recursive-inline-max-stacksize",
    "std::optional<size_t>", "RecurStackSizeThreshold", "",
    "Do not inline recursive functions with a stack size that exceeds the "
    "specified limit">;
defm inline_cost_full : Bool<"inline-cost-full", "ComputeFullInlineCost",
    "false",
    "Compute the full inline cost of a call site even when the cost exceeds "
    "the threshold.">;
defm inline_caller_superset_nobuiltin : Bool<
    "inline-caller-superset-nobuiltin", "InlineCallerSupersetNoBuiltin", "true",
    "Allow inlining when caller has a superset of callee's nobuiltin "
    "attributes.">;
defm disable_gep_const_evaluation : Bool<"disable-gep-const-evaluation",
    "DisableGEPConstOperand", "false",
    "Disables evaluation of GetElementPtr with constant operands">;
defm inline_all_viable_calls : Bool<"inline-all-viable-calls",
    "InlineAllViableCalls", "false",
    "Inline all viable calls, even if they exceed the inlining threshold">;
