NVCC=nvcc
CUDAFLAGS=-O2 -std=c++17 -arch=sm_86
P2FLAGS=$(CUDAFLAGS) -fmad=false

.PHONY: all problem1 problem2 problem2-v1 problem2-v2 problem2-v3 problem3 problem3-vanilla problem3-blocked traces clean

all: problem1 problem2 problem3

problem1: problem1/230217-prob1.cu
	$(NVCC) $(CUDAFLAGS) $< -o problem1/problem1.out

problem2: problem2-v1 problem2-v2 problem2-v3

problem2-v1: problem2/230217-prob2.cu
	$(NVCC) $(P2FLAGS) -DVERSION=1 $< -o problem2/problem2-v1.out

problem2-v2: problem2/230217-prob2.cu
	$(NVCC) $(P2FLAGS) -DVERSION=2 $< -o problem2/problem2-v2.out

problem2-v3: problem2/230217-prob2.cu
	$(NVCC) $(P2FLAGS) -DVERSION=3 $< -o problem2/problem2-v3.out

problem3: problem3-vanilla problem3-blocked

problem3-vanilla: problem3/230217-prob3.cu
	$(NVCC) $(CUDAFLAGS) -lineinfo $< -o problem3/problem3-vanilla.out

problem3-blocked: problem3/230217-prob3.cu
	$(NVCC) $(CUDAFLAGS) -lineinfo -DBLOCKED $< -o problem3/problem3-blocked.out

traces: problem3/trace_generator.cpp
	g++ -O2 -std=c++17 $< -o problem3/trace_generator.out

clean:
	rm -f problem1/*.out problem2/*.out problem2/results-v*.txt problem3/*.out
