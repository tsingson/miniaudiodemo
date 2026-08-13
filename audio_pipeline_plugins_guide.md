# Audio Pipeline and Plugins Guide

## 1. Purpose

The audio path is organized as:

```text
source -> nodes/plugins -> route/fanout -> branch plugins -> mix -> outputs
```

Every processing, routing, mixing, and output operation is a pipeline node. A source only supplies audio blocks; it does not know whether the destination is PCM5102A, UAC2, a file, Bluetooth, or TCP.

The processing core is shared by macOS, Zephyr STM32F401, and future STM32H7 targets. Platform code owns device callbacks, threads, USB, sockets, and DMA. The node contract remains portable C.

## 2. Block contract

`play_frame_block` is the unit passed between nodes:

- `planar` or `interleaved`: storage layout;
- `frameCount`: valid frames in this block;
- `channels`: active channel count;
- `sampleRate`: frames per second;
- `channelMask`: optional channel selection, with zero meaning the default mask.

Nodes may modify samples in place. They must not retain block pointers after returning. The caller owns storage and controls lifetime.

The pipeline rejects empty blocks, zero sample rates, unsupported channel counts, invalid layouts, and blocks larger than the configured working buffer.

## 3. Node API

The basic node is a `play_pipeline_stage`:

```c
int process(void *ctx, play_frame_block *block);
```

Return `0` only when the node consumed the block successfully. A nonzero return stops the current pipeline run and increments the higher-level failure counter.

Typical nodes are:

- input adapter: converts a source callback into a block;
- EQ, compressor, or limiter: transforms samples in place;
- route node: selects channels or sends a copy to a branch;
- fanout node: executes multiple branch pipelines;
- mix node: combines branch results with explicit weights;
- output node: converts and submits samples to a device or sink.

## 4. Fanout and mixing

`play_pipeline_fanout` is a node-level composite. It owns branch scratch buffers, branch pipelines, and per-branch weights:

```c
play_pipeline_fanout fanout;
play_pipeline_fanout_init(&fanout, 2, 512);

int a = play_pipeline_fanout_add_branch(&fanout, 0.5f);
int b = play_pipeline_fanout_add_branch(&fanout, 0.5f);

play_pipeline_add_stage(play_pipeline_fanout_branch(&fanout, a),
                        "branch-a-plugin", 1, pluginA, pluginA_process);
play_pipeline_add_stage(play_pipeline_fanout_branch(&fanout, b),
                        "branch-b-plugin", 1, pluginB, pluginB_process);

play_pipeline_add_stage(&mainPipeline, "fanout-mix", 1,
                        &fanout, play_pipeline_fanout_process);
```

The node copies the input block to each branch, runs the branch pipeline, then writes the weighted sum back into the original block. This makes route and mix explicit nodes rather than hidden behavior in a source or output.

The current implementation supports four branches and two channels with a fixed maximum of 512 frames. H7 can raise these limits after measuring RAM and cache behavior.

## 5. Multiple inputs and outputs

Multiple inputs are represented by source adapters that produce blocks with the same format contract. A mixer source/node can combine them before the main pipeline. The adapter should define whether it blocks, returns silence, or reports underflow.

The reusable `play_pipeline_mix` node provides the simple multi-input interface:

```c
play_pipeline_mix mix;
play_pipeline_mix_init(&mix);
play_pipeline_mix_set_input(&mix, 0, &uac2Block, 1.0f);
play_pipeline_mix_set_input(&mix, 1, &bluetoothBlock, 0.25f);
play_pipeline_add_stage(&pipeline, "input-mix", 1, &mix,
                        play_pipeline_mix_process);
```

Inputs must have the same layout, frame count, channel count, and sample rate as the output block. Input block pointers are borrowed for the current processing call; the caller owns and synchronizes their storage.

Multiple outputs are represented by output stages attached after processing, or by fanout branches when each output needs different processing:

```text
source -> common EQ -> output fanout
                         |-> PCM5102A
                         |-> UAC2
                         |-> recorder
```

When outputs need different EQ, use:

```text
source -> route/fanout -> branch A plugins -> output A
                       -> branch B plugins -> output B
```

Output stages must not retain the caller's block pointer. A DMA output may copy into its own queue or hold a documented buffer owner until completion.

## 6. Realtime rules

The audio callback must not perform display I/O, logging, allocation, filesystem access, or network I/O. Build and configure node lists once; do not call `init` or `add_stage` inside the audio block loop.

The pipeline exposes processed and failed block counters. Counters are diagnostic only and should be read from a non-realtime status task.

The block size is a platform policy:

- macOS/CoreAudio chooses its callback block size;
- STM32F401 needs a lightweight graph and conservative block budget;
- STM32H7 can use larger graphs, CMSIS-DSP, and more branches after measurement.

Never hide an overrun by silently dropping audio blocks. Report the failure, apply an explicit underrun policy, and preserve ownership rules.

## 7. Plugin lifecycle

Plugins should expose three phases:

1. `init`: allocate or bind state and precompute coefficients;
2. `process`: transform one block without allocation or topology changes;
3. `update`: atomically publish new parameters for the next block.

The existing EQ/plugin implementation follows this model conceptually. The persistent EQ runtime now builds its stage container once and changes only enable flags during processing.

## 8. Platform parity

macOS and STM32 must use the same node order, parameter units, channel masks, and sample-rate rules. The host parity test compares direct DSP processing with the STM32-style pipeline DSP node and currently reports zero sample difference.

Windows 11 should reuse this core. Only the audio-device adapter, thread primitives, and HTTP/TCP adapter need platform implementations.

## 9. Current validation

The host suite covers EQ stages, plugin ordering, channel routing, pipeline bypass, fanout branch processing, weighted mixing, and macOS/STM32 numerical parity.

The STM32F401 test has also validated a CMSIS-DSP biquad plugin in the same pipeline. The F401 cannot run the full existing EQ graph at a stable 48 kHz real-time rate; the intended next target for the full graph is STM32H7.

## 10. UAC2 integration boundary

UAC2 is a source adapter. USB packet parsing, format validation, buffering, and feedback remain in the UAC2 device layer. Complete PCM blocks are submitted to the pipeline entry. UAC2 must not call PCM5102A directly.

The next UAC2 phase should add:

- a source-side format/underflow contract;
- packet-to-block buffering independent of the output device;
- checksum/frame diagnostics;
- a test sink before enabling physical DAC output.

The next implementation step is to make the UAC2 receive queue publish a
`play_frame_block` at this graph boundary, then validate UAC2 with a null/test
output before reconnecting PCM5102A.

The current UAC2 receiver already has the first adapter boundary: it converts
validated stereo PCM16 packets to float blocks and calls a registered sink. It
now also counts invalid packets, odd-byte packets, frames, and a rolling
payload checksum. The remaining work is to replace the device-specific sink
name with a graph/source adapter and add a null sink test before DAC output.