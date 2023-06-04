// RUN: mlir-clang %s --function=* -S | FileCheck %s

extern "C" {

double kernel_deriche(int x, float y) {
    ([&y,x]() {
        y *= x;
    })();
    return y;
}

}

// CHECK:   func @kernel_deriche(%arg0: i32, %arg1: f32) -> f64 attributes {llvm.linkage = #llvm.linkage<external>} {
// CHECK-DAG:     %c0_i32 = arith.constant 0 : i32
// CHECK-DAG:     %c1_i64 = arith.constant 1 : i64
// CHECK-NEXT:     %0 = llvm.alloca %c1_i64 x !llvm.struct<(memref<?xf32>, i32)> : (i64) -> !llvm.ptr<!llvm.struct<(memref<?xf32>, i32)>>
// CHECK-NEXT:     %1 = llvm.alloca %c1_i64 x !llvm.struct<(memref<?xf32>, i32)> : (i64) -> !llvm.ptr<!llvm.struct<(memref<?xf32>, i32)>>
// CHECK-NEXT:     %2 = memref.alloca() : memref<1xf32>
// CHECK-NEXT:     affine.store %arg1, %2[0] : memref<1xf32>
// CHECK-NEXT:     %3 = memref.cast %2 : memref<1xf32> to memref<?xf32>
// CHECK-NEXT:     %4 = llvm.getelementptr %1[%c0_i32, 0] : (!llvm.ptr<!llvm.struct<(memref<?xf32>, i32)>>, i32) -> !llvm.ptr<memref<?xf32>>
// CHECK-NEXT:     llvm.store %3, %4 : !llvm.ptr<memref<?xf32>>
// CHECK-NEXT:     %5 = llvm.getelementptr %1[%c0_i32, 1] : (!llvm.ptr<!llvm.struct<(memref<?xf32>, i32)>>, i32) -> !llvm.ptr<i32>
// CHECK-NEXT:     llvm.store %arg0, %5 : !llvm.ptr<i32>
// CHECK-NEXT:     %6 = llvm.load %1 : !llvm.ptr<!llvm.struct<(memref<?xf32>, i32)>>
// CHECK-NEXT:     llvm.store %6, %0 : !llvm.ptr<!llvm.struct<(memref<?xf32>, i32)>>
// CHECK-NEXT:     call @_ZZ14kernel_dericheENK3$_0clEv(%0) : (!llvm.ptr<!llvm.struct<(memref<?xf32>, i32)>>) -> ()
// CHECK-NEXT:     %7 = affine.load %2[0] : memref<1xf32>
// CHECK-NEXT:     %8 = arith.extf %7 : f32 to f64
// CHECK-NEXT:     return %8 : f64
// CHECK-NEXT:   }
// CHECK:   func private @_ZZ14kernel_dericheENK3$_0clEv(%arg0: !llvm.ptr<!llvm.struct<(memref<?xf32>, i32)>>) attributes {llvm.linkage = #llvm.linkage<internal>} {
// CHECK-NEXT:     %c0_i32 = arith.constant 0 : i32
// CHECK-NEXT:     %0 = llvm.getelementptr %arg0[%c0_i32, 0] : (!llvm.ptr<!llvm.struct<(memref<?xf32>, i32)>>, i32) -> !llvm.ptr<memref<?xf32>>
// CHECK-NEXT:     %1 = llvm.load %0 : !llvm.ptr<memref<?xf32>>
// CHECK-NEXT:     %2 = llvm.getelementptr %arg0[%c0_i32, 1] : (!llvm.ptr<!llvm.struct<(memref<?xf32>, i32)>>, i32) -> !llvm.ptr<i32>
// CHECK-NEXT:     %3 = llvm.load %2 : !llvm.ptr<i32>
// CHECK-NEXT:     %4 = arith.sitofp %3 : i32 to f32
// CHECK-NEXT:     %5 = affine.load %1[0] : memref<?xf32>
// CHECK-NEXT:     %6 = arith.mulf %5, %4 : f32
// CHECK-NEXT:     affine.store %6, %1[0] : memref<?xf32>
// CHECK-NEXT:     return
// CHECK-NEXT:   }
