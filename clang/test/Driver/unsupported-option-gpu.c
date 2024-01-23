/// Some target-specific options are ignored for GPU.
// DEFINE: %{gpu_ignored_opts} = --cuda-gpu-arch=sm_60 --cuda-path=%S/Inputs/CUDA/usr/local/cuda --no-cuda-version-check
// DEFINE: %{cuda_ignored_opts} = -fbasic-block-sections=all
// DEFINE: %{check} = %clang -### -c %{gpu_ignored_opts} -mcmodel=medium %s
// RUN: %{check} %{cuda_ignored_opts}

// REDEFINE: %{gpu_ignored_opts} = -x hip --rocm-path=%S/Inputs/rocm -nogpulib
// RUN: %{check}
