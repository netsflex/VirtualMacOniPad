// Opt-in diagnostics for guest GPU work that faults or hangs on the iPad GPU.
// A guest app sees only "Caused GPU Hang Error". When the flag file exists, the
// GPU task keeps its own record of every encoder, pipeline bind and compute
// dispatch in each command buffer, and logs that record when the command
// buffer completes with an error. Metal's encoder execution status is not
// used: its _MTLCommandBufferEncoderInfo objects end up in the NSError the
// task forwards over XPC, which cannot encode them and aborts the task.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "modern_pvg_hang_diagnostics.h"

static const char *const flagPath = "/var/root/VirtualMac2/shader-cache/hang-diagnostics";
static const NSUInteger recordLimit = 160;
static char recordKey, indirectKey;
static atomic_uint encoderSequence;
static atomic_uint reportCount;

static BOOL Enabled(void) {
    struct stat value;
    return !stat(flagPath, &value) && S_ISREG(value.st_mode) && value.st_uid == geteuid();
}

// Bisection switches read from the flag file, one per line:
//   skip-base-instance        drop draws whose base instance is not zero
//   skip-pipeline=<text>      drop draws while a render pipeline label contains <text>
// A dropped draw is logged in the record as "SKIP".
static BOOL skipBaseInstance;
static NSArray<NSString *> *skipPipelines;
static char pipelineKey;
static void LoadSwitches(void) {
    NSString *text = [NSString stringWithContentsOfFile:@(flagPath) encoding:NSUTF8StringEncoding error:NULL];
    NSMutableArray *pipelines = [NSMutableArray array];
    for (NSString *raw in [text componentsSeparatedByString:@"\n"]) {
        NSString *line = [raw stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if ([line isEqualToString:@"skip-base-instance"]) skipBaseInstance = YES;
        else if ([line hasPrefix:@"skip-pipeline="] && line.length > 14) [pipelines addObject:[line substringFromIndex:14]];
    }
    skipPipelines = [pipelines copy];
    fprintf(stderr, "[ModernHangDiag] switches skipBaseInstance=%d skipPipelines=%s\n", skipBaseInstance,
            [skipPipelines componentsJoinedByString:@","].UTF8String ?: "");
}
static BOOL SkipPipeline(id encoder) {
    NSString *label = objc_getAssociatedObject(encoder, &pipelineKey);
    for (NSString *needle in skipPipelines)
        if ([label containsString:needle]) return YES;
    return NO;
}

// The record belongs to the command buffer; each encoder points at it.
static void Append(id owner, NSString *entry) {
    NSMutableArray *record = objc_getAssociatedObject(owner, &recordKey);
    if (!record) return;
    @synchronized (record) {
        if (record.count < recordLimit) [record addObject:entry];
        else if (record.count == recordLimit) [record addObject:@"..."];
    }
}

static void Report(id<MTLCommandBuffer> buffer) {
    NSError *error = buffer.error;
    if (!error || atomic_fetch_add(&reportCount, 1) >= 64) return;
    NSMutableArray *record = objc_getAssociatedObject(buffer, &recordKey);
    NSString *joined;
    @synchronized (record) { joined = [record componentsJoinedByString:@" | "]; }
    NSArray *indirect = record ? objc_getAssociatedObject(record, &indirectKey) : nil;
    for (NSArray *entry in indirect) {
        id argument = entry[0];
        NSUInteger offset = [entry[1] unsignedIntegerValue];
        BOOL indexed = [entry[2] boolValue];
        size_t size = indexed ? sizeof(MTLDrawIndexedPrimitivesIndirectArguments) : sizeof(MTLDrawPrimitivesIndirectArguments);
        if ([argument storageMode] == MTLStorageModePrivate || offset + size > [argument length]) continue;
        const uint32_t *words = (const uint32_t *)((const char *)[argument contents] + offset);
        joined = [joined stringByAppendingFormat:@" || indirect@%lu=[%u %u %u %u%s]", (unsigned long)offset,
                  words[0], words[1], words[2], words[3], indexed ? " +" : ""];
    }
    fprintf(stderr, "[ModernHangDiag] commandBuffer=%p code=%ld gpu=%.3fms error=%s record=%s\n",
            buffer, (long)error.code, (buffer.GPUEndTime - buffer.GPUStartTime) * 1000.0,
            error.localizedDescription.UTF8String ?: "-", joined.UTF8String ?: "-");
}

static NSString *PipelineName(id pipeline) {
    NSString *label = [pipeline respondsToSelector:@selector(label)] ? [pipeline label] : nil;
    return label.length ? label : [NSString stringWithFormat:@"pipeline %p", pipeline];
}

static IMP nativeSetCompute, nativeSetRender, nativeDispatchThreads, nativeDispatchGroups;
static void SetComputePipeline(id self, SEL selector, id pipeline) {
    ((void (*)(id, SEL, id))nativeSetCompute)(self, selector, pipeline);
    if (pipeline) Append(self, [@"compute " stringByAppendingString:PipelineName(pipeline)]);
}
static void SetRenderPipeline(id self, SEL selector, id pipeline) {
    ((void (*)(id, SEL, id))nativeSetRender)(self, selector, pipeline);
    objc_setAssociatedObject(self, &pipelineKey, pipeline ? PipelineName(pipeline) : nil, OBJC_ASSOCIATION_COPY);
    if (pipeline) Append(self, [@"render " stringByAppendingString:PipelineName(pipeline)]);
}
static void DispatchThreads(id self, SEL selector, MTLSize grid, MTLSize group) {
    ((void (*)(id, SEL, MTLSize, MTLSize))nativeDispatchThreads)(self, selector, grid, group);
    Append(self, [NSString stringWithFormat:@"threads %lux%lux%lu/%lux%lux%lu",
        (unsigned long)grid.width, (unsigned long)grid.height, (unsigned long)grid.depth,
        (unsigned long)group.width, (unsigned long)group.height, (unsigned long)group.depth]);
}
static void DispatchGroups(id self, SEL selector, MTLSize groups, MTLSize group) {
    ((void (*)(id, SEL, MTLSize, MTLSize))nativeDispatchGroups)(self, selector, groups, group);
    Append(self, [NSString stringWithFormat:@"groups %lux%lux%lu/%lux%lux%lu",
        (unsigned long)groups.width, (unsigned long)groups.height, (unsigned long)groups.depth,
        (unsigned long)group.width, (unsigned long)group.height, (unsigned long)group.depth]);
}

// Render draws and buffer bindings, so a fault names its draw and inputs.
static NSString *BufferText(id buffer, NSUInteger offset) {
    if (!buffer) return @"nil";
    return [NSString stringWithFormat:@"%lu+%lu%s", (unsigned long)[buffer length], (unsigned long)offset,
            [buffer storageMode] == MTLStorageModePrivate ? "p" : ""];
}
// An indirect draw's arguments are read when the command buffer reports.
static NSString *IndirectText(id owner, id buffer, NSUInteger offset, BOOL indexed) {
    NSMutableArray *record = objc_getAssociatedObject(owner, &recordKey);
    if (record && buffer) {
        NSMutableArray *indirect = objc_getAssociatedObject(record, &indirectKey);
        if (!indirect) {
            indirect = [NSMutableArray array];
            objc_setAssociatedObject(record, &indirectKey, indirect, OBJC_ASSOCIATION_RETAIN);
        }
        @synchronized (indirect) { [indirect addObject:@[buffer, @(offset), @(indexed)]]; }
    }
    return [NSString stringWithFormat:@"indirect %@%s", BufferText(buffer, offset), indexed ? " indexed" : ""];
}

typedef void (*Draw3)(id, SEL, NSUInteger, NSUInteger, NSUInteger);
typedef void (*Draw4)(id, SEL, NSUInteger, NSUInteger, NSUInteger, NSUInteger);
typedef void (*Draw5)(id, SEL, NSUInteger, NSUInteger, NSUInteger, NSUInteger, NSUInteger);
typedef void (*DrawIndirect)(id, SEL, NSUInteger, id, NSUInteger);
typedef void (*Indexed5)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger);
typedef void (*Indexed6)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger);
typedef void (*Indexed8)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger, NSInteger, NSUInteger);
typedef void (*IndexedIndirect)(id, SEL, NSUInteger, NSUInteger, id, NSUInteger, id, NSUInteger);
typedef void (*SetBuffer)(id, SEL, id, NSUInteger, NSUInteger);
typedef void (*SetBytes)(id, SEL, const void *, NSUInteger, NSUInteger);
static IMP nDraw3, nDraw4, nDraw5, nDrawIndirect, nIndexed5, nIndexed6, nIndexed8, nIndexedIndirect;
static IMP nVertexBuffer, nVertexBytes, nVertexOffset, nFragmentBuffer;

