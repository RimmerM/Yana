; The same three shapes as bench.lower, instruction for instruction: the same loads from the same
; pointers, the same accumulation order, the same call, the same branch weights. Fed to `llc`, which
; is LLVM's backend alone - no mid-level pass runs, so what is being compared is instruction
; selection and register allocation against ours and not one pipeline against another.

target triple = "x86_64-unknown-linux-gnu"

declare i32 @sink(i32)

define i64 @acrossCall(i32 %n, ptr %p0, ptr %p1, ptr %p2, ptr %p3, ptr %p4, ptr %p5, ptr %p6, ptr %p7) {
  %r = call i32 @sink(i32 %n)

  %a0 = load volatile i64, ptr %p0, align 8
  %a1 = load volatile i64, ptr %p1, align 8
  %t0 = add i64 %a0, %a1
  %a2 = load volatile i64, ptr %p2, align 8
  %t1 = add i64 %t0, %a2
  %a3 = load volatile i64, ptr %p3, align 8
  %t2 = add i64 %t1, %a3
  %a4 = load volatile i64, ptr %p4, align 8
  %t3 = add i64 %t2, %a4
  %a5 = load volatile i64, ptr %p5, align 8
  %t4 = add i64 %t3, %a5
  %a6 = load volatile i64, ptr %p6, align 8
  %t5 = add i64 %t4, %a6
  %a7 = load volatile i64, ptr %p7, align 8
  %t6 = add i64 %t5, %a7

  %b0 = load volatile i64, ptr %p0, align 8
  %t7 = add i64 %t6, %b0
  %b1 = load volatile i64, ptr %p1, align 8
  %t8 = add i64 %t7, %b1
  %b2 = load volatile i64, ptr %p2, align 8
  %t9 = add i64 %t8, %b2
  %b3 = load volatile i64, ptr %p3, align 8
  %t10 = add i64 %t9, %b3
  %b4 = load volatile i64, ptr %p4, align 8
  %t11 = add i64 %t10, %b4
  %b5 = load volatile i64, ptr %p5, align 8
  %t12 = add i64 %t11, %b5
  %b6 = load volatile i64, ptr %p6, align 8
  %t13 = add i64 %t12, %b6
  %b7 = load volatile i64, ptr %p7, align 8
  %t14 = add i64 %t13, %b7

  %c0 = load volatile i64, ptr %p0, align 8
  %t15 = add i64 %t14, %c0
  %c1 = load volatile i64, ptr %p1, align 8
  %t16 = add i64 %t15, %c1
  %c2 = load volatile i64, ptr %p2, align 8
  %t17 = add i64 %t16, %c2
  %c3 = load volatile i64, ptr %p3, align 8
  %t18 = add i64 %t17, %c3
  %c4 = load volatile i64, ptr %p4, align 8
  %t19 = add i64 %t18, %c4
  %c5 = load volatile i64, ptr %p5, align 8
  %t20 = add i64 %t19, %c5
  %c6 = load volatile i64, ptr %p6, align 8
  %t21 = add i64 %t20, %c6
  %c7 = load volatile i64, ptr %p7, align 8
  %t22 = add i64 %t21, %c7

  ret i64 %t22
}

