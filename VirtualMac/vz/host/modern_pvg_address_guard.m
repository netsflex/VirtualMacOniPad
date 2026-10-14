// Skip guest GPU work that would dereference a guest-written GPU address.
//
// A macOS 27 guest encodes argument buffers with its own GPU addresses and
// resource handles, expecting the host to place resources at those addresses.
// iPadOS 16 Metal cannot, so a shader that loads such an address faults the
// iPad GPU ("GPU Address Fault"); the guest app is then blocked from further
// submissions ("Ignored for causing prior/excessive GPU errors"), which shows
// up as flicker, unwritten tiles and crashes. VFX particles in Weather do this.
//
// Converted libraries whose function loads an address are marked, the mark
// follows the functions made from them into pipelines, and draws and dispatches
// that use a marked pipeline are not encoded. The guest app loses those effects
// but keeps the rest of its rendering. shader-cache/address-guard-off disables it.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "modern_pvg_address_guard.h"

static const char *const disablePath = "/var/root/VirtualMac2/shader-cache/address-guard-off";
static char markKey;
static BOOL installed;
static atomic_uint markedPipelines;
static atomic_ulong skippedWork;

static void Mark(id object) {
    if (object) objc_setAssociatedObject(object, &markKey, @YES, OBJC_ASSOCIATION_RETAIN);
}

static BOOL Marked(id object) {
    return object && [objc_getAssociatedObject(object, &markKey) boolValue];
}

static void Skipped(const char *kind) {
    unsigned long count = atomic_fetch_add(&skippedWork, 1) + 1;
    if (count <= 4 || !(count % 1000))
        fprintf(stderr, "[ModernAddressGuard] skipped %s, %lu total\n", kind, count);
}

// Functions made from a marked library carry the mark.
static IMP nName, nNameConstants, nDescriptor;
static id FunctionName(id self, SEL s, NSString *name) {
    id function = ((id (*)(id, SEL, id))nName)(self, s, name);
    if (Marked(self)) Mark(function);
    return function;
}
static id FunctionNameConstants(id self, SEL s, NSString *name, id constants, NSError **error) {
    id function = ((id (*)(id, SEL, id, id, NSError **))nNameConstants)(self, s, name, constants, error);
    if (Marked(self)) Mark(function);
    return function;
}
static id FunctionDescriptor(id self, SEL s, id descriptor, NSError **error) {
    id function = ((id (*)(id, SEL, id, NSError **))nDescriptor)(self, s, descriptor, error);
    if (Marked(self)) Mark(function);
    return function;
}

static void Swap(Class cls, SEL selector, IMP replacement, IMP *saved) {
    Method m = class_getInstanceMethod(cls, selector);
    if (m) *saved = method_setImplementation(m, replacement);
}

void VZModernMarkAddressLibrary(id library) {
    if (!installed || !library) return;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Class cls = object_getClass(library);
        Swap(cls, @selector(newFunctionWithName:), (IMP)FunctionName, &nName);
        Swap(cls, @selector(newFunctionWithName:constantValues:error:), (IMP)FunctionNameConstants, &nNameConstants);
        Swap(cls, @selector(newFunctionWithDescriptor:error:), (IMP)FunctionDescriptor, &nDescriptor);
    });
    Mark(library);
}

static id Property(id object, const char *name) {
    SEL selector = sel_registerName(name);
    return [object respondsToSelector:selector] ? ((id (*)(id, SEL))objc_msgSend)(object, selector) : nil;
}

void VZModernMarkAddressPipeline(id pipeline, id source) {
    if (!installed || !pipeline || !source) return;
    id functions[] = {
        [source conformsToProtocol:@protocol(MTLFunction)] ? source : nil,
        Property(source, "vertexFunction"), Property(source, "fragmentFunction"), Property(source, "computeFunction"),
    };
    for (unsigned i = 0; i < sizeof(functions) / sizeof(functions[0]); ++i) {
        if (!Marked(functions[i])) continue;
        Mark(pipeline);
        if (atomic_fetch_add(&markedPipelines, 1) < 16)
            fprintf(stderr, "[ModernAddressGuard] pipeline %s uses guest addresses in %s; its work is skipped\n",
                    [Property(pipeline, "label") UTF8String] ?: "-", [[functions[i] name] UTF8String] ?: "-");
        return;
    }
}

// Encoders remember whether their current pipeline is marked.
static char currentKey;
static BOOL Current(id encoder) { return [objc_getAssociatedObject(encoder, &currentKey) boolValue]; }
static void SetCurrent(id encoder, id pipeline) {
    objc_setAssociatedObject(encoder, &currentKey, Marked(pipeline) ? @YES : nil, OBJC_ASSOCIATION_RETAIN);
}

static IMP nSetRender, nSetCompute;
static void SetRender(id self, SEL s, id pipeline) {
    ((void (*)(id, SEL, id))nSetRender)(self, s, pipeline);
    SetCurrent(self, pipeline);
}
static void SetCompute(id self, SEL s, id pipeline) {
    ((void (*)(id, SEL, id))nSetCompute)(self, s, pipeline);
    SetCurrent(self, pipeline);
}