static void D3(id e, SEL s, NSUInteger t, NSUInteger start, NSUInteger count) {
    if (SkipPipeline(e)) { Append(e, @"SKIP"); return; }
    ((Draw3)nDraw3)(e, s, t, start, count);
    Append(e, [NSString stringWithFormat:@"draw v%lu+%lu", (unsigned long)start, (unsigned long)count]);
}
static void D4(id e, SEL s, NSUInteger t, NSUInteger start, NSUInteger count, NSUInteger instances) {
    if (SkipPipeline(e)) { Append(e, @"SKIP"); return; }
    ((Draw4)nDraw4)(e, s, t, start, count, instances);
    Append(e, [NSString stringWithFormat:@"draw v%lu+%lu i%lu", (unsigned long)start, (unsigned long)count,
               (unsigned long)instances]);
}
static void D5(id e, SEL s, NSUInteger t, NSUInteger start, NSUInteger count, NSUInteger instances, NSUInteger base) {
    if (SkipPipeline(e) || (skipBaseInstance && base)) { Append(e, @"SKIP"); return; }
    ((Draw5)nDraw5)(e, s, t, start, count, instances, base);
    Append(e, [NSString stringWithFormat:@"draw v%lu+%lu i%lu+%lu", (unsigned long)start, (unsigned long)count,
               (unsigned long)base, (unsigned long)instances]);
}
static void DI(id e, SEL s, NSUInteger t, id buffer, NSUInteger offset) {
    if (SkipPipeline(e)) { Append(e, @"SKIP"); return; }
    ((DrawIndirect)nDrawIndirect)(e, s, t, buffer, offset);
    Append(e, [@"draw " stringByAppendingString:IndirectText(e, buffer, offset, NO)]);
}
static void X5(id e, SEL s, NSUInteger t, NSUInteger count, NSUInteger type, id index, NSUInteger offset) {
    ((Indexed5)nIndexed5)(e, s, t, count, type, index, offset);
    Append(e, [NSString stringWithFormat:@"drawIndexed n%lu idx%@", (unsigned long)count, BufferText(index, offset)]);
}
static void X6(id e, SEL s, NSUInteger t, NSUInteger count, NSUInteger type, id index, NSUInteger offset,
               NSUInteger instances) {
    ((Indexed6)nIndexed6)(e, s, t, count, type, index, offset, instances);
    Append(e, [NSString stringWithFormat:@"drawIndexed n%lu i%lu idx%@", (unsigned long)count,
               (unsigned long)instances, BufferText(index, offset)]);
}
static void X8(id e, SEL s, NSUInteger t, NSUInteger count, NSUInteger type, id index, NSUInteger offset,
               NSUInteger instances, NSInteger baseVertex, NSUInteger baseInstance) {
    if (SkipPipeline(e) || (skipBaseInstance && baseInstance)) { Append(e, @"SKIP"); return; }
    ((Indexed8)nIndexed8)(e, s, t, count, type, index, offset, instances, baseVertex, baseInstance);
    Append(e, [NSString stringWithFormat:@"drawIndexed n%lu i%lu+%lu bv%ld idx%@", (unsigned long)count,
               (unsigned long)baseInstance, (unsigned long)instances, (long)baseVertex, BufferText(index, offset)]);
}
static void XI(id e, SEL s, NSUInteger t, NSUInteger type, id index, NSUInteger indexOffset, id buffer,
               NSUInteger offset) {
    ((IndexedIndirect)nIndexedIndirect)(e, s, t, type, index, indexOffset, buffer, offset);
    Append(e, [NSString stringWithFormat:@"drawIndexed idx%@ %@", BufferText(index, indexOffset),
               IndirectText(e, buffer, offset, YES)]);
}
static void VB(id e, SEL s, id buffer, NSUInteger offset, NSUInteger index) {
    ((SetBuffer)nVertexBuffer)(e, s, buffer, offset, index);
    Append(e, [NSString stringWithFormat:@"vb%lu=%@", (unsigned long)index, BufferText(buffer, offset)]);
}
static void VO(id e, SEL s, NSUInteger offset, NSUInteger index) {
    ((void (*)(id, SEL, NSUInteger, NSUInteger))nVertexOffset)(e, s, offset, index);
    Append(e, [NSString stringWithFormat:@"vb%lu@%lu", (unsigned long)index, (unsigned long)offset]);
}
static void VBytes(id e, SEL s, const void *bytes, NSUInteger length, NSUInteger index) {
    ((SetBytes)nVertexBytes)(e, s, bytes, length, index);
    Append(e, [NSString stringWithFormat:@"vb%lu=bytes%lu", (unsigned long)index, (unsigned long)length]);
}
static IMP nVertexBuffers, nFragmentBuffers;
static void Buffers(id e, const char *stage, const id *buffers, const NSUInteger *offsets, NSRange range) {
    NSMutableString *text = [NSMutableString stringWithFormat:@"%s%lu..", stage, (unsigned long)range.location];
    for (NSUInteger i = 0; i < range.length; ++i)
        [text appendFormat:@"%@%@", i ? @"," : @"", BufferText(buffers ? buffers[i] : nil, offsets ? offsets[i] : 0)];
    Append(e, text);
}
static void VBs(id e, SEL s, const id *buffers, const NSUInteger *offsets, NSRange range) {
    ((void (*)(id, SEL, const id *, const NSUInteger *, NSRange))nVertexBuffers)(e, s, buffers, offsets, range);
    Buffers(e, "vb", buffers, offsets, range);
}
static void FBs(id e, SEL s, const id *buffers, const NSUInteger *offsets, NSRange range) {
    ((void (*)(id, SEL, const id *, const NSUInteger *, NSRange))nFragmentBuffers)(e, s, buffers, offsets, range);
    Buffers(e, "fb", buffers, offsets, range);
}
static void FB(id e, SEL s, id buffer, NSUInteger offset, NSUInteger index) {
    ((SetBuffer)nFragmentBuffer)(e, s, buffer, offset, index);
    Append(e, [NSString stringWithFormat:@"fb%lu=%@", (unsigned long)index, BufferText(buffer, offset)]);
}