define i64 @loopCall(i32 %n, ptr %p0, ptr %p1, ptr %p2, ptr %p3, ptr %p4, ptr %p5, ptr %p6, ptr %p7) {
entry:
  br label %head

head:
  %i = phi i64 [ 0, %entry ], [ %i2, %body ]
  %acc = phi i64 [ 0, %entry ], [ %acc2, %body ]
  %c = icmp eq i32 %n, 0
  br i1 %c, label %exit, label %body

body:
  %r = call i32 @sink(i32 %n)


  %a0 = load volatile i64, ptr %p0, align 8
  %a1 = load volatile i64, ptr %p1, align 8
  %t0 = add i64 %a0, %a1
  %a2 = load volatile i64, ptr %p2, align 8
  %t1 = add i64 %t0, %a2
  %a3 = load volatile i64, ptr %p3, align 8
  %t2 = add i64 %t1, %a3
  %a4 = load volatile i64, ptr %p4, align 8
  %t3 = add i64 %t2, %a4
  %a5 = load volatile i64, ptr %p5, align 8
  %t4 = add i64 %t3, %a5
  %a6 = load volatile i64, ptr %p6, align 8
  %t5 = add i64 %t4, %a6
  %a7 = load volatile i64, ptr %p7, align 8
  %t6 = add i64 %t5, %a7

  %b0 = load volatile i64, ptr %p0, align 8
  %u0 = add i64 %t6, %b0
  %b1 = load volatile i64, ptr %p1, align 8
  %u1 = add i64 %u0, %b1
  %b2 = load volatile i64, ptr %p2, align 8
  %u2 = add i64 %u1, %b2
  %b3 = load volatile i64, ptr %p3, align 8
  %u3 = add i64 %u2, %b3
  %b4 = load volatile i64, ptr %p4, align 8
  %u4 = add i64 %u3, %b4
  %b5 = load volatile i64, ptr %p5, align 8
  %u5 = add i64 %u4, %b5
  %b6 = load volatile i64, ptr %p6, align 8
  %u6 = add i64 %u5, %b6
  %b7 = load volatile i64, ptr %p7, align 8
  %u7 = add i64 %u6, %b7

  %c0 = load volatile i64, ptr %p0, align 8
  %v0 = add i64 %u7, %c0
  %c1 = load volatile i64, ptr %p1, align 8
  %v1 = add i64 %v0, %c1
  %c2 = load volatile i64, ptr %p2, align 8
  %v2 = add i64 %v1, %c2
  %c3 = load volatile i64, ptr %p3, align 8
  %v3 = add i64 %v2, %c3
  %c4 = load volatile i64, ptr %p4, align 8
  %v4 = add i64 %v3, %c4
  %c5 = load volatile i64, ptr %p5, align 8
  %v5 = add i64 %v4, %c5
  %c6 = load volatile i64, ptr %p6, align 8
  %v6 = add i64 %v5, %c6
  %c7 = load volatile i64, ptr %p7, align 8
  %v7 = add i64 %v6, %c7

  %acc2 = add i64 %acc, %v7
  %i2 = add i64 %i, 1
  br label %head

exit:
  ret i64 %acc
}

define i64 @coldCall(i32 %n, ptr %p0, ptr %p1, ptr %p2, ptr %p3, ptr %p4, ptr %p5, ptr %p6, ptr %p7) {
entry:
  br label %head

head:
  %i = phi i64 [ 0, %entry ], [ %i2, %rejoin ]
  %acc = phi i64 [ 0, %entry ], [ %acc2, %rejoin ]
  %c = icmp eq i32 %n, 0
  br i1 %c, label %exit, label %hot

hot:
  %a0 = load volatile i64, ptr %p0, align 8
  %a1 = load volatile i64, ptr %p1, align 8
  %t0 = add i64 %a0, %a1
  %a2 = load volatile i64, ptr %p2, align 8
  %t1 = add i64 %t0, %a2
  %a3 = load volatile i64, ptr %p3, align 8
  %t2 = add i64 %t1, %a3
  %a4 = load volatile i64, ptr %p4, align 8
  %t3 = add i64 %t2, %a4
  %a5 = load volatile i64, ptr %p5, align 8
  %t4 = add i64 %t3, %a5
  %a6 = load volatile i64, ptr %p6, align 8
  %t5 = add i64 %t4, %a6
  %a7 = load volatile i64, ptr %p7, align 8
  %t6 = add i64 %t5, %a7
  %d = icmp eq i64 %i, 0
  br i1 %d, label %rejoin, label %cold, !prof !0

cold:
  %r = call i32 @sink(i32 %n)
  br label %rejoin

rejoin:
  %acc2 = add i64 %acc, %t6
  %i2 = add i64 %i, 1
  br label %head

exit:
  ret i64 %acc
}

!0 = !{!"branch_weights", i32 999, i32 1}
