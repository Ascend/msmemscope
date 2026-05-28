# **Introduction**

## Overview

MindStudio MemScope (msMemScope) is a memory analysis tool developed based on the Ascend hardware to locate memory problems during model training and inference. It provides functions such as memory leak detection, memory comparison, memory block monitoring, memory decomposition, and identification of inefficient memory, helping you locate and handle problems.

## Functions

msMemScope provides memory collection and analysis functions.

|Function|Description|
|---|---|
|Memory collection|msMemScope can collection memory events and allows custom memory collection scope and items to provide raw data for subsequent analysis.<br> &#8226; **Collection via Python APIs**: Python APIs are utilized to collect information, including custom collection scopes and items, memory events, and Python Trace events, for precise collection and efficient analysis.<br> &#8226; **Collection through CLIs**: formation is collected through CLIs, and memory event collection and memory analysis capabilities are supported in non-Python scenarios.|
|Memory analysis|msMemScope provides analysis capabilities such as leak detection, comparison, monitoring, decomposition, and identification of inefficient memory based on the collected memory data, helping you quickly diagnose and optimize memory problems.<br> &#8226; **Memory leak analysis**: If memory is not released for a long time or memory leaks occur, msMemScope provides memory leak analysis and kernel-launch-based memory change analysis to locate and analyze problems.<br> &#8226; **Memory comparison**: If the memory usage differs between two steps, it may lead to excessive memory usage or even out of memory (OOM) errors. In this case, use the memory comparison analysis function of msMemScope to locate and analyze the problem.<br> &#8226; **Memory block monitoring**: In foundation model scenarios, if it is difficult to locate memory corruption, msMemScope can monitor the specified memory blocks before and after operator execution through Python APIs and CLIs. Based on changes in the memory block data, it can quickly determine the scope or exact location of memory corruption between operators.<br> &#8226; **Memory decomposition**: msMemScope supports memory decomposition to analyze the memory usage of the CANN layer and Ascend Extension for PyTorch framework and outputs model weights, activations, gradients, and optimizer and other component memory usage.<br> &#8226; **Identification of inefficient memory**: During model training and inference, some memory blocks may not be used immediately after being allocated or may not be deallocated in a timely manner after being used. msMemScope identifies the inefficient memory usage to optimize model training and inference.|

## Compatibility Information

msMemScope supports memory collection of CANN, Ascend Extension for PyTorch, MindSpore, and Aten operators. The following table lists the supported versions.

|Product|Compatibility Description|
|--------|--------|
|CANN|Ascend Transformers Boost (ATB) operators of CANN 8.2.RC1 and later versions|
|Ascend Extension for PyTorch|Ascend Extension for PyTorch 7.0.0 and later versions|
|MindSpore|MindSpore 2.7.0 and later versions|
|Aten operators|To collect Aten operator dispatch and access events, use PyTorch 2.3.1 or later.|
