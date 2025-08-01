; NOTE: Assertions have been autogenerated by utils/update_test_checks.py UTC_ARGS: --check-globals none --version 5
; RUN: opt < %s -passes=loop-vectorize -mtriple riscv64 -mattr=+v -S | FileCheck %s -check-prefix=NO-ZVFHMIN
; RUN: opt < %s -passes=loop-vectorize -mtriple riscv64 -mattr=+v -S -prefer-predicate-over-epilogue=predicate-else-scalar-epilogue | FileCheck %s -check-prefix=NO-ZVFHMIN
; RUN: opt < %s -passes=loop-vectorize -mtriple riscv64 -mattr=+v,+zvfhmin -S | FileCheck %s -check-prefix=ZVFHMIN

define void @fadd(ptr noalias %a, ptr noalias %b, i64 %n) {
; NO-ZVFHMIN-LABEL: define void @fadd(
; NO-ZVFHMIN-SAME: ptr noalias [[A:%.*]], ptr noalias [[B:%.*]], i64 [[N:%.*]]) #[[ATTR0:[0-9]+]] {
; NO-ZVFHMIN-NEXT:  [[ENTRY:.*]]:
; NO-ZVFHMIN-NEXT:    br label %[[LOOP:.*]]
; NO-ZVFHMIN:       [[LOOP]]:
; NO-ZVFHMIN-NEXT:    [[I:%.*]] = phi i64 [ 0, %[[ENTRY]] ], [ [[I_NEXT:%.*]], %[[LOOP]] ]
; NO-ZVFHMIN-NEXT:    [[A_GEP:%.*]] = getelementptr half, ptr [[A]], i64 [[I]]
; NO-ZVFHMIN-NEXT:    [[B_GEP:%.*]] = getelementptr half, ptr [[B]], i64 [[I]]
; NO-ZVFHMIN-NEXT:    [[X:%.*]] = load half, ptr [[A_GEP]], align 2
; NO-ZVFHMIN-NEXT:    [[Y:%.*]] = load half, ptr [[B_GEP]], align 2
; NO-ZVFHMIN-NEXT:    [[Z:%.*]] = fadd half [[X]], [[Y]]
; NO-ZVFHMIN-NEXT:    store half [[Z]], ptr [[A_GEP]], align 2
; NO-ZVFHMIN-NEXT:    [[I_NEXT]] = add i64 [[I]], 1
; NO-ZVFHMIN-NEXT:    [[DONE:%.*]] = icmp eq i64 [[I_NEXT]], [[N]]
; NO-ZVFHMIN-NEXT:    br i1 [[DONE]], label %[[EXIT:.*]], label %[[LOOP]]
; NO-ZVFHMIN:       [[EXIT]]:
; NO-ZVFHMIN-NEXT:    ret void
;
; ZVFHMIN-LABEL: define void @fadd(
; ZVFHMIN-SAME: ptr noalias [[A:%.*]], ptr noalias [[B:%.*]], i64 [[N:%.*]]) #[[ATTR0:[0-9]+]] {
; ZVFHMIN-NEXT:  [[ENTRY:.*]]:
; ZVFHMIN-NEXT:    [[TMP7:%.*]] = call i64 @llvm.vscale.i64()
; ZVFHMIN-NEXT:    [[TMP8:%.*]] = mul nuw i64 [[TMP7]], 8
; ZVFHMIN-NEXT:    [[MIN_ITERS_CHECK:%.*]] = icmp ult i64 [[N]], [[TMP8]]
; ZVFHMIN-NEXT:    br i1 [[MIN_ITERS_CHECK]], label %[[SCALAR_PH:.*]], label %[[VECTOR_PH:.*]]
; ZVFHMIN:       [[VECTOR_PH]]:
; ZVFHMIN-NEXT:    [[TMP9:%.*]] = call i64 @llvm.vscale.i64()
; ZVFHMIN-NEXT:    [[TMP10:%.*]] = mul nuw i64 [[TMP9]], 8
; ZVFHMIN-NEXT:    [[N_MOD_VF:%.*]] = urem i64 [[N]], [[TMP10]]
; ZVFHMIN-NEXT:    [[N_VEC:%.*]] = sub i64 [[N]], [[N_MOD_VF]]
; ZVFHMIN-NEXT:    [[TMP12:%.*]] = call i64 @llvm.vscale.i64()
; ZVFHMIN-NEXT:    [[TMP5:%.*]] = mul nuw i64 [[TMP12]], 8
; ZVFHMIN-NEXT:    br label %[[VECTOR_BODY:.*]]
; ZVFHMIN:       [[VECTOR_BODY]]:
; ZVFHMIN-NEXT:    [[INDEX:%.*]] = phi i64 [ 0, %[[VECTOR_PH]] ], [ [[INDEX_NEXT:%.*]], %[[VECTOR_BODY]] ]
; ZVFHMIN-NEXT:    [[TMP1:%.*]] = getelementptr half, ptr [[A]], i64 [[INDEX]]
; ZVFHMIN-NEXT:    [[TMP2:%.*]] = getelementptr half, ptr [[B]], i64 [[INDEX]]
; ZVFHMIN-NEXT:    [[WIDE_LOAD:%.*]] = load <vscale x 8 x half>, ptr [[TMP1]], align 2
; ZVFHMIN-NEXT:    [[WIDE_LOAD1:%.*]] = load <vscale x 8 x half>, ptr [[TMP2]], align 2
; ZVFHMIN-NEXT:    [[TMP11:%.*]] = fadd <vscale x 8 x half> [[WIDE_LOAD]], [[WIDE_LOAD1]]
; ZVFHMIN-NEXT:    store <vscale x 8 x half> [[TMP11]], ptr [[TMP1]], align 2
; ZVFHMIN-NEXT:    [[INDEX_NEXT]] = add nuw i64 [[INDEX]], [[TMP5]]
; ZVFHMIN-NEXT:    [[TMP6:%.*]] = icmp eq i64 [[INDEX_NEXT]], [[N_VEC]]
; ZVFHMIN-NEXT:    br i1 [[TMP6]], label %[[MIDDLE_BLOCK:.*]], label %[[VECTOR_BODY]], !llvm.loop [[LOOP0:![0-9]+]]
; ZVFHMIN:       [[MIDDLE_BLOCK]]:
; ZVFHMIN-NEXT:    [[CMP_N:%.*]] = icmp eq i64 [[N]], [[N_VEC]]
; ZVFHMIN-NEXT:    br i1 [[CMP_N]], label %[[EXIT:.*]], label %[[SCALAR_PH]]
; ZVFHMIN:       [[SCALAR_PH]]:
; ZVFHMIN-NEXT:    [[BC_RESUME_VAL:%.*]] = phi i64 [ [[N_VEC]], %[[MIDDLE_BLOCK]] ], [ 0, %[[ENTRY]] ]
; ZVFHMIN-NEXT:    br label %[[LOOP:.*]]
; ZVFHMIN:       [[LOOP]]:
; ZVFHMIN-NEXT:    [[I:%.*]] = phi i64 [ [[BC_RESUME_VAL]], %[[SCALAR_PH]] ], [ [[I_NEXT:%.*]], %[[LOOP]] ]
; ZVFHMIN-NEXT:    [[A_GEP:%.*]] = getelementptr half, ptr [[A]], i64 [[I]]
; ZVFHMIN-NEXT:    [[B_GEP:%.*]] = getelementptr half, ptr [[B]], i64 [[I]]
; ZVFHMIN-NEXT:    [[X:%.*]] = load half, ptr [[A_GEP]], align 2
; ZVFHMIN-NEXT:    [[Y:%.*]] = load half, ptr [[B_GEP]], align 2
; ZVFHMIN-NEXT:    [[Z:%.*]] = fadd half [[X]], [[Y]]
; ZVFHMIN-NEXT:    store half [[Z]], ptr [[A_GEP]], align 2
; ZVFHMIN-NEXT:    [[I_NEXT]] = add i64 [[I]], 1
; ZVFHMIN-NEXT:    [[DONE:%.*]] = icmp eq i64 [[I_NEXT]], [[N]]
; ZVFHMIN-NEXT:    br i1 [[DONE]], label %[[EXIT]], label %[[LOOP]], !llvm.loop [[LOOP3:![0-9]+]]
; ZVFHMIN:       [[EXIT]]:
; ZVFHMIN-NEXT:    ret void
;
; NO-ZVFHMIN-PREDICATED-LABEL: define void @fadd(
; NO-ZVFHMIN-PREDICATED-SAME: ptr noalias [[A:%.*]], ptr noalias [[B:%.*]], i64 [[N:%.*]]) #[[ATTR0:[0-9]+]] {
; NO-ZVFHMIN-PREDICATED-NEXT:  [[ENTRY:.*]]:
; NO-ZVFHMIN-PREDICATED-NEXT:    br label %[[LOOP:.*]]
; NO-ZVFHMIN-PREDICATED:       [[LOOP]]:
; NO-ZVFHMIN-PREDICATED-NEXT:    [[I:%.*]] = phi i64 [ 0, %[[ENTRY]] ], [ [[I_NEXT:%.*]], %[[LOOP]] ]
; NO-ZVFHMIN-PREDICATED-NEXT:    [[A_GEP:%.*]] = getelementptr half, ptr [[A]], i64 [[I]]
; NO-ZVFHMIN-PREDICATED-NEXT:    [[B_GEP:%.*]] = getelementptr half, ptr [[B]], i64 [[I]]
; NO-ZVFHMIN-PREDICATED-NEXT:    [[X:%.*]] = load half, ptr [[A_GEP]], align 2
; NO-ZVFHMIN-PREDICATED-NEXT:    [[Y:%.*]] = load half, ptr [[B_GEP]], align 2
; NO-ZVFHMIN-PREDICATED-NEXT:    [[Z:%.*]] = fadd half [[X]], [[Y]]
; NO-ZVFHMIN-PREDICATED-NEXT:    store half [[Z]], ptr [[A_GEP]], align 2
; NO-ZVFHMIN-PREDICATED-NEXT:    [[I_NEXT]] = add i64 [[I]], 1
; NO-ZVFHMIN-PREDICATED-NEXT:    [[DONE:%.*]] = icmp eq i64 [[I_NEXT]], [[N]]
; NO-ZVFHMIN-PREDICATED-NEXT:    br i1 [[DONE]], label %[[EXIT:.*]], label %[[LOOP]]
; NO-ZVFHMIN-PREDICATED:       [[EXIT]]:
; NO-ZVFHMIN-PREDICATED-NEXT:    ret void
entry:
  br label %loop
loop:
  %i = phi i64 [0, %entry], [%i.next, %loop]
  %a.gep = getelementptr half, ptr %a, i64 %i
  %b.gep = getelementptr half, ptr %b, i64 %i
  %x = load half, ptr %a.gep
  %y = load half, ptr %b.gep
  %z = fadd half %x, %y
  store half %z, ptr %a.gep
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  ret void
}