static void Swap(Class cls, SEL selector, IMP replacement, IMP *saved) {
    Method m = class_getInstanceMethod(cls, selector);
    if (m) *saved = method_setImplementation(m, replacement);
}

// Each encoder joins its command buffer's record; its concrete class gets the
// hooks the first time an encoder of that kind is seen.
static id Prepare(id buffer, id encoder, const char *kind) {
    if (!encoder) return encoder;
    NSMutableArray *record = objc_getAssociatedObject(buffer, &recordKey);
    if (!record) return encoder;
    objc_setAssociatedObject(encoder, &recordKey, record, OBJC_ASSOCIATION_RETAIN);
    Append(encoder, [NSString stringWithFormat:@"%s#%u", kind, atomic_fetch_add(&encoderSequence, 1)]);
    static dispatch_once_t computeOnce, renderOnce;
    Class cls = object_getClass(encoder);
    if (!strcmp(kind, "compute"))
        dispatch_once(&computeOnce, ^{
            Swap(cls, @selector(setComputePipelineState:), (IMP)SetComputePipeline, &nativeSetCompute);
            Swap(cls, @selector(dispatchThreads:threadsPerThreadgroup:), (IMP)DispatchThreads, &nativeDispatchThreads);
            Swap(cls, @selector(dispatchThreadgroups:threadsPerThreadgroup:), (IMP)DispatchGroups, &nativeDispatchGroups);
        });
    else
        dispatch_once(&renderOnce, ^{
            Swap(cls, @selector(setRenderPipelineState:), (IMP)SetRenderPipeline, &nativeSetRender);
            Swap(cls, @selector(drawPrimitives:vertexStart:vertexCount:), (IMP)D3, &nDraw3);
            Swap(cls, @selector(drawPrimitives:vertexStart:vertexCount:instanceCount:), (IMP)D4, &nDraw4);
            Swap(cls, @selector(drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:), (IMP)D5, &nDraw5);
            Swap(cls, @selector(drawPrimitives:indirectBuffer:indirectBufferOffset:), (IMP)DI, &nDrawIndirect);
            Swap(cls, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:),
                 (IMP)X5, &nIndexed5);
            Swap(cls, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:),
                 (IMP)X6, &nIndexed6);
            Swap(cls, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:),
                 (IMP)X8, &nIndexed8);
            Swap(cls, @selector(drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:),
                 (IMP)XI, &nIndexedIndirect);
            Swap(cls, @selector(setVertexBuffer:offset:atIndex:), (IMP)VB, &nVertexBuffer);
            Swap(cls, @selector(setVertexBufferOffset:atIndex:), (IMP)VO, &nVertexOffset);
            Swap(cls, @selector(setVertexBytes:length:atIndex:), (IMP)VBytes, &nVertexBytes);
            Swap(cls, @selector(setFragmentBuffer:offset:atIndex:), (IMP)FB, &nFragmentBuffer);
            Swap(cls, @selector(setVertexBuffers:offsets:withRange:), (IMP)VBs, &nVertexBuffers);
            Swap(cls, @selector(setFragmentBuffers:offsets:withRange:), (IMP)FBs, &nFragmentBuffers);
        });
    return encoder;
}

