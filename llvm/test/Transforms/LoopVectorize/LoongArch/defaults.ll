; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --version 4
; RUN: opt < %s -passes=loop-vectorize -mtriple loongarch64-linux-gnu -mattr=+lasx -S | FileCheck %s

;; This is a collection of tests whose only purpose is to show changes in the
;; default configuration.  Please keep these tests minimal - if you're testing
;; functionality of some specific configuration, please place that in a
;; separate test file with a hard coded configuration (even if that
;; configuration is the current default).

target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n64-S128"
target triple = "loongarch64"

define void @vector_add(ptr noalias nocapture %a, i64 %v) {
; CHECK-LABEL: define void @vector_add(
; CHECK-SAME: ptr noalias captures(none) [[A:%.*]], i64 [[V:%.*]]) #[[ATTR0:[0-9]+]] {
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br i1 false, label [[SCALAR_PH:%.*]], label [[VECTOR_PH:%.*]]
; CHECK:       vector.ph:
; CHECK-NEXT:    [[BROADCAST_SPLATINSERT:%.*]] = insertelement <4 x i64> poison, i64 [[V]], i64 0
; CHECK-NEXT:    [[BROADCAST_SPLAT:%.*]] = shufflevector <4 x i64> [[BROADCAST_SPLATINSERT]], <4 x i64> poison, <4 x i32> zeroinitializer
; CHECK-NEXT:    br label [[VECTOR_BODY:%.*]]
; CHECK:       vector.body:
; CHECK-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, [[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], [[VECTOR_BODY]] ]
; CHECK-NEXT:    [[TMP2:%.*]] = getelementptr inbounds i64, ptr [[A]], i64 [[INDEX]]
; CHECK-NEXT:    [[TMP5:%.*]] = getelementptr inbounds i64, ptr [[TMP2]], i32 4
; CHECK-NEXT:    [[WIDE_LOAD:%.*]] = load <4 x i64>, ptr [[TMP2]], align 8
; CHECK-NEXT:    [[WIDE_LOAD1:%.*]] = load <4 x i64>, ptr [[TMP5]], align 8
; CHECK-NEXT:    [[TMP6:%.*]] = add <4 x i64> [[WIDE_LOAD]], [[BROADCAST_SPLAT]]
; CHECK-NEXT:    [[TMP7:%.*]] = add <4 x i64> [[WIDE_LOAD1]], [[BROADCAST_SPLAT]]
; CHECK-NEXT:    store <4 x i64> [[TMP6]], ptr [[TMP2]], align 8
; CHECK-NEXT:    store <4 x i64> [[TMP7]], ptr [[TMP5]], align 8
; CHECK-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], 8
; CHECK-NEXT:    [[TMP8:%.*]] = icmp eq i64 [[INDEX_NEXT]], 1024
; CHECK-NEXT:    br i1 [[TMP8]], label [[MIDDLE_BLOCK:%.*]], label [[VECTOR_BODY]], !llvm.loop [[LOOP0:![0-9]+]]
; CHECK:       middle.block:
; CHECK-NEXT:    br label [[FOR_END:%.*]]
; CHECK:       scalar.ph:
; CHECK-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ 0, [[ENTRY:%.*]] ]
; CHECK-NEXT:    br label [[FOR_BODY:%.*]]
; CHECK:       for.body:
; CHECK-NEXT:    [[IV:%.*]] = phi i64 [ [[BC_RESUME_VAL]], [[SCALAR_PH]] ], [ [[IV_NEXT:%.*]], [[FOR_BODY]] ]
; CHECK-NEXT:    [[ARRAYIDX:%.*]] = getelementptr inbounds i64, ptr [[A]], i64 [[IV]]
; CHECK-NEXT:    [[ELEM:%.*]] = load i64, ptr [[ARRAYIDX]], align 8
; CHECK-NEXT:    [[ADD:%.*]] = add i64 [[ELEM]], [[V]]
; CHECK-NEXT:    store i64 [[ADD]], ptr [[ARRAYIDX]], align 8
; CHECK-NEXT:    [[IV_NEXT]] = add nuw nsw i64 [[IV]], 1
; CHECK-NEXT:    [[EXITCOND_NOT:%.*]] = icmp eq i64 [[IV_NEXT]], 1024
; CHECK-NEXT:    br i1 [[EXITCOND_NOT]], label [[FOR_END]], label [[FOR_BODY]], !llvm.loop [[LOOP3:![0-9]+]]
; CHECK:       for.end:
; CHECK-NEXT:    ret void
;
entry:
  br label %for.body

for.body:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %for.body ]
  %arrayidx = getelementptr inbounds i64, ptr %a, i64 %iv
  %elem = load i64, ptr %arrayidx
  %add = add i64 %elem, %v
  store i64 %add, ptr %arrayidx
  %iv.next = add nuw nsw i64 %iv, 1
  %exitcond.not = icmp eq i64 %iv.next, 1024
  br i1 %exitcond.not, label %for.end, label %for.body

for.end:
  ret void
}
;.
; CHECK: [[LOOP0]] = distinct !{[[LOOP0]], [[META1:![0-9]+]], [[META2:![0-9]+]]}
; CHECK: [[META1]] = !{!"llvm.loop.isvectorized", i32 1}
; CHECK: [[META2]] = !{!"llvm.loop.unroll.runtime.disable"}
; CHECK: [[LOOP3]] = distinct !{[[LOOP3]], [[META2]], [[META1]]}
;.