static IMP nD3, nD4, nD5, nDI, nX5, nX6, nX8, nXI, nThreads, nGroups, nGroupsIndirect;
static void D3(id e, SEL s, NSUInteger t, NSUInteger a, NSUInteger b) {
    if (Current(e)) { Skipped("draw"); return; }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger))nD3)(e, s, t, a, b);
}
static void D4(id e, SEL s, NSUInteger t, NSUInteger a, NSUInteger b, NSUInteger c) {
    if (Current(e)) { Skipped("draw"); return; }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, NSUInteger))nD4)(e, s, t, a, b, c);
}
static void D5(id e, SEL s, NSUInteger t, NSUInteger a, NSUInteger b, NSUInteger c, NSUInteger d) {
    if (Current(e)) { Skipped("draw"); return; }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, NSUInteger, NSUInteger))nD5)(e, s, t, a, b, c, d);
}
static void DI(id e, SEL s, NSUInteger t, id buffer, NSUInteger offset) {
    if (Current(e)) { Skipped("draw"); return; }
    ((void (*)(id, SEL, NSUInteger, id, NSUInteger))nDI)(e, s, t, buffer, offset);
}
static void X5(id e, SEL s, NSUInteger t, NSUInteger n, NSUInteger type, id index, NSUInteger offset) {
    if (Current(e)) { Skipped("draw"); return; }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger))nX5)(e, s, t, n, type, index, offset);
}
static void X6(id e, SEL s, NSUInteger t, NSUInteger n, NSUInteger type, id index, NSUInteger offset, NSUInteger i) {
    if (Current(e)) { Skipped("draw"); return; }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger))nX6)(
        e, s, t, n, type, index, offset, i);
}
static void X8(id e, SEL s, NSUInteger t, NSUInteger n, NSUInteger type, id index, NSUInteger offset,
               NSUInteger i, NSInteger baseVertex, NSUInteger baseInstance) {
    if (Current(e)) { Skipped("draw"); return; }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, NSUInteger, id, NSUInteger, NSUInteger, NSInteger, NSUInteger))nX8)(
        e, s, t, n, type, index, offset, i, baseVertex, baseInstance);
}
static void XI(id e, SEL s, NSUInteger t, NSUInteger type, id index, NSUInteger indexOffset, id buffer, NSUInteger offset) {
    if (Current(e)) { Skipped("draw"); return; }
    ((void (*)(id, SEL, NSUInteger, NSUInteger, id, NSUInteger, id, NSUInteger))nXI)(
        e, s, t, type, index, indexOffset, buffer, offset);
}
static void Threads(id e, SEL s, MTLSize grid, MTLSize group) {
    if (Current(e)) { Skipped("dispatch"); return; }
    ((void (*)(id, SEL, MTLSize, MTLSize))nThreads)(e, s, grid, group);
}
static void Groups(id e, SEL s, MTLSize groups, MTLSize group) {
    if (Current(e)) { Skipped("dispatch"); return; }
    ((void (*)(id, SEL, MTLSize, MTLSize))nGroups)(e, s, groups, group);
}
static void GroupsIndirect(id e, SEL s, id buffer, NSUInteger offset, MTLSize group) {
    if (Current(e)) { Skipped("dispatch"); return; }
    ((void (*)(id, SEL, id, NSUInteger, MTLSize))nGroupsIndirect)(e, s, buffer, offset, group);
}

// The concrete encoder classes come from throwaway encoders that are ended
// without being committed.
bool VZModernInstallAddressGuard(id<MTLDevice> device) {
    struct stat value;
    if (!device || !stat(disablePath, &value)) return false;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> buffer = [queue commandBuffer];
    id<MTLComputeCommandEncoder> compute = [buffer computeCommandEncoder];
    Class computeClass = object_getClass(compute);
    [compute endEncoding];
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                         width:1 height:1 mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:descriptor];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    id<MTLRenderCommandEncoder> render = [buffer renderCommandEncoderWithDescriptor:pass];
    Class renderClass = object_getClass(render);
    [render endEncoding];
    [target release];
    [queue release];
    if (!computeClass || !renderClass) return false;

    Swap(renderClass, @selector(setRenderPipelineState:), (IMP)SetRender, &nSetRender);
    Swap(renderClass, @selector(drawPrimitives:vertexStart:vertexCount:), (IMP)D3, &nD3);
    Swap(renderClass, @selector(drawPrimitives:vertexStart:vertexCount:instanceCount:), (IMP)D4, &nD4);
    Swap(renderClass, @selector(drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:), (IMP)D5, &nD5);
    Swap(renderClass, @selector(drawPrimitives:indirectBuffer:indirectBufferOffset:), (IMP)DI, &nDI);
    Swap(renderClass, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:), (IMP)X5, &nX5);
    Swap(renderClass, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:),
         (IMP)X6, &nX6);
    Swap(renderClass, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:),
         (IMP)X8, &nX8);
    Swap(renderClass, @selector(drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:indirectBufferOffset:),
         (IMP)XI, &nXI);
    Swap(computeClass, @selector(setComputePipelineState:), (IMP)SetCompute, &nSetCompute);
    Swap(computeClass, @selector(dispatchThreads:threadsPerThreadgroup:), (IMP)Threads, &nThreads);
    Swap(computeClass, @selector(dispatchThreadgroups:threadsPerThreadgroup:), (IMP)Groups, &nGroups);
    Swap(computeClass, @selector(dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:),
         (IMP)GroupsIndirect, &nGroupsIndirect);
    installed = YES;
    fprintf(stderr, "[ModernAddressGuard] installed on %s and %s\n", class_getName(renderClass), class_getName(computeClass));
    return true;
}