static IMP nativeCompute, nativeComputeType, nativeComputeDescriptor, nativeRender;
static id Compute(id self, SEL s) { return Prepare(self, ((id (*)(id, SEL))nativeCompute)(self, s), "compute"); }
static id ComputeType(id self, SEL s, MTLDispatchType t) {
    return Prepare(self, ((id (*)(id, SEL, MTLDispatchType))nativeComputeType)(self, s, t), "compute");
}
static id ComputeDescriptor(id self, SEL s, id d) {
    return Prepare(self, ((id (*)(id, SEL, id))nativeComputeDescriptor)(self, s, d), "compute");
}
static id Render(id self, SEL s, id d) {
    return Prepare(self, ((id (*)(id, SEL, id))nativeRender)(self, s, d), "render");
}

static IMP nativeCommandBuffer, nativeCommandBufferUnretained, nativeCommandBufferDescriptor;
static id Observe(id buffer) {
    if (!buffer) return buffer;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Class cls = object_getClass(buffer);
        Swap(cls, @selector(computeCommandEncoder), (IMP)Compute, &nativeCompute);
        Swap(cls, @selector(computeCommandEncoderWithDispatchType:), (IMP)ComputeType, &nativeComputeType);
        Swap(cls, @selector(computeCommandEncoderWithDescriptor:), (IMP)ComputeDescriptor, &nativeComputeDescriptor);
        Swap(cls, @selector(renderCommandEncoderWithDescriptor:), (IMP)Render, &nativeRender);
    });
    objc_setAssociatedObject(buffer, &recordKey, [NSMutableArray array], OBJC_ASSOCIATION_RETAIN);
    [buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) { Report(completed); }];
    return buffer;
}

