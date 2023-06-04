// RUN: mlir-clang %s %stdinclude --function=alloc -S | FileCheck %s

#include <stdio.h>
#include <stdlib.h>

int* alloc() {
    int no_of_nodes;

	scanf("%d",&no_of_nodes);
   
	// allocate host memory
	int* h_graph_nodes = (int*) malloc(sizeof(int)*no_of_nodes);

	// initalize the memory
	for( unsigned int i = 0; i < no_of_nodes; i++) 
	{
		scanf("%d\n", &h_graph_nodes[i]);
    }
	return h_graph_nodes;
}

// CHECK: llvm.mlir.global internal constant @str1("%d\0A\00")
// CHECK-NEXT: llvm.mlir.global internal constant @str0("%d\00")
// CHECK-NEXT: llvm.func @__isoc99_scanf(!llvm.ptr<i8>, ...) -> i32
// CHECK-NEXT:  func @alloc() -> memref<?xi32>
// CHECK-DAG:    %c1 = arith.constant 1 : index
// CHECK-DAG:    %c0 = arith.constant 0 : index
// CHECK-DAG:    %c4 = arith.constant 4 : index
// CHECK-DAG:    %c0_i32 = arith.constant 0 : i32
// CHECK-DAG:    %c1_i64 = arith.constant 1 : i64
// CHECK-NEXT:    %0 = llvm.alloca %c1_i64 x i32 : (i64) -> !llvm.ptr<i32>
// CHECK-NEXT:    %1 = llvm.mlir.addressof @str0 : !llvm.ptr<array<3 x i8>>
// CHECK-NEXT:    %2 = llvm.getelementptr %1[%c0_i32, %c0_i32] : (!llvm.ptr<array<3 x i8>>, i32, i32) -> !llvm.ptr<i8>
// CHECK-NEXT:    %3 = llvm.call @__isoc99_scanf(%2, %0) : (!llvm.ptr<i8>, !llvm.ptr<i32>) -> i32
// CHECK-NEXT:    %4 = llvm.load %0 : !llvm.ptr<i32>
// CHECK-NEXT:    %5 = arith.index_cast %4 : i32 to index
// CHECK-NEXT:    %6 = arith.muli %5, %c4 : index
// CHECK-NEXT:    %7 = arith.divui %6, %c4 : index
// CHECK-NEXT:    %8 = memref.alloc(%7) : memref<?xi32>
// CHECK-NEXT:      %9 = llvm.mlir.addressof @str1 : !llvm.ptr<array<4 x i8>>
// CHECK-NEXT:      %10 = llvm.getelementptr %9[%c0_i32, %c0_i32] : (!llvm.ptr<array<4 x i8>>, i32, i32) -> !llvm.ptr<i8>
// CHECK-NEXT:    scf.for %arg0 = %c0 to %5 step %c1 {
// CHECK-NEXT:      %11 = llvm.call @__isoc99_scanf(%10, %0) : (!llvm.ptr<i8>, !llvm.ptr<i32>) -> i32
// CHECK-NEXT:      %12 = llvm.load %0 : !llvm.ptr<i32>
// CHECK-NEXT:      memref.store %12, %8[%arg0] : memref<?xi32>
// CHECK-NEXT:    }
// CHECK-NEXT:    return %8 : memref<?xi32>
// CHECK-NEXT:  }
