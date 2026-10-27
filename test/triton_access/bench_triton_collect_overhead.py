#!/usr/bin/env python3
# -------------------------------------------------------------------------
# This file is part of the MindStudio project.
# Copyright (c) 2025 Huawei Technologies Co.,Ltd.
#
# MindStudio is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#
#          http://license.coscl.org.cn/MulanPSL2
#
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
# -------------------------------------------------------------------------

"""Microbench: Python-side Triton launch overhead (collector enable vs disable).

Covers the same operator categories as ``test_triton_access_suite.py`` plus a
vector size sweep. Writes a multi-case report to ``performance.log``.

Note: this script toggles ``enable_triton_collector`` / ``disable_triton_collector``
only - it does **not** call ``msmemscope.start()``, so MSTX marks may early-return
in C++ when tracing is off. Numbers are order-of-magnitude Python interception
cost, not end-to-end tracing+dump cost. Do not pin docs to a fixed ms value.

Requires NPU + triton-ascend + msmemscope (with triton_collection).
"""

# pylint: disable=too-many-lines,duplicate-code

from __future__ import annotations

import argparse
import statistics
import time
import traceback
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List

import torch
import torch_npu  # noqa: F401
import triton
import triton.language as tl

# ---------------------------------------------------------------------------
# Kernels (subset aligned with ST suite)
# ---------------------------------------------------------------------------


