; RUN: llc -O0 -mtriple=aarch64-linux-gnu -relocation-model=pic -onepass-codegen -onepass-codegen-abort -filetype=obj %s -o %t.o
; RUN: llvm-dwarfdump --verify %t.o
; RUN: llvm-dwarfdump --debug-info --debug-line %t.o | FileCheck %s

;; Debug info for a one-pass function: the subprogram covers the function, a
;; variable in a frame slot is described relative to x29, and the
;; line table marks the end of the prologue. A function without a subprogram is
;; still emitted. Each function's first row is at its start, so its prologue is
;; not attributed to the previous function.

define i32 @f(i32 %a) uwtable "frame-pointer"="all" !dbg !6 {
entry:
  %a.addr = alloca i32
  %x = alloca i32
  store i32 %a, ptr %a.addr
    #dbg_declare(ptr %a.addr, !11, !DIExpression(), !13)
    #dbg_declare(ptr %x, !12, !DIExpression(), !14)
  %0 = load i32, ptr %a.addr, !dbg !15
  %add = add i32 %0, 1, !dbg !15
  store i32 %add, ptr %x, !dbg !14
  %1 = load i32, ptr %x, !dbg !16
  ret i32 %1, !dbg !16
}

define i32 @g() uwtable "frame-pointer"="all" !dbg !17 {
  ret i32 7, !dbg !18
}

define i32 @nodebug(i32 %a) uwtable "frame-pointer"="all" {
  ret i32 %a
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4, !5}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "clang", emissionKind: FullDebug)
!1 = !DIFile(filename: "a.c", directory: "/tmp")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 7, !"uwtable", i32 2}
!5 = !{i32 7, !"frame-pointer", i32 2}
!6 = distinct !DISubprogram(name: "f", scope: !1, file: !1, line: 1, type: !7, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !10)
!7 = !DISubroutineType(types: !8)
!8 = !{!9, !9}
!9 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!10 = !{}
!11 = !DILocalVariable(name: "a", arg: 1, scope: !6, file: !1, line: 1, type: !9)
!12 = !DILocalVariable(name: "x", scope: !6, file: !1, line: 2, type: !9)
!13 = !DILocation(line: 1, column: 11, scope: !6)
!14 = !DILocation(line: 2, column: 7, scope: !6)
!15 = !DILocation(line: 2, column: 11, scope: !6)
!16 = !DILocation(line: 3, column: 3, scope: !6)
!17 = distinct !DISubprogram(name: "g", scope: !1, file: !1, line: 6, type: !19, scopeLine: 6, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !10)
!18 = !DILocation(line: 7, column: 3, scope: !17)
!19 = !DISubroutineType(types: !20)
!20 = !{!9}

; CHECK:      DW_TAG_subprogram
; CHECK:        DW_AT_low_pc
; CHECK:        DW_AT_high_pc
; CHECK:        DW_AT_frame_base (DW_OP_reg29 W29)
; CHECK:        DW_AT_name ("f")
; CHECK:        DW_TAG_formal_parameter
; CHECK-NEXT:     DW_AT_location (DW_OP_fbreg +16)
; CHECK-NEXT:     DW_AT_name ("a")
; CHECK:        DW_TAG_variable
; CHECK-NEXT:     DW_AT_location (DW_OP_fbreg +24)
; CHECK-NEXT:     DW_AT_name ("x")

;; The prologue is written last and shorter than the space reserved for it; the
;; rows follow the code.
; CHECK:      Address            Line   Column File   ISA Discriminator OpIndex Flags
; CHECK:      0x0000000000000000      1      0      0   0             0       0  is_stmt
; CHECK-NEXT: 0x000000000000000c      2     11      0   0             0       0  is_stmt prologue_end
; CHECK-NEXT: 0x{{[0-9a-f]+}}      2      7
; CHECK-NEXT: 0x{{[0-9a-f]+}}      3      3      0   0             0       0  is_stmt
; CHECK:      epilogue_begin
; CHECK-NEXT: 0x[[#%.16x,G:]]      6      0      0   0             0       0  is_stmt
; CHECK-NEXT: 0x[[#%.16x,G+8]]      7      3      0   0             0       0  is_stmt prologue_end