static id CommandBuffer(id self, SEL s) {
    return Observe(((id (*)(id, SEL))nativeCommandBuffer)(self, s));
}
static id CommandBufferUnretained(id self, SEL s) {
    return Observe(((id (*)(id, SEL))nativeCommandBufferUnretained)(self, s));
}
static id CommandBufferDescriptor(id self, SEL s, id descriptor) {
    return Observe(((id (*)(id, SEL, id))nativeCommandBufferDescriptor)(self, s, descriptor));
}

bool VZModernInstallHangDiagnostics(id device) {
    if (!device || !Enabled()) return false;
    LoadSwitches();
    id queue = [device newCommandQueue];
    if (!queue) return false;
    Class cls = object_getClass(queue);
    [queue release];
    Swap(cls, @selector(commandBuffer), (IMP)CommandBuffer, &nativeCommandBuffer);
    Swap(cls, @selector(commandBufferWithUnretainedReferences), (IMP)CommandBufferUnretained,
         &nativeCommandBufferUnretained);
    Swap(cls, @selector(commandBufferWithDescriptor:), (IMP)CommandBufferDescriptor,
         &nativeCommandBufferDescriptor);
    fprintf(stderr, "[ModernHangDiag] installed command buffer records on %s\n", class_getName(cls));
    return true;
}