@triton.jit
def msm_bench_pair_sum(x_ptr, y_ptr, out_ptr, n_elements, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    x = tl.load(x_ptr + offsets, mask=mask)
    y = tl.load(y_ptr + offsets, mask=mask)
    tl.store(out_ptr + offsets, x + y, mask=mask)


@triton.jit
def msm_bench_pair_prod(x_ptr, y_ptr, out_ptr, n_elements, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    x = tl.load(x_ptr + offsets, mask=mask)
    y = tl.load(y_ptr + offsets, mask=mask)
    tl.store(out_ptr + offsets, x * y, mask=mask)


@triton.jit
def msm_bench_clamp_pos(x_ptr, out_ptr, n_elements, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    x = tl.load(x_ptr + offsets, mask=mask)
    tl.store(out_ptr + offsets, tl.maximum(x, 0.0), mask=mask)


@triton.jit
def msm_bench_select_where(cond_ptr, x_ptr, y_ptr, out_ptr, n_elements, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    c = tl.load(cond_ptr + offsets, mask=mask)
    x = tl.load(x_ptr + offsets, mask=mask)
    y = tl.load(y_ptr + offsets, mask=mask)
    tl.store(out_ptr + offsets, tl.where(c != 0, x, y), mask=mask)


@triton.jit
def msm_bench_const_write(out_ptr, n_elements, value, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    tl.store(out_ptr + offsets, value, mask=mask)


@triton.jit
def msm_bench_inplace_gain(x_ptr, n_elements, scale, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    x = tl.load(x_ptr + offsets, mask=mask)
    tl.store(x_ptr + offsets, x * scale, mask=mask)


@triton.jit
def msm_bench_row_reduce(x_ptr, out_ptr, stride_xm, n_cols, BLOCK_SIZE: tl.constexpr):
    row = tl.program_id(0)
    cols = tl.arange(0, BLOCK_SIZE)
    mask = cols < n_cols
    vals = tl.load(x_ptr + row * stride_xm + cols, mask=mask, other=0.0)
    tl.store(out_ptr + row, tl.sum(vals, axis=0))


@triton.jit
def msm_bench_gather_slots(src_ptr, idx_ptr, out_ptr, n_elements, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    idx = tl.load(idx_ptr + offsets, mask=mask).to(tl.int32)
    vals = tl.load(src_ptr + idx, mask=mask)
    tl.store(out_ptr + offsets, vals, mask=mask)


@triton.jit
def msm_bench_scatter_slots(src_ptr, idx_ptr, out_ptr, n_elements, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    idx = tl.load(idx_ptr + offsets, mask=mask).to(tl.int32)
    src = tl.load(src_ptr + offsets, mask=mask)
    tl.store(out_ptr + idx, src, mask=mask)


@triton.jit
def msm_bench_cube_mm(
    a_ptr,
    b_ptr,
    c_ptr,
    M,
    N,
    K,
    stride_am,
    stride_ak,
    stride_bk,
    stride_bn,
    stride_cm,
    stride_cn,
    BLOCK_M: tl.constexpr,
    BLOCK_N: tl.constexpr,
    BLOCK_K: tl.constexpr,
):
    pid_m = tl.program_id(0)
    pid_n = tl.program_id(1)
    offs_m = pid_m * BLOCK_M + tl.arange(0, BLOCK_M)
    offs_n = pid_n * BLOCK_N + tl.arange(0, BLOCK_N)
    offs_k = tl.arange(0, BLOCK_K)
    acc = tl.zeros((BLOCK_M, BLOCK_N), dtype=tl.float32)
    for k in range(0, K, BLOCK_K):
        a = tl.load(
            a_ptr + offs_m[:, None] * stride_am + (k + offs_k)[None, :] * stride_ak,
            mask=(offs_m[:, None] < M) & ((k + offs_k)[None, :] < K),
            other=0.0,
        )
        b = tl.load(
            b_ptr + (k + offs_k)[:, None] * stride_bk + offs_n[None, :] * stride_bn,
            mask=((k + offs_k)[:, None] < K) & (offs_n[None, :] < N),
            other=0.0,
        )
        acc += tl.dot(a, b)
    c = acc.to(tl.float32)
    tl.store(
        c_ptr + offs_m[:, None] * stride_cm + offs_n[None, :] * stride_cn,
        c,
        mask=(offs_m[:, None] < M) & (offs_n[None, :] < N),
    )


@triton.jit
def msm_bench_row_softmax(x_ptr, out_ptr, stride_xm, n_cols, BLOCK_SIZE: tl.constexpr):
    row = tl.program_id(0)
    cols = tl.arange(0, BLOCK_SIZE)
    mask = cols < n_cols
    x = tl.load(x_ptr + row * stride_xm + cols, mask=mask, other=-float("inf"))
    x_max = tl.max(x, axis=0)
    num = tl.exp(x - x_max)
    den = tl.sum(num, axis=0)
    tl.store(out_ptr + row * stride_xm + cols, num / den, mask=mask)


@triton.jit
def msm_bench_layernorm(x_ptr, w_ptr, b_ptr, out_ptr, stride_xm, n_cols, eps, BLOCK_SIZE: tl.constexpr):
    row = tl.program_id(0)
    cols = tl.arange(0, BLOCK_SIZE)
    mask = cols < n_cols
    x = tl.load(x_ptr + row * stride_xm + cols, mask=mask, other=0.0)
    mean = tl.sum(x, axis=0) / n_cols
    x0 = x - mean
    var = tl.sum(x0 * x0, axis=0) / n_cols
    rstd = 1.0 / tl.sqrt(var + eps)
    w = tl.load(w_ptr + cols, mask=mask, other=1.0)
    b = tl.load(b_ptr + cols, mask=mask, other=0.0)
    tl.store(out_ptr + row * stride_xm + cols, x0 * rstd * w + b, mask=mask)


@triton.jit
def msm_bench_pair_sum_fp16(x_ptr, y_ptr, out_ptr, n_elements, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    x = tl.load(x_ptr + offsets, mask=mask)
    y = tl.load(y_ptr + offsets, mask=mask)
    tl.store(out_ptr + offsets, x + y, mask=mask)


@triton.jit
def msm_bench_pair_sum_alias(x_ptr, y_ptr, out_ptr, n_elements, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    x = tl.load(x_ptr + offsets, mask=mask)
    y = tl.load(y_ptr + offsets, mask=mask)
    tl.store(out_ptr + offsets, x + y, mask=mask)


@triton.jit
def msm_bench_pair_sum_bf16(x_ptr, y_ptr, out_ptr, n_elements, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    x = tl.load(x_ptr + offsets, mask=mask)
    y = tl.load(y_ptr + offsets, mask=mask)
    tl.store(out_ptr + offsets, x + y, mask=mask)


@triton.jit
def msm_bench_silu(x_ptr, out_ptr, n_elements, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    x = tl.load(x_ptr + offsets, mask=mask)
    y = x * (1.0 / (1.0 + tl.exp(-x)))
    tl.store(out_ptr + offsets, y, mask=mask)


@triton.jit
def msm_bench_clamp_range(x_ptr, out_ptr, n_elements, lo, hi, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    x = tl.load(x_ptr + offsets, mask=mask)
    y = tl.minimum(tl.maximum(x, lo), hi)
    tl.store(out_ptr + offsets, y, mask=mask)


@triton.jit
def msm_bench_memcpy(src_ptr, dst_ptr, n_elements, BLOCK_SIZE: tl.constexpr):
    pid = tl.program_id(0)
    offsets = pid * BLOCK_SIZE + tl.arange(0, BLOCK_SIZE)
    mask = offsets < n_elements
    x = tl.load(src_ptr + offsets, mask=mask)
    tl.store(dst_ptr + offsets, x, mask=mask)


@triton.jit
def msm_bench_gemm_bias_act(
    a_ptr,
    b_ptr,
    bias_ptr,
    c_ptr,
    M,
    N,
    K,
    stride_am,
    stride_ak,
    stride_bk,
    stride_bn,
    stride_cm,
    stride_cn,
    BLOCK_M: tl.constexpr,
    BLOCK_N: tl.constexpr,
    BLOCK_K: tl.constexpr,
):
    pid_m = tl.program_id(0)
    pid_n = tl.program_id(1)
    offs_m = pid_m * BLOCK_M + tl.arange(0, BLOCK_M)
    offs_n = pid_n * BLOCK_N + tl.arange(0, BLOCK_N)
    offs_k = tl.arange(0, BLOCK_K)
    a_ptrs = a_ptr + offs_m[:, None] * stride_am + offs_k[None, :] * stride_ak
    b_ptrs = b_ptr + offs_k[:, None] * stride_bk + offs_n[None, :] * stride_bn
    acc = tl.zeros((BLOCK_M, BLOCK_N), dtype=tl.float32)
    for k in range(0, K, BLOCK_K):
        a = tl.load(a_ptrs, mask=(offs_m[:, None] < M) & (offs_k[None, :] < K - k), other=0.0)
        b = tl.load(b_ptrs, mask=(offs_k[:, None] < K - k) & (offs_n[None, :] < N), other=0.0)
        acc += tl.dot(a, b)
        a_ptrs += BLOCK_K * stride_ak
        b_ptrs += BLOCK_K * stride_bk
    bias = tl.load(bias_ptr + offs_n, mask=offs_n < N, other=0.0)
    acc = tl.maximum(acc + bias[None, :], 0.0)
    c_ptrs = c_ptr + offs_m[:, None] * stride_cm + offs_n[None, :] * stride_cn
    tl.store(c_ptrs, acc, mask=(offs_m[:, None] < M) & (offs_n[None, :] < N))


@triton.jit
def msm_bench_attn_dot(
    q_ptr,
    k_ptr,
    v_ptr,
    out_ptr,
    seq_len,
    head_dim,
    scale,
    BLOCK_SEQ: tl.constexpr,
    BLOCK_D: tl.constexpr,
):
    row = tl.program_id(0)
    offs_s = tl.arange(0, BLOCK_SEQ)
    offs_d = tl.arange(0, BLOCK_D)
    mask_s = offs_s < seq_len
    mask_d = offs_d < head_dim
    q = tl.load(q_ptr + row * head_dim + offs_d, mask=mask_d, other=0.0)
    k = tl.load(
        k_ptr + offs_s[:, None] * head_dim + offs_d[None, :],
        mask=mask_s[:, None] & mask_d[None, :],
        other=0.0,
    )
    scores = tl.sum(k * q[None, :], axis=1) * scale
    scores = tl.where(mask_s, scores, float("-inf"))
    scores = scores - tl.max(scores, axis=0)
    probs = tl.exp(scores)
    probs = probs / tl.sum(probs, axis=0)
    v = tl.load(
        v_ptr + offs_s[:, None] * head_dim + offs_d[None, :],
        mask=mask_s[:, None] & mask_d[None, :],
        other=0.0,
    )
    out = tl.sum(v * probs[:, None], axis=0)
    tl.store(out_ptr + row * head_dim + offs_d, out, mask=mask_d)


# ---------------------------------------------------------------------------
# Case registry
# ---------------------------------------------------------------------------

LaunchFn = Callable[[], None]


@dataclass
class BenchCase:
    name: str
    category: str
    prepare: Callable[[torch.device], LaunchFn]


@dataclass
class CaseResult:
    name: str
    category: str
    status: str  # PASS / SKIP / FAIL
    off_mean: float = 0.0
    on_mean: float = 0.0
    delta: float = 0.0
    pct: float = 0.0
    detail: str = ""


def _fmt_ms(seconds: float) -> str:
    return f"{seconds * 1e3:.4f}"


# Regression gates (ms). Tuned from Ascend950PR baselines (~0.02 ms mean).
MAX_CASE_DELTA_MS = 0.10
MAX_MEAN_DELTA_MS = 0.05
MAX_SIZE_SWEEP_SPREAD_MS = 0.025


def _time_launches(launch: LaunchFn, iters: int) -> List[float]:
    samples = []
    for _ in range(iters):
        t0 = time.perf_counter()
        launch()
        samples.append(time.perf_counter() - t0)
    return samples


def _vec_add_case(n: int, block: int = 256) -> BenchCase:
    def prepare(device: torch.device) -> LaunchFn:
        x = torch.randn(n, device=device)
        y = torch.randn(n, device=device)
        out = torch.empty_like(x)
        grid = (triton.cdiv(n, block),)

        def launch() -> None:
            msm_bench_pair_sum[grid](x, y, out, n, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    return BenchCase(f"add_n{n}", "size_sweep_vector", prepare)


def build_cases() -> List[BenchCase]:
    n = 4096
    block = 256
    rows, cols = 64, 128

    def prep_add(device):
        x = torch.randn(n, device=device)
        y = torch.randn(n, device=device)
        out = torch.empty_like(x)
        grid = (triton.cdiv(n, block),)

        def launch():
            msm_bench_pair_sum[grid](x, y, out, n, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_mul(device):
        x = torch.randn(n, device=device)
        y = torch.randn(n, device=device)
        out = torch.empty_like(x)
        grid = (triton.cdiv(n, block),)

        def launch():
            msm_bench_pair_prod[grid](x, y, out, n, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_relu(device):
        x = torch.randn(n, device=device)
        out = torch.empty_like(x)
        grid = (triton.cdiv(n, block),)

        def launch():
            msm_bench_clamp_pos[grid](x, out, n, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_where(device):
        cond = (torch.randn(n, device=device) > 0).to(torch.int32)
        x = torch.randn(n, device=device)
        y = torch.randn(n, device=device)
        out = torch.empty_like(x)
        grid = (triton.cdiv(n, block),)

        def launch():
            msm_bench_select_where[grid](cond, x, y, out, n, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_fill(device):
        out = torch.empty(n, device=device)
        grid = (triton.cdiv(n, block),)

        def launch():
            msm_bench_const_write[grid](out, n, 1.5, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_inplace(device):
        x = torch.randn(n, device=device)
        grid = (triton.cdiv(n, block),)

        def launch():
            msm_bench_inplace_gain[grid](x, n, 1.01, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_row_sum(device):
        x = torch.randn(rows, cols, device=device)
        out = torch.empty(rows, device=device)
        grid = (rows,)

        def launch():
            msm_bench_row_reduce[grid](x, out, x.stride(0), cols, BLOCK_SIZE=128)
            torch.npu.synchronize()

        return launch

    def prep_gather(device):
        src = torch.randn(n, device=device)
        idx = torch.randint(0, n, (n // 2,), device=device, dtype=torch.int32)
        out = torch.empty(idx.numel(), device=device)
        m = idx.numel()
        grid = (triton.cdiv(m, block),)

        def launch():
            msm_bench_gather_slots[grid](src, idx, out, m, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_scatter(device):
        m = 1024
        src = torch.randn(m, device=device)
        idx = torch.arange(m, device=device, dtype=torch.int32)
        out = torch.zeros(m, device=device)
        grid = (triton.cdiv(m, block),)

        def launch():
            msm_bench_scatter_slots[grid](src, idx, out, m, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_matmul(device):
        M = N = K = 128
        a = torch.randn(M, K, device=device, dtype=torch.float16)
        b = torch.randn(K, N, device=device, dtype=torch.float16)
        c = torch.empty(M, N, device=device, dtype=torch.float32)
        bm = bn = bk = 32
        grid = (triton.cdiv(M, bm), triton.cdiv(N, bn))

        def launch():
            msm_bench_cube_mm[grid](
                a,
                b,
                c,
                M,
                N,
                K,
                a.stride(0),
                a.stride(1),
                b.stride(0),
                b.stride(1),
                c.stride(0),
                c.stride(1),
                BLOCK_M=bm,
                BLOCK_N=bn,
                BLOCK_K=bk,
            )
            torch.npu.synchronize()

        return launch

    def prep_softmax(device):
        x = torch.randn(rows, cols, device=device)
        out = torch.empty_like(x)
        grid = (rows,)

        def launch():
            msm_bench_row_softmax[grid](x, out, x.stride(0), cols, BLOCK_SIZE=128)
            torch.npu.synchronize()

        return launch

    def prep_layernorm(device):
        x = torch.randn(rows, cols, device=device)
        w = torch.randn(cols, device=device)
        b = torch.randn(cols, device=device)
        out = torch.empty_like(x)
        grid = (rows,)

        def launch():
            msm_bench_layernorm[grid](x, w, b, out, x.stride(0), cols, 1e-5, BLOCK_SIZE=128)
            torch.npu.synchronize()

        return launch

    def prep_add_fp16(device):
        x = torch.randn(n, device=device, dtype=torch.float16)
        y = torch.randn(n, device=device, dtype=torch.float16)
        out = torch.empty_like(x)
        grid = (triton.cdiv(n, block),)

        def launch():
            msm_bench_pair_sum_fp16[grid](x, y, out, n, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_add_alias(device):
        x = torch.randn(n, device=device)
        out = torch.empty_like(x)
        grid = (triton.cdiv(n, block),)

        def launch():
            msm_bench_pair_sum_alias[grid](x, x, out, n, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_add_bf16(device):
        x = torch.randn(n, device=device, dtype=torch.bfloat16)
        y = torch.randn(n, device=device, dtype=torch.bfloat16)
        out = torch.empty_like(x)
        grid = (triton.cdiv(n, block),)

        def launch():
            msm_bench_pair_sum_bf16[grid](x, y, out, n, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_silu(device):
        x = torch.randn(n, device=device)
        out = torch.empty_like(x)
        grid = (triton.cdiv(n, block),)

        def launch():
            msm_bench_silu[grid](x, out, n, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_clamp(device):
        x = torch.randn(n, device=device)
        out = torch.empty_like(x)
        grid = (triton.cdiv(n, block),)

        def launch():
            msm_bench_clamp_range[grid](x, out, n, -0.5, 0.5, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_copy(device):
        src = torch.randn(n, device=device)
        dst = torch.empty_like(src)
        grid = (triton.cdiv(n, block),)

        def launch():
            msm_bench_memcpy[grid](src, dst, n, BLOCK_SIZE=block)
            torch.npu.synchronize()

        return launch

    def prep_gemm_bias_relu(device):
        M = N = K = 64
        a = torch.randn(M, K, device=device, dtype=torch.float16)
        b = torch.randn(K, N, device=device, dtype=torch.float16)
        bias = torch.randn(N, device=device, dtype=torch.float32)
        c = torch.empty(M, N, device=device, dtype=torch.float32)
        bm = bn = bk = 32
        grid = (triton.cdiv(M, bm), triton.cdiv(N, bn))

        def launch():
            msm_bench_gemm_bias_act[grid](
                a,
                b,
                bias,
                c,
                M,
                N,
                K,
                a.stride(0),
                a.stride(1),
                b.stride(0),
                b.stride(1),
                c.stride(0),
                c.stride(1),
                BLOCK_M=bm,
                BLOCK_N=bn,
                BLOCK_K=bk,
            )
            torch.npu.synchronize()

        return launch

    def prep_attention(device):
        seq_len, head_dim = 16, 32
        q = torch.randn(seq_len, head_dim, device=device)
        k = torch.randn(seq_len, head_dim, device=device)
        v = torch.randn(seq_len, head_dim, device=device)
        out = torch.empty_like(q)
        scale = head_dim**-0.5
        grid = (seq_len,)

        def launch():
            msm_bench_attn_dot[grid](q, k, v, out, seq_len, head_dim, scale, BLOCK_SEQ=16, BLOCK_D=32)
            torch.npu.synchronize()

        return launch

    category_cases = [
        BenchCase("msm_bench_pair_sum", "vector_elemwise", prep_add),
        BenchCase("msm_bench_pair_prod", "vector_elemwise", prep_mul),
        BenchCase("msm_bench_clamp_pos", "vector_elemwise", prep_relu),
        BenchCase("msm_bench_select_where", "vector_elemwise", prep_where),
        BenchCase("msm_bench_const_write", "access_write_only", prep_fill),
        BenchCase("msm_bench_inplace_gain", "access_inplace_inout", prep_inplace),
        BenchCase("msm_bench_pair_sum_alias", "access_alias_repeat", prep_add_alias),
        BenchCase("msm_bench_row_reduce", "vector_reduce", prep_row_sum),
        BenchCase("msm_bench_gather_slots", "vector_gather_scatter", prep_gather),
        BenchCase("msm_bench_scatter_slots", "vector_gather_scatter", prep_scatter),
        BenchCase("msm_bench_cube_mm", "cube_matmul", prep_matmul),
        BenchCase("msm_bench_gemm_bias_act", "fusion_cv", prep_gemm_bias_relu),
        BenchCase("msm_bench_row_softmax", "fusion_elemwise", prep_softmax),
        BenchCase("msm_bench_layernorm", "typical_layernorm", prep_layernorm),
        BenchCase("msm_bench_attn_dot", "typical_attention", prep_attention),
        BenchCase("msm_bench_pair_sum_fp16", "dtype_fp16", prep_add_fp16),
        BenchCase("msm_bench_pair_sum_bf16", "dtype_bf16", prep_add_bf16),
        BenchCase("msm_bench_silu", "activation_silu", prep_silu),
        BenchCase("msm_bench_clamp_range", "clamp_elemwise", prep_clamp),
        BenchCase("msm_bench_memcpy", "access_copy", prep_copy),
    ]
    # Size sweep: collector overhead should stay near-constant vs problem size.
    size_cases = [
        _vec_add_case(1024),
        _vec_add_case(4096),
        _vec_add_case(65536),
        _vec_add_case(262144),
        _vec_add_case(1048576),
        _vec_add_case(2097152),
        _vec_add_case(4194304),
    ]
    return category_cases + size_cases


def run_one(
    case: BenchCase,
    device: torch.device,
    *,
    warmup: int,
    iters: int,
) -> CaseResult:
    from msmemscope.triton_collection import (
        disable_triton_collector,
        enable_triton_collector,
    )

    try:
        launch = case.prepare(device)
        for _ in range(warmup):
            launch()
    except Exception as exc:
        return CaseResult(
            case.name,
            case.category,
            "SKIP",
            detail=f"prepare/warmup failed: {exc}",
        )

    try:
        disable_triton_collector()
        off = _time_launches(launch, iters)
        enable_triton_collector()
        on = _time_launches(launch, iters)
        disable_triton_collector()
    except Exception as exc:
        disable_triton_collector()
        return CaseResult(
            case.name,
            case.category,
            "FAIL",
            detail=f"timing failed: {exc}",
        )

    off_mean = statistics.mean(off)
    on_mean = statistics.mean(on)
    delta = on_mean - off_mean
    pct = (delta / off_mean * 100.0) if off_mean > 0 else float("nan")
    delta_ms = delta * 1e3
    status = "PASS"
    detail = f"off={_fmt_ms(off_mean)}ms on={_fmt_ms(on_mean)}ms delta={_fmt_ms(delta)}ms ({pct:.1f}%)"
    if delta_ms > MAX_CASE_DELTA_MS:
        status = "FAIL"
        detail += f" [gate FAIL: delta_ms={delta_ms:.4f} > {MAX_CASE_DELTA_MS}]"
    return CaseResult(
        case.name,
        case.category,
        status,
        off_mean=off_mean,
        on_mean=on_mean,
        delta=delta,
        pct=pct,
        detail=detail,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iters", type=int, default=30)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--out", type=str, default="performance.log")
    args = parser.parse_args()

    device = torch.device("npu:0")
    torch.npu.set_device(device)

    cases = build_cases()
    results: List[CaseResult] = []
    print(
        f"bench cases={len(cases)} warmup={args.warmup} iters={args.iters} "
        f"triton={triton.__version__} torch_npu={getattr(torch_npu, '__version__', '?')}"
    )

    for case in cases:
        r = run_one(case, device, warmup=args.warmup, iters=args.iters)
        results.append(r)
        print(f"[{r.status}] {r.category}/{r.name}: {r.detail}")
        if r.status == "SKIP":
            traceback.print_exc(limit=1)

    passed = [r for r in results if r.status == "PASS"]
    failed = [r for r in results if r.status == "FAIL"]
    skipped = [r for r in results if r.status == "SKIP"]

    lines = [
        "Triton collector launch overhead (Ascend NPU) - multi-case",
        f"environment: triton={triton.__version__} torch_npu={getattr(torch_npu, '__version__', '?')}",
        f"params: warmup={args.warmup} iters={args.iters}",
        f"TOTAL PASS={len(passed)} FAIL={len(failed)} SKIP={len(skipped)} / {len(results)}",
        "",
        f"{'category':<24} {'case':<28} {'off_ms':>10} {'on_ms':>10} {'delta_ms':>10} {'pct':>8} status",
    ]
    for r in results:
        if r.status == "PASS":
            lines.append(
                f"{r.category:<24} {r.name:<28} {_fmt_ms(r.off_mean):>10} "
                f"{_fmt_ms(r.on_mean):>10} {_fmt_ms(r.delta):>10} {r.pct:>7.1f}% {r.status}"
            )
        else:
            lines.append(f"{r.category:<24} {r.name:<28} {'-':>10} {'-':>10} {'-':>10} {'-':>8} {r.status} {r.detail}")

    gate_fail = 0
    if passed:
        mean_delta = statistics.mean(r.delta for r in passed)
        mean_pct = statistics.mean(r.pct for r in passed)
        mean_delta_ms = mean_delta * 1e3
        lines.extend(
            [
                "",
                f"summary_mean_delta: {_fmt_ms(mean_delta)} ms",
                f"summary_mean_pct: {mean_pct:.2f}% vs disabled",
                f"summary_min_pct: {min(r.pct for r in passed):.2f}%",
                f"summary_max_pct: {max(r.pct for r in passed):.2f}%",
                f"gate_max_case_delta_ms: {MAX_CASE_DELTA_MS}",
                f"gate_max_mean_delta_ms: {MAX_MEAN_DELTA_MS}",
                f"gate_max_size_sweep_spread_ms: {MAX_SIZE_SWEEP_SPREAD_MS}",
            ]
        )
        if mean_delta_ms > MAX_MEAN_DELTA_MS:
            lines.append(f"GATE FAIL: mean_delta_ms={mean_delta_ms:.4f} > {MAX_MEAN_DELTA_MS}")
            gate_fail = 1
        size_pass = [r for r in passed if r.category == "size_sweep_vector"]
        if len(size_pass) >= 2:
            deltas_ms = [r.delta * 1e3 for r in size_pass]
            spread = max(deltas_ms) - min(deltas_ms)
            lines.append(f"size_sweep_delta_spread_ms: {spread:.4f}")
            if spread > MAX_SIZE_SWEEP_SPREAD_MS:
                lines.append(f"GATE FAIL: size_sweep spread={spread:.4f} > {MAX_SIZE_SWEEP_SPREAD_MS}")
                gate_fail = 1

    lines.extend(
        [
            "",
            "note: measures Python launch + synchronize per case; not Inductor Fast Launch.",
            "status=OK" if not (failed or gate_fail) else "status=FAIL",
        ]
    )
    text = "\n".join(lines) + "\n"
    print("\n" + text)
    Path(args.out).write_text(text, encoding="utf-8")
    print(f"wrote {args.out}")
    return 1 if (failed or gate_fail) else 0


if __name__ == "__main__":
    raise SystemExit(main())
